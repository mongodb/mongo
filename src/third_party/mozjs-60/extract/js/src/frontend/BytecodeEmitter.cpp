/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS bytecode generation.
 */

#include "frontend/BytecodeEmitter.h"

#include "mozilla/ArrayUtils.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/Maybe.h"
#include "mozilla/PodOperations.h"

#include <string.h>

#include "jsapi.h"
#include "jsnum.h"
#include "jstypes.h"
#include "jsutil.h"

#include "ds/Nestable.h"
#include "frontend/Parser.h"
#include "frontend/TokenStream.h"
#include "vm/BytecodeUtil.h"
#include "vm/Debugger.h"
#include "vm/GeneratorObject.h"
#include "vm/JSAtom.h"
#include "vm/JSContext.h"
#include "vm/JSFunction.h"
#include "vm/JSScript.h"
#include "vm/Stack.h"
#include "wasm/AsmJS.h"

#include "frontend/ParseNode-inl.h"
#include "vm/EnvironmentObject-inl.h"
#include "vm/JSAtom-inl.h"
#include "vm/JSScript-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;
using namespace js::gc;
using namespace js::frontend;

using mozilla::AssertedCast;
using mozilla::DebugOnly;
using mozilla::Maybe;
using mozilla::Nothing;
using mozilla::NumberIsInt32;
using mozilla::PodCopy;
using mozilla::Some;
using mozilla::Unused;

class BreakableControl;
class LabelControl;
class LoopControl;
class ForOfLoopControl;
class TryFinallyControl;

static bool
ParseNodeRequiresSpecialLineNumberNotes(ParseNode* pn)
{
    // The few node types listed below are exceptions to the usual
    // location-source-note-emitting code in BytecodeEmitter::emitTree().
    // Single-line `while` loops and C-style `for` loops require careful
    // handling to avoid strange stepping behavior.
    // Functions usually shouldn't have location information (bug 1431202).

    ParseNodeKind kind = pn->getKind();
    return kind == ParseNodeKind::While ||
           kind == ParseNodeKind::For ||
           kind == ParseNodeKind::Function;
}

// A cache that tracks superfluous TDZ checks.
//
// Each basic block should have a TDZCheckCache in scope. Some NestableControl
// subclasses contain a TDZCheckCache.
class BytecodeEmitter::TDZCheckCache : public Nestable<BytecodeEmitter::TDZCheckCache>
{
    PooledMapPtr<CheckTDZMap> cache_;

    MOZ_MUST_USE bool ensureCache(BytecodeEmitter* bce) {
        return cache_ || cache_.acquire(bce->cx);
    }

  public:
    explicit TDZCheckCache(BytecodeEmitter* bce)
      : Nestable<TDZCheckCache>(&bce->innermostTDZCheckCache),
        cache_(bce->cx->frontendCollectionPool())
    { }

    Maybe<MaybeCheckTDZ> needsTDZCheck(BytecodeEmitter* bce, JSAtom* name);
    MOZ_MUST_USE bool noteTDZCheck(BytecodeEmitter* bce, JSAtom* name, MaybeCheckTDZ check);
};

class BytecodeEmitter::NestableControl : public Nestable<BytecodeEmitter::NestableControl>
{
    StatementKind kind_;

    // The innermost scope when this was pushed.
    EmitterScope* emitterScope_;

  protected:
    NestableControl(BytecodeEmitter* bce, StatementKind kind)
      : Nestable<NestableControl>(&bce->innermostNestableControl),
        kind_(kind),
        emitterScope_(bce->innermostEmitterScopeNoCheck())
    { }

  public:
    using Nestable<NestableControl>::enclosing;
    using Nestable<NestableControl>::findNearest;

    StatementKind kind() const {
        return kind_;
    }

    EmitterScope* emitterScope() const {
        return emitterScope_;
    }

    template <typename T>
    bool is() const;

    template <typename T>
    T& as() {
        MOZ_ASSERT(this->is<T>());
        return static_cast<T&>(*this);
    }
};

// Template specializations are disallowed in different namespaces; specialize
// all the NestableControl subtypes up front.
namespace js {
namespace frontend {

template <>
bool
BytecodeEmitter::NestableControl::is<BreakableControl>() const
{
    return StatementKindIsUnlabeledBreakTarget(kind_) || kind_ == StatementKind::Label;
}

template <>
bool
BytecodeEmitter::NestableControl::is<LabelControl>() const
{
    return kind_ == StatementKind::Label;
}

template <>
bool
BytecodeEmitter::NestableControl::is<LoopControl>() const
{
    return StatementKindIsLoop(kind_);
}

template <>
bool
BytecodeEmitter::NestableControl::is<ForOfLoopControl>() const
{
    return kind_ == StatementKind::ForOfLoop;
}

template <>
bool
BytecodeEmitter::NestableControl::is<TryFinallyControl>() const
{
    return kind_ == StatementKind::Try || kind_ == StatementKind::Finally;
}

} // namespace frontend
} // namespace js

class BreakableControl : public BytecodeEmitter::NestableControl
{
  public:
    // Offset of the last break.
    JumpList breaks;

    BreakableControl(BytecodeEmitter* bce, StatementKind kind)
      : NestableControl(bce, kind)
    {
        MOZ_ASSERT(is<BreakableControl>());
    }

    MOZ_MUST_USE bool patchBreaks(BytecodeEmitter* bce) {
        return bce->emitJumpTargetAndPatch(breaks);
    }
};

class LabelControl : public BreakableControl
{
    RootedAtom label_;

    // The code offset when this was pushed. Used for effectfulness checking.
    ptrdiff_t startOffset_;

  public:
    LabelControl(BytecodeEmitter* bce, JSAtom* label, ptrdiff_t startOffset)
      : BreakableControl(bce, StatementKind::Label),
        label_(bce->cx, label),
        startOffset_(startOffset)
    { }

    HandleAtom label() const {
        return label_;
    }

    ptrdiff_t startOffset() const {
        return startOffset_;
    }
};

class LoopControl : public BreakableControl
{
    // Loops' children are emitted in dominance order, so they can always
    // have a TDZCheckCache.
    BytecodeEmitter::TDZCheckCache tdzCache_;

    // Stack depth when this loop was pushed on the control stack.
    int32_t stackDepth_;

    // The loop nesting depth. Used as a hint to Ion.
    uint32_t loopDepth_;

    // Can we OSR into Ion from here? True unless there is non-loop state on the stack.
    bool canIonOsr_;

  public:
    // The target of continue statement jumps, e.g., the update portion of a
    // for(;;) loop.
    JumpTarget continueTarget;

    // Offset of the last continue in the loop.
    JumpList continues;

    LoopControl(BytecodeEmitter* bce, StatementKind loopKind)
      : BreakableControl(bce, loopKind),
        tdzCache_(bce),
        continueTarget({ -1 })
    {
        MOZ_ASSERT(is<LoopControl>());

        LoopControl* enclosingLoop = findNearest<LoopControl>(enclosing());

        stackDepth_ = bce->stackDepth;
        loopDepth_ = enclosingLoop ? enclosingLoop->loopDepth_ + 1 : 1;

        int loopSlots;
        if (loopKind == StatementKind::Spread) {
            // The iterator next method, the iterator, the result array, and
            // the current array index are on the stack.
            loopSlots = 4;
        } else if (loopKind == StatementKind::ForOfLoop) {
            // The iterator next method, the iterator, and the current value
            // are on the stack.
            loopSlots = 3;
        } else if (loopKind == StatementKind::ForInLoop) {
            // The iterator and the current value are on the stack.
            loopSlots = 2;
        } else {
            // No additional loop values are on the stack.
            loopSlots = 0;
        }

        MOZ_ASSERT(loopSlots <= stackDepth_);

        if (enclosingLoop) {
            canIonOsr_ = (enclosingLoop->canIonOsr_ &&
                          stackDepth_ == enclosingLoop->stackDepth_ + loopSlots);
        } else {
            canIonOsr_ = stackDepth_ == loopSlots;
        }
    }

    uint32_t loopDepth() const {
        return loopDepth_;
    }

    bool canIonOsr() const {
        return canIonOsr_;
    }

    MOZ_MUST_USE bool emitSpecialBreakForDone(BytecodeEmitter* bce) {
        // This doesn't pop stack values, nor handle any other controls.
        // Should be called on the toplevel of the loop.
        MOZ_ASSERT(bce->stackDepth == stackDepth_);
        MOZ_ASSERT(bce->innermostNestableControl == this);

        if (!bce->newSrcNote(SRC_BREAK))
            return false;
        if (!bce->emitJump(JSOP_GOTO, &breaks))
            return false;

        return true;
    }

    MOZ_MUST_USE bool patchBreaksAndContinues(BytecodeEmitter* bce) {
        MOZ_ASSERT(continueTarget.offset != -1);
        if (!patchBreaks(bce))
            return false;
        bce->patchJumpsToTarget(continues, continueTarget);
        return true;
    }
};

class TryFinallyControl : public BytecodeEmitter::NestableControl
{
    bool emittingSubroutine_;

  public:
    // The subroutine when emitting a finally block.
    JumpList gosubs;

    TryFinallyControl(BytecodeEmitter* bce, StatementKind kind)
      : NestableControl(bce, kind),
        emittingSubroutine_(false)
    {
        MOZ_ASSERT(is<TryFinallyControl>());
    }

    void setEmittingSubroutine() {
        emittingSubroutine_ = true;
    }

    bool emittingSubroutine() const {
        return emittingSubroutine_;
    }
};

static inline void
MarkAllBindingsClosedOver(LexicalScope::Data& data)
{
    BindingName* names = data.names;
    for (uint32_t i = 0; i < data.length; i++)
        names[i] = BindingName(names[i].name(), true);
}

// A scope that introduces bindings.
class BytecodeEmitter::EmitterScope : public Nestable<BytecodeEmitter::EmitterScope>
{
    // The cache of bound names that may be looked up in the
    // scope. Initially populated as the set of names this scope binds. As
    // names are looked up in enclosing scopes, they are cached on the
    // current scope.
    PooledMapPtr<NameLocationMap> nameCache_;

    // If this scope's cache does not include free names, such as the
    // global scope, the NameLocation to return.
    Maybe<NameLocation> fallbackFreeNameLocation_;

    // True if there is a corresponding EnvironmentObject on the environment
    // chain, false if all bindings are stored in frame slots on the stack.
    bool hasEnvironment_;

    // The number of enclosing environments. Used for error checking.
    uint8_t environmentChainLength_;

    // The next usable slot on the frame for not-closed over bindings.
    //
    // The initial frame slot when assigning slots to bindings is the
    // enclosing scope's nextFrameSlot. For the first scope in a frame,
    // the initial frame slot is 0.
    uint32_t nextFrameSlot_;

    // The index in the BytecodeEmitter's interned scope vector, otherwise
    // ScopeNote::NoScopeIndex.
    uint32_t scopeIndex_;

    // If kind is Lexical, Catch, or With, the index in the BytecodeEmitter's
    // block scope note list. Otherwise ScopeNote::NoScopeNote.
    uint32_t noteIndex_;

    MOZ_MUST_USE bool ensureCache(BytecodeEmitter* bce) {
        return nameCache_.acquire(bce->cx);
    }

    template <typename BindingIter>
    MOZ_MUST_USE bool checkSlotLimits(BytecodeEmitter* bce, const BindingIter& bi) {
        if (bi.nextFrameSlot() >= LOCALNO_LIMIT ||
            bi.nextEnvironmentSlot() >= ENVCOORD_SLOT_LIMIT)
        {
            bce->reportError(nullptr, JSMSG_TOO_MANY_LOCALS);
            return false;
        }
        return true;
    }

    MOZ_MUST_USE bool checkEnvironmentChainLength(BytecodeEmitter* bce) {
        uint32_t hops;
        if (EmitterScope* emitterScope = enclosing(&bce))
            hops = emitterScope->environmentChainLength_;
        else
            hops = bce->sc->compilationEnclosingScope()->environmentChainLength();

        if (hops >= ENVCOORD_HOPS_LIMIT - 1) {
            bce->reportError(nullptr, JSMSG_TOO_DEEP, js_function_str);
            return false;
        }

        environmentChainLength_ = mozilla::AssertedCast<uint8_t>(hops + 1);
        return true;
    }

    void updateFrameFixedSlots(BytecodeEmitter* bce, const BindingIter& bi) {
        nextFrameSlot_ = bi.nextFrameSlot();
        if (nextFrameSlot_ > bce->maxFixedSlots)
            bce->maxFixedSlots = nextFrameSlot_;
        MOZ_ASSERT_IF(bce->sc->isFunctionBox() &&
                      (bce->sc->asFunctionBox()->isGenerator() ||
                       bce->sc->asFunctionBox()->isAsync()),
                      bce->maxFixedSlots == 0);
    }

    MOZ_MUST_USE bool putNameInCache(BytecodeEmitter* bce, JSAtom* name, NameLocation loc) {
        NameLocationMap& cache = *nameCache_;
        NameLocationMap::AddPtr p = cache.lookupForAdd(name);
        MOZ_ASSERT(!p);
        if (!cache.add(p, name, loc)) {
            ReportOutOfMemory(bce->cx);
            return false;
        }
        return true;
    }

    Maybe<NameLocation> lookupInCache(BytecodeEmitter* bce, JSAtom* name) {
        if (NameLocationMap::Ptr p = nameCache_->lookup(name))
            return Some(p->value().wrapped);
        if (fallbackFreeNameLocation_ && nameCanBeFree(bce, name))
            return fallbackFreeNameLocation_;
        return Nothing();
    }

    friend bool BytecodeEmitter::needsImplicitThis();

    EmitterScope* enclosing(BytecodeEmitter** bce) const {
        // There is an enclosing scope with access to the same frame.
        if (EmitterScope* inFrame = enclosingInFrame())
            return inFrame;

        // We are currently compiling the enclosing script, look in the
        // enclosing BCE.
        if ((*bce)->parent) {
            *bce = (*bce)->parent;
            return (*bce)->innermostEmitterScopeNoCheck();
        }

        return nullptr;
    }

    Scope* enclosingScope(BytecodeEmitter* bce) const {
        if (EmitterScope* es = enclosing(&bce))
            return es->scope(bce);

        // The enclosing script is already compiled or the current script is the
        // global script.
        return bce->sc->compilationEnclosingScope();
    }

    static bool nameCanBeFree(BytecodeEmitter* bce, JSAtom* name) {
        // '.generator' cannot be accessed by name.
        return name != bce->cx->names().dotGenerator;
    }

    static NameLocation searchInEnclosingScope(JSAtom* name, Scope* scope, uint8_t hops);
    NameLocation searchAndCache(BytecodeEmitter* bce, JSAtom* name);

    template <typename ScopeCreator>
    MOZ_MUST_USE bool internScope(BytecodeEmitter* bce, ScopeCreator createScope);
    template <typename ScopeCreator>
    MOZ_MUST_USE bool internBodyScope(BytecodeEmitter* bce, ScopeCreator createScope);
    MOZ_MUST_USE bool appendScopeNote(BytecodeEmitter* bce);

    MOZ_MUST_USE bool deadZoneFrameSlotRange(BytecodeEmitter* bce, uint32_t slotStart,
                                             uint32_t slotEnd);

  public:
    explicit EmitterScope(BytecodeEmitter* bce)
      : Nestable<EmitterScope>(&bce->innermostEmitterScope_),
        nameCache_(bce->cx->frontendCollectionPool()),
        hasEnvironment_(false),
        environmentChainLength_(0),
        nextFrameSlot_(0),
        scopeIndex_(ScopeNote::NoScopeIndex),
        noteIndex_(ScopeNote::NoScopeNoteIndex)
    { }

    void dump(BytecodeEmitter* bce);

    MOZ_MUST_USE bool enterLexical(BytecodeEmitter* bce, ScopeKind kind,
                                   Handle<LexicalScope::Data*> bindings);
    MOZ_MUST_USE bool enterNamedLambda(BytecodeEmitter* bce, FunctionBox* funbox);
    MOZ_MUST_USE bool enterFunction(BytecodeEmitter* bce, FunctionBox* funbox);
    MOZ_MUST_USE bool enterFunctionExtraBodyVar(BytecodeEmitter* bce, FunctionBox* funbox);
    MOZ_MUST_USE bool enterParameterExpressionVar(BytecodeEmitter* bce);
    MOZ_MUST_USE bool enterGlobal(BytecodeEmitter* bce, GlobalSharedContext* globalsc);
    MOZ_MUST_USE bool enterEval(BytecodeEmitter* bce, EvalSharedContext* evalsc);
    MOZ_MUST_USE bool enterModule(BytecodeEmitter* module, ModuleSharedContext* modulesc);
    MOZ_MUST_USE bool enterWith(BytecodeEmitter* bce);
    MOZ_MUST_USE bool deadZoneFrameSlots(BytecodeEmitter* bce);

    MOZ_MUST_USE bool leave(BytecodeEmitter* bce, bool nonLocal = false);

    uint32_t index() const {
        MOZ_ASSERT(scopeIndex_ != ScopeNote::NoScopeIndex, "Did you forget to intern a Scope?");
        return scopeIndex_;
    }

    uint32_t noteIndex() const {
        return noteIndex_;
    }

    Scope* scope(const BytecodeEmitter* bce) const {
        return bce->scopeList.vector[index()];
    }

    bool hasEnvironment() const {
        return hasEnvironment_;
    }

    // The first frame slot used.
    uint32_t frameSlotStart() const {
        if (EmitterScope* inFrame = enclosingInFrame())
            return inFrame->nextFrameSlot_;
        return 0;
    }

    // The last frame slot used + 1.
    uint32_t frameSlotEnd() const {
        return nextFrameSlot_;
    }

    uint32_t numFrameSlots() const {
        return frameSlotEnd() - frameSlotStart();
    }

    EmitterScope* enclosingInFrame() const {
        return Nestable<EmitterScope>::enclosing();
    }

    NameLocation lookup(BytecodeEmitter* bce, JSAtom* name) {
        if (Maybe<NameLocation> loc = lookupInCache(bce, name))
            return *loc;
        return searchAndCache(bce, name);
    }

    Maybe<NameLocation> locationBoundInScope(JSAtom* name, EmitterScope* target);
};

void
BytecodeEmitter::EmitterScope::dump(BytecodeEmitter* bce)
{
    fprintf(stdout, "EmitterScope [%s] %p\n", ScopeKindString(scope(bce)->kind()), this);

    for (NameLocationMap::Range r = nameCache_->all(); !r.empty(); r.popFront()) {
        const NameLocation& l = r.front().value();

        JSAutoByteString bytes;
        if (!AtomToPrintableString(bce->cx, r.front().key(), &bytes))
            return;
        if (l.kind() != NameLocation::Kind::Dynamic)
            fprintf(stdout, "  %s %s ", BindingKindString(l.bindingKind()), bytes.ptr());
        else
            fprintf(stdout, "  %s ", bytes.ptr());

        switch (l.kind()) {
          case NameLocation::Kind::Dynamic:
            fprintf(stdout, "dynamic\n");
            break;
          case NameLocation::Kind::Global:
            fprintf(stdout, "global\n");
            break;
          case NameLocation::Kind::Intrinsic:
            fprintf(stdout, "intrinsic\n");
            break;
          case NameLocation::Kind::NamedLambdaCallee:
            fprintf(stdout, "named lambda callee\n");
            break;
          case NameLocation::Kind::Import:
            fprintf(stdout, "import\n");
            break;
          case NameLocation::Kind::ArgumentSlot:
            fprintf(stdout, "arg slot=%u\n", l.argumentSlot());
            break;
          case NameLocation::Kind::FrameSlot:
            fprintf(stdout, "frame slot=%u\n", l.frameSlot());
            break;
          case NameLocation::Kind::EnvironmentCoordinate:
            fprintf(stdout, "environment hops=%u slot=%u\n",
                    l.environmentCoordinate().hops(), l.environmentCoordinate().slot());
            break;
          case NameLocation::Kind::DynamicAnnexBVar:
            fprintf(stdout, "dynamic annex b var\n");
            break;
        }
    }

    fprintf(stdout, "\n");
}

template <typename ScopeCreator>
bool
BytecodeEmitter::EmitterScope::internScope(BytecodeEmitter* bce, ScopeCreator createScope)
{
    RootedScope enclosing(bce->cx, enclosingScope(bce));
    Scope* scope = createScope(bce->cx, enclosing);
    if (!scope)
        return false;
    hasEnvironment_ = scope->hasEnvironment();
    scopeIndex_ = bce->scopeList.length();
    return bce->scopeList.append(scope);
}

template <typename ScopeCreator>
bool
BytecodeEmitter::EmitterScope::internBodyScope(BytecodeEmitter* bce, ScopeCreator createScope)
{
    MOZ_ASSERT(bce->bodyScopeIndex == UINT32_MAX, "There can be only one body scope");
    bce->bodyScopeIndex = bce->scopeList.length();
    return internScope(bce, createScope);
}

bool
BytecodeEmitter::EmitterScope::appendScopeNote(BytecodeEmitter* bce)
{
    MOZ_ASSERT(ScopeKindIsInBody(scope(bce)->kind()) && enclosingInFrame(),
               "Scope notes are not needed for body-level scopes.");
    noteIndex_ = bce->scopeNoteList.length();
    return bce->scopeNoteList.append(index(), bce->offset(), bce->inPrologue(),
                                     enclosingInFrame() ? enclosingInFrame()->noteIndex()
                                                        : ScopeNote::NoScopeNoteIndex);
}

#ifdef DEBUG
static bool
NameIsOnEnvironment(Scope* scope, JSAtom* name)
{
    for (BindingIter bi(scope); bi; bi++) {
        // If found, the name must already be on the environment or an import,
        // or else there is a bug in the closed-over name analysis in the
        // Parser.
        if (bi.name() == name) {
            BindingLocation::Kind kind = bi.location().kind();

            if (bi.hasArgumentSlot()) {
                JSScript* script = scope->as<FunctionScope>().script();
                if (!script->strict() && !script->functionHasParameterExprs()) {
                    // Check for duplicate positional formal parameters.
                    for (BindingIter bi2(bi); bi2 && bi2.hasArgumentSlot(); bi2++) {
                        if (bi2.name() == name)
                            kind = bi2.location().kind();
                    }
                }
            }

            return kind == BindingLocation::Kind::Global ||
                   kind == BindingLocation::Kind::Environment ||
                   kind == BindingLocation::Kind::Import;
        }
    }

    // If not found, assume it's on the global or dynamically accessed.
    return true;
}
#endif

/* static */ NameLocation
BytecodeEmitter::EmitterScope::searchInEnclosingScope(JSAtom* name, Scope* scope, uint8_t hops)
{
    for (ScopeIter si(scope); si; si++) {
        MOZ_ASSERT(NameIsOnEnvironment(si.scope(), name));

        bool hasEnv = si.hasSyntacticEnvironment();

        switch (si.kind()) {
          case ScopeKind::Function:
            if (hasEnv) {
                JSScript* script = si.scope()->as<FunctionScope>().script();
                if (script->funHasExtensibleScope())
                    return NameLocation::Dynamic();

                for (BindingIter bi(si.scope()); bi; bi++) {
                    if (bi.name() != name)
                        continue;

                    BindingLocation bindLoc = bi.location();
                    if (bi.hasArgumentSlot() &&
                        !script->strict() &&
                        !script->functionHasParameterExprs())
                    {
                        // Check for duplicate positional formal parameters.
                        for (BindingIter bi2(bi); bi2 && bi2.hasArgumentSlot(); bi2++) {
                            if (bi2.name() == name)
                                bindLoc = bi2.location();
                        }
                    }

                    MOZ_ASSERT(bindLoc.kind() == BindingLocation::Kind::Environment);
                    return NameLocation::EnvironmentCoordinate(bi.kind(), hops, bindLoc.slot());
                }
            }
            break;

          case ScopeKind::FunctionBodyVar:
          case ScopeKind::ParameterExpressionVar:
          case ScopeKind::Lexical:
          case ScopeKind::NamedLambda:
          case ScopeKind::StrictNamedLambda:
          case ScopeKind::SimpleCatch:
          case ScopeKind::Catch:
            if (hasEnv) {
                for (BindingIter bi(si.scope()); bi; bi++) {
                    if (bi.name() != name)
                        continue;

                    // The name must already have been marked as closed
                    // over. If this assertion is hit, there is a bug in the
                    // name analysis.
                    BindingLocation bindLoc = bi.location();
                    MOZ_ASSERT(bindLoc.kind() == BindingLocation::Kind::Environment);
                    return NameLocation::EnvironmentCoordinate(bi.kind(), hops, bindLoc.slot());
                }
            }
            break;

          case ScopeKind::Module:
            if (hasEnv) {
                for (BindingIter bi(si.scope()); bi; bi++) {
                    if (bi.name() != name)
                        continue;

                    BindingLocation bindLoc = bi.location();

                    // Imports are on the environment but are indirect
                    // bindings and must be accessed dynamically instead of
                    // using an EnvironmentCoordinate.
                    if (bindLoc.kind() == BindingLocation::Kind::Import) {
                        MOZ_ASSERT(si.kind() == ScopeKind::Module);
                        return NameLocation::Import();
                    }

                    MOZ_ASSERT(bindLoc.kind() == BindingLocation::Kind::Environment);
                    return NameLocation::EnvironmentCoordinate(bi.kind(), hops, bindLoc.slot());
                }
            }
            break;

          case ScopeKind::Eval:
          case ScopeKind::StrictEval:
            // As an optimization, if the eval doesn't have its own var
            // environment and its immediate enclosing scope is a global
            // scope, all accesses are global.
            if (!hasEnv && si.scope()->enclosing()->is<GlobalScope>())
                return NameLocation::Global(BindingKind::Var);
            return NameLocation::Dynamic();

          case ScopeKind::Global:
            return NameLocation::Global(BindingKind::Var);

          case ScopeKind::With:
          case ScopeKind::NonSyntactic:
            return NameLocation::Dynamic();

          case ScopeKind::WasmInstance:
          case ScopeKind::WasmFunction:
            MOZ_CRASH("No direct eval inside wasm functions");
        }

        if (hasEnv) {
            MOZ_ASSERT(hops < ENVCOORD_HOPS_LIMIT - 1);
            hops++;
        }
    }

    MOZ_CRASH("Malformed scope chain");
}

NameLocation
BytecodeEmitter::EmitterScope::searchAndCache(BytecodeEmitter* bce, JSAtom* name)
{
    Maybe<NameLocation> loc;
    uint8_t hops = hasEnvironment() ? 1 : 0;
    DebugOnly<bool> inCurrentScript = enclosingInFrame();

    // Start searching in the current compilation.
    for (EmitterScope* es = enclosing(&bce); es; es = es->enclosing(&bce)) {
        loc = es->lookupInCache(bce, name);
        if (loc) {
            if (loc->kind() == NameLocation::Kind::EnvironmentCoordinate)
                *loc = loc->addHops(hops);
            break;
        }

        if (es->hasEnvironment())
            hops++;

#ifdef DEBUG
        if (!es->enclosingInFrame())
            inCurrentScript = false;
#endif
    }

    // If the name is not found in the current compilation, walk the Scope
    // chain encompassing the compilation.
    if (!loc) {
        inCurrentScript = false;
        loc = Some(searchInEnclosingScope(name, bce->sc->compilationEnclosingScope(), hops));
    }

    // Each script has its own frame. A free name that is accessed
    // from an inner script must not be a frame slot access. If this
    // assertion is hit, it is a bug in the free name analysis in the
    // parser.
    MOZ_ASSERT_IF(!inCurrentScript, loc->kind() != NameLocation::Kind::FrameSlot);

    // It is always correct to not cache the location. Ignore OOMs to make
    // lookups infallible.
    if (!putNameInCache(bce, name, *loc))
        bce->cx->recoverFromOutOfMemory();

    return *loc;
}

Maybe<NameLocation>
BytecodeEmitter::EmitterScope::locationBoundInScope(JSAtom* name, EmitterScope* target)
{
    // The target scope must be an intra-frame enclosing scope of this
    // one. Count the number of extra hops to reach it.
    uint8_t extraHops = 0;
    for (EmitterScope* es = this; es != target; es = es->enclosingInFrame()) {
        if (es->hasEnvironment())
            extraHops++;
    }

    // Caches are prepopulated with bound names. So if the name is bound in a
    // particular scope, it must already be in the cache. Furthermore, don't
    // consult the fallback location as we only care about binding names.
    Maybe<NameLocation> loc;
    if (NameLocationMap::Ptr p = target->nameCache_->lookup(name)) {
        NameLocation l = p->value().wrapped;
        if (l.kind() == NameLocation::Kind::EnvironmentCoordinate)
            loc = Some(l.addHops(extraHops));
        else
            loc = Some(l);
    }
    return loc;
}

bool
BytecodeEmitter::EmitterScope::deadZoneFrameSlotRange(BytecodeEmitter* bce, uint32_t slotStart,
                                                      uint32_t slotEnd)
{
    // Lexical bindings throw ReferenceErrors if they are used before
    // initialization. See ES6 8.1.1.1.6.
    //
    // For completeness, lexical bindings are initialized in ES6 by calling
    // InitializeBinding, after which touching the binding will no longer
    // throw reference errors. See 13.1.11, 9.2.13, 13.6.3.4, 13.6.4.6,
    // 13.6.4.8, 13.14.5, 15.1.8, and 15.2.0.15.
    if (slotStart != slotEnd) {
        if (!bce->emit1(JSOP_UNINITIALIZED))
            return false;
        for (uint32_t slot = slotStart; slot < slotEnd; slot++) {
            if (!bce->emitLocalOp(JSOP_INITLEXICAL, slot))
                return false;
        }
        if (!bce->emit1(JSOP_POP))
            return false;
    }

    return true;
}

bool
BytecodeEmitter::EmitterScope::deadZoneFrameSlots(BytecodeEmitter* bce)
{
    return deadZoneFrameSlotRange(bce, frameSlotStart(), frameSlotEnd());
}

bool
BytecodeEmitter::EmitterScope::enterLexical(BytecodeEmitter* bce, ScopeKind kind,
                                            Handle<LexicalScope::Data*> bindings)
{
    MOZ_ASSERT(kind != ScopeKind::NamedLambda && kind != ScopeKind::StrictNamedLambda);
    MOZ_ASSERT(this == bce->innermostEmitterScopeNoCheck());

    if (!ensureCache(bce))
        return false;

    // Marks all names as closed over if the context requires it. This
    // cannot be done in the Parser as we may not know if the context requires
    // all bindings to be closed over until after parsing is finished. For
    // example, legacy generators require all bindings to be closed over but
    // it is unknown if a function is a legacy generator until the first
    // 'yield' expression is parsed.
    //
    // This is not a problem with other scopes, as all other scopes with
    // bindings are body-level. At the time of their creation, whether or not
    // the context requires all bindings to be closed over is already known.
    if (bce->sc->allBindingsClosedOver())
        MarkAllBindingsClosedOver(*bindings);

    // Resolve bindings.
    TDZCheckCache* tdzCache = bce->innermostTDZCheckCache;
    uint32_t firstFrameSlot = frameSlotStart();
    BindingIter bi(*bindings, firstFrameSlot, /* isNamedLambda = */ false);
    for (; bi; bi++) {
        if (!checkSlotLimits(bce, bi))
            return false;

        NameLocation loc = NameLocation::fromBinding(bi.kind(), bi.location());
        if (!putNameInCache(bce, bi.name(), loc))
            return false;

        if (!tdzCache->noteTDZCheck(bce, bi.name(), CheckTDZ))
            return false;
    }

    updateFrameFixedSlots(bce, bi);

    // Create and intern the VM scope.
    auto createScope = [kind, bindings, firstFrameSlot](JSContext* cx,
                                                        HandleScope enclosing)
    {
        return LexicalScope::create(cx, kind, bindings, firstFrameSlot, enclosing);
    };
    if (!internScope(bce, createScope))
        return false;

    if (ScopeKindIsInBody(kind) && hasEnvironment()) {
        // After interning the VM scope we can get the scope index.
        if (!bce->emitInternedScopeOp(index(), JSOP_PUSHLEXICALENV))
            return false;
    }

    // Lexical scopes need notes to be mapped from a pc.
    if (!appendScopeNote(bce))
        return false;

    // Put frame slots in TDZ. Environment slots are poisoned during
    // environment creation.
    //
    // This must be done after appendScopeNote to be considered in the extent
    // of the scope.
    if (!deadZoneFrameSlotRange(bce, firstFrameSlot, frameSlotEnd()))
        return false;

    return checkEnvironmentChainLength(bce);
}

bool
BytecodeEmitter::EmitterScope::enterNamedLambda(BytecodeEmitter* bce, FunctionBox* funbox)
{
    MOZ_ASSERT(this == bce->innermostEmitterScopeNoCheck());
    MOZ_ASSERT(funbox->namedLambdaBindings());

    if (!ensureCache(bce))
        return false;

    // See comment in enterLexical about allBindingsClosedOver.
    if (funbox->allBindingsClosedOver())
        MarkAllBindingsClosedOver(*funbox->namedLambdaBindings());

    BindingIter bi(*funbox->namedLambdaBindings(), LOCALNO_LIMIT, /* isNamedLambda = */ true);
    MOZ_ASSERT(bi.kind() == BindingKind::NamedLambdaCallee);

    // The lambda name, if not closed over, is accessed via JSOP_CALLEE and
    // not a frame slot. Do not update frame slot information.
    NameLocation loc = NameLocation::fromBinding(bi.kind(), bi.location());
    if (!putNameInCache(bce, bi.name(), loc))
        return false;

    bi++;
    MOZ_ASSERT(!bi, "There should be exactly one binding in a NamedLambda scope");

    auto createScope = [funbox](JSContext* cx, HandleScope enclosing) {
        ScopeKind scopeKind =
            funbox->strict() ? ScopeKind::StrictNamedLambda : ScopeKind::NamedLambda;
        return LexicalScope::create(cx, scopeKind, funbox->namedLambdaBindings(),
                                    LOCALNO_LIMIT, enclosing);
    };
    if (!internScope(bce, createScope))
        return false;

    return checkEnvironmentChainLength(bce);
}

bool
BytecodeEmitter::EmitterScope::enterParameterExpressionVar(BytecodeEmitter* bce)
{
    MOZ_ASSERT(this == bce->innermostEmitterScopeNoCheck());

    if (!ensureCache(bce))
        return false;

    // Parameter expressions var scopes have no pre-set bindings and are
    // always extensible, as they are needed for eval.
    fallbackFreeNameLocation_ = Some(NameLocation::Dynamic());

    // Create and intern the VM scope.
    uint32_t firstFrameSlot = frameSlotStart();
    auto createScope = [firstFrameSlot](JSContext* cx, HandleScope enclosing) {
        return VarScope::create(cx, ScopeKind::ParameterExpressionVar,
                                /* data = */ nullptr, firstFrameSlot,
                                /* needsEnvironment = */ true, enclosing);
    };
    if (!internScope(bce, createScope))
        return false;

    MOZ_ASSERT(hasEnvironment());
    if (!bce->emitInternedScopeOp(index(), JSOP_PUSHVARENV))
        return false;

    // The extra var scope needs a note to be mapped from a pc.
    if (!appendScopeNote(bce))
        return false;

    return checkEnvironmentChainLength(bce);
}

bool
BytecodeEmitter::EmitterScope::enterFunction(BytecodeEmitter* bce, FunctionBox* funbox)
{
    MOZ_ASSERT(this == bce->innermostEmitterScopeNoCheck());

    // If there are parameter expressions, there is an extra var scope.
    if (!funbox->hasExtraBodyVarScope())
        bce->setVarEmitterScope(this);

    if (!ensureCache(bce))
        return false;

    // Resolve body-level bindings, if there are any.
    auto bindings = funbox->functionScopeBindings();
    Maybe<uint32_t> lastLexicalSlot;
    if (bindings) {
        NameLocationMap& cache = *nameCache_;

        BindingIter bi(*bindings, funbox->hasParameterExprs);
        for (; bi; bi++) {
            if (!checkSlotLimits(bce, bi))
                return false;

            NameLocation loc = NameLocation::fromBinding(bi.kind(), bi.location());
            NameLocationMap::AddPtr p = cache.lookupForAdd(bi.name());

            // The only duplicate bindings that occur are simple formal
            // parameters, in which case the last position counts, so update the
            // location.
            if (p) {
                MOZ_ASSERT(bi.kind() == BindingKind::FormalParameter);
                MOZ_ASSERT(!funbox->hasDestructuringArgs);
                MOZ_ASSERT(!funbox->hasRest());
                p->value() = loc;
                continue;
            }

            if (!cache.add(p, bi.name(), loc)) {
                ReportOutOfMemory(bce->cx);
                return false;
            }
        }

        updateFrameFixedSlots(bce, bi);
    } else {
        nextFrameSlot_ = 0;
    }

    // If the function's scope may be extended at runtime due to sloppy direct
    // eval and there is no extra var scope, any names beyond the function
    // scope must be accessed dynamically as we don't know if the name will
    // become a 'var' binding due to direct eval.
    if (!funbox->hasParameterExprs && funbox->hasExtensibleScope())
        fallbackFreeNameLocation_ = Some(NameLocation::Dynamic());

    // In case of parameter expressions, the parameters are lexical
    // bindings and have TDZ.
    if (funbox->hasParameterExprs && nextFrameSlot_) {
        uint32_t paramFrameSlotEnd = 0;
        for (BindingIter bi(*bindings, true); bi; bi++) {
            if (!BindingKindIsLexical(bi.kind()))
                break;

            NameLocation loc = NameLocation::fromBinding(bi.kind(), bi.location());
            if (loc.kind() == NameLocation::Kind::FrameSlot) {
                MOZ_ASSERT(paramFrameSlotEnd <= loc.frameSlot());
                paramFrameSlotEnd = loc.frameSlot() + 1;
            }
        }

        if (!deadZoneFrameSlotRange(bce, 0, paramFrameSlotEnd))
            return false;
    }

    // Create and intern the VM scope.
    auto createScope = [funbox](JSContext* cx, HandleScope enclosing) {
        RootedFunction fun(cx, funbox->function());
        return FunctionScope::create(cx, funbox->functionScopeBindings(),
                                     funbox->hasParameterExprs,
                                     funbox->needsCallObjectRegardlessOfBindings(),
                                     fun, enclosing);
    };
    if (!internBodyScope(bce, createScope))
        return false;

    return checkEnvironmentChainLength(bce);
}

bool
BytecodeEmitter::EmitterScope::enterFunctionExtraBodyVar(BytecodeEmitter* bce, FunctionBox* funbox)
{
    MOZ_ASSERT(funbox->hasParameterExprs);
    MOZ_ASSERT(funbox->extraVarScopeBindings() ||
               funbox->needsExtraBodyVarEnvironmentRegardlessOfBindings());
    MOZ_ASSERT(this == bce->innermostEmitterScopeNoCheck());

    // The extra var scope is never popped once it's entered. It replaces the
    // function scope as the var emitter scope.
    bce->setVarEmitterScope(this);

    if (!ensureCache(bce))
        return false;

    // Resolve body-level bindings, if there are any.
    uint32_t firstFrameSlot = frameSlotStart();
    if (auto bindings = funbox->extraVarScopeBindings()) {
        BindingIter bi(*bindings, firstFrameSlot);
        for (; bi; bi++) {
            if (!checkSlotLimits(bce, bi))
                return false;

            NameLocation loc = NameLocation::fromBinding(bi.kind(), bi.location());
            if (!putNameInCache(bce, bi.name(), loc))
                return false;
        }

        updateFrameFixedSlots(bce, bi);
    } else {
        nextFrameSlot_ = firstFrameSlot;
    }

    // If the extra var scope may be extended at runtime due to sloppy
    // direct eval, any names beyond the var scope must be accessed
    // dynamically as we don't know if the name will become a 'var' binding
    // due to direct eval.
    if (funbox->hasExtensibleScope())
        fallbackFreeNameLocation_ = Some(NameLocation::Dynamic());

    // Create and intern the VM scope.
    auto createScope = [funbox, firstFrameSlot](JSContext* cx, HandleScope enclosing) {
        return VarScope::create(cx, ScopeKind::FunctionBodyVar,
                                funbox->extraVarScopeBindings(), firstFrameSlot,
                                funbox->needsExtraBodyVarEnvironmentRegardlessOfBindings(),
                                enclosing);
    };
    if (!internScope(bce, createScope))
        return false;

    if (hasEnvironment()) {
        if (!bce->emitInternedScopeOp(index(), JSOP_PUSHVARENV))
            return false;
    }

    // The extra var scope needs a note to be mapped from a pc.
    if (!appendScopeNote(bce))
        return false;

    return checkEnvironmentChainLength(bce);
}

class DynamicBindingIter : public BindingIter
{
  public:
    explicit DynamicBindingIter(GlobalSharedContext* sc)
      : BindingIter(*sc->bindings)
    { }

    explicit DynamicBindingIter(EvalSharedContext* sc)
      : BindingIter(*sc->bindings, /* strict = */ false)
    {
        MOZ_ASSERT(!sc->strict());
    }

    JSOp bindingOp() const {
        switch (kind()) {
          case BindingKind::Var:
            return JSOP_DEFVAR;
          case BindingKind::Let:
            return JSOP_DEFLET;
          case BindingKind::Const:
            return JSOP_DEFCONST;
          default:
            MOZ_CRASH("Bad BindingKind");
        }
    }
};

bool
BytecodeEmitter::EmitterScope::enterGlobal(BytecodeEmitter* bce, GlobalSharedContext* globalsc)
{
    MOZ_ASSERT(this == bce->innermostEmitterScopeNoCheck());

    bce->setVarEmitterScope(this);

    if (!ensureCache(bce))
        return false;

    if (bce->emitterMode == BytecodeEmitter::SelfHosting) {
        // In self-hosting, it is incorrect to consult the global scope because
        // self-hosted scripts are cloned into their target compartments before
        // they are run. Instead of Global, Intrinsic is used for all names.
        //
        // Intrinsic lookups are redirected to the special intrinsics holder
        // in the global object, into which any missing values are cloned
        // lazily upon first access.
        fallbackFreeNameLocation_ = Some(NameLocation::Intrinsic());

        auto createScope = [](JSContext* cx, HandleScope enclosing) {
            MOZ_ASSERT(!enclosing);
            return &cx->global()->emptyGlobalScope();
        };
        return internBodyScope(bce, createScope);
    }

    // Resolve binding names and emit DEF{VAR,LET,CONST} prologue ops.
    if (globalsc->bindings) {
        for (DynamicBindingIter bi(globalsc); bi; bi++) {
            NameLocation loc = NameLocation::fromBinding(bi.kind(), bi.location());
            JSAtom* name = bi.name();
            if (!putNameInCache(bce, name, loc))
                return false;

            // Define the name in the prologue. Do not emit DEFVAR for
            // functions that we'll emit DEFFUN for.
            if (bi.isTopLevelFunction())
                continue;

            if (!bce->emitAtomOp(name, bi.bindingOp()))
                return false;
        }
    }

    // Note that to save space, we don't add free names to the cache for
    // global scopes. They are assumed to be global vars in the syntactic
    // global scope, dynamic accesses under non-syntactic global scope.
    if (globalsc->scopeKind() == ScopeKind::Global)
        fallbackFreeNameLocation_ = Some(NameLocation::Global(BindingKind::Var));
    else
        fallbackFreeNameLocation_ = Some(NameLocation::Dynamic());

    auto createScope = [globalsc](JSContext* cx, HandleScope enclosing) {
        MOZ_ASSERT(!enclosing);
        return GlobalScope::create(cx, globalsc->scopeKind(), globalsc->bindings);
    };
    return internBodyScope(bce, createScope);
}

bool
BytecodeEmitter::EmitterScope::enterEval(BytecodeEmitter* bce, EvalSharedContext* evalsc)
{
    MOZ_ASSERT(this == bce->innermostEmitterScopeNoCheck());

    bce->setVarEmitterScope(this);

    if (!ensureCache(bce))
        return false;

    // For simplicity, treat all free name lookups in eval scripts as dynamic.
    fallbackFreeNameLocation_ = Some(NameLocation::Dynamic());

    // Create the `var` scope. Note that there is also a lexical scope, created
    // separately in emitScript().
    auto createScope = [evalsc](JSContext* cx, HandleScope enclosing) {
        ScopeKind scopeKind = evalsc->strict() ? ScopeKind::StrictEval : ScopeKind::Eval;
        return EvalScope::create(cx, scopeKind, evalsc->bindings, enclosing);
    };
    if (!internBodyScope(bce, createScope))
        return false;

    if (hasEnvironment()) {
        if (!bce->emitInternedScopeOp(index(), JSOP_PUSHVARENV))
            return false;
    } else {
        // Resolve binding names and emit DEFVAR prologue ops if we don't have
        // an environment (i.e., a sloppy eval not in a parameter expression).
        // Eval scripts always have their own lexical scope, but non-strict
        // scopes may introduce 'var' bindings to the nearest var scope.
        //
        // TODO: We may optimize strict eval bindings in the future to be on
        // the frame. For now, handle everything dynamically.
        if (!hasEnvironment() && evalsc->bindings) {
            for (DynamicBindingIter bi(evalsc); bi; bi++) {
                MOZ_ASSERT(bi.bindingOp() == JSOP_DEFVAR);

                if (bi.isTopLevelFunction())
                    continue;

                if (!bce->emitAtomOp(bi.name(), JSOP_DEFVAR))
                    return false;
            }
        }

        // As an optimization, if the eval does not have its own var
        // environment and is directly enclosed in a global scope, then all
        // free name lookups are global.
        if (scope(bce)->enclosing()->is<GlobalScope>())
            fallbackFreeNameLocation_ = Some(NameLocation::Global(BindingKind::Var));
    }

    return true;
}

bool
BytecodeEmitter::EmitterScope::enterModule(BytecodeEmitter* bce, ModuleSharedContext* modulesc)
{
    MOZ_ASSERT(this == bce->innermostEmitterScopeNoCheck());

    bce->setVarEmitterScope(this);

    if (!ensureCache(bce))
        return false;

    // Resolve body-level bindings, if there are any.
    TDZCheckCache* tdzCache = bce->innermostTDZCheckCache;
    Maybe<uint32_t> firstLexicalFrameSlot;
    if (ModuleScope::Data* bindings = modulesc->bindings) {
        BindingIter bi(*bindings);
        for (; bi; bi++) {
            if (!checkSlotLimits(bce, bi))
                return false;

            NameLocation loc = NameLocation::fromBinding(bi.kind(), bi.location());
            if (!putNameInCache(bce, bi.name(), loc))
                return false;

            if (BindingKindIsLexical(bi.kind())) {
                if (loc.kind() == NameLocation::Kind::FrameSlot && !firstLexicalFrameSlot)
                    firstLexicalFrameSlot = Some(loc.frameSlot());

                if (!tdzCache->noteTDZCheck(bce, bi.name(), CheckTDZ))
                    return false;
            }
        }

        updateFrameFixedSlots(bce, bi);
    } else {
        nextFrameSlot_ = 0;
    }

    // Modules are toplevel, so any free names are global.
    fallbackFreeNameLocation_ = Some(NameLocation::Global(BindingKind::Var));

    // Put lexical frame slots in TDZ. Environment slots are poisoned during
    // environment creation.
    if (firstLexicalFrameSlot) {
        if (!deadZoneFrameSlotRange(bce, *firstLexicalFrameSlot, frameSlotEnd()))
            return false;
    }

    // Create and intern the VM scope.
    auto createScope = [modulesc](JSContext* cx, HandleScope enclosing) {
        return ModuleScope::create(cx, modulesc->bindings, modulesc->module(), enclosing);
    };
    if (!internBodyScope(bce, createScope))
        return false;

    return checkEnvironmentChainLength(bce);
}

bool
BytecodeEmitter::EmitterScope::enterWith(BytecodeEmitter* bce)
{
    MOZ_ASSERT(this == bce->innermostEmitterScopeNoCheck());

    if (!ensureCache(bce))
        return false;

    // 'with' make all accesses dynamic and unanalyzable.
    fallbackFreeNameLocation_ = Some(NameLocation::Dynamic());

    auto createScope = [](JSContext* cx, HandleScope enclosing) {
        return WithScope::create(cx, enclosing);
    };
    if (!internScope(bce, createScope))
        return false;

    if (!bce->emitInternedScopeOp(index(), JSOP_ENTERWITH))
        return false;

    if (!appendScopeNote(bce))
        return false;

    return checkEnvironmentChainLength(bce);
}

bool
BytecodeEmitter::EmitterScope::leave(BytecodeEmitter* bce, bool nonLocal)
{
    // If we aren't leaving the scope due to a non-local jump (e.g., break),
    // we must be the innermost scope.
    MOZ_ASSERT_IF(!nonLocal, this == bce->innermostEmitterScopeNoCheck());

    ScopeKind kind = scope(bce)->kind();
    switch (kind) {
      case ScopeKind::Lexical:
      case ScopeKind::SimpleCatch:
      case ScopeKind::Catch:
        if (!bce->emit1(hasEnvironment() ? JSOP_POPLEXICALENV : JSOP_DEBUGLEAVELEXICALENV))
            return false;
        break;

      case ScopeKind::With:
        if (!bce->emit1(JSOP_LEAVEWITH))
            return false;
        break;

      case ScopeKind::ParameterExpressionVar:
        MOZ_ASSERT(hasEnvironment());
        if (!bce->emit1(JSOP_POPVARENV))
            return false;
        break;

      case ScopeKind::Function:
      case ScopeKind::FunctionBodyVar:
      case ScopeKind::NamedLambda:
      case ScopeKind::StrictNamedLambda:
      case ScopeKind::Eval:
      case ScopeKind::StrictEval:
      case ScopeKind::Global:
      case ScopeKind::NonSyntactic:
      case ScopeKind::Module:
        break;

      case ScopeKind::WasmInstance:
      case ScopeKind::WasmFunction:
        MOZ_CRASH("No wasm function scopes in JS");
    }

    // Finish up the scope if we are leaving it in LIFO fashion.
    if (!nonLocal) {
        // Popping scopes due to non-local jumps generate additional scope
        // notes. See NonLocalExitControl::prepareForNonLocalJump.
        if (ScopeKindIsInBody(kind)) {
            // The extra function var scope is never popped once it's pushed,
            // so its scope note extends until the end of any possible code.
            uint32_t offset = kind == ScopeKind::FunctionBodyVar ? UINT32_MAX : bce->offset();
            bce->scopeNoteList.recordEnd(noteIndex_, offset, bce->inPrologue());
        }
    }

    return true;
}

Maybe<MaybeCheckTDZ>
BytecodeEmitter::TDZCheckCache::needsTDZCheck(BytecodeEmitter* bce, JSAtom* name)
{
    if (!ensureCache(bce))
        return Nothing();

    CheckTDZMap::AddPtr p = cache_->lookupForAdd(name);
    if (p)
        return Some(p->value().wrapped);

    MaybeCheckTDZ rv = CheckTDZ;
    for (TDZCheckCache* it = enclosing(); it; it = it->enclosing()) {
        if (it->cache_) {
            if (CheckTDZMap::Ptr p2 = it->cache_->lookup(name)) {
                rv = p2->value();
                break;
            }
        }
    }

    if (!cache_->add(p, name, rv)) {
        ReportOutOfMemory(bce->cx);
        return Nothing();
    }

    return Some(rv);
}

bool
BytecodeEmitter::TDZCheckCache::noteTDZCheck(BytecodeEmitter* bce, JSAtom* name,
                                             MaybeCheckTDZ check)
{
    if (!ensureCache(bce))
        return false;

    CheckTDZMap::AddPtr p = cache_->lookupForAdd(name);
    if (p) {
        MOZ_ASSERT(!check, "TDZ only needs to be checked once per binding per basic block.");
        p->value() = check;
    } else {
        if (!cache_->add(p, name, check))
            return false;
    }

    return true;
}

class MOZ_STACK_CLASS TryEmitter
{
  public:
    enum Kind {
        TryCatch,
        TryCatchFinally,
        TryFinally
    };
    enum ShouldUseRetVal {
        UseRetVal,
        DontUseRetVal
    };
    enum ShouldUseControl {
        UseControl,
        DontUseControl,
    };

  private:
    BytecodeEmitter* bce_;
    Kind kind_;
    ShouldUseRetVal retValKind_;

    // Track jumps-over-catches and gosubs-to-finally for later fixup.
    //
    // When a finally block is active, non-local jumps (including
    // jumps-over-catches) result in a GOSUB being written into the bytecode
    // stream and fixed-up later.
    //
    // If ShouldUseControl is DontUseControl, all that handling is skipped.
    // DontUseControl is used by yield* and the internal try-catch around
    // IteratorClose. These internal uses must:
    //   * have only one catch block
    //   * have JSOP_GOTO at the end of catch-block
    //   * have no non-local-jump
    //   * don't use finally block for normal completion of try-block and
    //     catch-block
    //
    // Additionally, a finally block may be emitted when ShouldUseControl is
    // DontUseControl, even if the kind is not TryCatchFinally or TryFinally,
    // because GOSUBs are not emitted. This internal use shares the
    // requirements as above.
    Maybe<TryFinallyControl> controlInfo_;

    int depth_;
    unsigned noteIndex_;
    ptrdiff_t tryStart_;
    JumpList catchAndFinallyJump_;
    JumpTarget tryEnd_;
    JumpTarget finallyStart_;

    enum State {
        Start,
        Try,
        TryEnd,
        Catch,
        CatchEnd,
        Finally,
        FinallyEnd,
        End
    };
    State state_;

    bool hasCatch() const {
        return kind_ == TryCatch || kind_ == TryCatchFinally;
    }
    bool hasFinally() const {
        return kind_ == TryCatchFinally || kind_ == TryFinally;
    }

  public:
    TryEmitter(BytecodeEmitter* bce, Kind kind, ShouldUseRetVal retValKind = UseRetVal,
               ShouldUseControl controlKind = UseControl)
      : bce_(bce),
        kind_(kind),
        retValKind_(retValKind),
        depth_(0),
        noteIndex_(0),
        tryStart_(0),
        state_(Start)
    {
        if (controlKind == UseControl)
            controlInfo_.emplace(bce_, hasFinally() ? StatementKind::Finally : StatementKind::Try);
        finallyStart_.offset = 0;
    }

    bool emitJumpOverCatchAndFinally() {
        if (!bce_->emitJump(JSOP_GOTO, &catchAndFinallyJump_))
            return false;
        return true;
    }

    bool emitTry() {
        MOZ_ASSERT(state_ == Start);

        // Since an exception can be thrown at any place inside the try block,
        // we need to restore the stack and the scope chain before we transfer
        // the control to the exception handler.
        //
        // For that we store in a try note associated with the catch or
        // finally block the stack depth upon the try entry. The interpreter
        // uses this depth to properly unwind the stack and the scope chain.
        depth_ = bce_->stackDepth;

        // Record the try location, then emit the try block.
        if (!bce_->newSrcNote(SRC_TRY, &noteIndex_))
            return false;
        if (!bce_->emit1(JSOP_TRY))
            return false;
        tryStart_ = bce_->offset();

        state_ = Try;
        return true;
    }

  private:
    bool emitTryEnd() {
        MOZ_ASSERT(state_ == Try);
        MOZ_ASSERT(depth_ == bce_->stackDepth);

        // GOSUB to finally, if present.
        if (hasFinally() && controlInfo_) {
            if (!bce_->emitJump(JSOP_GOSUB, &controlInfo_->gosubs))
                return false;
        }

        // Source note points to the jump at the end of the try block.
        if (!bce_->setSrcNoteOffset(noteIndex_, 0, bce_->offset() - tryStart_ + JSOP_TRY_LENGTH))
            return false;

        // Emit jump over catch and/or finally.
        if (!bce_->emitJump(JSOP_GOTO, &catchAndFinallyJump_))
            return false;

        if (!bce_->emitJumpTarget(&tryEnd_))
            return false;

        return true;
    }

  public:
    bool emitCatch() {
        MOZ_ASSERT(state_ == Try);
        if (!emitTryEnd())
            return false;

        MOZ_ASSERT(bce_->stackDepth == depth_);

        if (retValKind_ == UseRetVal) {
            // Clear the frame's return value that might have been set by the
            // try block:
            //
            //   eval("try { 1; throw 2 } catch(e) {}"); // undefined, not 1
            if (!bce_->emit1(JSOP_UNDEFINED))
                return false;
            if (!bce_->emit1(JSOP_SETRVAL))
                return false;
        }

        state_ = Catch;
        return true;
    }

  private:
    bool emitCatchEnd() {
        MOZ_ASSERT(state_ == Catch);

        if (!controlInfo_)
            return true;

        // gosub <finally>, if required.
        if (hasFinally()) {
            if (!bce_->emitJump(JSOP_GOSUB, &controlInfo_->gosubs))
                return false;
            MOZ_ASSERT(bce_->stackDepth == depth_);

            // Jump over the finally block.
            if (!bce_->emitJump(JSOP_GOTO, &catchAndFinallyJump_))
                return false;
        }

        return true;
    }

  public:
    bool emitFinally(const Maybe<uint32_t>& finallyPos = Nothing()) {
        // If we are using controlInfo_ (i.e., emitting a syntactic try
        // blocks), we must have specified up front if there will be a finally
        // close. For internal try blocks, like those emitted for yield* and
        // IteratorClose inside for-of loops, we can emitFinally even without
        // specifying up front, since the internal try blocks emit no GOSUBs.
        if (!controlInfo_) {
            if (kind_ == TryCatch)
                kind_ = TryCatchFinally;
        } else {
            MOZ_ASSERT(hasFinally());
        }

        if (state_ == Try) {
            if (!emitTryEnd())
                return false;
        } else {
            MOZ_ASSERT(state_ == Catch);
            if (!emitCatchEnd())
                return false;
        }

        MOZ_ASSERT(bce_->stackDepth == depth_);

        if (!bce_->emitJumpTarget(&finallyStart_))
            return false;

        if (controlInfo_) {
            // Fix up the gosubs that might have been emitted before non-local
            // jumps to the finally code.
            bce_->patchJumpsToTarget(controlInfo_->gosubs, finallyStart_);

            // Indicate that we're emitting a subroutine body.
            controlInfo_->setEmittingSubroutine();
        }
        if (finallyPos) {
            if (!bce_->updateSourceCoordNotes(finallyPos.value()))
                return false;
        }
        if (!bce_->emit1(JSOP_FINALLY))
            return false;

        if (retValKind_ == UseRetVal) {
            if (!bce_->emit1(JSOP_GETRVAL))
                return false;

            // Clear the frame's return value to make break/continue return
            // correct value even if there's no other statement before them:
            //
            //   eval("x: try { 1 } finally { break x; }"); // undefined, not 1
            if (!bce_->emit1(JSOP_UNDEFINED))
                return false;
            if (!bce_->emit1(JSOP_SETRVAL))
                return false;
        }

        state_ = Finally;
        return true;
    }

  private:
    bool emitFinallyEnd() {
        MOZ_ASSERT(state_ == Finally);

        if (retValKind_ == UseRetVal) {
            if (!bce_->emit1(JSOP_SETRVAL))
                return false;
        }

        if (!bce_->emit1(JSOP_RETSUB))
            return false;

        bce_->hasTryFinally = true;
        return true;
    }

  public:
    bool emitEnd() {
        if (state_ == Catch) {
            MOZ_ASSERT(!hasFinally());
            if (!emitCatchEnd())
                return false;
        } else {
            MOZ_ASSERT(state_ == Finally);
            MOZ_ASSERT(hasFinally());
            if (!emitFinallyEnd())
                return false;
        }

        MOZ_ASSERT(bce_->stackDepth == depth_);

        // ReconstructPCStack needs a NOP here to mark the end of the last
        // catch block.
        if (!bce_->emit1(JSOP_NOP))
            return false;

        // Fix up the end-of-try/catch jumps to come here.
        if (!bce_->emitJumpTargetAndPatch(catchAndFinallyJump_))
            return false;

        // Add the try note last, to let post-order give us the right ordering
        // (first to last for a given nesting level, inner to outer by level).
        if (hasCatch()) {
            if (!bce_->tryNoteList.append(JSTRY_CATCH, depth_, tryStart_, tryEnd_.offset))
                return false;
        }

        // If we've got a finally, mark try+catch region with additional
        // trynote to catch exceptions (re)thrown from a catch block or
        // for the try{}finally{} case.
        if (hasFinally()) {
            if (!bce_->tryNoteList.append(JSTRY_FINALLY, depth_, tryStart_, finallyStart_.offset))
                return false;
        }

        state_ = End;
        return true;
    }
};

class MOZ_STACK_CLASS IfThenElseEmitter
{
    BytecodeEmitter* bce_;
    JumpList jumpAroundThen_;
    JumpList jumpsAroundElse_;
    unsigned noteIndex_;
    int32_t thenDepth_;
#ifdef DEBUG
    int32_t pushed_;
    bool calculatedPushed_;
#endif
    enum State {
        Start,
        If,
        Cond,
        IfElse,
        Else,
        End
    };
    State state_;

  public:
    explicit IfThenElseEmitter(BytecodeEmitter* bce)
      : bce_(bce),
        noteIndex_(-1),
        thenDepth_(0),
#ifdef DEBUG
        pushed_(0),
        calculatedPushed_(false),
#endif
        state_(Start)
    {}

    ~IfThenElseEmitter()
    {}

  private:
    bool emitIf(State nextState) {
        MOZ_ASSERT(state_ == Start || state_ == Else);
        MOZ_ASSERT(nextState == If || nextState == IfElse || nextState == Cond);

        // Clear jumpAroundThen_ offset that points previous JSOP_IFEQ.
        if (state_ == Else)
            jumpAroundThen_ = JumpList();

        // Emit an annotated branch-if-false around the then part.
        SrcNoteType type = nextState == If ? SRC_IF : nextState == IfElse ? SRC_IF_ELSE : SRC_COND;
        if (!bce_->newSrcNote(type, &noteIndex_))
            return false;
        if (!bce_->emitJump(JSOP_IFEQ, &jumpAroundThen_))
            return false;

        // To restore stack depth in else part, save depth of the then part.
#ifdef DEBUG
        // If DEBUG, this is also necessary to calculate |pushed_|.
        thenDepth_ = bce_->stackDepth;
#else
        if (nextState == IfElse || nextState == Cond)
            thenDepth_ = bce_->stackDepth;
#endif
        state_ = nextState;
        return true;
    }

  public:
    bool emitIf() {
        return emitIf(If);
    }

    bool emitCond() {
        return emitIf(Cond);
    }

    bool emitIfElse() {
        return emitIf(IfElse);
    }

    bool emitElse() {
        MOZ_ASSERT(state_ == IfElse || state_ == Cond);

        calculateOrCheckPushed();

        // Emit a jump from the end of our then part around the else part. The
        // patchJumpsToTarget call at the bottom of this function will fix up
        // the offset with jumpsAroundElse value.
        if (!bce_->emitJump(JSOP_GOTO, &jumpsAroundElse_))
            return false;

        // Ensure the branch-if-false comes here, then emit the else.
        if (!bce_->emitJumpTargetAndPatch(jumpAroundThen_))
            return false;

        // Annotate SRC_IF_ELSE or SRC_COND with the offset from branch to
        // jump, for IonMonkey's benefit.  We can't just "back up" from the pc
        // of the else clause, because we don't know whether an extended
        // jump was required to leap from the end of the then clause over
        // the else clause.
        if (!bce_->setSrcNoteOffset(noteIndex_, 0,
                                    jumpsAroundElse_.offset - jumpAroundThen_.offset))
        {
            return false;
        }

        // Restore stack depth of the then part.
        bce_->stackDepth = thenDepth_;
        state_ = Else;
        return true;
    }

    bool emitEnd() {
        MOZ_ASSERT(state_ == If || state_ == Else);

        calculateOrCheckPushed();

        if (state_ == If) {
            // No else part, fixup the branch-if-false to come here.
            if (!bce_->emitJumpTargetAndPatch(jumpAroundThen_))
                return false;
        }

        // Patch all the jumps around else parts.
        if (!bce_->emitJumpTargetAndPatch(jumpsAroundElse_))
            return false;

        state_ = End;
        return true;
    }

    void calculateOrCheckPushed() {
#ifdef DEBUG
        if (!calculatedPushed_) {
            pushed_ = bce_->stackDepth - thenDepth_;
            calculatedPushed_ = true;
        } else {
            MOZ_ASSERT(pushed_ == bce_->stackDepth - thenDepth_);
        }
#endif
    }

#ifdef DEBUG
    int32_t pushed() const {
        return pushed_;
    }

    int32_t popped() const {
        return -pushed_;
    }
#endif
};

class ForOfLoopControl : public LoopControl
{
    using EmitterScope = BytecodeEmitter::EmitterScope;

    // The stack depth of the iterator.
    int32_t iterDepth_;

    // for-of loops, when throwing from non-iterator code (i.e. from the body
    // or from evaluating the LHS of the loop condition), need to call
    // IteratorClose.  This is done by enclosing non-iterator code with
    // try-catch and call IteratorClose in `catch` block.
    // If IteratorClose itself throws, we must not re-call IteratorClose. Since
    // non-local jumps like break and return call IteratorClose, whenever a
    // non-local jump is emitted, we must tell catch block not to perform
    // IteratorClose.
    //
    //   for (x of y) {
    //     // Operations for iterator (IteratorNext etc) are outside of
    //     // try-block.
    //     try {
    //       ...
    //       if (...) {
    //         // Before non-local jump, clear iterator on the stack to tell
    //         // catch block not to perform IteratorClose.
    //         tmpIterator = iterator;
    //         iterator = undefined;
    //         IteratorClose(tmpIterator, { break });
    //         break;
    //       }
    //       ...
    //     } catch (e) {
    //       // Just throw again when iterator is cleared by non-local jump.
    //       if (iterator === undefined)
    //         throw e;
    //       IteratorClose(iterator, { throw, e });
    //     }
    //   }
    Maybe<TryEmitter> tryCatch_;

    // Used to track if any yields were emitted between calls to to
    // emitBeginCodeNeedingIteratorClose and emitEndCodeNeedingIteratorClose.
    uint32_t numYieldsAtBeginCodeNeedingIterClose_;

    bool allowSelfHosted_;

    IteratorKind iterKind_;

  public:
    ForOfLoopControl(BytecodeEmitter* bce, int32_t iterDepth, bool allowSelfHosted,
                     IteratorKind iterKind)
      : LoopControl(bce, StatementKind::ForOfLoop),
        iterDepth_(iterDepth),
        numYieldsAtBeginCodeNeedingIterClose_(UINT32_MAX),
        allowSelfHosted_(allowSelfHosted),
        iterKind_(iterKind)
    {
    }

    bool emitBeginCodeNeedingIteratorClose(BytecodeEmitter* bce) {
        tryCatch_.emplace(bce, TryEmitter::TryCatch, TryEmitter::DontUseRetVal,
                          TryEmitter::DontUseControl);

        if (!tryCatch_->emitTry())
            return false;

        MOZ_ASSERT(numYieldsAtBeginCodeNeedingIterClose_ == UINT32_MAX);
        numYieldsAtBeginCodeNeedingIterClose_ = bce->yieldAndAwaitOffsetList.numYields;

        return true;
    }

    bool emitEndCodeNeedingIteratorClose(BytecodeEmitter* bce) {
        if (!tryCatch_->emitCatch())              // ITER ...
            return false;

        if (!bce->emit1(JSOP_EXCEPTION))          // ITER ... EXCEPTION
            return false;
        unsigned slotFromTop = bce->stackDepth - iterDepth_;
        if (!bce->emitDupAt(slotFromTop))         // ITER ... EXCEPTION ITER
            return false;

        // If ITER is undefined, it means the exception is thrown by
        // IteratorClose for non-local jump, and we should't perform
        // IteratorClose again here.
        if (!bce->emit1(JSOP_UNDEFINED))          // ITER ... EXCEPTION ITER UNDEF
            return false;
        if (!bce->emit1(JSOP_STRICTNE))           // ITER ... EXCEPTION NE
            return false;

        IfThenElseEmitter ifIteratorIsNotClosed(bce);
        if (!ifIteratorIsNotClosed.emitIf())      // ITER ... EXCEPTION
            return false;

        MOZ_ASSERT(slotFromTop == unsigned(bce->stackDepth - iterDepth_));
        if (!bce->emitDupAt(slotFromTop))         // ITER ... EXCEPTION ITER
            return false;
        if (!emitIteratorCloseInInnermostScope(bce, CompletionKind::Throw))
            return false;                         // ITER ... EXCEPTION

        if (!ifIteratorIsNotClosed.emitEnd())     // ITER ... EXCEPTION
            return false;

        if (!bce->emit1(JSOP_THROW))              // ITER ...
            return false;

        // If any yields were emitted, then this for-of loop is inside a star
        // generator and must handle the case of Generator.return. Like in
        // yield*, it is handled with a finally block.
        uint32_t numYieldsEmitted = bce->yieldAndAwaitOffsetList.numYields;
        if (numYieldsEmitted > numYieldsAtBeginCodeNeedingIterClose_) {
            if (!tryCatch_->emitFinally())
                return false;

            IfThenElseEmitter ifGeneratorClosing(bce);
            if (!bce->emit1(JSOP_ISGENCLOSING))   // ITER ... FTYPE FVALUE CLOSING
                return false;
            if (!ifGeneratorClosing.emitIf())     // ITER ... FTYPE FVALUE
                return false;
            if (!bce->emitDupAt(slotFromTop + 1)) // ITER ... FTYPE FVALUE ITER
                return false;
            if (!emitIteratorCloseInInnermostScope(bce, CompletionKind::Normal))
                return false;                     // ITER ... FTYPE FVALUE
            if (!ifGeneratorClosing.emitEnd())    // ITER ... FTYPE FVALUE
                return false;
        }

        if (!tryCatch_->emitEnd())
            return false;

        tryCatch_.reset();
        numYieldsAtBeginCodeNeedingIterClose_ = UINT32_MAX;

        return true;
    }

    bool emitIteratorCloseInInnermostScope(BytecodeEmitter* bce,
                                           CompletionKind completionKind = CompletionKind::Normal) {
        return emitIteratorCloseInScope(bce,  *bce->innermostEmitterScope(), completionKind);
    }

    bool emitIteratorCloseInScope(BytecodeEmitter* bce,
                                  EmitterScope& currentScope,
                                  CompletionKind completionKind = CompletionKind::Normal) {
        ptrdiff_t start = bce->offset();
        if (!bce->emitIteratorCloseInScope(currentScope, iterKind_, completionKind,
                                           allowSelfHosted_))
        {
            return false;
        }
        ptrdiff_t end = bce->offset();
        return bce->tryNoteList.append(JSTRY_FOR_OF_ITERCLOSE, 0, start, end);
    }

    bool emitPrepareForNonLocalJumpFromScope(BytecodeEmitter* bce,
                                             EmitterScope& currentScope,
                                             bool isTarget) {
        // Pop unnecessary value from the stack.  Effectively this means
        // leaving try-catch block.  However, the performing IteratorClose can
        // reach the depth for try-catch, and effectively re-enter the
        // try-catch block.
        if (!bce->emit1(JSOP_POP))                        // NEXT ITER
            return false;

        // Pop the iterator's next method.
        if (!bce->emit1(JSOP_SWAP))                       // ITER NEXT
            return false;
        if (!bce->emit1(JSOP_POP))                        // ITER
            return false;

        // Clear ITER slot on the stack to tell catch block to avoid performing
        // IteratorClose again.
        if (!bce->emit1(JSOP_UNDEFINED))                  // ITER UNDEF
            return false;
        if (!bce->emit1(JSOP_SWAP))                       // UNDEF ITER
            return false;

        if (!emitIteratorCloseInScope(bce, currentScope, CompletionKind::Normal)) // UNDEF
            return false;

        if (isTarget) {
            // At the level of the target block, there's bytecode after the
            // loop that will pop the next method, the iterator, and the
            // value, so push two undefineds to balance the stack.
            if (!bce->emit1(JSOP_UNDEFINED))              // UNDEF UNDEF
                return false;
            if (!bce->emit1(JSOP_UNDEFINED))              // UNDEF UNDEF UNDEF
                return false;
        } else {
            if (!bce->emit1(JSOP_POP))                    //
                return false;
        }

        return true;
    }
};

BytecodeEmitter::BytecodeEmitter(BytecodeEmitter* parent,
                                 const EitherParser<FullParseHandler>& parser, SharedContext* sc,
                                 HandleScript script, Handle<LazyScript*> lazyScript,
                                 uint32_t lineNum, EmitterMode emitterMode)
  : sc(sc),
    cx(sc->context),
    parent(parent),
    script(cx, script),
    lazyScript(cx, lazyScript),
    prologue(cx, lineNum),
    main(cx, lineNum),
    current(&main),
    parser(parser),
    atomIndices(cx->frontendCollectionPool()),
    firstLine(lineNum),
    maxFixedSlots(0),
    maxStackDepth(0),
    stackDepth(0),
    emitLevel(0),
    bodyScopeIndex(UINT32_MAX),
    varEmitterScope(nullptr),
    innermostNestableControl(nullptr),
    innermostEmitterScope_(nullptr),
    innermostTDZCheckCache(nullptr),
#ifdef DEBUG
    unstableEmitterScope(false),
#endif
    constList(cx),
    scopeList(cx),
    tryNoteList(cx),
    scopeNoteList(cx),
    yieldAndAwaitOffsetList(cx),
    typesetCount(0),
    hasSingletons(false),
    hasTryFinally(false),
    emittingRunOnceLambda(false),
    emitterMode(emitterMode),
    functionBodyEndPosSet(false)
{
    MOZ_ASSERT_IF(emitterMode == LazyFunction, lazyScript);
}

BytecodeEmitter::BytecodeEmitter(BytecodeEmitter* parent,
                                 const EitherParser<FullParseHandler>& parser, SharedContext* sc,
                                 HandleScript script, Handle<LazyScript*> lazyScript,
                                 TokenPos bodyPosition, EmitterMode emitterMode)
    : BytecodeEmitter(parent, parser, sc, script, lazyScript,
                      parser.tokenStream().srcCoords.lineNum(bodyPosition.begin),
                      emitterMode)
{
    setFunctionBodyEndPos(bodyPosition);
}

bool
BytecodeEmitter::init()
{
    return atomIndices.acquire(cx);
}

template <typename Predicate /* (NestableControl*) -> bool */>
BytecodeEmitter::NestableControl*
BytecodeEmitter::findInnermostNestableControl(Predicate predicate) const
{
    return NestableControl::findNearest(innermostNestableControl, predicate);
}

template <typename T>
T*
BytecodeEmitter::findInnermostNestableControl() const
{
    return NestableControl::findNearest<T>(innermostNestableControl);
}

template <typename T, typename Predicate /* (T*) -> bool */>
T*
BytecodeEmitter::findInnermostNestableControl(Predicate predicate) const
{
    return NestableControl::findNearest<T>(innermostNestableControl, predicate);
}

NameLocation
BytecodeEmitter::lookupName(JSAtom* name)
{
    return innermostEmitterScope()->lookup(this, name);
}

Maybe<NameLocation>
BytecodeEmitter::locationOfNameBoundInScope(JSAtom* name, EmitterScope* target)
{
    return innermostEmitterScope()->locationBoundInScope(name, target);
}

Maybe<NameLocation>
BytecodeEmitter::locationOfNameBoundInFunctionScope(JSAtom* name, EmitterScope* source)
{
    EmitterScope* funScope = source;
    while (!funScope->scope(this)->is<FunctionScope>())
        funScope = funScope->enclosingInFrame();
    return source->locationBoundInScope(name, funScope);
}

bool
BytecodeEmitter::emitCheck(ptrdiff_t delta, ptrdiff_t* offset)
{
    *offset = code().length();

    if (!code().growBy(delta)) {
        ReportOutOfMemory(cx);
        return false;
    }
    return true;
}

void
BytecodeEmitter::updateDepth(ptrdiff_t target)
{
    jsbytecode* pc = code(target);

    int nuses = StackUses(pc);
    int ndefs = StackDefs(pc);

    stackDepth -= nuses;
    MOZ_ASSERT(stackDepth >= 0);
    stackDepth += ndefs;

    if ((uint32_t)stackDepth > maxStackDepth)
        maxStackDepth = stackDepth;
}

#ifdef DEBUG
bool
BytecodeEmitter::checkStrictOrSloppy(JSOp op)
{
    if (IsCheckStrictOp(op) && !sc->strict())
        return false;
    if (IsCheckSloppyOp(op) && sc->strict())
        return false;
    return true;
}
#endif

bool
BytecodeEmitter::emit1(JSOp op)
{
    MOZ_ASSERT(checkStrictOrSloppy(op));

    ptrdiff_t offset;
    if (!emitCheck(1, &offset))
        return false;

    jsbytecode* code = this->code(offset);
    code[0] = jsbytecode(op);
    updateDepth(offset);
    return true;
}

bool
BytecodeEmitter::emit2(JSOp op, uint8_t op1)
{
    MOZ_ASSERT(checkStrictOrSloppy(op));

    ptrdiff_t offset;
    if (!emitCheck(2, &offset))
        return false;

    jsbytecode* code = this->code(offset);
    code[0] = jsbytecode(op);
    code[1] = jsbytecode(op1);
    updateDepth(offset);
    return true;
}

bool
BytecodeEmitter::emit3(JSOp op, jsbytecode op1, jsbytecode op2)
{
    MOZ_ASSERT(checkStrictOrSloppy(op));

    /* These should filter through emitVarOp. */
    MOZ_ASSERT(!IsArgOp(op));
    MOZ_ASSERT(!IsLocalOp(op));

    ptrdiff_t offset;
    if (!emitCheck(3, &offset))
        return false;

    jsbytecode* code = this->code(offset);
    code[0] = jsbytecode(op);
    code[1] = op1;
    code[2] = op2;
    updateDepth(offset);
    return true;
}

bool
BytecodeEmitter::emitN(JSOp op, size_t extra, ptrdiff_t* offset)
{
    MOZ_ASSERT(checkStrictOrSloppy(op));
    ptrdiff_t length = 1 + ptrdiff_t(extra);

    ptrdiff_t off;
    if (!emitCheck(length, &off))
        return false;

    jsbytecode* code = this->code(off);
    code[0] = jsbytecode(op);
    /* The remaining |extra| bytes are set by the caller */

    /*
     * Don't updateDepth if op's use-count comes from the immediate
     * operand yet to be stored in the extra bytes after op.
     */
    if (CodeSpec[op].nuses >= 0)
        updateDepth(off);

    if (offset)
        *offset = off;
    return true;
}

bool
BytecodeEmitter::emitJumpTarget(JumpTarget* target)
{
    ptrdiff_t off = offset();

    // Alias consecutive jump targets.
    if (off == current->lastTarget.offset + ptrdiff_t(JSOP_JUMPTARGET_LENGTH)) {
        target->offset = current->lastTarget.offset;
        return true;
    }

    target->offset = off;
    current->lastTarget.offset = off;
    if (!emit1(JSOP_JUMPTARGET))
        return false;
    return true;
}

void
JumpList::push(jsbytecode* code, ptrdiff_t jumpOffset)
{
    SET_JUMP_OFFSET(&code[jumpOffset], offset - jumpOffset);
    offset = jumpOffset;
}

void
JumpList::patchAll(jsbytecode* code, JumpTarget target)
{
    ptrdiff_t delta;
    for (ptrdiff_t jumpOffset = offset; jumpOffset != -1; jumpOffset += delta) {
        jsbytecode* pc = &code[jumpOffset];
        MOZ_ASSERT(IsJumpOpcode(JSOp(*pc)) || JSOp(*pc) == JSOP_LABEL);
        delta = GET_JUMP_OFFSET(pc);
        MOZ_ASSERT(delta < 0);
        ptrdiff_t span = target.offset - jumpOffset;
        SET_JUMP_OFFSET(pc, span);
    }
}

bool
BytecodeEmitter::emitJumpNoFallthrough(JSOp op, JumpList* jump)
{
    ptrdiff_t offset;
    if (!emitCheck(5, &offset))
        return false;

    jsbytecode* code = this->code(offset);
    code[0] = jsbytecode(op);
    MOZ_ASSERT(-1 <= jump->offset && jump->offset < offset);
    jump->push(this->code(0), offset);
    updateDepth(offset);
    return true;
}

bool
BytecodeEmitter::emitJump(JSOp op, JumpList* jump)
{
    if (!emitJumpNoFallthrough(op, jump))
        return false;
    if (BytecodeFallsThrough(op)) {
        JumpTarget fallthrough;
        if (!emitJumpTarget(&fallthrough))
            return false;
    }
    return true;
}

bool
BytecodeEmitter::emitBackwardJump(JSOp op, JumpTarget target, JumpList* jump, JumpTarget* fallthrough)
{
    if (!emitJumpNoFallthrough(op, jump))
        return false;
    patchJumpsToTarget(*jump, target);

    // Unconditionally create a fallthrough for closing iterators, and as a
    // target for break statements.
    if (!emitJumpTarget(fallthrough))
        return false;
    return true;
}

void
BytecodeEmitter::patchJumpsToTarget(JumpList jump, JumpTarget target)
{
    MOZ_ASSERT(-1 <= jump.offset && jump.offset <= offset());
    MOZ_ASSERT(0 <= target.offset && target.offset <= offset());
    MOZ_ASSERT_IF(jump.offset != -1 && target.offset + 4 <= offset(),
                  BytecodeIsJumpTarget(JSOp(*code(target.offset))));
    jump.patchAll(code(0), target);
}

bool
BytecodeEmitter::emitJumpTargetAndPatch(JumpList jump)
{
    if (jump.offset == -1)
        return true;
    JumpTarget target;
    if (!emitJumpTarget(&target))
        return false;
    patchJumpsToTarget(jump, target);
    return true;
}

bool
BytecodeEmitter::emitCall(JSOp op, uint16_t argc, ParseNode* pn)
{
    if (pn && !updateSourceCoordNotes(pn->pn_pos.begin))
        return false;
    return emit3(op, ARGC_LO(argc), ARGC_HI(argc));
}

bool
BytecodeEmitter::emitDupAt(unsigned slotFromTop)
{
    MOZ_ASSERT(slotFromTop < unsigned(stackDepth));

    if (slotFromTop == 0)
        return emit1(JSOP_DUP);

    if (slotFromTop >= JS_BIT(24)) {
        reportError(nullptr, JSMSG_TOO_MANY_LOCALS);
        return false;
    }

    ptrdiff_t off;
    if (!emitN(JSOP_DUPAT, 3, &off))
        return false;

    jsbytecode* pc = code(off);
    SET_UINT24(pc, slotFromTop);
    return true;
}

bool
BytecodeEmitter::emitPopN(unsigned n)
{
    MOZ_ASSERT(n != 0);

    if (n == 1)
        return emit1(JSOP_POP);

    // 2 JSOP_POPs (2 bytes) are shorter than JSOP_POPN (3 bytes).
    if (n == 2)
        return emit1(JSOP_POP) && emit1(JSOP_POP);

    return emitUint16Operand(JSOP_POPN, n);
}

bool
BytecodeEmitter::emitCheckIsObj(CheckIsObjectKind kind)
{
    return emit2(JSOP_CHECKISOBJ, uint8_t(kind));
}

bool
BytecodeEmitter::emitCheckIsCallable(CheckIsCallableKind kind)
{
    return emit2(JSOP_CHECKISCALLABLE, uint8_t(kind));
}

static inline unsigned
LengthOfSetLine(unsigned line)
{
    return 1 /* SRC_SETLINE */ + (line > SN_4BYTE_OFFSET_MASK ? 4 : 1);
}

/* Updates line number notes, not column notes. */
bool
BytecodeEmitter::updateLineNumberNotes(uint32_t offset)
{
    TokenStreamAnyChars* ts = &parser.tokenStream();
    bool onThisLine;
    if (!ts->srcCoords.isOnThisLine(offset, currentLine(), &onThisLine)) {
        ts->reportErrorNoOffset(JSMSG_OUT_OF_MEMORY);
        return false;
    }

    if (!onThisLine) {
        unsigned line = ts->srcCoords.lineNum(offset);
        unsigned delta = line - currentLine();

        /*
         * Encode any change in the current source line number by using
         * either several SRC_NEWLINE notes or just one SRC_SETLINE note,
         * whichever consumes less space.
         *
         * NB: We handle backward line number deltas (possible with for
         * loops where the update part is emitted after the body, but its
         * line number is <= any line number in the body) here by letting
         * unsigned delta_ wrap to a very large number, which triggers a
         * SRC_SETLINE.
         */
        current->currentLine = line;
        current->lastColumn  = 0;
        if (delta >= LengthOfSetLine(line)) {
            if (!newSrcNote2(SRC_SETLINE, ptrdiff_t(line)))
                return false;
        } else {
            do {
                if (!newSrcNote(SRC_NEWLINE))
                    return false;
            } while (--delta != 0);
        }
    }
    return true;
}

/* Updates the line number and column number information in the source notes. */
bool
BytecodeEmitter::updateSourceCoordNotes(uint32_t offset)
{
    if (!updateLineNumberNotes(offset))
        return false;

    uint32_t columnIndex = parser.tokenStream().srcCoords.columnIndex(offset);
    ptrdiff_t colspan = ptrdiff_t(columnIndex) - ptrdiff_t(current->lastColumn);
    if (colspan != 0) {
        // If the column span is so large that we can't store it, then just
        // discard this information. This can happen with minimized or otherwise
        // machine-generated code. Even gigantic column numbers are still
        // valuable if you have a source map to relate them to something real;
        // but it's better to fail soft here.
        if (!SN_REPRESENTABLE_COLSPAN(colspan))
            return true;
        if (!newSrcNote2(SRC_COLSPAN, SN_COLSPAN_TO_OFFSET(colspan)))
            return false;
        current->lastColumn = columnIndex;
    }
    return true;
}

bool
BytecodeEmitter::emitLoopHead(ParseNode* nextpn, JumpTarget* top)
{
    if (nextpn) {
        /*
         * Try to give the JSOP_LOOPHEAD the same line number as the next
         * instruction. nextpn is often a block, in which case the next
         * instruction typically comes from the first statement inside.
         */
        if (nextpn->isKind(ParseNodeKind::LexicalScope))
            nextpn = nextpn->scopeBody();
        MOZ_ASSERT_IF(nextpn->isKind(ParseNodeKind::StatementList), nextpn->isArity(PN_LIST));
        if (nextpn->isKind(ParseNodeKind::StatementList) && nextpn->pn_head)
            nextpn = nextpn->pn_head;
        if (!updateSourceCoordNotes(nextpn->pn_pos.begin))
            return false;
    }

    *top = { offset() };
    return emit1(JSOP_LOOPHEAD);
}

bool
BytecodeEmitter::emitLoopEntry(ParseNode* nextpn, JumpList entryJump)
{
    if (nextpn) {
        /* Update the line number, as for LOOPHEAD. */
        if (nextpn->isKind(ParseNodeKind::LexicalScope))
            nextpn = nextpn->scopeBody();
        MOZ_ASSERT_IF(nextpn->isKind(ParseNodeKind::StatementList), nextpn->isArity(PN_LIST));
        if (nextpn->isKind(ParseNodeKind::StatementList) && nextpn->pn_head)
            nextpn = nextpn->pn_head;
        if (!updateSourceCoordNotes(nextpn->pn_pos.begin))
            return false;
    }

    JumpTarget entry{ offset() };
    patchJumpsToTarget(entryJump, entry);

    LoopControl& loopInfo = innermostNestableControl->as<LoopControl>();
    MOZ_ASSERT(loopInfo.loopDepth() > 0);

    uint8_t loopDepthAndFlags = PackLoopEntryDepthHintAndFlags(loopInfo.loopDepth(),
                                                               loopInfo.canIonOsr());
    return emit2(JSOP_LOOPENTRY, loopDepthAndFlags);
}

void
BytecodeEmitter::checkTypeSet(JSOp op)
{
    if (CodeSpec[op].format & JOF_TYPESET) {
        if (typesetCount < UINT16_MAX)
            typesetCount++;
    }
}

bool
BytecodeEmitter::emitUint16Operand(JSOp op, uint32_t operand)
{
    MOZ_ASSERT(operand <= UINT16_MAX);
    if (!emit3(op, UINT16_LO(operand), UINT16_HI(operand)))
        return false;
    checkTypeSet(op);
    return true;
}

bool
BytecodeEmitter::emitUint32Operand(JSOp op, uint32_t operand)
{
    ptrdiff_t off;
    if (!emitN(op, 4, &off))
        return false;
    SET_UINT32(code(off), operand);
    checkTypeSet(op);
    return true;
}

namespace {

class NonLocalExitControl
{
  public:
    enum Kind
    {
        // IteratorClose is handled especially inside the exception unwinder.
        Throw,

        // A 'continue' statement does not call IteratorClose for the loop it
        // is continuing, i.e. excluding the target loop.
        Continue,

        // A 'break' or 'return' statement does call IteratorClose for the
        // loop it is breaking out of or returning from, i.e. including the
        // target loop.
        Break,
        Return
    };

  private:
    BytecodeEmitter* bce_;
    const uint32_t savedScopeNoteIndex_;
    const int savedDepth_;
    uint32_t openScopeNoteIndex_;
    Kind kind_;

    NonLocalExitControl(const NonLocalExitControl&) = delete;

    MOZ_MUST_USE bool leaveScope(BytecodeEmitter::EmitterScope* scope);

  public:
    NonLocalExitControl(BytecodeEmitter* bce, Kind kind)
      : bce_(bce),
        savedScopeNoteIndex_(bce->scopeNoteList.length()),
        savedDepth_(bce->stackDepth),
        openScopeNoteIndex_(bce->innermostEmitterScope()->noteIndex()),
        kind_(kind)
    { }

    ~NonLocalExitControl() {
        for (uint32_t n = savedScopeNoteIndex_; n < bce_->scopeNoteList.length(); n++)
            bce_->scopeNoteList.recordEnd(n, bce_->offset(), bce_->inPrologue());
        bce_->stackDepth = savedDepth_;
    }

    MOZ_MUST_USE bool prepareForNonLocalJump(BytecodeEmitter::NestableControl* target);

    MOZ_MUST_USE bool prepareForNonLocalJumpToOutermost() {
        return prepareForNonLocalJump(nullptr);
    }
};

bool
NonLocalExitControl::leaveScope(BytecodeEmitter::EmitterScope* es)
{
    if (!es->leave(bce_, /* nonLocal = */ true))
        return false;

    // As we pop each scope due to the non-local jump, emit notes that
    // record the extent of the enclosing scope. These notes will have
    // their ends recorded in ~NonLocalExitControl().
    uint32_t enclosingScopeIndex = ScopeNote::NoScopeIndex;
    if (es->enclosingInFrame())
        enclosingScopeIndex = es->enclosingInFrame()->index();
    if (!bce_->scopeNoteList.append(enclosingScopeIndex, bce_->offset(), bce_->inPrologue(),
                                    openScopeNoteIndex_))
        return false;
    openScopeNoteIndex_ = bce_->scopeNoteList.length() - 1;

    return true;
}

/*
 * Emit additional bytecode(s) for non-local jumps.
 */
bool
NonLocalExitControl::prepareForNonLocalJump(BytecodeEmitter::NestableControl* target)
{
    using NestableControl = BytecodeEmitter::NestableControl;
    using EmitterScope = BytecodeEmitter::EmitterScope;

    EmitterScope* es = bce_->innermostEmitterScope();
    int npops = 0;

    AutoCheckUnstableEmitterScope cues(bce_);

    // For 'continue', 'break', and 'return' statements, emit IteratorClose
    // bytecode inline. 'continue' statements do not call IteratorClose for
    // the loop they are continuing.
    bool emitIteratorClose = kind_ == Continue || kind_ == Break || kind_ == Return;
    bool emitIteratorCloseAtTarget = emitIteratorClose && kind_ != Continue;

    auto flushPops = [&npops](BytecodeEmitter* bce) {
        if (npops && !bce->emitPopN(npops))
            return false;
        npops = 0;
        return true;
    };

    // Walk the nestable control stack and patch jumps.
    for (NestableControl* control = bce_->innermostNestableControl;
         control != target;
         control = control->enclosing())
    {
        // Walk the scope stack and leave the scopes we entered. Leaving a scope
        // may emit administrative ops like JSOP_POPLEXICALENV but never anything
        // that manipulates the stack.
        for (; es != control->emitterScope(); es = es->enclosingInFrame()) {
            if (!leaveScope(es))
                return false;
        }

        switch (control->kind()) {
          case StatementKind::Finally: {
            TryFinallyControl& finallyControl = control->as<TryFinallyControl>();
            if (finallyControl.emittingSubroutine()) {
                /*
                 * There's a [exception or hole, retsub pc-index] pair and the
                 * possible return value on the stack that we need to pop.
                 */
                npops += 3;
            } else {
                if (!flushPops(bce_))
                    return false;
                if (!bce_->emitJump(JSOP_GOSUB, &finallyControl.gosubs)) // ...
                    return false;
            }
            break;
          }

          case StatementKind::ForOfLoop:
            if (emitIteratorClose) {
                if (!flushPops(bce_))
                    return false;

                ForOfLoopControl& loopinfo = control->as<ForOfLoopControl>();
                if (!loopinfo.emitPrepareForNonLocalJumpFromScope(bce_, *es,
                                                                  /* isTarget = */ false))
                {                                         // ...
                    return false;
                }
            } else {
                // The iterator next method, the iterator, and the current
                // value are on the stack.
                npops += 3;
            }
            break;

          case StatementKind::ForInLoop:
            if (!flushPops(bce_))
                return false;

            // The iterator and the current value are on the stack.
            if (!bce_->emit1(JSOP_POP))                   // ... ITER
                return false;
            if (!bce_->emit1(JSOP_ENDITER))               // ...
                return false;
            break;

          default:
            break;
        }
    }

    if (!flushPops(bce_))
        return false;

    if (target && emitIteratorCloseAtTarget && target->is<ForOfLoopControl>()) {
        ForOfLoopControl& loopinfo = target->as<ForOfLoopControl>();
        if (!loopinfo.emitPrepareForNonLocalJumpFromScope(bce_, *es,
                                                          /* isTarget = */ true))
        {                                                 // ... UNDEF UNDEF UNDEF
            return false;
        }
    }

    EmitterScope* targetEmitterScope = target ? target->emitterScope() : bce_->varEmitterScope;
    for (; es != targetEmitterScope; es = es->enclosingInFrame()) {
        if (!leaveScope(es))
            return false;
    }

    return true;
}

}  // anonymous namespace

bool
BytecodeEmitter::emitGoto(NestableControl* target, JumpList* jumplist, SrcNoteType noteType)
{
    NonLocalExitControl nle(this, noteType == SRC_CONTINUE
                                  ? NonLocalExitControl::Continue
                                  : NonLocalExitControl::Break);

    if (!nle.prepareForNonLocalJump(target))
        return false;

    if (noteType != SRC_NULL) {
        if (!newSrcNote(noteType))
            return false;
    }

    return emitJump(JSOP_GOTO, jumplist);
}

Scope*
BytecodeEmitter::innermostScope() const
{
    return innermostEmitterScope()->scope(this);
}

bool
BytecodeEmitter::emitIndex32(JSOp op, uint32_t index)
{
    MOZ_ASSERT(checkStrictOrSloppy(op));

    const size_t len = 1 + UINT32_INDEX_LEN;
    MOZ_ASSERT(len == size_t(CodeSpec[op].length));

    ptrdiff_t offset;
    if (!emitCheck(len, &offset))
        return false;

    jsbytecode* code = this->code(offset);
    code[0] = jsbytecode(op);
    SET_UINT32_INDEX(code, index);
    checkTypeSet(op);
    updateDepth(offset);
    return true;
}

bool
BytecodeEmitter::emitIndexOp(JSOp op, uint32_t index)
{
    MOZ_ASSERT(checkStrictOrSloppy(op));

    const size_t len = CodeSpec[op].length;
    MOZ_ASSERT(len >= 1 + UINT32_INDEX_LEN);

    ptrdiff_t offset;
    if (!emitCheck(len, &offset))
        return false;

    jsbytecode* code = this->code(offset);
    code[0] = jsbytecode(op);
    SET_UINT32_INDEX(code, index);
    checkTypeSet(op);
    updateDepth(offset);
    return true;
}

bool
BytecodeEmitter::emitAtomOp(JSAtom* atom, JSOp op)
{
    MOZ_ASSERT(atom);
    MOZ_ASSERT(JOF_OPTYPE(op) == JOF_ATOM);

    // .generator lookups should be emitted as JSOP_GETALIASEDVAR instead of
    // JSOP_GETNAME etc, to bypass |with| objects on the scope chain.
    // It's safe to emit .this lookups though because |with| objects skip
    // those.
    MOZ_ASSERT_IF(op == JSOP_GETNAME || op == JSOP_GETGNAME,
                  atom != cx->names().dotGenerator);

    if (op == JSOP_GETPROP && atom == cx->names().length) {
        /* Specialize length accesses for the interpreter. */
        op = JSOP_LENGTH;
    }

    uint32_t index;
    if (!makeAtomIndex(atom, &index))
        return false;

    return emitIndexOp(op, index);
}

bool
BytecodeEmitter::emitAtomOp(ParseNode* pn, JSOp op)
{
    MOZ_ASSERT(pn->pn_atom != nullptr);
    return emitAtomOp(pn->pn_atom, op);
}

bool
BytecodeEmitter::emitInternedScopeOp(uint32_t index, JSOp op)
{
    MOZ_ASSERT(JOF_OPTYPE(op) == JOF_SCOPE);
    MOZ_ASSERT(index < scopeList.length());
    return emitIndex32(op, index);
}

bool
BytecodeEmitter::emitInternedObjectOp(uint32_t index, JSOp op)
{
    MOZ_ASSERT(JOF_OPTYPE(op) == JOF_OBJECT);
    MOZ_ASSERT(index < objectList.length);
    return emitIndex32(op, index);
}

bool
BytecodeEmitter::emitObjectOp(ObjectBox* objbox, JSOp op)
{
    return emitInternedObjectOp(objectList.add(objbox), op);
}

bool
BytecodeEmitter::emitObjectPairOp(ObjectBox* objbox1, ObjectBox* objbox2, JSOp op)
{
    uint32_t index = objectList.add(objbox1);
    objectList.add(objbox2);
    return emitInternedObjectOp(index, op);
}

bool
BytecodeEmitter::emitRegExp(uint32_t index)
{
    return emitIndex32(JSOP_REGEXP, index);
}

bool
BytecodeEmitter::emitLocalOp(JSOp op, uint32_t slot)
{
    MOZ_ASSERT(JOF_OPTYPE(op) != JOF_ENVCOORD);
    MOZ_ASSERT(IsLocalOp(op));

    ptrdiff_t off;
    if (!emitN(op, LOCALNO_LEN, &off))
        return false;

    SET_LOCALNO(code(off), slot);
    return true;
}

bool
BytecodeEmitter::emitArgOp(JSOp op, uint16_t slot)
{
    MOZ_ASSERT(IsArgOp(op));
    ptrdiff_t off;
    if (!emitN(op, ARGNO_LEN, &off))
        return false;

    SET_ARGNO(code(off), slot);
    return true;
}

bool
BytecodeEmitter::emitEnvCoordOp(JSOp op, EnvironmentCoordinate ec)
{
    MOZ_ASSERT(JOF_OPTYPE(op) == JOF_ENVCOORD);

    unsigned n = ENVCOORD_HOPS_LEN + ENVCOORD_SLOT_LEN;
    MOZ_ASSERT(int(n) + 1 /* op */ == CodeSpec[op].length);

    ptrdiff_t off;
    if (!emitN(op, n, &off))
        return false;

    jsbytecode* pc = code(off);
    SET_ENVCOORD_HOPS(pc, ec.hops());
    pc += ENVCOORD_HOPS_LEN;
    SET_ENVCOORD_SLOT(pc, ec.slot());
    pc += ENVCOORD_SLOT_LEN;
    checkTypeSet(op);
    return true;
}

static JSOp
GetIncDecInfo(ParseNodeKind kind, bool* post)
{
    MOZ_ASSERT(kind == ParseNodeKind::PostIncrement ||
               kind == ParseNodeKind::PreIncrement ||
               kind == ParseNodeKind::PostDecrement ||
               kind == ParseNodeKind::PreDecrement);
    *post = kind == ParseNodeKind::PostIncrement || kind == ParseNodeKind::PostDecrement;
    return (kind == ParseNodeKind::PostIncrement || kind == ParseNodeKind::PreIncrement)
           ? JSOP_ADD
           : JSOP_SUB;
}

JSOp
BytecodeEmitter::strictifySetNameOp(JSOp op)
{
    switch (op) {
      case JSOP_SETNAME:
        if (sc->strict())
            op = JSOP_STRICTSETNAME;
        break;
      case JSOP_SETGNAME:
        if (sc->strict())
            op = JSOP_STRICTSETGNAME;
        break;
        default:;
    }
    return op;
}

bool
BytecodeEmitter::checkSideEffects(ParseNode* pn, bool* answer)
{
    if (!CheckRecursionLimit(cx))
        return false;

 restart:

    switch (pn->getKind()) {
      // Trivial cases with no side effects.
      case ParseNodeKind::EmptyStatement:
      case ParseNodeKind::String:
      case ParseNodeKind::TemplateString:
      case ParseNodeKind::RegExp:
      case ParseNodeKind::True:
      case ParseNodeKind::False:
      case ParseNodeKind::Null:
      case ParseNodeKind::RawUndefined:
      case ParseNodeKind::Elision:
      case ParseNodeKind::Generator:
      case ParseNodeKind::Number:
      case ParseNodeKind::ObjectPropertyName:
        MOZ_ASSERT(pn->isArity(PN_NULLARY));
        *answer = false;
        return true;

      // |this| can throw in derived class constructors, including nested arrow
      // functions or eval.
      case ParseNodeKind::This:
        MOZ_ASSERT(pn->isArity(PN_UNARY));
        *answer = sc->needsThisTDZChecks();
        return true;

      // Trivial binary nodes with more token pos holders.
      case ParseNodeKind::NewTarget:
        MOZ_ASSERT(pn->isArity(PN_BINARY));
        MOZ_ASSERT(pn->pn_left->isKind(ParseNodeKind::PosHolder));
        MOZ_ASSERT(pn->pn_right->isKind(ParseNodeKind::PosHolder));
        *answer = false;
        return true;

      case ParseNodeKind::Break:
      case ParseNodeKind::Continue:
      case ParseNodeKind::Debugger:
        MOZ_ASSERT(pn->isArity(PN_NULLARY));
        *answer = true;
        return true;

      // Watch out for getters!
      case ParseNodeKind::Dot:
        MOZ_ASSERT(pn->isArity(PN_NAME));
        *answer = true;
        return true;

      // Unary cases with side effects only if the child has them.
      case ParseNodeKind::TypeOfExpr:
      case ParseNodeKind::Void:
      case ParseNodeKind::Not:
        MOZ_ASSERT(pn->isArity(PN_UNARY));
        return checkSideEffects(pn->pn_kid, answer);

      // Even if the name expression is effect-free, performing ToPropertyKey on
      // it might not be effect-free:
      //
      //   RegExp.prototype.toString = () => { throw 42; };
      //   ({ [/regex/]: 0 }); // ToPropertyKey(/regex/) throws 42
      //
      //   function Q() {
      //     ({ [new.target]: 0 });
      //   }
      //   Q.toString = () => { throw 17; };
      //   new Q; // new.target will be Q, ToPropertyKey(Q) throws 17
      case ParseNodeKind::ComputedName:
        MOZ_ASSERT(pn->isArity(PN_UNARY));
        *answer = true;
        return true;

      // Looking up or evaluating the associated name could throw.
      case ParseNodeKind::TypeOfName:
        MOZ_ASSERT(pn->isArity(PN_UNARY));
        *answer = true;
        return true;

      // This unary case has side effects on the enclosing object, sure.  But
      // that's not the question this function answers: it's whether the
      // operation may have a side effect on something *other* than the result
      // of the overall operation in which it's embedded.  The answer to that
      // is no, because an object literal having a mutated prototype only
      // produces a value, without affecting anything else.
      case ParseNodeKind::MutateProto:
        MOZ_ASSERT(pn->isArity(PN_UNARY));
        return checkSideEffects(pn->pn_kid, answer);

      // Unary cases with obvious side effects.
      case ParseNodeKind::PreIncrement:
      case ParseNodeKind::PostIncrement:
      case ParseNodeKind::PreDecrement:
      case ParseNodeKind::PostDecrement:
      case ParseNodeKind::Throw:
        MOZ_ASSERT(pn->isArity(PN_UNARY));
        *answer = true;
        return true;

      // These might invoke valueOf/toString, even with a subexpression without
      // side effects!  Consider |+{ valueOf: null, toString: null }|.
      case ParseNodeKind::BitNot:
      case ParseNodeKind::Pos:
      case ParseNodeKind::Neg:
        MOZ_ASSERT(pn->isArity(PN_UNARY));
        *answer = true;
        return true;

      // This invokes the (user-controllable) iterator protocol.
      case ParseNodeKind::Spread:
        MOZ_ASSERT(pn->isArity(PN_UNARY));
        *answer = true;
        return true;

      case ParseNodeKind::InitialYield:
      case ParseNodeKind::YieldStar:
      case ParseNodeKind::Yield:
      case ParseNodeKind::Await:
        MOZ_ASSERT(pn->isArity(PN_UNARY));
        *answer = true;
        return true;

      // Deletion generally has side effects, even if isolated cases have none.
      case ParseNodeKind::DeleteName:
      case ParseNodeKind::DeleteProp:
      case ParseNodeKind::DeleteElem:
        MOZ_ASSERT(pn->isArity(PN_UNARY));
        *answer = true;
        return true;

      // Deletion of a non-Reference expression has side effects only through
      // evaluating the expression.
      case ParseNodeKind::DeleteExpr: {
        MOZ_ASSERT(pn->isArity(PN_UNARY));
        ParseNode* expr = pn->pn_kid;
        return checkSideEffects(expr, answer);
      }

      case ParseNodeKind::ExpressionStatement:
        MOZ_ASSERT(pn->isArity(PN_UNARY));
        return checkSideEffects(pn->pn_kid, answer);

      // Binary cases with obvious side effects.
      case ParseNodeKind::Assign:
      case ParseNodeKind::AddAssign:
      case ParseNodeKind::SubAssign:
      case ParseNodeKind::BitOrAssign:
      case ParseNodeKind::BitXorAssign:
      case ParseNodeKind::BitAndAssign:
      case ParseNodeKind::LshAssign:
      case ParseNodeKind::RshAssign:
      case ParseNodeKind::UrshAssign:
      case ParseNodeKind::MulAssign:
      case ParseNodeKind::DivAssign:
      case ParseNodeKind::ModAssign:
      case ParseNodeKind::PowAssign:
      case ParseNodeKind::SetThis:
        MOZ_ASSERT(pn->isArity(PN_BINARY));
        *answer = true;
        return true;

      case ParseNodeKind::StatementList:
      // Strict equality operations and logical operators are well-behaved and
      // perform no conversions.
      case ParseNodeKind::Or:
      case ParseNodeKind::And:
      case ParseNodeKind::StrictEq:
      case ParseNodeKind::StrictNe:
      // Any subexpression of a comma expression could be effectful.
      case ParseNodeKind::Comma:
        MOZ_ASSERT(pn->pn_count > 0);
        MOZ_FALLTHROUGH;
      // Subcomponents of a literal may be effectful.
      case ParseNodeKind::Array:
      case ParseNodeKind::Object:
        MOZ_ASSERT(pn->isArity(PN_LIST));
        for (ParseNode* item = pn->pn_head; item; item = item->pn_next) {
            if (!checkSideEffects(item, answer))
                return false;
            if (*answer)
                return true;
        }
        return true;

      // Most other binary operations (parsed as lists in SpiderMonkey) may
      // perform conversions triggering side effects.  Math operations perform
      // ToNumber and may fail invoking invalid user-defined toString/valueOf:
      // |5 < { toString: null }|.  |instanceof| throws if provided a
      // non-object constructor: |null instanceof null|.  |in| throws if given
      // a non-object RHS: |5 in null|.
      case ParseNodeKind::BitOr:
      case ParseNodeKind::BitXor:
      case ParseNodeKind::BitAnd:
      case ParseNodeKind::Eq:
      case ParseNodeKind::Ne:
      case ParseNodeKind::Lt:
      case ParseNodeKind::Le:
      case ParseNodeKind::Gt:
      case ParseNodeKind::Ge:
      case ParseNodeKind::InstanceOf:
      case ParseNodeKind::In:
      case ParseNodeKind::Lsh:
      case ParseNodeKind::Rsh:
      case ParseNodeKind::Ursh:
      case ParseNodeKind::Add:
      case ParseNodeKind::Sub:
      case ParseNodeKind::Star:
      case ParseNodeKind::Div:
      case ParseNodeKind::Mod:
      case ParseNodeKind::Pow:
        MOZ_ASSERT(pn->isArity(PN_LIST));
        MOZ_ASSERT(pn->pn_count >= 2);
        *answer = true;
        return true;

      case ParseNodeKind::Colon:
      case ParseNodeKind::Case:
        MOZ_ASSERT(pn->isArity(PN_BINARY));
        if (!checkSideEffects(pn->pn_left, answer))
            return false;
        if (*answer)
            return true;
        return checkSideEffects(pn->pn_right, answer);

      // More getters.
      case ParseNodeKind::Elem:
        MOZ_ASSERT(pn->isArity(PN_BINARY));
        *answer = true;
        return true;

      // These affect visible names in this code, or in other code.
      case ParseNodeKind::Import:
      case ParseNodeKind::ExportFrom:
      case ParseNodeKind::ExportDefault:
        MOZ_ASSERT(pn->isArity(PN_BINARY));
        *answer = true;
        return true;

      // Likewise.
      case ParseNodeKind::Export:
        MOZ_ASSERT(pn->isArity(PN_UNARY));
        *answer = true;
        return true;

      // Every part of a loop might be effect-free, but looping infinitely *is*
      // an effect.  (Language lawyer trivia: C++ says threads can be assumed
      // to exit or have side effects, C++14 [intro.multithread]p27, so a C++
      // implementation's equivalent of the below could set |*answer = false;|
      // if all loop sub-nodes set |*answer = false|!)
      case ParseNodeKind::DoWhile:
      case ParseNodeKind::While:
      case ParseNodeKind::For:
        MOZ_ASSERT(pn->isArity(PN_BINARY));
        *answer = true;
        return true;

      // Declarations affect the name set of the relevant scope.
      case ParseNodeKind::Var:
      case ParseNodeKind::Const:
      case ParseNodeKind::Let:
        MOZ_ASSERT(pn->isArity(PN_LIST));
        *answer = true;
        return true;

      case ParseNodeKind::If:
      case ParseNodeKind::Conditional:
        MOZ_ASSERT(pn->isArity(PN_TERNARY));
        if (!checkSideEffects(pn->pn_kid1, answer))
            return false;
        if (*answer)
            return true;
        if (!checkSideEffects(pn->pn_kid2, answer))
            return false;
        if (*answer)
            return true;
        if ((pn = pn->pn_kid3))
            goto restart;
        return true;

      // Function calls can invoke non-local code.
      case ParseNodeKind::New:
      case ParseNodeKind::Call:
      case ParseNodeKind::TaggedTemplate:
      case ParseNodeKind::SuperCall:
        MOZ_ASSERT(pn->isArity(PN_LIST));
        *answer = true;
        return true;

      case ParseNodeKind::Pipeline:
        MOZ_ASSERT(pn->isArity(PN_LIST));
        MOZ_ASSERT(pn->pn_count >= 2);
        *answer = true;
        return true;

      // Classes typically introduce names.  Even if no name is introduced,
      // the heritage and/or class body (through computed property names)
      // usually have effects.
      case ParseNodeKind::Class:
        MOZ_ASSERT(pn->isArity(PN_TERNARY));
        *answer = true;
        return true;

      // |with| calls |ToObject| on its expression and so throws if that value
      // is null/undefined.
      case ParseNodeKind::With:
        MOZ_ASSERT(pn->isArity(PN_BINARY));
        *answer = true;
        return true;

      case ParseNodeKind::Return:
        MOZ_ASSERT(pn->isArity(PN_BINARY));
        *answer = true;
        return true;

      case ParseNodeKind::Name:
        MOZ_ASSERT(pn->isArity(PN_NAME));
        *answer = true;
        return true;

      // Shorthands could trigger getters: the |x| in the object literal in
      // |with ({ get x() { throw 42; } }) ({ x });|, for example, triggers
      // one.  (Of course, it isn't necessary to use |with| for a shorthand to
      // trigger a getter.)
      case ParseNodeKind::Shorthand:
        MOZ_ASSERT(pn->isArity(PN_BINARY));
        *answer = true;
        return true;

      case ParseNodeKind::Function:
        MOZ_ASSERT(pn->isArity(PN_CODE));
        /*
         * A named function, contrary to ES3, is no longer effectful, because
         * we bind its name lexically (using JSOP_CALLEE) instead of creating
         * an Object instance and binding a readonly, permanent property in it
         * (the object and binding can be detected and hijacked or captured).
         * This is a bug fix to ES3; it is fixed in ES3.1 drafts.
         */
        *answer = false;
        return true;

      case ParseNodeKind::Module:
        *answer = false;
        return true;

      case ParseNodeKind::Try:
        MOZ_ASSERT(pn->isArity(PN_TERNARY));
        if (!checkSideEffects(pn->pn_kid1, answer))
            return false;
        if (*answer)
            return true;
        if (ParseNode* catchScope = pn->pn_kid2) {
            MOZ_ASSERT(catchScope->isKind(ParseNodeKind::LexicalScope));
            if (!checkSideEffects(catchScope, answer))
                return false;
            if (*answer)
                return true;
        }
        if (ParseNode* finallyBlock = pn->pn_kid3) {
            if (!checkSideEffects(finallyBlock, answer))
                return false;
        }
        return true;

      case ParseNodeKind::Catch:
        MOZ_ASSERT(pn->isArity(PN_BINARY));
        if (ParseNode* name = pn->pn_left) {
            if (!checkSideEffects(name, answer))
                return false;
            if (*answer)
                return true;
        }
        return checkSideEffects(pn->pn_right, answer);

      case ParseNodeKind::Switch:
        MOZ_ASSERT(pn->isArity(PN_BINARY));
        if (!checkSideEffects(pn->pn_left, answer))
            return false;
        return *answer || checkSideEffects(pn->pn_right, answer);

      case ParseNodeKind::Label:
        MOZ_ASSERT(pn->isArity(PN_NAME));
        return checkSideEffects(pn->expr(), answer);

      case ParseNodeKind::LexicalScope:
        MOZ_ASSERT(pn->isArity(PN_SCOPE));
        return checkSideEffects(pn->scopeBody(), answer);

      // We could methodically check every interpolated expression, but it's
      // probably not worth the trouble.  Treat template strings as effect-free
      // only if they don't contain any substitutions.
      case ParseNodeKind::TemplateStringList:
        MOZ_ASSERT(pn->isArity(PN_LIST));
        MOZ_ASSERT(pn->pn_count > 0);
        MOZ_ASSERT((pn->pn_count % 2) == 1,
                   "template strings must alternate template and substitution "
                   "parts");
        *answer = pn->pn_count > 1;
        return true;

      // This should be unreachable but is left as-is for now.
      case ParseNodeKind::ParamsBody:
        *answer = true;
        return true;

      case ParseNodeKind::ForIn:           // by ParseNodeKind::For
      case ParseNodeKind::ForOf:           // by ParseNodeKind::For
      case ParseNodeKind::ForHead:         // by ParseNodeKind::For
      case ParseNodeKind::ClassMethod:     // by ParseNodeKind::Class
      case ParseNodeKind::ClassNames:      // by ParseNodeKind::Class
      case ParseNodeKind::ClassMethodList: // by ParseNodeKind::Class
      case ParseNodeKind::ImportSpecList: // by ParseNodeKind::Import
      case ParseNodeKind::ImportSpec:      // by ParseNodeKind::Import
      case ParseNodeKind::ExportBatchSpec:// by ParseNodeKind::Export
      case ParseNodeKind::ExportSpecList: // by ParseNodeKind::Export
      case ParseNodeKind::ExportSpec:      // by ParseNodeKind::Export
      case ParseNodeKind::CallSiteObj:      // by ParseNodeKind::TaggedTemplate
      case ParseNodeKind::PosHolder:        // by ParseNodeKind::NewTarget
      case ParseNodeKind::SuperBase:        // by ParseNodeKind::Elem and others
        MOZ_CRASH("handled by parent nodes");

      case ParseNodeKind::Limit: // invalid sentinel value
        MOZ_CRASH("invalid node kind");
    }

    MOZ_CRASH("invalid, unenumerated ParseNodeKind value encountered in "
              "BytecodeEmitter::checkSideEffects");
}

bool
BytecodeEmitter::isInLoop()
{
    return findInnermostNestableControl<LoopControl>();
}

bool
BytecodeEmitter::checkSingletonContext()
{
    if (!script->treatAsRunOnce() || sc->isFunctionBox() || isInLoop())
        return false;
    hasSingletons = true;
    return true;
}

bool
BytecodeEmitter::checkRunOnceContext()
{
    return checkSingletonContext() || (!isInLoop() && isRunOnceLambda());
}

bool
BytecodeEmitter::needsImplicitThis()
{
    // Short-circuit if there is an enclosing 'with' scope.
    if (sc->inWith())
        return true;

    // Otherwise see if the current point is under a 'with'.
    for (EmitterScope* es = innermostEmitterScope(); es; es = es->enclosingInFrame()) {
        if (es->scope(this)->kind() == ScopeKind::With)
            return true;
    }

    return false;
}

bool
BytecodeEmitter::maybeSetDisplayURL()
{
    if (tokenStream().hasDisplayURL()) {
        if (!parser.ss()->setDisplayURL(cx, tokenStream().displayURL()))
            return false;
    }
    return true;
}

bool
BytecodeEmitter::maybeSetSourceMap()
{
    if (tokenStream().hasSourceMapURL()) {
        MOZ_ASSERT(!parser.ss()->hasSourceMapURL());
        if (!parser.ss()->setSourceMapURL(cx, tokenStream().sourceMapURL()))
            return false;
    }

    /*
     * Source map URLs passed as a compile option (usually via a HTTP source map
     * header) override any source map urls passed as comment pragmas.
     */
    if (parser.options().sourceMapURL()) {
        // Warn about the replacement, but use the new one.
        if (parser.ss()->hasSourceMapURL()) {
            if (!parser.warningNoOffset(JSMSG_ALREADY_HAS_PRAGMA,
                                        parser.ss()->filename(), "//# sourceMappingURL"))
            {
                return false;
            }
        }

        if (!parser.ss()->setSourceMapURL(cx, parser.options().sourceMapURL()))
            return false;
    }

    return true;
}

void
BytecodeEmitter::tellDebuggerAboutCompiledScript(JSContext* cx)
{
    // Note: when parsing off thread the resulting scripts need to be handed to
    // the debugger after rejoining to the active thread.
    if (cx->helperThread())
        return;

    // Lazy scripts are never top level (despite always being invoked with a
    // nullptr parent), and so the hook should never be fired.
    if (emitterMode != LazyFunction && !parent)
        Debugger::onNewScript(cx, script);
}

inline TokenStreamAnyChars&
BytecodeEmitter::tokenStream()
{
    return parser.tokenStream();
}

void
BytecodeEmitter::reportError(ParseNode* pn, unsigned errorNumber, ...)
{
    TokenPos pos = pn ? pn->pn_pos : tokenStream().currentToken().pos;

    va_list args;
    va_start(args, errorNumber);

    ErrorMetadata metadata;
    if (parser.computeErrorMetadata(&metadata, pos.begin))
        ReportCompileError(cx, Move(metadata), nullptr, JSREPORT_ERROR, errorNumber, args);

    va_end(args);
}

bool
BytecodeEmitter::reportExtraWarning(ParseNode* pn, unsigned errorNumber, ...)
{
    TokenPos pos = pn ? pn->pn_pos : tokenStream().currentToken().pos;

    va_list args;
    va_start(args, errorNumber);

    bool result = parser.reportExtraWarningErrorNumberVA(nullptr, pos.begin, errorNumber, &args);

    va_end(args);
    return result;
}

bool
BytecodeEmitter::emitNewInit(JSProtoKey key)
{
    const size_t len = 1 + UINT32_INDEX_LEN;
    ptrdiff_t offset;
    if (!emitCheck(len, &offset))
        return false;

    jsbytecode* code = this->code(offset);
    code[0] = JSOP_NEWINIT;
    code[1] = jsbytecode(key);
    code[2] = 0;
    code[3] = 0;
    code[4] = 0;
    checkTypeSet(JSOP_NEWINIT);
    updateDepth(offset);
    return true;
}

bool
BytecodeEmitter::iteratorResultShape(unsigned* shape)
{
    // No need to do any guessing for the object kind, since we know exactly how
    // many properties we plan to have.
    gc::AllocKind kind = gc::GetGCObjectKind(2);
    RootedPlainObject obj(cx, NewBuiltinClassInstance<PlainObject>(cx, kind, TenuredObject));
    if (!obj)
        return false;

    Rooted<jsid> value_id(cx, NameToId(cx->names().value));
    Rooted<jsid> done_id(cx, NameToId(cx->names().done));
    if (!NativeDefineDataProperty(cx, obj, value_id, UndefinedHandleValue, JSPROP_ENUMERATE))
        return false;
    if (!NativeDefineDataProperty(cx, obj, done_id, UndefinedHandleValue, JSPROP_ENUMERATE))
        return false;

    ObjectBox* objbox = parser.newObjectBox(obj);
    if (!objbox)
        return false;

    *shape = objectList.add(objbox);

    return true;
}

bool
BytecodeEmitter::emitPrepareIteratorResult()
{
    unsigned shape;
    if (!iteratorResultShape(&shape))
        return false;
    return emitIndex32(JSOP_NEWOBJECT, shape);
}

bool
BytecodeEmitter::emitFinishIteratorResult(bool done)
{
    uint32_t value_id;
    if (!makeAtomIndex(cx->names().value, &value_id))
        return false;
    uint32_t done_id;
    if (!makeAtomIndex(cx->names().done, &done_id))
        return false;

    if (!emitIndex32(JSOP_INITPROP, value_id))
        return false;
    if (!emit1(done ? JSOP_TRUE : JSOP_FALSE))
        return false;
    if (!emitIndex32(JSOP_INITPROP, done_id))
        return false;
    return true;
}

bool
BytecodeEmitter::emitGetNameAtLocation(JSAtom* name, const NameLocation& loc, bool callContext)
{
    switch (loc.kind()) {
      case NameLocation::Kind::Dynamic:
        if (!emitAtomOp(name, JSOP_GETNAME))
            return false;
        break;

      case NameLocation::Kind::Global:
        if (!emitAtomOp(name, JSOP_GETGNAME))
            return false;
        break;

      case NameLocation::Kind::Intrinsic:
        if (!emitAtomOp(name, JSOP_GETINTRINSIC))
            return false;
        break;

      case NameLocation::Kind::NamedLambdaCallee:
        if (!emit1(JSOP_CALLEE))
            return false;
        break;

      case NameLocation::Kind::Import:
        if (!emitAtomOp(name, JSOP_GETIMPORT))
            return false;
        break;

      case NameLocation::Kind::ArgumentSlot:
        if (!emitArgOp(JSOP_GETARG, loc.argumentSlot()))
            return false;
        break;

      case NameLocation::Kind::FrameSlot:
        if (loc.isLexical()) {
            if (!emitTDZCheckIfNeeded(name, loc))
                return false;
        }
        if (!emitLocalOp(JSOP_GETLOCAL, loc.frameSlot()))
            return false;
        break;

      case NameLocation::Kind::EnvironmentCoordinate:
        if (loc.isLexical()) {
            if (!emitTDZCheckIfNeeded(name, loc))
                return false;
        }
        if (!emitEnvCoordOp(JSOP_GETALIASEDVAR, loc.environmentCoordinate()))
            return false;
        break;

      case NameLocation::Kind::DynamicAnnexBVar:
        MOZ_CRASH("Synthesized vars for Annex B.3.3 should only be used in initialization");
    }

    // Need to provide |this| value for call.
    if (callContext) {
        switch (loc.kind()) {
          case NameLocation::Kind::Dynamic: {
            JSOp thisOp = needsImplicitThis() ? JSOP_IMPLICITTHIS : JSOP_GIMPLICITTHIS;
            if (!emitAtomOp(name, thisOp))
                return false;
            break;
          }

          case NameLocation::Kind::Global:
            if (!emitAtomOp(name, JSOP_GIMPLICITTHIS))
                return false;
            break;

          case NameLocation::Kind::Intrinsic:
          case NameLocation::Kind::NamedLambdaCallee:
          case NameLocation::Kind::Import:
          case NameLocation::Kind::ArgumentSlot:
          case NameLocation::Kind::FrameSlot:
          case NameLocation::Kind::EnvironmentCoordinate:
            if (!emit1(JSOP_UNDEFINED))
                return false;
            break;

          case NameLocation::Kind::DynamicAnnexBVar:
            MOZ_CRASH("Synthesized vars for Annex B.3.3 should only be used in initialization");
        }
    }

    return true;
}

bool
BytecodeEmitter::emitGetName(ParseNode* pn, bool callContext)
{
    return emitGetName(pn->name(), callContext);
}

template <typename RHSEmitter>
bool
BytecodeEmitter::emitSetOrInitializeNameAtLocation(HandleAtom name, const NameLocation& loc,
                                                   RHSEmitter emitRhs, bool initialize)
{
    bool emittedBindOp = false;

    switch (loc.kind()) {
      case NameLocation::Kind::Dynamic:
      case NameLocation::Kind::Import:
      case NameLocation::Kind::DynamicAnnexBVar: {
        uint32_t atomIndex;
        if (!makeAtomIndex(name, &atomIndex))
            return false;
        if (loc.kind() == NameLocation::Kind::DynamicAnnexBVar) {
            // Annex B vars always go on the nearest variable environment,
            // even if lexical environments in between contain same-named
            // bindings.
            if (!emit1(JSOP_BINDVAR))
                return false;
        } else {
            if (!emitIndexOp(JSOP_BINDNAME, atomIndex))
                return false;
        }
        emittedBindOp = true;
        if (!emitRhs(this, loc, emittedBindOp))
            return false;
        if (!emitIndexOp(strictifySetNameOp(JSOP_SETNAME), atomIndex))
            return false;
        break;
      }

      case NameLocation::Kind::Global: {
        JSOp op;
        uint32_t atomIndex;
        if (!makeAtomIndex(name, &atomIndex))
            return false;
        if (loc.isLexical() && initialize) {
            // INITGLEXICAL always gets the global lexical scope. It doesn't
            // need a BINDGNAME.
            MOZ_ASSERT(innermostScope()->is<GlobalScope>());
            op = JSOP_INITGLEXICAL;
        } else {
            if (!emitIndexOp(JSOP_BINDGNAME, atomIndex))
                return false;
            emittedBindOp = true;
            op = strictifySetNameOp(JSOP_SETGNAME);
        }
        if (!emitRhs(this, loc, emittedBindOp))
            return false;
        if (!emitIndexOp(op, atomIndex))
            return false;
        break;
      }

      case NameLocation::Kind::Intrinsic:
        if (!emitRhs(this, loc, emittedBindOp))
            return false;
        if (!emitAtomOp(name, JSOP_SETINTRINSIC))
            return false;
        break;

      case NameLocation::Kind::NamedLambdaCallee:
        if (!emitRhs(this, loc, emittedBindOp))
            return false;
        // Assigning to the named lambda is a no-op in sloppy mode but
        // throws in strict mode.
        if (sc->strict() && !emit1(JSOP_THROWSETCALLEE))
            return false;
        break;

      case NameLocation::Kind::ArgumentSlot: {
        // If we assign to a positional formal parameter and the arguments
        // object is unmapped (strict mode or function with
        // default/rest/destructing args), parameters do not alias
        // arguments[i], and to make the arguments object reflect initial
        // parameter values prior to any mutation we create it eagerly
        // whenever parameters are (or might, in the case of calls to eval)
        // assigned.
        FunctionBox* funbox = sc->asFunctionBox();
        if (funbox->argumentsHasLocalBinding() && !funbox->hasMappedArgsObj())
            funbox->setDefinitelyNeedsArgsObj();

        if (!emitRhs(this, loc, emittedBindOp))
            return false;
        if (!emitArgOp(JSOP_SETARG, loc.argumentSlot()))
            return false;
        break;
      }

      case NameLocation::Kind::FrameSlot: {
        JSOp op = JSOP_SETLOCAL;
        if (!emitRhs(this, loc, emittedBindOp))
            return false;
        if (loc.isLexical()) {
            if (initialize) {
                op = JSOP_INITLEXICAL;
            } else {
                if (loc.isConst())
                    op = JSOP_THROWSETCONST;

                if (!emitTDZCheckIfNeeded(name, loc))
                    return false;
            }
        }
        if (!emitLocalOp(op, loc.frameSlot()))
            return false;
        if (op == JSOP_INITLEXICAL) {
            if (!innermostTDZCheckCache->noteTDZCheck(this, name, DontCheckTDZ))
                return false;
        }
        break;
      }

      case NameLocation::Kind::EnvironmentCoordinate: {
        JSOp op = JSOP_SETALIASEDVAR;
        if (!emitRhs(this, loc, emittedBindOp))
            return false;
        if (loc.isLexical()) {
            if (initialize) {
                op = JSOP_INITALIASEDLEXICAL;
            } else {
                if (loc.isConst())
                    op = JSOP_THROWSETALIASEDCONST;

                if (!emitTDZCheckIfNeeded(name, loc))
                    return false;
            }
        }
        if (loc.bindingKind() == BindingKind::NamedLambdaCallee) {
            // Assigning to the named lambda is a no-op in sloppy mode and throws
            // in strict mode.
            op = JSOP_THROWSETALIASEDCONST;
            if (sc->strict() && !emitEnvCoordOp(op, loc.environmentCoordinate()))
                return false;
        } else {
            if (!emitEnvCoordOp(op, loc.environmentCoordinate()))
                return false;
        }
        if (op == JSOP_INITALIASEDLEXICAL) {
            if (!innermostTDZCheckCache->noteTDZCheck(this, name, DontCheckTDZ))
                return false;
        }
        break;
      }
    }

    return true;
}

bool
BytecodeEmitter::emitTDZCheckIfNeeded(JSAtom* name, const NameLocation& loc)
{
    // Dynamic accesses have TDZ checks built into their VM code and should
    // never emit explicit TDZ checks.
    MOZ_ASSERT(loc.hasKnownSlot());
    MOZ_ASSERT(loc.isLexical());

    Maybe<MaybeCheckTDZ> check = innermostTDZCheckCache->needsTDZCheck(this, name);
    if (!check)
        return false;

    // We've already emitted a check in this basic block.
    if (*check == DontCheckTDZ)
        return true;

    if (loc.kind() == NameLocation::Kind::FrameSlot) {
        if (!emitLocalOp(JSOP_CHECKLEXICAL, loc.frameSlot()))
            return false;
    } else {
        if (!emitEnvCoordOp(JSOP_CHECKALIASEDLEXICAL, loc.environmentCoordinate()))
            return false;
    }

    return innermostTDZCheckCache->noteTDZCheck(this, name, DontCheckTDZ);
}

bool
BytecodeEmitter::emitPropLHS(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(ParseNodeKind::Dot));
    MOZ_ASSERT(!pn->as<PropertyAccess>().isSuper());

    ParseNode* pn2 = pn->pn_expr;

    /*
     * If the object operand is also a dotted property reference, reverse the
     * list linked via pn_expr temporarily so we can iterate over it from the
     * bottom up (reversing again as we go), to avoid excessive recursion.
     */
    if (pn2->isKind(ParseNodeKind::Dot) && !pn2->as<PropertyAccess>().isSuper()) {
        ParseNode* pndot = pn2;
        ParseNode* pnup = nullptr;
        ParseNode* pndown;
        for (;;) {
            /* Reverse pndot->pn_expr to point up, not down. */
            pndown = pndot->pn_expr;
            pndot->pn_expr = pnup;
            if (!pndown->isKind(ParseNodeKind::Dot) || pndown->as<PropertyAccess>().isSuper())
                break;
            pnup = pndot;
            pndot = pndown;
        }

        /* pndown is a primary expression, not a dotted property reference. */
        if (!emitTree(pndown))
            return false;

        do {
            /* Walk back up the list, emitting annotated name ops. */
            if (!emitAtomOp(pndot, JSOP_GETPROP))
                return false;

            /* Reverse the pn_expr link again. */
            pnup = pndot->pn_expr;
            pndot->pn_expr = pndown;
            pndown = pndot;
        } while ((pndot = pnup) != nullptr);
        return true;
    }

    // The non-optimized case.
    return emitTree(pn2);
}

bool
BytecodeEmitter::emitSuperPropLHS(ParseNode* superBase, bool isCall)
{
    if (!emitGetThisForSuperBase(superBase))
        return false;
    if (isCall && !emit1(JSOP_DUP))
        return false;
    if (!emit1(JSOP_SUPERBASE))
        return false;
    return true;
}

bool
BytecodeEmitter::emitPropOp(ParseNode* pn, JSOp op)
{
    MOZ_ASSERT(pn->isArity(PN_NAME));

    if (!emitPropLHS(pn))
        return false;

    if (op == JSOP_CALLPROP && !emit1(JSOP_DUP))
        return false;

    if (!emitAtomOp(pn, op))
        return false;

    if (op == JSOP_CALLPROP && !emit1(JSOP_SWAP))
        return false;

    return true;
}

bool
BytecodeEmitter::emitSuperPropOp(ParseNode* pn, JSOp op, bool isCall)
{
    ParseNode* base = &pn->as<PropertyAccess>().expression();
    if (!emitSuperPropLHS(base, isCall))
        return false;

    if (!emitAtomOp(pn, op))
        return false;

    if (isCall && !emit1(JSOP_SWAP))
        return false;

    return true;
}

bool
BytecodeEmitter::emitPropIncDec(ParseNode* pn)
{
    MOZ_ASSERT(pn->pn_kid->isKind(ParseNodeKind::Dot));

    bool post;
    bool isSuper = pn->pn_kid->as<PropertyAccess>().isSuper();
    JSOp binop = GetIncDecInfo(pn->getKind(), &post);

    if (isSuper) {
        ParseNode* base = &pn->pn_kid->as<PropertyAccess>().expression();
        if (!emitSuperPropLHS(base))                // THIS OBJ
            return false;
        if (!emit1(JSOP_DUP2))                      // THIS OBJ THIS OBJ
            return false;
    } else {
        if (!emitPropLHS(pn->pn_kid))               // OBJ
            return false;
        if (!emit1(JSOP_DUP))                       // OBJ OBJ
            return false;
    }
    if (!emitAtomOp(pn->pn_kid, isSuper? JSOP_GETPROP_SUPER : JSOP_GETPROP)) // OBJ V
        return false;
    if (!emit1(JSOP_POS))                           // OBJ N
        return false;
    if (post && !emit1(JSOP_DUP))                   // OBJ N? N
        return false;
    if (!emit1(JSOP_ONE))                           // OBJ N? N 1
        return false;
    if (!emit1(binop))                              // OBJ N? N+1
        return false;

    if (post) {
        if (!emit2(JSOP_PICK, 2 + isSuper))        // N? N+1 OBJ
            return false;
        if (!emit1(JSOP_SWAP))                     // N? OBJ N+1
            return false;
        if (isSuper) {
            if (!emit2(JSOP_PICK, 3))              // N THIS N+1 OBJ
                return false;
            if (!emit1(JSOP_SWAP))                 // N THIS OBJ N+1
                return false;
        }
    }

    JSOp setOp = isSuper ? sc->strict() ? JSOP_STRICTSETPROP_SUPER : JSOP_SETPROP_SUPER
                         : sc->strict() ? JSOP_STRICTSETPROP : JSOP_SETPROP;
    if (!emitAtomOp(pn->pn_kid, setOp))             // N? N+1
        return false;
    if (post && !emit1(JSOP_POP))                   // RESULT
        return false;

    return true;
}

bool
BytecodeEmitter::emitGetNameAtLocationForCompoundAssignment(JSAtom* name, const NameLocation& loc)
{
    if (loc.kind() == NameLocation::Kind::Dynamic) {
        // For dynamic accesses we need to emit GETBOUNDNAME instead of
        // GETNAME for correctness: looking up @@unscopables on the
        // environment chain (due to 'with' environments) must only happen
        // once.
        //
        // GETBOUNDNAME uses the environment already pushed on the stack from
        // the earlier BINDNAME.
        if (!emit1(JSOP_DUP))                              // ENV ENV
            return false;
        if (!emitAtomOp(name, JSOP_GETBOUNDNAME))          // ENV V
            return false;
    } else {
        if (!emitGetNameAtLocation(name, loc))             // ENV? V
            return false;
    }

    return true;
}

bool
BytecodeEmitter::emitNameIncDec(ParseNode* pn)
{
    MOZ_ASSERT(pn->pn_kid->isKind(ParseNodeKind::Name));

    bool post;
    JSOp binop = GetIncDecInfo(pn->getKind(), &post);

    auto emitRhs = [pn, post, binop](BytecodeEmitter* bce, const NameLocation& loc,
                                     bool emittedBindOp)
    {
        JSAtom* name = pn->pn_kid->name();
        if (!bce->emitGetNameAtLocationForCompoundAssignment(name, loc)) // ENV? V
            return false;
        if (!bce->emit1(JSOP_POS))                         // ENV? N
            return false;
        if (post && !bce->emit1(JSOP_DUP))                 // ENV? N? N
            return false;
        if (!bce->emit1(JSOP_ONE))                         // ENV? N? N 1
            return false;
        if (!bce->emit1(binop))                            // ENV? N? N+1
            return false;

        if (post && emittedBindOp) {
            if (!bce->emit2(JSOP_PICK, 2))                 // N? N+1 ENV?
                return false;
            if (!bce->emit1(JSOP_SWAP))                    // N? ENV? N+1
                return false;
        }

        return true;
    };

    if (!emitSetName(pn->pn_kid, emitRhs))
        return false;

    if (post && !emit1(JSOP_POP))
        return false;

    return true;
}

bool
BytecodeEmitter::emitElemOperands(ParseNode* pn, EmitElemOption opts)
{
    MOZ_ASSERT(pn->isArity(PN_BINARY));

    if (!emitTree(pn->pn_left))
        return false;

    if (opts == EmitElemOption::IncDec) {
        if (!emit1(JSOP_CHECKOBJCOERCIBLE))
            return false;
    } else if (opts == EmitElemOption::Call) {
        if (!emit1(JSOP_DUP))
            return false;
    }

    if (!emitTree(pn->pn_right))
        return false;

    if (opts == EmitElemOption::Set) {
        if (!emit2(JSOP_PICK, 2))
            return false;
    } else if (opts == EmitElemOption::IncDec || opts == EmitElemOption::CompoundAssign) {
        if (!emit1(JSOP_TOID))
            return false;
    }
    return true;
}

bool
BytecodeEmitter::emitSuperElemOperands(ParseNode* pn, EmitElemOption opts)
{
    MOZ_ASSERT(pn->isKind(ParseNodeKind::Elem) && pn->as<PropertyByValue>().isSuper());

    // The ordering here is somewhat screwy. We need to evaluate the propval
    // first, by spec. Do a little dance to not emit more than one JSOP_THIS.
    // Since JSOP_THIS might throw in derived class constructors, we cannot
    // just push it earlier as the receiver. We have to swap it down instead.

    if (!emitTree(pn->pn_right))
        return false;

    // We need to convert the key to an object id first, so that we do not do
    // it inside both the GETELEM and the SETELEM.
    if (opts == EmitElemOption::IncDec || opts == EmitElemOption::CompoundAssign) {
        if (!emit1(JSOP_TOID))
            return false;
    }

    if (!emitGetThisForSuperBase(pn->pn_left))
        return false;

    if (opts == EmitElemOption::Call) {
        if (!emit1(JSOP_SWAP))
            return false;

        // We need another |this| on top, also
        if (!emitDupAt(1))
            return false;
    }

    if (!emit1(JSOP_SUPERBASE))
        return false;

    if (opts == EmitElemOption::Set && !emit2(JSOP_PICK, 3))
        return false;

    return true;
}

bool
BytecodeEmitter::emitElemOpBase(JSOp op)
{
    if (!emit1(op))
        return false;

    checkTypeSet(op);
    return true;
}

bool
BytecodeEmitter::emitElemOp(ParseNode* pn, JSOp op)
{
    EmitElemOption opts = EmitElemOption::Get;
    if (op == JSOP_CALLELEM)
        opts = EmitElemOption::Call;
    else if (op == JSOP_SETELEM || op == JSOP_STRICTSETELEM)
        opts = EmitElemOption::Set;

    return emitElemOperands(pn, opts) && emitElemOpBase(op);
}

bool
BytecodeEmitter::emitSuperElemOp(ParseNode* pn, JSOp op, bool isCall)
{
    EmitElemOption opts = EmitElemOption::Get;
    if (isCall)
        opts = EmitElemOption::Call;
    else if (op == JSOP_SETELEM_SUPER || op == JSOP_STRICTSETELEM_SUPER)
        opts = EmitElemOption::Set;

    if (!emitSuperElemOperands(pn, opts))
        return false;
    if (!emitElemOpBase(op))
        return false;

    if (isCall && !emit1(JSOP_SWAP))
        return false;

    return true;
}

bool
BytecodeEmitter::emitElemIncDec(ParseNode* pn)
{
    MOZ_ASSERT(pn->pn_kid->isKind(ParseNodeKind::Elem));

    bool isSuper = pn->pn_kid->as<PropertyByValue>().isSuper();

    // We need to convert the key to an object id first, so that we do not do
    // it inside both the GETELEM and the SETELEM. This is done by
    // emit(Super)ElemOperands.
    if (isSuper) {
        if (!emitSuperElemOperands(pn->pn_kid, EmitElemOption::IncDec))
            return false;
    } else {
        if (!emitElemOperands(pn->pn_kid, EmitElemOption::IncDec))
            return false;
    }

    bool post;
    JSOp binop = GetIncDecInfo(pn->getKind(), &post);

    JSOp getOp;
    if (isSuper) {
        // There's no such thing as JSOP_DUP3, so we have to be creative.
        // Note that pushing things again is no fewer JSOps.
        if (!emitDupAt(2))                              // KEY THIS OBJ KEY
            return false;
        if (!emitDupAt(2))                              // KEY THIS OBJ KEY THIS
            return false;
        if (!emitDupAt(2))                              // KEY THIS OBJ KEY THIS OBJ
            return false;
        getOp = JSOP_GETELEM_SUPER;
    } else {
                                                        // OBJ KEY
        if (!emit1(JSOP_DUP2))                          // OBJ KEY OBJ KEY
            return false;
        getOp = JSOP_GETELEM;
    }
    if (!emitElemOpBase(getOp))                         // OBJ KEY V
        return false;
    if (!emit1(JSOP_POS))                               // OBJ KEY N
        return false;
    if (post && !emit1(JSOP_DUP))                       // OBJ KEY N? N
        return false;
    if (!emit1(JSOP_ONE))                               // OBJ KEY N? N 1
        return false;
    if (!emit1(binop))                                  // OBJ KEY N? N+1
        return false;

    if (post) {
        if (isSuper) {
            // We have one more value to rotate around, because of |this|
            // on the stack
            if (!emit2(JSOP_PICK, 4))
                return false;
        }
        if (!emit2(JSOP_PICK, 3 + isSuper))             // KEY N N+1 OBJ
            return false;
        if (!emit2(JSOP_PICK, 3 + isSuper))             // N N+1 OBJ KEY
            return false;
        if (!emit2(JSOP_PICK, 2 + isSuper))             // N OBJ KEY N+1
            return false;
    }

    JSOp setOp = isSuper ? (sc->strict() ? JSOP_STRICTSETELEM_SUPER : JSOP_SETELEM_SUPER)
                         : (sc->strict() ? JSOP_STRICTSETELEM : JSOP_SETELEM);
    if (!emitElemOpBase(setOp))                         // N? N+1
        return false;
    if (post && !emit1(JSOP_POP))                       // RESULT
        return false;

    return true;
}

bool
BytecodeEmitter::emitCallIncDec(ParseNode* incDec)
{
    MOZ_ASSERT(incDec->isKind(ParseNodeKind::PreIncrement) ||
               incDec->isKind(ParseNodeKind::PostIncrement) ||
               incDec->isKind(ParseNodeKind::PreDecrement) ||
               incDec->isKind(ParseNodeKind::PostDecrement));

    MOZ_ASSERT(incDec->pn_kid->isKind(ParseNodeKind::Call));

    ParseNode* call = incDec->pn_kid;
    if (!emitTree(call))                                // CALLRESULT
        return false;
    if (!emit1(JSOP_POS))                               // N
        return false;

    // The increment/decrement has no side effects, so proceed to throw for
    // invalid assignment target.
    return emitUint16Operand(JSOP_THROWMSG, JSMSG_BAD_LEFTSIDE_OF_ASS);
}

bool
BytecodeEmitter::emitNumberOp(double dval)
{
    int32_t ival;
    if (NumberIsInt32(dval, &ival)) {
        if (ival == 0)
            return emit1(JSOP_ZERO);
        if (ival == 1)
            return emit1(JSOP_ONE);
        if ((int)(int8_t)ival == ival)
            return emit2(JSOP_INT8, uint8_t(int8_t(ival)));

        uint32_t u = uint32_t(ival);
        if (u < JS_BIT(16)) {
            if (!emitUint16Operand(JSOP_UINT16, u))
                return false;
        } else if (u < JS_BIT(24)) {
            ptrdiff_t off;
            if (!emitN(JSOP_UINT24, 3, &off))
                return false;
            SET_UINT24(code(off), u);
        } else {
            ptrdiff_t off;
            if (!emitN(JSOP_INT32, 4, &off))
                return false;
            SET_INT32(code(off), ival);
        }
        return true;
    }

    if (!constList.append(DoubleValue(dval)))
        return false;

    return emitIndex32(JSOP_DOUBLE, constList.length() - 1);
}

/*
 * Using MOZ_NEVER_INLINE in here is a workaround for llvm.org/pr14047.
 * LLVM is deciding to inline this function which uses a lot of stack space
 * into emitTree which is recursive and uses relatively little stack space.
 */
MOZ_NEVER_INLINE bool
BytecodeEmitter::emitSwitch(ParseNode* pn)
{
    ParseNode* cases = pn->pn_right;
    MOZ_ASSERT(cases->isKind(ParseNodeKind::LexicalScope) ||
               cases->isKind(ParseNodeKind::StatementList));

    // Emit code for the discriminant.
    if (!emitTree(pn->pn_left))
        return false;

    // Enter the scope before pushing the switch BreakableControl since all
    // breaks are under this scope.
    Maybe<TDZCheckCache> tdzCache;
    Maybe<EmitterScope> emitterScope;
    if (cases->isKind(ParseNodeKind::LexicalScope)) {
        if (!cases->isEmptyScope()) {
            tdzCache.emplace(this);
            emitterScope.emplace(this);
            if (!emitterScope->enterLexical(this, ScopeKind::Lexical, cases->scopeBindings()))
                return false;
        }

        // Advance |cases| to refer to the switch case list.
        cases = cases->scopeBody();

        // A switch statement may contain hoisted functions inside its
        // cases. The PNX_FUNCDEFS flag is propagated from the STATEMENTLIST
        // bodies of the cases to the case list.
        if (cases->pn_xflags & PNX_FUNCDEFS) {
            MOZ_ASSERT(emitterScope);
            for (ParseNode* caseNode = cases->pn_head; caseNode; caseNode = caseNode->pn_next) {
                if (caseNode->pn_right->pn_xflags & PNX_FUNCDEFS) {
                    if (!emitHoistedFunctionsInList(caseNode->pn_right))
                        return false;
                }
            }
        }
    }

    // After entering the scope, push the switch control.
    BreakableControl controlInfo(this, StatementKind::Switch);

    ptrdiff_t top = offset();

    // Switch bytecodes run from here till end of final case.
    uint32_t caseCount = cases->pn_count;
    if (caseCount > JS_BIT(16)) {
        parser.reportError(JSMSG_TOO_MANY_CASES);
        return false;
    }

    // Try for most optimal, fall back if not dense ints.
    JSOp switchOp = JSOP_TABLESWITCH;
    uint32_t tableLength = 0;
    int32_t low, high;
    bool hasDefault = false;
    CaseClause* firstCase = cases->pn_head ? &cases->pn_head->as<CaseClause>() : nullptr;
    if (caseCount == 0 ||
        (caseCount == 1 && (hasDefault = firstCase->isDefault())))
    {
        caseCount = 0;
        low = 0;
        high = -1;
    } else {
        Vector<jsbitmap, 128, SystemAllocPolicy> intmap;
        int32_t intmapBitLength = 0;

        low  = JSVAL_INT_MAX;
        high = JSVAL_INT_MIN;

        for (CaseClause* caseNode = firstCase; caseNode; caseNode = caseNode->next()) {
            if (caseNode->isDefault()) {
                hasDefault = true;
                caseCount--;  // one of the "cases" was the default
                continue;
            }

            if (switchOp == JSOP_CONDSWITCH)
                continue;

            MOZ_ASSERT(switchOp == JSOP_TABLESWITCH);

            ParseNode* caseValue = caseNode->caseExpression();

            if (caseValue->getKind() != ParseNodeKind::Number) {
                switchOp = JSOP_CONDSWITCH;
                continue;
            }

            int32_t i;
            if (!NumberIsInt32(caseValue->pn_dval, &i)) {
                switchOp = JSOP_CONDSWITCH;
                continue;
            }

            if (unsigned(i + int(JS_BIT(15))) >= unsigned(JS_BIT(16))) {
                switchOp = JSOP_CONDSWITCH;
                continue;
            }
            if (i < low)
                low = i;
            if (i > high)
                high = i;

            // Check for duplicates, which require a JSOP_CONDSWITCH.
            // We bias i by 65536 if it's negative, and hope that's a rare
            // case (because it requires a malloc'd bitmap).
            if (i < 0)
                i += JS_BIT(16);
            if (i >= intmapBitLength) {
                size_t newLength = (i / JS_BITMAP_NBITS) + 1;
                if (!intmap.resize(newLength))
                    return false;
                intmapBitLength = newLength * JS_BITMAP_NBITS;
            }
            if (JS_TEST_BIT(intmap, i)) {
                switchOp = JSOP_CONDSWITCH;
                continue;
            }
            JS_SET_BIT(intmap, i);
        }

        // Compute table length and select condswitch instead if overlarge or
        // more than half-sparse.
        if (switchOp == JSOP_TABLESWITCH) {
            tableLength = uint32_t(high - low + 1);
            if (tableLength >= JS_BIT(16) || tableLength > 2 * caseCount)
                switchOp = JSOP_CONDSWITCH;
        }
    }

    // The note has one or two offsets: first tells total switch code length;
    // second (if condswitch) tells offset to first JSOP_CASE.
    unsigned noteIndex;
    size_t switchSize;
    if (switchOp == JSOP_CONDSWITCH) {
        // 0 bytes of immediate for unoptimized switch.
        switchSize = 0;
        if (!newSrcNote3(SRC_CONDSWITCH, 0, 0, &noteIndex))
            return false;
    } else {
        MOZ_ASSERT(switchOp == JSOP_TABLESWITCH);

        // 3 offsets (len, low, high) before the table, 1 per entry.
        switchSize = size_t(JUMP_OFFSET_LEN * (3 + tableLength));
        if (!newSrcNote2(SRC_TABLESWITCH, 0, &noteIndex))
            return false;
    }

    // Emit switchOp followed by switchSize bytes of jump or lookup table.
    MOZ_ASSERT(top == offset());
    if (!emitN(switchOp, switchSize))
        return false;

    Vector<CaseClause*, 32, SystemAllocPolicy> table;

    JumpList condSwitchDefaultOff;
    if (switchOp == JSOP_CONDSWITCH) {
        unsigned caseNoteIndex;
        bool beforeCases = true;
        ptrdiff_t lastCaseOffset = -1;

        // The case conditions need their own TDZ cache since they might not
        // all execute.
        TDZCheckCache tdzCache(this);

        // Emit code for evaluating cases and jumping to case statements.
        for (CaseClause* caseNode = firstCase; caseNode; caseNode = caseNode->next()) {
            ParseNode* caseValue = caseNode->caseExpression();

            // If the expression is a literal, suppress line number emission so
            // that debugging works more naturally.
            if (caseValue) {
                if (!emitTree(caseValue, ValueUsage::WantValue,
                              caseValue->isLiteral() ? SUPPRESS_LINENOTE : EMIT_LINENOTE))
                {
                    return false;
                }
            }

            if (!beforeCases) {
                // prevCase is the previous JSOP_CASE's bytecode offset.
                if (!setSrcNoteOffset(caseNoteIndex, 0, offset() - lastCaseOffset))
                    return false;
            }
            if (!caseValue) {
                // This is the default clause.
                continue;
            }

            if (!newSrcNote2(SRC_NEXTCASE, 0, &caseNoteIndex))
                return false;

            // The case clauses are produced before any of the case body. The
            // JumpList is saved on the parsed tree, then later restored and
            // patched when generating the cases body.
            JumpList caseJump;
            if (!emitJump(JSOP_CASE, &caseJump))
                return false;
            caseNode->setOffset(caseJump.offset);
            lastCaseOffset = caseJump.offset;

            if (beforeCases) {
                // Switch note's second offset is to first JSOP_CASE.
                unsigned noteCount = notes().length();
                if (!setSrcNoteOffset(noteIndex, 1, lastCaseOffset - top))
                    return false;
                unsigned noteCountDelta = notes().length() - noteCount;
                if (noteCountDelta != 0)
                    caseNoteIndex += noteCountDelta;
                beforeCases = false;
            }
        }

        // If we didn't have an explicit default (which could fall in between
        // cases, preventing us from fusing this setSrcNoteOffset with the call
        // in the loop above), link the last case to the implicit default for
        // the benefit of IonBuilder.
        if (!hasDefault &&
            !beforeCases &&
            !setSrcNoteOffset(caseNoteIndex, 0, offset() - lastCaseOffset))
        {
            return false;
        }

        // Emit default even if no explicit default statement.
        if (!emitJump(JSOP_DEFAULT, &condSwitchDefaultOff))
            return false;
    } else {
        MOZ_ASSERT(switchOp == JSOP_TABLESWITCH);

        // skip default offset.
        jsbytecode* pc = code(top + JUMP_OFFSET_LEN);

        // Fill in switch bounds, which we know fit in 16-bit offsets.
        SET_JUMP_OFFSET(pc, low);
        pc += JUMP_OFFSET_LEN;
        SET_JUMP_OFFSET(pc, high);
        pc += JUMP_OFFSET_LEN;

        if (tableLength != 0) {
            if (!table.growBy(tableLength))
                return false;

            for (CaseClause* caseNode = firstCase; caseNode; caseNode = caseNode->next()) {
                if (ParseNode* caseValue = caseNode->caseExpression()) {
                    MOZ_ASSERT(caseValue->isKind(ParseNodeKind::Number));

                    int32_t i = int32_t(caseValue->pn_dval);
                    MOZ_ASSERT(double(i) == caseValue->pn_dval);

                    i -= low;
                    MOZ_ASSERT(uint32_t(i) < tableLength);
                    MOZ_ASSERT(!table[i]);
                    table[i] = caseNode;
                }
            }
        }
    }

    JumpTarget defaultOffset{ -1 };

    // Emit code for each case's statements.
    for (CaseClause* caseNode = firstCase; caseNode; caseNode = caseNode->next()) {
        if (switchOp == JSOP_CONDSWITCH && !caseNode->isDefault()) {
            // The case offset got saved in the caseNode structure after
            // emitting the JSOP_CASE jump instruction above.
            JumpList caseCond;
            caseCond.offset = caseNode->offset();
            if (!emitJumpTargetAndPatch(caseCond))
                return false;
        }

        JumpTarget here;
        if (!emitJumpTarget(&here))
            return false;
        if (caseNode->isDefault())
            defaultOffset = here;

        // If this is emitted as a TABLESWITCH, we'll need to know this case's
        // offset later when emitting the table. Store it in the node's
        // pn_offset (giving the field a different meaning vs. how we used it
        // on the immediately preceding line of code).
        caseNode->setOffset(here.offset);

        TDZCheckCache tdzCache(this);

        if (!emitTree(caseNode->statementList()))
            return false;
    }

    if (!hasDefault) {
        // If no default case, offset for default is to end of switch.
        if (!emitJumpTarget(&defaultOffset))
            return false;
    }
    MOZ_ASSERT(defaultOffset.offset != -1);

    // Set the default offset (to end of switch if no default).
    jsbytecode* pc;
    if (switchOp == JSOP_CONDSWITCH) {
        pc = nullptr;
        patchJumpsToTarget(condSwitchDefaultOff, defaultOffset);
    } else {
        MOZ_ASSERT(switchOp == JSOP_TABLESWITCH);
        pc = code(top);
        SET_JUMP_OFFSET(pc, defaultOffset.offset - top);
        pc += JUMP_OFFSET_LEN;
    }

    // Set the SRC_SWITCH note's offset operand to tell end of switch.
    if (!setSrcNoteOffset(noteIndex, 0, lastNonJumpTargetOffset() - top))
        return false;

    if (switchOp == JSOP_TABLESWITCH) {
        // Skip over the already-initialized switch bounds.
        pc += 2 * JUMP_OFFSET_LEN;

        // Fill in the jump table, if there is one.
        for (uint32_t i = 0; i < tableLength; i++) {
            CaseClause* caseNode = table[i];
            ptrdiff_t off = caseNode ? caseNode->offset() - top : 0;
            SET_JUMP_OFFSET(pc, off);
            pc += JUMP_OFFSET_LEN;
        }
    }

    // Patch breaks before leaving the scope, as all breaks are under the
    // lexical scope if it exists.
    if (!controlInfo.patchBreaks(this))
        return false;

    if (emitterScope && !emitterScope->leave(this))
        return false;

    return true;
}

bool
BytecodeEmitter::isRunOnceLambda()
{
    // The run once lambda flags set by the parser are approximate, and we look
    // at properties of the function itself before deciding to emit a function
    // as a run once lambda.

    if (!(parent && parent->emittingRunOnceLambda) &&
        (emitterMode != LazyFunction || !lazyScript->treatAsRunOnce()))
    {
        return false;
    }

    FunctionBox* funbox = sc->asFunctionBox();
    return !funbox->argumentsHasLocalBinding() &&
           !funbox->isGenerator() &&
           !funbox->isAsync() &&
           !funbox->function()->explicitName();
}

bool
BytecodeEmitter::emitYieldOp(JSOp op)
{
    if (op == JSOP_FINALYIELDRVAL)
        return emit1(JSOP_FINALYIELDRVAL);

    MOZ_ASSERT(op == JSOP_INITIALYIELD || op == JSOP_YIELD || op == JSOP_AWAIT);

    ptrdiff_t off;
    if (!emitN(op, 3, &off))
        return false;

    uint32_t yieldAndAwaitIndex = yieldAndAwaitOffsetList.length();
    if (yieldAndAwaitIndex >= JS_BIT(24)) {
        reportError(nullptr, JSMSG_TOO_MANY_YIELDS);
        return false;
    }

    if (op == JSOP_AWAIT)
        yieldAndAwaitOffsetList.numAwaits++;
    else
        yieldAndAwaitOffsetList.numYields++;

    SET_UINT24(code(off), yieldAndAwaitIndex);

    if (!yieldAndAwaitOffsetList.append(offset()))
        return false;

    return emit1(JSOP_DEBUGAFTERYIELD);
}

bool
BytecodeEmitter::emitSetThis(ParseNode* pn)
{
    // ParseNodeKind::SetThis is used to update |this| after a super() call
    // in a derived class constructor.

    MOZ_ASSERT(pn->isKind(ParseNodeKind::SetThis));
    MOZ_ASSERT(pn->pn_left->isKind(ParseNodeKind::Name));

    RootedAtom name(cx, pn->pn_left->name());
    auto emitRhs = [&name, pn](BytecodeEmitter* bce, const NameLocation&, bool) {
        // Emit the new |this| value.
        if (!bce->emitTree(pn->pn_right))
            return false;
        // Get the original |this| and throw if we already initialized
        // it. Do *not* use the NameLocation argument, as that's the special
        // lexical location below to deal with super() semantics.
        if (!bce->emitGetName(name))
            return false;
        if (!bce->emit1(JSOP_CHECKTHISREINIT))
            return false;
        if (!bce->emit1(JSOP_POP))
            return false;
        return true;
    };

    // The 'this' binding is not lexical, but due to super() semantics this
    // initialization needs to be treated as a lexical one.
    NameLocation loc = lookupName(name);
    NameLocation lexicalLoc;
    if (loc.kind() == NameLocation::Kind::FrameSlot) {
        lexicalLoc = NameLocation::FrameSlot(BindingKind::Let, loc.frameSlot());
    } else if (loc.kind() == NameLocation::Kind::EnvironmentCoordinate) {
        EnvironmentCoordinate coord = loc.environmentCoordinate();
        uint8_t hops = AssertedCast<uint8_t>(coord.hops());
        lexicalLoc = NameLocation::EnvironmentCoordinate(BindingKind::Let, hops, coord.slot());
    } else {
        MOZ_ASSERT(loc.kind() == NameLocation::Kind::Dynamic);
        lexicalLoc = loc;
    }

    return emitSetOrInitializeNameAtLocation(name, lexicalLoc, emitRhs, true);
}

bool
BytecodeEmitter::emitScript(ParseNode* body)
{
    AutoFrontendTraceLog traceLog(cx, TraceLogger_BytecodeEmission, tokenStream(), body);

    TDZCheckCache tdzCache(this);
    EmitterScope emitterScope(this);
    if (sc->isGlobalContext()) {
        switchToPrologue();
        if (!emitterScope.enterGlobal(this, sc->asGlobalContext()))
            return false;
        switchToMain();
    } else if (sc->isEvalContext()) {
        switchToPrologue();
        if (!emitterScope.enterEval(this, sc->asEvalContext()))
            return false;
        switchToMain();
    } else {
        MOZ_ASSERT(sc->isModuleContext());
        if (!emitterScope.enterModule(this, sc->asModuleContext()))
            return false;
    }

    setFunctionBodyEndPos(body->pn_pos);

    if (sc->isEvalContext() && !sc->strict() &&
        body->isKind(ParseNodeKind::LexicalScope) && !body->isEmptyScope())
    {
        // Sloppy eval scripts may need to emit DEFFUNs in the prologue. If there is
        // an immediately enclosed lexical scope, we need to enter the lexical
        // scope in the prologue for the DEFFUNs to pick up the right
        // environment chain.
        EmitterScope lexicalEmitterScope(this);

        switchToPrologue();
        if (!lexicalEmitterScope.enterLexical(this, ScopeKind::Lexical, body->scopeBindings()))
            return false;
        switchToMain();

        if (!emitLexicalScopeBody(body->scopeBody()))
            return false;

        if (!lexicalEmitterScope.leave(this))
            return false;
    } else {
        if (!emitTree(body))
            return false;
    }

    if (!updateSourceCoordNotes(body->pn_pos.end))
        return false;

    if (!emit1(JSOP_RETRVAL))
        return false;

    if (!emitterScope.leave(this))
        return false;

    if (!JSScript::fullyInitFromEmitter(cx, script, this))
        return false;

    // URL and source map information must be set before firing
    // Debugger::onNewScript.
    if (!maybeSetDisplayURL() || !maybeSetSourceMap())
        return false;

    tellDebuggerAboutCompiledScript(cx);

    return true;
}

bool
BytecodeEmitter::emitFunctionScript(ParseNode* body)
{
    FunctionBox* funbox = sc->asFunctionBox();
    AutoFrontendTraceLog traceLog(cx, TraceLogger_BytecodeEmission, tokenStream(), funbox);

    // The ordering of these EmitterScopes is important. The named lambda
    // scope needs to enclose the function scope needs to enclose the extra
    // var scope.

    Maybe<EmitterScope> namedLambdaEmitterScope;
    if (funbox->namedLambdaBindings()) {
        namedLambdaEmitterScope.emplace(this);
        if (!namedLambdaEmitterScope->enterNamedLambda(this, funbox))
            return false;
    }

    /*
     * Emit a prologue for run-once scripts which will deoptimize JIT code
     * if the script ends up running multiple times via foo.caller related
     * shenanigans.
     *
     * Also mark the script so that initializers created within it may be
     * given more precise types.
     */
    if (isRunOnceLambda()) {
        script->setTreatAsRunOnce();
        MOZ_ASSERT(!script->hasRunOnce());

        switchToPrologue();
        if (!emit1(JSOP_RUNONCE))
            return false;
        switchToMain();
    }

    setFunctionBodyEndPos(body->pn_pos);
    if (!emitTree(body))
        return false;

    if (!updateSourceCoordNotes(body->pn_pos.end))
        return false;

    // Always end the script with a JSOP_RETRVAL. Some other parts of the
    // codebase depend on this opcode,
    // e.g. InterpreterRegs::setToEndOfScript.
    if (!emit1(JSOP_RETRVAL))
        return false;

    if (namedLambdaEmitterScope) {
        if (!namedLambdaEmitterScope->leave(this))
            return false;
        namedLambdaEmitterScope.reset();
    }

    if (!JSScript::fullyInitFromEmitter(cx, script, this))
        return false;

    // URL and source map information must be set before firing
    // Debugger::onNewScript. Only top-level functions need this, as compiling
    // the outer scripts of nested functions already processed the source.
    if (emitterMode != LazyFunction && !parent) {
        if (!maybeSetDisplayURL() || !maybeSetSourceMap())
            return false;

        tellDebuggerAboutCompiledScript(cx);
    }

    return true;
}

template <typename NameEmitter>
bool
BytecodeEmitter::emitDestructuringDeclsWithEmitter(ParseNode* pattern, NameEmitter emitName)
{
    if (pattern->isKind(ParseNodeKind::Array)) {
        for (ParseNode* element = pattern->pn_head; element; element = element->pn_next) {
            if (element->isKind(ParseNodeKind::Elision))
                continue;
            ParseNode* target = element;
            if (element->isKind(ParseNodeKind::Spread)) {
                target = element->pn_kid;
            }
            if (target->isKind(ParseNodeKind::Assign))
                target = target->pn_left;
            if (target->isKind(ParseNodeKind::Name)) {
                if (!emitName(this, target))
                    return false;
            } else {
                if (!emitDestructuringDeclsWithEmitter(target, emitName))
                    return false;
            }
        }
        return true;
    }

    MOZ_ASSERT(pattern->isKind(ParseNodeKind::Object));
    for (ParseNode* member = pattern->pn_head; member; member = member->pn_next) {
        MOZ_ASSERT(member->isKind(ParseNodeKind::MutateProto) ||
                   member->isKind(ParseNodeKind::Colon) ||
                   member->isKind(ParseNodeKind::Shorthand));

        ParseNode* target = member->isKind(ParseNodeKind::MutateProto)
                            ? member->pn_kid
                            : member->pn_right;

        if (target->isKind(ParseNodeKind::Assign))
            target = target->pn_left;
        if (target->isKind(ParseNodeKind::Name)) {
            if (!emitName(this, target))
                return false;
        } else {
            if (!emitDestructuringDeclsWithEmitter(target, emitName))
                return false;
        }
    }
    return true;
}

bool
BytecodeEmitter::emitDestructuringLHSRef(ParseNode* target, size_t* emitted)
{
    *emitted = 0;

    if (target->isKind(ParseNodeKind::Spread))
        target = target->pn_kid;
    else if (target->isKind(ParseNodeKind::Assign))
        target = target->pn_left;

    // No need to recur into ParseNodeKind::Array and
    // ParseNodeKind::Object subpatterns here, since
    // emitSetOrInitializeDestructuring does the recursion when
    // setting or initializing value.  Getting reference doesn't recur.
    if (target->isKind(ParseNodeKind::Name) ||
        target->isKind(ParseNodeKind::Array) ||
        target->isKind(ParseNodeKind::Object))
    {
        return true;
    }

#ifdef DEBUG
    int depth = stackDepth;
#endif

    switch (target->getKind()) {
      case ParseNodeKind::Dot: {
        if (target->as<PropertyAccess>().isSuper()) {
            if (!emitSuperPropLHS(&target->as<PropertyAccess>().expression()))
                return false;
            *emitted = 2;
        } else {
            if (!emitTree(target->pn_expr))
                return false;
            *emitted = 1;
        }
        break;
      }

      case ParseNodeKind::Elem: {
        if (target->as<PropertyByValue>().isSuper()) {
            if (!emitSuperElemOperands(target, EmitElemOption::Ref))
                return false;
            *emitted = 3;
        } else {
            if (!emitElemOperands(target, EmitElemOption::Ref))
                return false;
            *emitted = 2;
        }
        break;
      }

      case ParseNodeKind::Call:
        MOZ_ASSERT_UNREACHABLE("Parser::reportIfNotValidSimpleAssignmentTarget "
                               "rejects function calls as assignment "
                               "targets in destructuring assignments");
        break;

      default:
        MOZ_CRASH("emitDestructuringLHSRef: bad lhs kind");
    }

    MOZ_ASSERT(stackDepth == depth + int(*emitted));

    return true;
}

bool
BytecodeEmitter::emitSetOrInitializeDestructuring(ParseNode* target, DestructuringFlavor flav)
{
    // Now emit the lvalue opcode sequence. If the lvalue is a nested
    // destructuring initialiser-form, call ourselves to handle it, then pop
    // the matched value. Otherwise emit an lvalue bytecode sequence followed
    // by an assignment op.
    if (target->isKind(ParseNodeKind::Spread))
        target = target->pn_kid;
    else if (target->isKind(ParseNodeKind::Assign))
        target = target->pn_left;
    if (target->isKind(ParseNodeKind::Array) || target->isKind(ParseNodeKind::Object)) {
        if (!emitDestructuringOps(target, flav))
            return false;
        // Per its post-condition, emitDestructuringOps has left the
        // to-be-destructured value on top of the stack.
        if (!emit1(JSOP_POP))
            return false;
    } else {
        switch (target->getKind()) {
          case ParseNodeKind::Name: {
            auto emitSwapScopeAndRhs = [](BytecodeEmitter* bce, const NameLocation&,
                                          bool emittedBindOp)
            {
                if (emittedBindOp) {
                    // This is like ordinary assignment, but with one
                    // difference.
                    //
                    // In `a = b`, we first determine a binding for `a` (using
                    // JSOP_BINDNAME or JSOP_BINDGNAME), then we evaluate `b`,
                    // then a JSOP_SETNAME instruction.
                    //
                    // In `[a] = [b]`, per spec, `b` is evaluated first, then
                    // we determine a binding for `a`. Then we need to do
                    // assignment-- but the operands are on the stack in the
                    // wrong order for JSOP_SETPROP, so we have to add a
                    // JSOP_SWAP.
                    //
                    // In the cases where we are emitting a name op, emit a
                    // swap because of this.
                    return bce->emit1(JSOP_SWAP);
                }

                // In cases of emitting a frame slot or environment slot,
                // nothing needs be done.
                return true;
            };

            RootedAtom name(cx, target->name());
            switch (flav) {
              case DestructuringDeclaration:
                if (!emitInitializeName(name, emitSwapScopeAndRhs))
                    return false;
                break;

              case DestructuringFormalParameterInVarScope: {
                // If there's an parameter expression var scope, the
                // destructuring declaration needs to initialize the name in
                // the function scope. The innermost scope is the var scope,
                // and its enclosing scope is the function scope.
                EmitterScope* funScope = innermostEmitterScope()->enclosingInFrame();
                NameLocation paramLoc = *locationOfNameBoundInScope(name, funScope);
                if (!emitSetOrInitializeNameAtLocation(name, paramLoc, emitSwapScopeAndRhs, true))
                    return false;
                break;
              }

              case DestructuringAssignment:
                if (!emitSetName(name, emitSwapScopeAndRhs))
                    return false;
                break;
            }

            break;
          }

          case ParseNodeKind::Dot: {
            // The reference is already pushed by emitDestructuringLHSRef.
            JSOp setOp;
            if (target->as<PropertyAccess>().isSuper())
                setOp = sc->strict() ? JSOP_STRICTSETPROP_SUPER : JSOP_SETPROP_SUPER;
            else
                setOp = sc->strict() ? JSOP_STRICTSETPROP : JSOP_SETPROP;
            if (!emitAtomOp(target, setOp))
                return false;
            break;
          }

          case ParseNodeKind::Elem: {
            // The reference is already pushed by emitDestructuringLHSRef.
            if (target->as<PropertyByValue>().isSuper()) {
                JSOp setOp = sc->strict() ? JSOP_STRICTSETELEM_SUPER : JSOP_SETELEM_SUPER;
                // emitDestructuringLHSRef already did emitSuperElemOperands
                // part of emitSuperElemOp.  Perform remaining part here.
                if (!emitElemOpBase(setOp))
                    return false;
            } else {
                JSOp setOp = sc->strict() ? JSOP_STRICTSETELEM : JSOP_SETELEM;
                if (!emitElemOpBase(setOp))
                    return false;
            }
            break;
          }

          case ParseNodeKind::Call:
            MOZ_ASSERT_UNREACHABLE("Parser::reportIfNotValidSimpleAssignmentTarget "
                                   "rejects function calls as assignment "
                                   "targets in destructuring assignments");
            break;

          default:
            MOZ_CRASH("emitSetOrInitializeDestructuring: bad lhs kind");
        }

        // Pop the assigned value.
        if (!emit1(JSOP_POP))
            return false;
    }

    return true;
}

bool
BytecodeEmitter::emitIteratorNext(ParseNode* pn, IteratorKind iterKind /* = IteratorKind::Sync */,
                                  bool allowSelfHosted /* = false */)
{
    MOZ_ASSERT(allowSelfHosted || emitterMode != BytecodeEmitter::SelfHosting,
               ".next() iteration is prohibited in self-hosted code because it "
               "can run user-modifiable iteration code");

    MOZ_ASSERT(this->stackDepth >= 2);                    // ... NEXT ITER

    if (!emitCall(JSOP_CALL, 0, pn))                      // ... RESULT
        return false;

    if (iterKind == IteratorKind::Async) {
        if (!emitAwaitInInnermostScope())                 // ... RESULT
            return false;
    }

    if (!emitCheckIsObj(CheckIsObjectKind::IteratorNext)) // ... RESULT
        return false;
    checkTypeSet(JSOP_CALL);
    return true;
}

bool
BytecodeEmitter::emitPushNotUndefinedOrNull()
{
    MOZ_ASSERT(this->stackDepth > 0);                     // V

    if (!emit1(JSOP_DUP))                                 // V V
        return false;
    if (!emit1(JSOP_UNDEFINED))                           // V V UNDEFINED
        return false;
    if (!emit1(JSOP_STRICTNE))                            // V ?NEQL
        return false;

    JumpList undefinedOrNullJump;
    if (!emitJump(JSOP_AND, &undefinedOrNullJump))        // V ?NEQL
        return false;

    if (!emit1(JSOP_POP))                                 // V
        return false;
    if (!emit1(JSOP_DUP))                                 // V V
        return false;
    if (!emit1(JSOP_NULL))                                // V V NULL
        return false;
    if (!emit1(JSOP_STRICTNE))                            // V ?NEQL
        return false;

    if (!emitJumpTargetAndPatch(undefinedOrNullJump))     // V NOT-UNDEF-OR-NULL
        return false;

    return true;
}

bool
BytecodeEmitter::emitIteratorCloseInScope(EmitterScope& currentScope,
                                          IteratorKind iterKind /* = IteratorKind::Sync */,
                                          CompletionKind completionKind /* = CompletionKind::Normal */,
                                          bool allowSelfHosted /* = false */)
{
    MOZ_ASSERT(allowSelfHosted || emitterMode != BytecodeEmitter::SelfHosting,
               ".close() on iterators is prohibited in self-hosted code because it "
               "can run user-modifiable iteration code");

    // Generate inline logic corresponding to IteratorClose (ES 7.4.6).
    //
    // Callers need to ensure that the iterator object is at the top of the
    // stack.

    if (!emit1(JSOP_DUP))                                 // ... ITER ITER
        return false;

    // Step 3.
    //
    // Get the "return" method.
    if (!emitAtomOp(cx->names().return_, JSOP_CALLPROP))  // ... ITER RET
        return false;

    // Step 4.
    //
    // Do nothing if "return" is undefined or null.
    IfThenElseEmitter ifReturnMethodIsDefined(this);
    if (!emitPushNotUndefinedOrNull())                    // ... ITER RET NOT-UNDEF-OR-NULL
        return false;

    if (!ifReturnMethodIsDefined.emitIfElse())            // ... ITER RET
        return false;

    if (completionKind == CompletionKind::Throw) {
        // 7.4.6 IteratorClose ( iterator, completion )
        //   ...
        //   3. Let return be ? GetMethod(iterator, "return").
        //   4. If return is undefined, return Completion(completion).
        //   5. Let innerResult be Call(return, iterator, « »).
        //   6. If completion.[[Type]] is throw, return Completion(completion).
        //   7. If innerResult.[[Type]] is throw, return
        //      Completion(innerResult).
        //
        // For CompletionKind::Normal case, JSOP_CALL for step 5 checks if RET
        // is callable, and throws if not.  Since step 6 doesn't match and
        // error handling in step 3 and step 7 can be merged.
        //
        // For CompletionKind::Throw case, an error thrown by JSOP_CALL for
        // step 5 is ignored by try-catch.  So we should check if RET is
        // callable here, outside of try-catch, and the throw immediately if
        // not.
        CheckIsCallableKind kind = CheckIsCallableKind::IteratorReturn;
        if (!emitCheckIsCallable(kind))                   // ... ITER RET
            return false;
    }

    // Steps 5, 8.
    //
    // Call "return" if it is not undefined or null, and check that it returns
    // an Object.
    if (!emit1(JSOP_SWAP))                                // ... RET ITER
        return false;

    Maybe<TryEmitter> tryCatch;

    if (completionKind == CompletionKind::Throw) {
        tryCatch.emplace(this, TryEmitter::TryCatch, TryEmitter::DontUseRetVal,
                         TryEmitter::DontUseControl);

        // Mutate stack to balance stack for try-catch.
        if (!emit1(JSOP_UNDEFINED))                       // ... RET ITER UNDEF
            return false;
        if (!tryCatch->emitTry())                         // ... RET ITER UNDEF
            return false;
        if (!emitDupAt(2))                                // ... RET ITER UNDEF RET
            return false;
        if (!emitDupAt(2))                                // ... RET ITER UNDEF RET ITER
            return false;
    }

    if (!emitCall(JSOP_CALL, 0))                          // ... ... RESULT
        return false;
    checkTypeSet(JSOP_CALL);

    if (iterKind == IteratorKind::Async) {
        if (completionKind != CompletionKind::Throw) {
            // Await clobbers rval, so save the current rval.
            if (!emit1(JSOP_GETRVAL))                     // ... ... RESULT RVAL
                return false;
            if (!emit1(JSOP_SWAP))                        // ... ... RVAL RESULT
                return false;
        }
        if (!emitAwaitInScope(currentScope))              // ... ... RVAL? RESULT
            return false;
    }

    if (completionKind == CompletionKind::Throw) {
        if (!emit1(JSOP_SWAP))                            // ... RET ITER RESULT UNDEF
            return false;
        if (!emit1(JSOP_POP))                             // ... RET ITER RESULT
            return false;

        if (!tryCatch->emitCatch())                       // ... RET ITER RESULT
            return false;

        // Just ignore the exception thrown by call and await.
        if (!emit1(JSOP_EXCEPTION))                       // ... RET ITER RESULT EXC
            return false;
        if (!emit1(JSOP_POP))                             // ... RET ITER RESULT
            return false;

        if (!tryCatch->emitEnd())                         // ... RET ITER RESULT
            return false;

        // Restore stack.
        if (!emit2(JSOP_UNPICK, 2))                       // ... RESULT RET ITER
            return false;
        if (!emitPopN(2))                                 // ... RESULT
            return false;
    } else {
        if (!emitCheckIsObj(CheckIsObjectKind::IteratorReturn)) // ... RVAL? RESULT
            return false;

        if (iterKind == IteratorKind::Async) {
            if (!emit1(JSOP_SWAP))                        // ... RESULT RVAL
                return false;
            if (!emit1(JSOP_SETRVAL))                     // ... RESULT
                return false;
        }
    }

    if (!ifReturnMethodIsDefined.emitElse())              // ... ITER RET
        return false;

    if (!emit1(JSOP_POP))                                 // ... ITER
        return false;

    if (!ifReturnMethodIsDefined.emitEnd())
        return false;

    return emit1(JSOP_POP);                               // ...
}

template <typename InnerEmitter>
bool
BytecodeEmitter::wrapWithDestructuringIteratorCloseTryNote(int32_t iterDepth, InnerEmitter emitter)
{
    MOZ_ASSERT(this->stackDepth >= iterDepth);

    // Pad a nop at the beginning of the bytecode covered by the trynote so
    // that when unwinding environments, we may unwind to the scope
    // corresponding to the pc *before* the start, in case the first bytecode
    // emitted by |emitter| is the start of an inner scope. See comment above
    // UnwindEnvironmentToTryPc.
    if (!emit1(JSOP_TRY_DESTRUCTURING_ITERCLOSE))
        return false;

    ptrdiff_t start = offset();
    if (!emitter(this))
        return false;
    ptrdiff_t end = offset();
    if (start != end)
        return tryNoteList.append(JSTRY_DESTRUCTURING_ITERCLOSE, iterDepth, start, end);
    return true;
}

bool
BytecodeEmitter::emitDefault(ParseNode* defaultExpr, ParseNode* pattern)
{
    if (!emit1(JSOP_DUP))                                 // VALUE VALUE
        return false;
    if (!emit1(JSOP_UNDEFINED))                           // VALUE VALUE UNDEFINED
        return false;
    if (!emit1(JSOP_STRICTEQ))                            // VALUE EQL?
        return false;
    // Emit source note to enable ion compilation.
    if (!newSrcNote(SRC_IF))
        return false;
    JumpList jump;
    if (!emitJump(JSOP_IFEQ, &jump))                      // VALUE
        return false;
    if (!emit1(JSOP_POP))                                 // .
        return false;
    if (!emitInitializerInBranch(defaultExpr, pattern))   // DEFAULTVALUE
        return false;
    if (!emitJumpTargetAndPatch(jump))
        return false;
    return true;
}

bool
BytecodeEmitter::setOrEmitSetFunName(ParseNode* maybeFun, HandleAtom name,
                                     FunctionPrefixKind prefixKind)
{
    if (maybeFun->isKind(ParseNodeKind::Function)) {
        // Function doesn't have 'name' property at this point.
        // Set function's name at compile time.
        JSFunction* fun = maybeFun->pn_funbox->function();

        // Single node can be emitted multiple times if it appears in
        // array destructuring default.  If function already has a name,
        // just return.
        if (fun->hasCompileTimeName()) {
#ifdef DEBUG
            RootedFunction rootedFun(cx, fun);
            JSAtom* funName = NameToFunctionName(cx, name, prefixKind);
            if (!funName)
                return false;
            MOZ_ASSERT(funName == rootedFun->compileTimeName());
#endif
            return true;
        }

        fun->setCompileTimeName(name);
        return true;
    }

    uint32_t nameIndex;
    if (!makeAtomIndex(name, &nameIndex))
        return false;
    if (!emitIndexOp(JSOP_STRING, nameIndex))   // FUN NAME
        return false;
    uint8_t kind = uint8_t(prefixKind);
    if (!emit2(JSOP_SETFUNNAME, kind))          // FUN
        return false;
    return true;
}

bool
BytecodeEmitter::emitInitializer(ParseNode* initializer, ParseNode* pattern)
{
    if (!emitTree(initializer))
        return false;

    if (!pattern->isInParens() && pattern->isKind(ParseNodeKind::Name) &&
        initializer->isDirectRHSAnonFunction())
    {
        RootedAtom name(cx, pattern->name());
        if (!setOrEmitSetFunName(initializer, name, FunctionPrefixKind::None))
            return false;
    }

    return true;
}

bool
BytecodeEmitter::emitInitializerInBranch(ParseNode* initializer, ParseNode* pattern)
{
    TDZCheckCache tdzCache(this);
    return emitInitializer(initializer, pattern);
}

bool
BytecodeEmitter::emitDestructuringOpsArray(ParseNode* pattern, DestructuringFlavor flav)
{
    MOZ_ASSERT(pattern->isKind(ParseNodeKind::Array));
    MOZ_ASSERT(pattern->isArity(PN_LIST));
    MOZ_ASSERT(this->stackDepth != 0);

    // Here's pseudo code for |let [a, b, , c=y, ...d] = x;|
    //
    // Lines that are annotated "covered by trynote" mean that upon throwing
    // an exception, IteratorClose is called on iter only if done is false.
    //
    //   let x, y;
    //   let a, b, c, d;
    //   let iter, next, lref, result, done, value; // stack values
    //
    //   iter = x[Symbol.iterator]();
    //   next = iter.next;
    //
    //   // ==== emitted by loop for a ====
    //   lref = GetReference(a);              // covered by trynote
    //
    //   result = Call(next, iter);
    //   done = result.done;
    //
    //   if (done)
    //     value = undefined;
    //   else
    //     value = result.value;
    //
    //   SetOrInitialize(lref, value);        // covered by trynote
    //
    //   // ==== emitted by loop for b ====
    //   lref = GetReference(b);              // covered by trynote
    //
    //   if (done) {
    //     value = undefined;
    //   } else {
    //     result = Call(next, iter);
    //     done = result.done;
    //     if (done)
    //       value = undefined;
    //     else
    //       value = result.value;
    //   }
    //
    //   SetOrInitialize(lref, value);        // covered by trynote
    //
    //   // ==== emitted by loop for elision ====
    //   if (done) {
    //     value = undefined;
    //   } else {
    //     result = Call(next, iter);
    //     done = result.done;
    //     if (done)
    //       value = undefined;
    //     else
    //       value = result.value;
    //   }
    //
    //   // ==== emitted by loop for c ====
    //   lref = GetReference(c);              // covered by trynote
    //
    //   if (done) {
    //     value = undefined;
    //   } else {
    //     result = Call(next, iter);
    //     done = result.done;
    //     if (done)
    //       value = undefined;
    //     else
    //       value = result.value;
    //   }
    //
    //   if (value === undefined)
    //     value = y;                         // covered by trynote
    //
    //   SetOrInitialize(lref, value);        // covered by trynote
    //
    //   // ==== emitted by loop for d ====
    //   lref = GetReference(d);              // covered by trynote
    //
    //   if (done)
    //     value = [];
    //   else
    //     value = [...iter];
    //
    //   SetOrInitialize(lref, value);        // covered by trynote
    //
    //   // === emitted after loop ===
    //   if (!done)
    //      IteratorClose(iter);

    // Use an iterator to destructure the RHS, instead of index lookup. We
    // must leave the *original* value on the stack.
    if (!emit1(JSOP_DUP))                                         // ... OBJ OBJ
        return false;
    if (!emitIterator())                                          // ... OBJ NEXT ITER
        return false;

    // For an empty pattern [], call IteratorClose unconditionally. Nothing
    // else needs to be done.
    if (!pattern->pn_head) {
        if (!emit1(JSOP_SWAP))                                    // ... OBJ ITER NEXT
            return false;
        if (!emit1(JSOP_POP))                                     // ... OBJ ITER
            return false;

        return emitIteratorCloseInInnermostScope();               // ... OBJ
    }

    // Push an initial FALSE value for DONE.
    if (!emit1(JSOP_FALSE))                                       // ... OBJ NEXT ITER FALSE
        return false;

    // JSTRY_DESTRUCTURING_ITERCLOSE expects the iterator and the done value
    // to be the second to top and the top of the stack, respectively.
    // IteratorClose is called upon exception only if done is false.
    int32_t tryNoteDepth = stackDepth;

    for (ParseNode* member = pattern->pn_head; member; member = member->pn_next) {
        bool isFirst = member == pattern->pn_head;
        DebugOnly<bool> hasNext = !!member->pn_next;

        size_t emitted = 0;

        // Spec requires LHS reference to be evaluated first.
        ParseNode* lhsPattern = member;
        if (lhsPattern->isKind(ParseNodeKind::Assign))
            lhsPattern = lhsPattern->pn_left;

        bool isElision = lhsPattern->isKind(ParseNodeKind::Elision);
        if (!isElision) {
            auto emitLHSRef = [lhsPattern, &emitted](BytecodeEmitter* bce) {
                return bce->emitDestructuringLHSRef(lhsPattern, &emitted); // ... OBJ NEXT ITER DONE *LREF
            };
            if (!wrapWithDestructuringIteratorCloseTryNote(tryNoteDepth, emitLHSRef))
                return false;
        }

        // Pick the DONE value to the top of the stack.
        if (emitted) {
            if (!emit2(JSOP_PICK, emitted))                       // ... OBJ NEXT ITER *LREF DONE
                return false;
        }

        if (isFirst) {
            // If this element is the first, DONE is always FALSE, so pop it.
            //
            // Non-first elements should emit if-else depending on the
            // member pattern, below.
            if (!emit1(JSOP_POP))                                 // ... OBJ NEXT ITER *LREF
                return false;
        }

        if (member->isKind(ParseNodeKind::Spread)) {
            IfThenElseEmitter ifThenElse(this);
            if (!isFirst) {
                // If spread is not the first element of the pattern,
                // iterator can already be completed.
                                                                  // ... OBJ NEXT ITER *LREF DONE
                if (!ifThenElse.emitIfElse())                     // ... OBJ NEXT ITER *LREF
                    return false;

                if (!emitUint32Operand(JSOP_NEWARRAY, 0))         // ... OBJ NEXT ITER *LREF ARRAY
                    return false;
                if (!ifThenElse.emitElse())                       // ... OBJ NEXT ITER *LREF
                    return false;
            }

            // If iterator is not completed, create a new array with the rest
            // of the iterator.
            if (!emitDupAt(emitted + 1))                          // ... OBJ NEXT ITER *LREF NEXT
                return false;
            if (!emitDupAt(emitted + 1))                          // ... OBJ NEXT ITER *LREF NEXT ITER
                return false;
            if (!emitUint32Operand(JSOP_NEWARRAY, 0))             // ... OBJ NEXT ITER *LREF NEXT ITER ARRAY
                return false;
            if (!emitNumberOp(0))                                 // ... OBJ NEXT ITER *LREF NEXT ITER ARRAY INDEX
                return false;
            if (!emitSpread())                                    // ... OBJ NEXT ITER *LREF ARRAY INDEX
                return false;
            if (!emit1(JSOP_POP))                                 // ... OBJ NEXT ITER *LREF ARRAY
                return false;

            if (!isFirst) {
                if (!ifThenElse.emitEnd())
                    return false;
                MOZ_ASSERT(ifThenElse.pushed() == 1);
            }

            // At this point the iterator is done. Unpick a TRUE value for DONE above ITER.
            if (!emit1(JSOP_TRUE))                                // ... OBJ NEXT ITER *LREF ARRAY TRUE
                return false;
            if (!emit2(JSOP_UNPICK, emitted + 1))                 // ... OBJ NEXT ITER TRUE *LREF ARRAY
                return false;

            auto emitAssignment = [member, flav](BytecodeEmitter* bce) {
                return bce->emitSetOrInitializeDestructuring(member, flav); // ... OBJ NEXT ITER TRUE
            };
            if (!wrapWithDestructuringIteratorCloseTryNote(tryNoteDepth, emitAssignment))
                return false;

            MOZ_ASSERT(!hasNext);
            break;
        }

        ParseNode* pndefault = nullptr;
        if (member->isKind(ParseNodeKind::Assign))
            pndefault = member->pn_right;

        MOZ_ASSERT(!member->isKind(ParseNodeKind::Spread));

        IfThenElseEmitter ifAlreadyDone(this);
        if (!isFirst) {
                                                                  // ... OBJ NEXT ITER *LREF DONE
            if (!ifAlreadyDone.emitIfElse())                      // ... OBJ NEXT ITER *LREF
                return false;

            if (!emit1(JSOP_UNDEFINED))                           // ... OBJ NEXT ITER *LREF UNDEF
                return false;
            if (!emit1(JSOP_NOP_DESTRUCTURING))                   // ... OBJ NEXT ITER *LREF UNDEF
                return false;

            // The iterator is done. Unpick a TRUE value for DONE above ITER.
            if (!emit1(JSOP_TRUE))                                // ... OBJ NEXT ITER *LREF UNDEF TRUE
                return false;
            if (!emit2(JSOP_UNPICK, emitted + 1))                 // ... OBJ NEXT ITER TRUE *LREF UNDEF
                return false;

            if (!ifAlreadyDone.emitElse())                        // ... OBJ NEXT ITER *LREF
                return false;
        }

        if (!emitDupAt(emitted + 1))                              // ... OBJ NEXT ITER *LREF NEXT
            return false;
        if (!emitDupAt(emitted + 1))                              // ... OBJ NEXT ITER *LREF NEXT ITER
            return false;
        if (!emitIteratorNext(pattern))                           // ... OBJ NEXT ITER *LREF RESULT
            return false;
        if (!emit1(JSOP_DUP))                                     // ... OBJ NEXT ITER *LREF RESULT RESULT
            return false;
        if (!emitAtomOp(cx->names().done, JSOP_GETPROP))          // ... OBJ NEXT ITER *LREF RESULT DONE
            return false;

        if (!emit1(JSOP_DUP))                                     // ... OBJ NEXT ITER *LREF RESULT DONE DONE
            return false;
        if (!emit2(JSOP_UNPICK, emitted + 2))                     // ... OBJ NEXT ITER DONE *LREF RESULT DONE
            return false;

        IfThenElseEmitter ifDone(this);
        if (!ifDone.emitIfElse())                                 // ... OBJ NEXT ITER DONE *LREF RESULT
            return false;

        if (!emit1(JSOP_POP))                                     // ... OBJ NEXT ITER DONE *LREF
            return false;
        if (!emit1(JSOP_UNDEFINED))                               // ... OBJ NEXT ITER DONE *LREF UNDEF
            return false;
        if (!emit1(JSOP_NOP_DESTRUCTURING))                       // ... OBJ NEXT ITER DONE *LREF UNDEF
            return false;

        if (!ifDone.emitElse())                                   // ... OBJ NEXT ITER DONE *LREF RESULT
            return false;

        if (!emitAtomOp(cx->names().value, JSOP_GETPROP))         // ... OBJ NEXT ITER DONE *LREF VALUE
            return false;

        if (!ifDone.emitEnd())
            return false;
        MOZ_ASSERT(ifDone.pushed() == 0);

        if (!isFirst) {
            if (!ifAlreadyDone.emitEnd())
                return false;
            MOZ_ASSERT(ifAlreadyDone.pushed() == 2);
        }

        if (pndefault) {
            auto emitDefault = [pndefault, lhsPattern](BytecodeEmitter* bce) {
                return bce->emitDefault(pndefault, lhsPattern);    // ... OBJ NEXT ITER DONE *LREF VALUE
            };

            if (!wrapWithDestructuringIteratorCloseTryNote(tryNoteDepth, emitDefault))
                return false;
        }

        if (!isElision) {
            auto emitAssignment = [lhsPattern, flav](BytecodeEmitter* bce) {
                return bce->emitSetOrInitializeDestructuring(lhsPattern, flav); // ... OBJ NEXT ITER DONE
            };

            if (!wrapWithDestructuringIteratorCloseTryNote(tryNoteDepth, emitAssignment))
                return false;
        } else {
            if (!emit1(JSOP_POP))                                 // ... OBJ NEXT ITER DONE
                return false;
        }
    }

    // The last DONE value is on top of the stack. If not DONE, call
    // IteratorClose.
                                                                  // ... OBJ NEXT ITER DONE
    IfThenElseEmitter ifDone(this);
    if (!ifDone.emitIfElse())                                     // ... OBJ NEXT ITER
        return false;
    if (!emitPopN(2))                                             // ... OBJ
        return false;
    if (!ifDone.emitElse())                                       // ... OBJ NEXT ITER
        return false;
    if (!emit1(JSOP_SWAP))                                        // ... OBJ ITER NEXT
        return false;
    if (!emit1(JSOP_POP))                                         // ... OBJ ITER
        return false;
    if (!emitIteratorCloseInInnermostScope())                     // ... OBJ
        return false;
    if (!ifDone.emitEnd())
        return false;

    return true;
}

bool
BytecodeEmitter::emitComputedPropertyName(ParseNode* computedPropName)
{
    MOZ_ASSERT(computedPropName->isKind(ParseNodeKind::ComputedName));
    return emitTree(computedPropName->pn_kid) && emit1(JSOP_TOID);
}

bool
BytecodeEmitter::emitDestructuringOpsObject(ParseNode* pattern, DestructuringFlavor flav)
{
    MOZ_ASSERT(pattern->isKind(ParseNodeKind::Object));
    MOZ_ASSERT(pattern->isArity(PN_LIST));

    MOZ_ASSERT(this->stackDepth > 0);                             // ... RHS

    if (!emit1(JSOP_CHECKOBJCOERCIBLE))                           // ... RHS
        return false;

    bool needsRestPropertyExcludedSet = pattern->pn_count > 1 &&
                                        pattern->last()->isKind(ParseNodeKind::Spread);
    if (needsRestPropertyExcludedSet) {
        if (!emitDestructuringObjRestExclusionSet(pattern))       // ... RHS SET
            return false;

        if (!emit1(JSOP_SWAP))                                    // ... SET RHS
            return false;
    }

    for (ParseNode* member = pattern->pn_head; member; member = member->pn_next) {
        ParseNode* subpattern;
        if (member->isKind(ParseNodeKind::MutateProto) ||
            member->isKind(ParseNodeKind::Spread))
        {
            subpattern = member->pn_kid;
        } else {
            subpattern = member->pn_right;
        }

        ParseNode* lhs = subpattern;
        MOZ_ASSERT_IF(member->isKind(ParseNodeKind::Spread),
                      !lhs->isKind(ParseNodeKind::Assign));
        if (lhs->isKind(ParseNodeKind::Assign))
            lhs = lhs->pn_left;

        size_t emitted;
        if (!emitDestructuringLHSRef(lhs, &emitted))              // ... *SET RHS *LREF
            return false;

        // Duplicate the value being destructured to use as a reference base.
        if (!emitDupAt(emitted))                                  // ... *SET RHS *LREF RHS
            return false;

        if (member->isKind(ParseNodeKind::Spread)) {
            if (!updateSourceCoordNotes(member->pn_pos.begin))
                return false;

            if (!emitNewInit(JSProto_Object))                     // ... *SET RHS *LREF RHS TARGET
                return false;
            if (!emit1(JSOP_DUP))                                 // ... *SET RHS *LREF RHS TARGET TARGET
                return false;
            if (!emit2(JSOP_PICK, 2))                             // ... *SET RHS *LREF TARGET TARGET RHS
                return false;

            if (needsRestPropertyExcludedSet) {
                if (!emit2(JSOP_PICK, emitted + 4))               // ... RHS *LREF TARGET TARGET RHS SET
                    return false;
            }

            CopyOption option = needsRestPropertyExcludedSet
                                ? CopyOption::Filtered
                                : CopyOption::Unfiltered;
            if (!emitCopyDataProperties(option))                  // ... RHS *LREF TARGET
                return false;

            // Destructure TARGET per this member's lhs.
            if (!emitSetOrInitializeDestructuring(lhs, flav))     // ... RHS
                return false;

            MOZ_ASSERT(member == pattern->last(), "Rest property is always last");
            break;
        }

        // Now push the property name currently being matched, which is the
        // current property name "label" on the left of a colon in the object
        // initialiser.
        bool needsGetElem = true;

        if (member->isKind(ParseNodeKind::MutateProto)) {
            if (!emitAtomOp(cx->names().proto, JSOP_GETPROP))     // ... *SET RHS *LREF PROP
                return false;
            needsGetElem = false;
        } else {
            MOZ_ASSERT(member->isKind(ParseNodeKind::Colon) ||
                       member->isKind(ParseNodeKind::Shorthand));

            ParseNode* key = member->pn_left;
            if (key->isKind(ParseNodeKind::Number)) {
                if (!emitNumberOp(key->pn_dval))                  // ... *SET RHS *LREF RHS KEY
                    return false;
            } else if (key->isKind(ParseNodeKind::ObjectPropertyName) ||
                       key->isKind(ParseNodeKind::String))
            {
                if (!emitAtomOp(key->pn_atom, JSOP_GETPROP))      // ... *SET RHS *LREF PROP
                    return false;
                needsGetElem = false;
            } else {
                if (!emitComputedPropertyName(key))               // ... *SET RHS *LREF RHS KEY
                    return false;

                // Add the computed property key to the exclusion set.
                if (needsRestPropertyExcludedSet) {
                    if (!emitDupAt(emitted + 3))                  // ... SET RHS *LREF RHS KEY SET
                        return false;
                    if (!emitDupAt(1))                            // ... SET RHS *LREF RHS KEY SET KEY
                        return false;
                    if (!emit1(JSOP_UNDEFINED))                   // ... SET RHS *LREF RHS KEY SET KEY UNDEFINED
                        return false;
                    if (!emit1(JSOP_INITELEM))                    // ... SET RHS *LREF RHS KEY SET
                        return false;
                    if (!emit1(JSOP_POP))                         // ... SET RHS *LREF RHS KEY
                        return false;
                }
            }
        }

        // Get the property value if not done already.
        if (needsGetElem && !emitElemOpBase(JSOP_GETELEM))        // ... *SET RHS *LREF PROP
            return false;

        if (subpattern->isKind(ParseNodeKind::Assign)) {
            if (!emitDefault(subpattern->pn_right, lhs))          // ... *SET RHS *LREF VALUE
                return false;
        }

        // Destructure PROP per this member's lhs.
        if (!emitSetOrInitializeDestructuring(subpattern, flav))  // ... *SET RHS
            return false;
    }

    return true;
}

bool
BytecodeEmitter::emitDestructuringObjRestExclusionSet(ParseNode* pattern)
{
    MOZ_ASSERT(pattern->isKind(ParseNodeKind::Object));
    MOZ_ASSERT(pattern->isArity(PN_LIST));
    MOZ_ASSERT(pattern->last()->isKind(ParseNodeKind::Spread));

    ptrdiff_t offset = this->offset();
    if (!emitNewInit(JSProto_Object))
        return false;

    // Try to construct the shape of the object as we go, so we can emit a
    // JSOP_NEWOBJECT with the final shape instead.
    // In the case of computed property names and indices, we cannot fix the
    // shape at bytecode compile time. When the shape cannot be determined,
    // |obj| is nulled out.

    // No need to do any guessing for the object kind, since we know the upper
    // bound of how many properties we plan to have.
    gc::AllocKind kind = gc::GetGCObjectKind(pattern->pn_count - 1);
    RootedPlainObject obj(cx, NewBuiltinClassInstance<PlainObject>(cx, kind, TenuredObject));
    if (!obj)
        return false;

    RootedAtom pnatom(cx);
    for (ParseNode* member = pattern->pn_head; member; member = member->pn_next) {
        if (member->isKind(ParseNodeKind::Spread))
            break;

        bool isIndex = false;
        if (member->isKind(ParseNodeKind::MutateProto)) {
            pnatom.set(cx->names().proto);
        } else {
            ParseNode* key = member->pn_left;
            if (key->isKind(ParseNodeKind::Number)) {
                if (!emitNumberOp(key->pn_dval))
                    return false;
                isIndex = true;
            } else if (key->isKind(ParseNodeKind::ObjectPropertyName) ||
                       key->isKind(ParseNodeKind::String))
            {
                pnatom.set(key->pn_atom);
            } else {
                // Otherwise this is a computed property name which needs to
                // be added dynamically.
                obj.set(nullptr);
                continue;
            }
        }

        // Initialize elements with |undefined|.
        if (!emit1(JSOP_UNDEFINED))
            return false;

        if (isIndex) {
            obj.set(nullptr);
            if (!emit1(JSOP_INITELEM))
                return false;
        } else {
            uint32_t index;
            if (!makeAtomIndex(pnatom, &index))
                return false;

            if (obj) {
                MOZ_ASSERT(!obj->inDictionaryMode());
                Rooted<jsid> id(cx, AtomToId(pnatom));
                if (!NativeDefineDataProperty(cx, obj, id, UndefinedHandleValue, JSPROP_ENUMERATE))
                    return false;
                if (obj->inDictionaryMode())
                    obj.set(nullptr);
            }

            if (!emitIndex32(JSOP_INITPROP, index))
                return false;
        }
    }

    if (obj) {
        // The object survived and has a predictable shape: update the
        // original bytecode.
        if (!replaceNewInitWithNewObject(obj, offset))
            return false;
    }

    return true;
}

bool
BytecodeEmitter::emitDestructuringOps(ParseNode* pattern, DestructuringFlavor flav)
{
    if (pattern->isKind(ParseNodeKind::Array))
        return emitDestructuringOpsArray(pattern, flav);
    return emitDestructuringOpsObject(pattern, flav);
}

bool
BytecodeEmitter::emitTemplateString(ParseNode* pn)
{
    MOZ_ASSERT(pn->isArity(PN_LIST));

    bool pushedString = false;

    for (ParseNode* pn2 = pn->pn_head; pn2 != NULL; pn2 = pn2->pn_next) {
        bool isString = (pn2->getKind() == ParseNodeKind::String ||
                         pn2->getKind() == ParseNodeKind::TemplateString);

        // Skip empty strings. These are very common: a template string like
        // `${a}${b}` has three empty strings and without this optimization
        // we'd emit four JSOP_ADD operations instead of just one.
        if (isString && pn2->pn_atom->empty())
            continue;

        if (!isString) {
            // We update source notes before emitting the expression
            if (!updateSourceCoordNotes(pn2->pn_pos.begin))
                return false;
        }

        if (!emitTree(pn2))
            return false;

        if (!isString) {
            // We need to convert the expression to a string
            if (!emit1(JSOP_TOSTRING))
                return false;
        }

        if (pushedString) {
            // We've pushed two strings onto the stack. Add them together, leaving just one.
            if (!emit1(JSOP_ADD))
                return false;
        } else {
            pushedString = true;
        }
    }

    if (!pushedString) {
        // All strings were empty, this can happen for something like `${""}`.
        // Just push an empty string.
        if (!emitAtomOp(cx->names().empty, JSOP_STRING))
            return false;
    }

    return true;
}

bool
BytecodeEmitter::emitDeclarationList(ParseNode* declList)
{
    MOZ_ASSERT(declList->isArity(PN_LIST));
    MOZ_ASSERT(declList->isOp(JSOP_NOP));

    ParseNode* next;
    for (ParseNode* decl = declList->pn_head; decl; decl = next) {
        if (!updateSourceCoordNotes(decl->pn_pos.begin))
            return false;
        next = decl->pn_next;

        if (decl->isKind(ParseNodeKind::Assign)) {
            MOZ_ASSERT(decl->isOp(JSOP_NOP));

            ParseNode* pattern = decl->pn_left;
            MOZ_ASSERT(pattern->isKind(ParseNodeKind::Array) ||
                       pattern->isKind(ParseNodeKind::Object));

            if (!emitTree(decl->pn_right))
                return false;

            if (!emitDestructuringOps(pattern, DestructuringDeclaration))
                return false;

            if (!emit1(JSOP_POP))
                return false;
        } else {
            if (!emitSingleDeclaration(declList, decl, decl->expr()))
                return false;
        }
    }
    return true;
}

bool
BytecodeEmitter::emitSingleDeclaration(ParseNode* declList, ParseNode* decl,
                                       ParseNode* initializer)
{
    MOZ_ASSERT(decl->isKind(ParseNodeKind::Name));

    // Nothing to do for initializer-less 'var' declarations, as there's no TDZ.
    if (!initializer && declList->isKind(ParseNodeKind::Var))
        return true;

    auto emitRhs = [initializer, declList, decl](BytecodeEmitter* bce, const NameLocation&, bool) {
        if (!initializer) {
            // Lexical declarations are initialized to undefined without an
            // initializer.
            MOZ_ASSERT(declList->isKind(ParseNodeKind::Let),
                       "var declarations without initializers handled above, "
                       "and const declarations must have initializers");
            Unused << declList; // silence clang -Wunused-lambda-capture in opt builds
            return bce->emit1(JSOP_UNDEFINED);
        }

        MOZ_ASSERT(initializer);
        return bce->emitInitializer(initializer, decl);
    };

    if (!emitInitializeName(decl, emitRhs))
        return false;

    // Pop the RHS.
    return emit1(JSOP_POP);
}

static bool
EmitAssignmentRhs(BytecodeEmitter* bce, ParseNode* rhs, uint8_t offset)
{
    // If there is a RHS tree, emit the tree.
    if (rhs)
        return bce->emitTree(rhs);

    // Otherwise the RHS value to assign is already on the stack, i.e., the
    // next enumeration value in a for-in or for-of loop. Depending on how
    // many other values have been pushed on the stack, we need to get the
    // already-pushed RHS value.
    if (offset != 1 && !bce->emit2(JSOP_PICK, offset - 1))
        return false;

    return true;
}

static inline JSOp
CompoundAssignmentParseNodeKindToJSOp(ParseNodeKind pnk)
{
    switch (pnk) {
      case ParseNodeKind::Assign:       return JSOP_NOP;
      case ParseNodeKind::AddAssign:    return JSOP_ADD;
      case ParseNodeKind::SubAssign:    return JSOP_SUB;
      case ParseNodeKind::BitOrAssign:  return JSOP_BITOR;
      case ParseNodeKind::BitXorAssign: return JSOP_BITXOR;
      case ParseNodeKind::BitAndAssign: return JSOP_BITAND;
      case ParseNodeKind::LshAssign:    return JSOP_LSH;
      case ParseNodeKind::RshAssign:    return JSOP_RSH;
      case ParseNodeKind::UrshAssign:   return JSOP_URSH;
      case ParseNodeKind::MulAssign:    return JSOP_MUL;
      case ParseNodeKind::DivAssign:    return JSOP_DIV;
      case ParseNodeKind::ModAssign:    return JSOP_MOD;
      case ParseNodeKind::PowAssign:    return JSOP_POW;
      default: MOZ_CRASH("unexpected compound assignment op");
    }
}

bool
BytecodeEmitter::emitAssignment(ParseNode* lhs, ParseNodeKind pnk, ParseNode* rhs)
{
    JSOp op = CompoundAssignmentParseNodeKindToJSOp(pnk);

    // Name assignments are handled separately because choosing ops and when
    // to emit BINDNAME is involved and should avoid duplication.
    if (lhs->isKind(ParseNodeKind::Name)) {
        auto emitRhs = [op, lhs, rhs](BytecodeEmitter* bce, const NameLocation& lhsLoc,
                                      bool emittedBindOp)
        {
            // For compound assignments, first get the LHS value, then emit
            // the RHS and the op.
            if (op != JSOP_NOP) {
                if (!bce->emitGetNameAtLocationForCompoundAssignment(lhs->name(), lhsLoc))
                    return false;
            }

            // Emit the RHS. If we emitted a BIND[G]NAME, then the scope is on
            // the top of the stack and we need to pick the right RHS value.
            if (!EmitAssignmentRhs(bce, rhs, emittedBindOp ? 2 : 1))
                return false;

            if (!lhs->isInParens() && op == JSOP_NOP && rhs && rhs->isDirectRHSAnonFunction()) {
                RootedAtom name(bce->cx, lhs->name());
                if (!bce->setOrEmitSetFunName(rhs, name, FunctionPrefixKind::None))
                    return false;
            }

            // Emit the compound assignment op if there is one.
            if (op != JSOP_NOP) {
                if (!bce->emit1(op))
                    return false;
            }

            return true;
        };

        return emitSetName(lhs, emitRhs);
    }

    // Deal with non-name assignments.
    uint32_t atomIndex = (uint32_t) -1;
    uint8_t offset = 1;

    switch (lhs->getKind()) {
      case ParseNodeKind::Dot:
        if (lhs->as<PropertyAccess>().isSuper()) {
            if (!emitSuperPropLHS(&lhs->as<PropertyAccess>().expression()))
                return false;
            offset += 2;
        } else {
            if (!emitTree(lhs->expr()))
                return false;
            offset += 1;
        }
        if (!makeAtomIndex(lhs->pn_atom, &atomIndex))
            return false;
        break;
      case ParseNodeKind::Elem: {
        MOZ_ASSERT(lhs->isArity(PN_BINARY));
        EmitElemOption opt = op == JSOP_NOP ? EmitElemOption::Get : EmitElemOption::CompoundAssign;
        if (lhs->as<PropertyByValue>().isSuper()) {
            if (!emitSuperElemOperands(lhs, opt))
                return false;
            offset += 3;
        } else {
            if (!emitElemOperands(lhs, opt))
                return false;
            offset += 2;
        }
        break;
      }
      case ParseNodeKind::Array:
      case ParseNodeKind::Object:
        break;
      case ParseNodeKind::Call:
        if (!emitTree(lhs))
            return false;

        // Assignment to function calls is forbidden, but we have to make the
        // call first.  Now we can throw.
        if (!emitUint16Operand(JSOP_THROWMSG, JSMSG_BAD_LEFTSIDE_OF_ASS))
            return false;

        // Rebalance the stack to placate stack-depth assertions.
        if (!emit1(JSOP_POP))
            return false;
        break;
      default:
        MOZ_ASSERT(0);
    }

    if (op != JSOP_NOP) {
        MOZ_ASSERT(rhs);
        switch (lhs->getKind()) {
          case ParseNodeKind::Dot: {
            JSOp getOp;
            if (lhs->as<PropertyAccess>().isSuper()) {
                if (!emit1(JSOP_DUP2))
                    return false;
                getOp = JSOP_GETPROP_SUPER;
            } else {
                if (!emit1(JSOP_DUP))
                    return false;
                bool isLength = (lhs->pn_atom == cx->names().length);
                getOp = isLength ? JSOP_LENGTH : JSOP_GETPROP;
            }
            if (!emitIndex32(getOp, atomIndex))
                return false;
            break;
          }
          case ParseNodeKind::Elem: {
            JSOp elemOp;
            if (lhs->as<PropertyByValue>().isSuper()) {
                if (!emitDupAt(2))
                    return false;
                if (!emitDupAt(2))
                    return false;
                if (!emitDupAt(2))
                    return false;
                elemOp = JSOP_GETELEM_SUPER;
            } else {
                if (!emit1(JSOP_DUP2))
                    return false;
                elemOp = JSOP_GETELEM;
            }
            if (!emitElemOpBase(elemOp))
                return false;
            break;
          }
          case ParseNodeKind::Call:
            // We just emitted a JSOP_THROWMSG and popped the call's return
            // value.  Push a random value to make sure the stack depth is
            // correct.
            if (!emit1(JSOP_NULL))
                return false;
            break;
          default:;
        }
    }

    if (!EmitAssignmentRhs(this, rhs, offset))
        return false;

    /* If += etc., emit the binary operator with a source note. */
    if (op != JSOP_NOP) {
        if (!newSrcNote(SRC_ASSIGNOP))
            return false;
        if (!emit1(op))
            return false;
    }

    /* Finally, emit the specialized assignment bytecode. */
    switch (lhs->getKind()) {
      case ParseNodeKind::Dot: {
        JSOp setOp = lhs->as<PropertyAccess>().isSuper() ?
                       (sc->strict() ? JSOP_STRICTSETPROP_SUPER : JSOP_SETPROP_SUPER) :
                       (sc->strict() ? JSOP_STRICTSETPROP : JSOP_SETPROP);
        if (!emitIndexOp(setOp, atomIndex))
            return false;
        break;
      }
      case ParseNodeKind::Call:
        // We threw above, so nothing to do here.
        break;
      case ParseNodeKind::Elem: {
        JSOp setOp = lhs->as<PropertyByValue>().isSuper() ?
                       sc->strict() ? JSOP_STRICTSETELEM_SUPER : JSOP_SETELEM_SUPER :
                       sc->strict() ? JSOP_STRICTSETELEM : JSOP_SETELEM;
        if (!emit1(setOp))
            return false;
        break;
      }
      case ParseNodeKind::Array:
      case ParseNodeKind::Object:
        if (!emitDestructuringOps(lhs, DestructuringAssignment))
            return false;
        break;
      default:
        MOZ_ASSERT(0);
    }
    return true;
}

bool
ParseNode::getConstantValue(JSContext* cx, AllowConstantObjects allowObjects,
                            MutableHandleValue vp, Value* compare, size_t ncompare,
                            NewObjectKind newKind)
{
    MOZ_ASSERT(newKind == TenuredObject || newKind == SingletonObject);

    switch (getKind()) {
      case ParseNodeKind::Number:
        vp.setNumber(pn_dval);
        return true;
      case ParseNodeKind::TemplateString:
      case ParseNodeKind::String:
        vp.setString(pn_atom);
        return true;
      case ParseNodeKind::True:
        vp.setBoolean(true);
        return true;
      case ParseNodeKind::False:
        vp.setBoolean(false);
        return true;
      case ParseNodeKind::Null:
        vp.setNull();
        return true;
      case ParseNodeKind::RawUndefined:
        vp.setUndefined();
        return true;
      case ParseNodeKind::CallSiteObj:
      case ParseNodeKind::Array: {
        unsigned count;
        ParseNode* pn;

        if (allowObjects == DontAllowObjects) {
            vp.setMagic(JS_GENERIC_MAGIC);
            return true;
        }

        ObjectGroup::NewArrayKind arrayKind = ObjectGroup::NewArrayKind::Normal;
        if (allowObjects == ForCopyOnWriteArray) {
            arrayKind = ObjectGroup::NewArrayKind::CopyOnWrite;
            allowObjects = DontAllowObjects;
        }

        if (getKind() == ParseNodeKind::CallSiteObj) {
            count = pn_count - 1;
            pn = pn_head->pn_next;
        } else {
            MOZ_ASSERT(!(pn_xflags & PNX_NONCONST));
            count = pn_count;
            pn = pn_head;
        }

        AutoValueVector values(cx);
        if (!values.appendN(MagicValue(JS_ELEMENTS_HOLE), count))
            return false;
        size_t idx;
        for (idx = 0; pn; idx++, pn = pn->pn_next) {
            if (!pn->getConstantValue(cx, allowObjects, values[idx], values.begin(), idx))
                return false;
            if (values[idx].isMagic(JS_GENERIC_MAGIC)) {
                vp.setMagic(JS_GENERIC_MAGIC);
                return true;
            }
        }
        MOZ_ASSERT(idx == count);

        ArrayObject* obj = ObjectGroup::newArrayObject(cx, values.begin(), values.length(),
                                                       newKind, arrayKind);
        if (!obj)
            return false;

        if (!CombineArrayElementTypes(cx, obj, compare, ncompare))
            return false;

        vp.setObject(*obj);
        return true;
      }
      case ParseNodeKind::Object: {
        MOZ_ASSERT(!(pn_xflags & PNX_NONCONST));

        if (allowObjects == DontAllowObjects) {
            vp.setMagic(JS_GENERIC_MAGIC);
            return true;
        }
        MOZ_ASSERT(allowObjects == AllowObjects);

        Rooted<IdValueVector> properties(cx, IdValueVector(cx));

        RootedValue value(cx), idvalue(cx);
        for (ParseNode* pn = pn_head; pn; pn = pn->pn_next) {
            if (!pn->pn_right->getConstantValue(cx, allowObjects, &value))
                return false;
            if (value.isMagic(JS_GENERIC_MAGIC)) {
                vp.setMagic(JS_GENERIC_MAGIC);
                return true;
            }

            ParseNode* pnid = pn->pn_left;
            if (pnid->isKind(ParseNodeKind::Number)) {
                idvalue = NumberValue(pnid->pn_dval);
            } else {
                MOZ_ASSERT(pnid->isKind(ParseNodeKind::ObjectPropertyName) ||
                           pnid->isKind(ParseNodeKind::String));
                MOZ_ASSERT(pnid->pn_atom != cx->names().proto);
                idvalue = StringValue(pnid->pn_atom);
            }

            RootedId id(cx);
            if (!ValueToId<CanGC>(cx, idvalue, &id))
                return false;

            if (!properties.append(IdValuePair(id, value)))
                return false;
        }

        JSObject* obj = ObjectGroup::newPlainObject(cx, properties.begin(), properties.length(),
                                                    newKind);
        if (!obj)
            return false;

        if (!CombinePlainObjectPropertyTypes(cx, obj, compare, ncompare))
            return false;

        vp.setObject(*obj);
        return true;
      }
      default:
        MOZ_CRASH("Unexpected node");
    }
    return false;
}

bool
BytecodeEmitter::emitSingletonInitialiser(ParseNode* pn)
{
    NewObjectKind newKind =
        (pn->getKind() == ParseNodeKind::Object) ? SingletonObject : TenuredObject;

    RootedValue value(cx);
    if (!pn->getConstantValue(cx, ParseNode::AllowObjects, &value, nullptr, 0, newKind))
        return false;

    MOZ_ASSERT_IF(newKind == SingletonObject, value.toObject().isSingleton());

    ObjectBox* objbox = parser.newObjectBox(&value.toObject());
    if (!objbox)
        return false;

    return emitObjectOp(objbox, JSOP_OBJECT);
}

bool
BytecodeEmitter::emitCallSiteObject(ParseNode* pn)
{
    RootedValue value(cx);
    if (!pn->getConstantValue(cx, ParseNode::AllowObjects, &value))
        return false;

    MOZ_ASSERT(value.isObject());

    ObjectBox* objbox1 = parser.newObjectBox(&value.toObject());
    if (!objbox1)
        return false;

    if (!pn->as<CallSiteNode>().getRawArrayValue(cx, &value))
        return false;

    MOZ_ASSERT(value.isObject());

    ObjectBox* objbox2 = parser.newObjectBox(&value.toObject());
    if (!objbox2)
        return false;

    return emitObjectPairOp(objbox1, objbox2, JSOP_CALLSITEOBJ);
}

/* See the SRC_FOR source note offsetBias comments later in this file. */
JS_STATIC_ASSERT(JSOP_NOP_LENGTH == 1);
JS_STATIC_ASSERT(JSOP_POP_LENGTH == 1);

namespace {

class EmitLevelManager
{
    BytecodeEmitter* bce;
  public:
    explicit EmitLevelManager(BytecodeEmitter* bce) : bce(bce) { bce->emitLevel++; }
    ~EmitLevelManager() { bce->emitLevel--; }
};

} /* anonymous namespace */

bool
BytecodeEmitter::emitCatch(ParseNode* pn)
{
    // We must be nested under a try-finally statement.
    MOZ_ASSERT(innermostNestableControl->is<TryFinallyControl>());

    /* Pick up the pending exception and bind it to the catch variable. */
    if (!emit1(JSOP_EXCEPTION))
        return false;

    ParseNode* pn2 = pn->pn_left;
    if (!pn2) {
        // Catch parameter was omitted; just discard the exception.
        if (!emit1(JSOP_POP))
            return false;
    } else {
        switch (pn2->getKind()) {
          case ParseNodeKind::Array:
          case ParseNodeKind::Object:
            if (!emitDestructuringOps(pn2, DestructuringDeclaration))
                return false;
            if (!emit1(JSOP_POP))
                return false;
            break;

          case ParseNodeKind::Name:
            if (!emitLexicalInitialization(pn2))
                return false;
            if (!emit1(JSOP_POP))
                return false;
            break;

          default:
            MOZ_ASSERT(0);
        }
    }

    /* Emit the catch body. */
    return emitTree(pn->pn_right);
}

// Using MOZ_NEVER_INLINE in here is a workaround for llvm.org/pr14047. See the
// comment on EmitSwitch.
MOZ_NEVER_INLINE bool
BytecodeEmitter::emitTry(ParseNode* pn)
{
    ParseNode* catchScope = pn->pn_kid2;
    ParseNode* finallyNode = pn->pn_kid3;

    TryEmitter::Kind kind;
    if (catchScope) {
        if (finallyNode)
            kind = TryEmitter::TryCatchFinally;
        else
            kind = TryEmitter::TryCatch;
    } else {
        MOZ_ASSERT(finallyNode);
        kind = TryEmitter::TryFinally;
    }
    TryEmitter tryCatch(this, kind);

    if (!tryCatch.emitTry())
        return false;

    if (!emitTree(pn->pn_kid1))
        return false;

    // If this try has a catch block, emit it.
    if (catchScope) {
        // The emitted code for a catch block looks like:
        //
        // [pushlexicalenv]             only if any local aliased
        // exception
        // setlocal 0; pop              assign or possibly destructure exception
        // < catch block contents >
        // debugleaveblock
        // [poplexicalenv]              only if any local aliased
        // if there is a finally block:
        //   gosub <finally>
        //   goto <after finally>
        if (!tryCatch.emitCatch())
            return false;

        // Emit the lexical scope and catch body.
        MOZ_ASSERT(catchScope->isKind(ParseNodeKind::LexicalScope));
        if (!emitTree(catchScope))
            return false;
    }

    // Emit the finally handler, if there is one.
    if (finallyNode) {
        if (!tryCatch.emitFinally(Some(finallyNode->pn_pos.begin)))
            return false;

        if (!emitTree(finallyNode))
            return false;
    }

    if (!tryCatch.emitEnd())
        return false;

    return true;
}

bool
BytecodeEmitter::emitIf(ParseNode* pn)
{
    IfThenElseEmitter ifThenElse(this);

  if_again:
    /* Emit code for the condition before pushing stmtInfo. */
    if (!emitTreeInBranch(pn->pn_kid1))
        return false;

    ParseNode* elseNode = pn->pn_kid3;
    if (elseNode) {
        if (!ifThenElse.emitIfElse())
            return false;
    } else {
        if (!ifThenElse.emitIf())
            return false;
    }

    /* Emit code for the then part. */
    if (!emitTreeInBranch(pn->pn_kid2))
        return false;

    if (elseNode) {
        if (!ifThenElse.emitElse())
            return false;

        if (elseNode->isKind(ParseNodeKind::If)) {
            pn = elseNode;
            goto if_again;
        }

        /* Emit code for the else part. */
        if (!emitTreeInBranch(elseNode))
            return false;
    }

    if (!ifThenElse.emitEnd())
        return false;

    return true;
}

bool
BytecodeEmitter::emitHoistedFunctionsInList(ParseNode* list)
{
    MOZ_ASSERT(list->pn_xflags & PNX_FUNCDEFS);

    for (ParseNode* pn = list->pn_head; pn; pn = pn->pn_next) {
        ParseNode* maybeFun = pn;

        if (!sc->strict()) {
            while (maybeFun->isKind(ParseNodeKind::Label))
                maybeFun = maybeFun->as<LabeledStatement>().statement();
        }

        if (maybeFun->isKind(ParseNodeKind::Function) && maybeFun->functionIsHoisted()) {
            if (!emitTree(maybeFun))
                return false;
        }
    }

    return true;
}

bool
BytecodeEmitter::emitLexicalScopeBody(ParseNode* body, EmitLineNumberNote emitLineNote)
{
    if (body->isKind(ParseNodeKind::StatementList) && body->pn_xflags & PNX_FUNCDEFS) {
        // This block contains function statements whose definitions are
        // hoisted to the top of the block. Emit these as a separate pass
        // before the rest of the block.
        if (!emitHoistedFunctionsInList(body))
            return false;
    }

    // Line notes were updated by emitLexicalScope.
    return emitTree(body, ValueUsage::WantValue, emitLineNote);
}

// Using MOZ_NEVER_INLINE in here is a workaround for llvm.org/pr14047. See
// the comment on emitSwitch.
MOZ_NEVER_INLINE bool
BytecodeEmitter::emitLexicalScope(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(ParseNodeKind::LexicalScope));

    TDZCheckCache tdzCache(this);

    ParseNode* body = pn->scopeBody();
    if (pn->isEmptyScope())
        return emitLexicalScopeBody(body);

    // We are about to emit some bytecode for what the spec calls "declaration
    // instantiation". Assign these instructions to the opening `{` of the
    // block. (Using the location of each declaration we're instantiating is
    // too weird when stepping in the debugger.)
    if (!ParseNodeRequiresSpecialLineNumberNotes(body)) {
        if (!updateSourceCoordNotes(pn->pn_pos.begin))
            return false;
    }

    EmitterScope emitterScope(this);
    ScopeKind kind;
    if (body->isKind(ParseNodeKind::Catch)) {
        kind = (!body->pn_left || body->pn_left->isKind(ParseNodeKind::Name))
               ? ScopeKind::SimpleCatch
               : ScopeKind::Catch;
    } else {
        kind = ScopeKind::Lexical;
    }

    if (!emitterScope.enterLexical(this, kind, pn->scopeBindings()))
        return false;

    if (body->isKind(ParseNodeKind::For)) {
        // for loops need to emit {FRESHEN,RECREATE}LEXICALENV if there are
        // lexical declarations in the head. Signal this by passing a
        // non-nullptr lexical scope.
        if (!emitFor(body, &emitterScope))
            return false;
    } else {
        if (!emitLexicalScopeBody(body, SUPPRESS_LINENOTE))
            return false;
    }

    return emitterScope.leave(this);
}

bool
BytecodeEmitter::emitWith(ParseNode* pn)
{
    if (!emitTree(pn->pn_left))
        return false;

    EmitterScope emitterScope(this);
    if (!emitterScope.enterWith(this))
        return false;

    if (!emitTree(pn->pn_right))
        return false;

    return emitterScope.leave(this);
}

bool
BytecodeEmitter::emitCopyDataProperties(CopyOption option)
{
    DebugOnly<int32_t> depth = this->stackDepth;

    uint32_t argc;
    if (option == CopyOption::Filtered) {
        MOZ_ASSERT(depth > 2);                 // TARGET SOURCE SET
        argc = 3;

        if (!emitAtomOp(cx->names().CopyDataProperties,
                        JSOP_GETINTRINSIC))    // TARGET SOURCE SET COPYDATAPROPERTIES
        {
            return false;
        }
    } else {
        MOZ_ASSERT(depth > 1);                 // TARGET SOURCE
        argc = 2;

        if (!emitAtomOp(cx->names().CopyDataPropertiesUnfiltered,
                        JSOP_GETINTRINSIC))    // TARGET SOURCE COPYDATAPROPERTIES
        {
            return false;
        }
    }

    if (!emit1(JSOP_UNDEFINED))                // TARGET SOURCE *SET COPYDATAPROPERTIES UNDEFINED
        return false;
    if (!emit2(JSOP_PICK, argc + 1))           // SOURCE *SET COPYDATAPROPERTIES UNDEFINED TARGET
        return false;
    if (!emit2(JSOP_PICK, argc + 1))           // *SET COPYDATAPROPERTIES UNDEFINED TARGET SOURCE
        return false;
    if (option == CopyOption::Filtered) {
        if (!emit2(JSOP_PICK, argc + 1))       // COPYDATAPROPERTIES UNDEFINED TARGET SOURCE SET
            return false;
    }
    if (!emitCall(JSOP_CALL_IGNORES_RV, argc)) // IGNORED
        return false;
    checkTypeSet(JSOP_CALL_IGNORES_RV);

    if (!emit1(JSOP_POP))                      // -
        return false;

    MOZ_ASSERT(depth - int(argc) == this->stackDepth);
    return true;
}

bool
BytecodeEmitter::emitIterator()
{
    // Convert iterable to iterator.
    if (!emit1(JSOP_DUP))                                         // OBJ OBJ
        return false;
    if (!emit2(JSOP_SYMBOL, uint8_t(JS::SymbolCode::iterator)))   // OBJ OBJ @@ITERATOR
        return false;
    if (!emitElemOpBase(JSOP_CALLELEM))                           // OBJ ITERFN
        return false;
    if (!emit1(JSOP_SWAP))                                        // ITERFN OBJ
        return false;
    if (!emitCall(JSOP_CALLITER, 0))                              // ITER
        return false;
    checkTypeSet(JSOP_CALLITER);
    if (!emitCheckIsObj(CheckIsObjectKind::GetIterator))          // ITER
        return false;
    if (!emit1(JSOP_DUP))                                         // ITER ITER
        return false;
    if (!emitAtomOp(cx->names().next, JSOP_GETPROP))              // ITER NEXT
        return false;
    if (!emit1(JSOP_SWAP))                                        // NEXT ITER
        return false;
    return true;
}

bool
BytecodeEmitter::emitAsyncIterator()
{
    // Convert iterable to iterator.
    if (!emit1(JSOP_DUP))                                         // OBJ OBJ
        return false;
    if (!emit2(JSOP_SYMBOL, uint8_t(JS::SymbolCode::asyncIterator))) // OBJ OBJ @@ASYNCITERATOR
        return false;
    if (!emitElemOpBase(JSOP_CALLELEM))                           // OBJ ITERFN
        return false;

    IfThenElseEmitter ifAsyncIterIsUndefined(this);
    if (!emitPushNotUndefinedOrNull())                            // OBJ ITERFN !UNDEF-OR-NULL
        return false;
    if (!emit1(JSOP_NOT))                                         // OBJ ITERFN UNDEF-OR-NULL
        return false;
    if (!ifAsyncIterIsUndefined.emitIfElse())                     // OBJ ITERFN
        return false;

    if (!emit1(JSOP_POP))                                         // OBJ
        return false;
    if (!emit1(JSOP_DUP))                                         // OBJ OBJ
        return false;
    if (!emit2(JSOP_SYMBOL, uint8_t(JS::SymbolCode::iterator)))   // OBJ OBJ @@ITERATOR
        return false;
    if (!emitElemOpBase(JSOP_CALLELEM))                           // OBJ ITERFN
        return false;
    if (!emit1(JSOP_SWAP))                                        // ITERFN OBJ
        return false;
    if (!emitCall(JSOP_CALLITER, 0))                              // ITER
        return false;
    checkTypeSet(JSOP_CALLITER);
    if (!emitCheckIsObj(CheckIsObjectKind::GetIterator))          // ITER
        return false;

    if (!emit1(JSOP_DUP))                                         // ITER ITER
        return false;
    if (!emitAtomOp(cx->names().next, JSOP_GETPROP))              // ITER SYNCNEXT
        return false;

    if (!emit1(JSOP_TOASYNCITER))                                 // ITER
        return false;

    if (!ifAsyncIterIsUndefined.emitElse())                       // OBJ ITERFN
        return false;

    if (!emit1(JSOP_SWAP))                                        // ITERFN OBJ
        return false;
    if (!emitCall(JSOP_CALLITER, 0))                              // ITER
        return false;
    checkTypeSet(JSOP_CALLITER);
    if (!emitCheckIsObj(CheckIsObjectKind::GetIterator))          // ITER
        return false;

    if (!ifAsyncIterIsUndefined.emitEnd())                        // ITER
        return false;

    if (!emit1(JSOP_DUP))                                         // ITER ITER
        return false;
    if (!emitAtomOp(cx->names().next, JSOP_GETPROP))              // ITER NEXT
        return false;
    if (!emit1(JSOP_SWAP))                                        // NEXT ITER
        return false;

    return true;
}

bool
BytecodeEmitter::emitSpread(bool allowSelfHosted)
{
    LoopControl loopInfo(this, StatementKind::Spread);

    // Jump down to the loop condition to minimize overhead assuming at least
    // one iteration, as the other loop forms do.  Annotate so IonMonkey can
    // find the loop-closing jump.
    unsigned noteIndex;
    if (!newSrcNote(SRC_FOR_OF, &noteIndex))
        return false;

    // Jump down to the loop condition to minimize overhead, assuming at least
    // one iteration.  (This is also what we do for loops; whether this
    // assumption holds for spreads is an unanswered question.)
    JumpList initialJump;
    if (!emitJump(JSOP_GOTO, &initialJump))               // NEXT ITER ARR I (during the goto)
        return false;

    JumpTarget top{ -1 };
    if (!emitLoopHead(nullptr, &top))                     // NEXT ITER ARR I
        return false;

    // When we enter the goto above, we have NEXT ITER ARR I on the stack. But
    // when we reach this point on the loop backedge (if spreading produces at
    // least one value), we've additionally pushed a RESULT iteration value.
    // Increment manually to reflect this.
    this->stackDepth++;

    JumpList beq;
    JumpTarget breakTarget{ -1 };
    {
#ifdef DEBUG
        auto loopDepth = this->stackDepth;
#endif

        // Emit code to assign result.value to the iteration variable.
        if (!emitAtomOp(cx->names().value, JSOP_GETPROP)) // NEXT ITER ARR I VALUE
            return false;
        if (!emit1(JSOP_INITELEM_INC))                    // NEXT ITER ARR (I+1)
            return false;

        MOZ_ASSERT(this->stackDepth == loopDepth - 1);

        // Spread operations can't contain |continue|, so don't bother setting loop
        // and enclosing "update" offsets, as we do with for-loops.

        // COME FROM the beginning of the loop to here.
        if (!emitLoopEntry(nullptr, initialJump))         // NEXT ITER ARR I
            return false;

        if (!emitDupAt(3))                                // NEXT ITER ARR I NEXT
            return false;
        if (!emitDupAt(3))                                // NEXT ITER ARR I NEXT ITER
            return false;
        if (!emitIteratorNext(nullptr, IteratorKind::Sync, allowSelfHosted))  // ITER ARR I RESULT
            return false;
        if (!emit1(JSOP_DUP))                             // NEXT ITER ARR I RESULT RESULT
            return false;
        if (!emitAtomOp(cx->names().done, JSOP_GETPROP))  // NEXT ITER ARR I RESULT DONE
            return false;

        if (!emitBackwardJump(JSOP_IFEQ, top, &beq, &breakTarget)) // NEXT ITER ARR I RESULT
            return false;

        MOZ_ASSERT(this->stackDepth == loopDepth);
    }

    // Let Ion know where the closing jump of this loop is.
    if (!setSrcNoteOffset(noteIndex, 0, beq.offset - initialJump.offset))
        return false;

    // No breaks or continues should occur in spreads.
    MOZ_ASSERT(loopInfo.breaks.offset == -1);
    MOZ_ASSERT(loopInfo.continues.offset == -1);

    if (!tryNoteList.append(JSTRY_FOR_OF, stackDepth, top.offset, breakTarget.offset))
        return false;

    if (!emit2(JSOP_PICK, 4))                             // ITER ARR FINAL_INDEX RESULT NEXT
        return false;
    if (!emit2(JSOP_PICK, 4))                             // ARR FINAL_INDEX RESULT NEXT ITER
        return false;

    return emitPopN(3);                                   // ARR FINAL_INDEX
}

bool
BytecodeEmitter::emitInitializeForInOrOfTarget(ParseNode* forHead)
{
    MOZ_ASSERT(forHead->isKind(ParseNodeKind::ForIn) ||
               forHead->isKind(ParseNodeKind::ForOf));
    MOZ_ASSERT(forHead->isArity(PN_TERNARY));

    MOZ_ASSERT(this->stackDepth >= 1,
               "must have a per-iteration value for initializing");

    ParseNode* target = forHead->pn_kid1;
    MOZ_ASSERT(!forHead->pn_kid2);

    // If the for-in/of loop didn't have a variable declaration, per-loop
    // initialization is just assigning the iteration value to a target
    // expression.
    if (!parser.isDeclarationList(target))
        return emitAssignment(target, ParseNodeKind::Assign, nullptr); // ... ITERVAL

    // Otherwise, per-loop initialization is (possibly) declaration
    // initialization.  If the declaration is a lexical declaration, it must be
    // initialized.  If the declaration is a variable declaration, an
    // assignment to that name (which does *not* necessarily assign to the
    // variable!) must be generated.

    if (!updateSourceCoordNotes(target->pn_pos.begin))
        return false;

    MOZ_ASSERT(target->isForLoopDeclaration());
    target = parser.singleBindingFromDeclaration(target);

    if (target->isKind(ParseNodeKind::Name)) {
        auto emitSwapScopeAndRhs = [](BytecodeEmitter* bce, const NameLocation&,
                                      bool emittedBindOp)
        {
            if (emittedBindOp) {
                // Per-iteration initialization in for-in/of loops computes the
                // iteration value *before* initializing.  Thus the
                // initializing value may be buried under a bind-specific value
                // on the stack.  Swap it to the top of the stack.
                MOZ_ASSERT(bce->stackDepth >= 2);
                return bce->emit1(JSOP_SWAP);
            }

            // In cases of emitting a frame slot or environment slot,
            // nothing needs be done.
            MOZ_ASSERT(bce->stackDepth >= 1);
            return true;
        };

        // The caller handles removing the iteration value from the stack.
        return emitInitializeName(target, emitSwapScopeAndRhs);
    }

    MOZ_ASSERT(!target->isKind(ParseNodeKind::Assign),
               "for-in/of loop destructuring declarations can't have initializers");

    MOZ_ASSERT(target->isKind(ParseNodeKind::Array) ||
               target->isKind(ParseNodeKind::Object));
    return emitDestructuringOps(target, DestructuringDeclaration);
}

bool
BytecodeEmitter::emitForOf(ParseNode* forOfLoop, EmitterScope* headLexicalEmitterScope)
{
    MOZ_ASSERT(forOfLoop->isKind(ParseNodeKind::For));
    MOZ_ASSERT(forOfLoop->isArity(PN_BINARY));

    ParseNode* forOfHead = forOfLoop->pn_left;
    MOZ_ASSERT(forOfHead->isKind(ParseNodeKind::ForOf));
    MOZ_ASSERT(forOfHead->isArity(PN_TERNARY));

    unsigned iflags = forOfLoop->pn_iflags;
    IteratorKind iterKind = (iflags & JSITER_FORAWAITOF)
                            ? IteratorKind::Async
                            : IteratorKind::Sync;
    MOZ_ASSERT_IF(iterKind == IteratorKind::Async, sc->asFunctionBox());
    MOZ_ASSERT_IF(iterKind == IteratorKind::Async, sc->asFunctionBox()->isAsync());

    ParseNode* forHeadExpr = forOfHead->pn_kid3;

    // Certain builtins (e.g. Array.from) are implemented in self-hosting
    // as for-of loops.
    bool allowSelfHostedIter = false;
    if (emitterMode == BytecodeEmitter::SelfHosting &&
        forHeadExpr->isKind(ParseNodeKind::Call) &&
        forHeadExpr->pn_head->name() == cx->names().allowContentIter)
    {
        allowSelfHostedIter = true;
    }

    // Evaluate the expression being iterated. The forHeadExpr should use a
    // distinct TDZCheckCache to evaluate since (abstractly) it runs in its own
    // LexicalEnvironment.
    if (!emitTreeInBranch(forHeadExpr))                   // ITERABLE
        return false;
    if (iterKind == IteratorKind::Async) {
        if (!emitAsyncIterator())                         // NEXT ITER
            return false;
    } else {
        if (!emitIterator())                              // NEXT ITER
            return false;
    }

    int32_t iterDepth = stackDepth;

    // For-of loops have the iterator next method, the iterator itself, and
    // the result.value on the stack.
    // Push an undefined to balance the stack.
    if (!emit1(JSOP_UNDEFINED))                           // NEXT ITER UNDEF
        return false;

    ForOfLoopControl loopInfo(this, iterDepth, allowSelfHostedIter, iterKind);

    // Annotate so IonMonkey can find the loop-closing jump.
    unsigned noteIndex;
    if (!newSrcNote(SRC_FOR_OF, &noteIndex))
        return false;

    JumpList initialJump;
    if (!emitJump(JSOP_GOTO, &initialJump))               // NEXT ITER UNDEF
        return false;

    JumpTarget top{ -1 };
    if (!emitLoopHead(nullptr, &top))                     // NEXT ITER UNDEF
        return false;

    // If the loop had an escaping lexical declaration, replace the current
    // environment with an dead zoned one to implement TDZ semantics.
    if (headLexicalEmitterScope) {
        // The environment chain only includes an environment for the for-of
        // loop head *if* a scope binding is captured, thereby requiring
        // recreation each iteration. If a lexical scope exists for the head,
        // it must be the innermost one. If that scope has closed-over
        // bindings inducing an environment, recreate the current environment.
        DebugOnly<ParseNode*> forOfTarget = forOfHead->pn_kid1;
        MOZ_ASSERT(forOfTarget->isKind(ParseNodeKind::Let) ||
                   forOfTarget->isKind(ParseNodeKind::Const));
        MOZ_ASSERT(headLexicalEmitterScope == innermostEmitterScope());
        MOZ_ASSERT(headLexicalEmitterScope->scope(this)->kind() == ScopeKind::Lexical);

        if (headLexicalEmitterScope->hasEnvironment()) {
            if (!emit1(JSOP_RECREATELEXICALENV))          // NEXT ITER UNDEF
                return false;
        }

        // For uncaptured bindings, put them back in TDZ.
        if (!headLexicalEmitterScope->deadZoneFrameSlots(this))
            return false;
    }

    JumpList beq;
    JumpTarget breakTarget{ -1 };
    {
#ifdef DEBUG
        auto loopDepth = this->stackDepth;
#endif

        // Make sure this code is attributed to the "for".
        if (!updateSourceCoordNotes(forOfHead->pn_pos.begin))
            return false;

        if (!emit1(JSOP_POP))                             // NEXT ITER
            return false;
        if (!emit1(JSOP_DUP2))                            // NEXT ITER NEXT ITER
            return false;

        if (!emitIteratorNext(forOfHead, iterKind, allowSelfHostedIter))
            return false;                                 // NEXT ITER RESULT

        if (!emit1(JSOP_DUP))                             // NEXT ITER RESULT RESULT
            return false;
        if (!emitAtomOp(cx->names().done, JSOP_GETPROP))  // NEXT ITER RESULT DONE
            return false;

        IfThenElseEmitter ifDone(this);

        if (!ifDone.emitIf())                             // NEXT ITER RESULT
            return false;

        // Remove RESULT from the stack to release it.
        if (!emit1(JSOP_POP))                             // NEXT ITER
            return false;
        if (!emit1(JSOP_UNDEFINED))                       // NEXT ITER UNDEF
            return false;

        // If the iteration is done, leave loop here, instead of the branch at
        // the end of the loop.
        if (!loopInfo.emitSpecialBreakForDone(this))      // NEXT ITER UNDEF
            return false;

        if (!ifDone.emitEnd())                            // NEXT ITER RESULT
            return false;

        // Emit code to assign result.value to the iteration variable.
        //
        // Note that ES 13.7.5.13, step 5.c says getting result.value does not
        // call IteratorClose, so start JSTRY_ITERCLOSE after the GETPROP.
        if (!emitAtomOp(cx->names().value, JSOP_GETPROP)) // NEXT ITER VALUE
            return false;

        if (!loopInfo.emitBeginCodeNeedingIteratorClose(this))
            return false;

        if (!emitInitializeForInOrOfTarget(forOfHead))    // NEXT ITER VALUE
            return false;

        MOZ_ASSERT(stackDepth == loopDepth,
                   "the stack must be balanced around the initializing "
                   "operation");

        // Remove VALUE from the stack to release it.
        if (!emit1(JSOP_POP))                             // NEXT ITER
            return false;
        if (!emit1(JSOP_UNDEFINED))                       // NEXT ITER UNDEF
            return false;

        // Perform the loop body.
        ParseNode* forBody = forOfLoop->pn_right;
        if (!emitTree(forBody))                           // NEXT ITER UNDEF
            return false;

        MOZ_ASSERT(stackDepth == loopDepth,
                   "the stack must be balanced around the for-of body");

        if (!loopInfo.emitEndCodeNeedingIteratorClose(this))
            return false;

        // Set offset for continues.
        loopInfo.continueTarget = { offset() };

        if (!emitLoopEntry(forHeadExpr, initialJump))     // NEXT ITER UNDEF
            return false;

        if (!emit1(JSOP_FALSE))                           // NEXT ITER UNDEF FALSE
            return false;
        if (!emitBackwardJump(JSOP_IFEQ, top, &beq, &breakTarget))
            return false;                                 // NEXT ITER UNDEF

        MOZ_ASSERT(this->stackDepth == loopDepth);
    }

    // Let Ion know where the closing jump of this loop is.
    if (!setSrcNoteOffset(noteIndex, 0, beq.offset - initialJump.offset))
        return false;

    if (!loopInfo.patchBreaksAndContinues(this))
        return false;

    if (!tryNoteList.append(JSTRY_FOR_OF, stackDepth, top.offset, breakTarget.offset))
        return false;

    return emitPopN(3);                                   //
}

bool
BytecodeEmitter::emitForIn(ParseNode* forInLoop, EmitterScope* headLexicalEmitterScope)
{
    MOZ_ASSERT(forInLoop->isKind(ParseNodeKind::For));
    MOZ_ASSERT(forInLoop->isArity(PN_BINARY));
    MOZ_ASSERT(forInLoop->isOp(JSOP_ITER));

    ParseNode* forInHead = forInLoop->pn_left;
    MOZ_ASSERT(forInHead->isKind(ParseNodeKind::ForIn));
    MOZ_ASSERT(forInHead->isArity(PN_TERNARY));

    // Annex B: Evaluate the var-initializer expression if present.
    // |for (var i = initializer in expr) { ... }|
    ParseNode* forInTarget = forInHead->pn_kid1;
    if (parser.isDeclarationList(forInTarget)) {
        ParseNode* decl = parser.singleBindingFromDeclaration(forInTarget);
        if (decl->isKind(ParseNodeKind::Name)) {
            if (ParseNode* initializer = decl->expr()) {
                MOZ_ASSERT(forInTarget->isKind(ParseNodeKind::Var),
                           "for-in initializers are only permitted for |var| declarations");

                if (!updateSourceCoordNotes(decl->pn_pos.begin))
                    return false;

                auto emitRhs = [decl, initializer](BytecodeEmitter* bce, const NameLocation&, bool) {
                    return bce->emitInitializer(initializer, decl);
                };

                if (!emitInitializeName(decl, emitRhs))
                    return false;

                // Pop the initializer.
                if (!emit1(JSOP_POP))
                    return false;
            }
        }
    }

    // Evaluate the expression being iterated.
    ParseNode* expr = forInHead->pn_kid3;
    if (!emitTreeInBranch(expr))                          // EXPR
        return false;

    MOZ_ASSERT(forInLoop->pn_iflags == 0);

    if (!emit1(JSOP_ITER))                                // ITER
        return false;

    // For-in loops have both the iterator and the value on the stack. Push
    // undefined to balance the stack.
    if (!emit1(JSOP_UNDEFINED))                           // ITER ITERVAL
        return false;

    LoopControl loopInfo(this, StatementKind::ForInLoop);

    /* Annotate so IonMonkey can find the loop-closing jump. */
    unsigned noteIndex;
    if (!newSrcNote(SRC_FOR_IN, &noteIndex))
        return false;

    // Jump down to the loop condition to minimize overhead (assuming at least
    // one iteration, just like the other loop forms).
    JumpList initialJump;
    if (!emitJump(JSOP_GOTO, &initialJump))               // ITER ITERVAL
        return false;

    JumpTarget top{ -1 };
    if (!emitLoopHead(nullptr, &top))                     // ITER ITERVAL
        return false;

    // If the loop had an escaping lexical declaration, replace the current
    // environment with an dead zoned one to implement TDZ semantics.
    if (headLexicalEmitterScope) {
        // The environment chain only includes an environment for the for-in
        // loop head *if* a scope binding is captured, thereby requiring
        // recreation each iteration. If a lexical scope exists for the head,
        // it must be the innermost one. If that scope has closed-over
        // bindings inducing an environment, recreate the current environment.
        MOZ_ASSERT(forInTarget->isKind(ParseNodeKind::Let) ||
                   forInTarget->isKind(ParseNodeKind::Const));
        MOZ_ASSERT(headLexicalEmitterScope == innermostEmitterScope());
        MOZ_ASSERT(headLexicalEmitterScope->scope(this)->kind() == ScopeKind::Lexical);

        if (headLexicalEmitterScope->hasEnvironment()) {
            if (!emit1(JSOP_RECREATELEXICALENV))          // ITER ITERVAL
                return false;
        }

        // For uncaptured bindings, put them back in TDZ.
        if (!headLexicalEmitterScope->deadZoneFrameSlots(this))
            return false;
    }

    {
#ifdef DEBUG
        auto loopDepth = this->stackDepth;
#endif
        MOZ_ASSERT(loopDepth >= 2);

        if (!emit1(JSOP_ITERNEXT))                        // ITER ITERVAL
            return false;

        if (!emitInitializeForInOrOfTarget(forInHead))    // ITER ITERVAL
            return false;

        MOZ_ASSERT(this->stackDepth == loopDepth,
                   "iterator and iterval must be left on the stack");
    }

    // Perform the loop body.
    ParseNode* forBody = forInLoop->pn_right;
    if (!emitTree(forBody))                               // ITER ITERVAL
        return false;

    // Set offset for continues.
    loopInfo.continueTarget = { offset() };

    // Make sure this code is attributed to the "for".
    if (!updateSourceCoordNotes(forInHead->pn_pos.begin))
        return false;

    if (!emitLoopEntry(nullptr, initialJump))             // ITER ITERVAL
        return false;
    if (!emit1(JSOP_POP))                                 // ITER
        return false;
    if (!emit1(JSOP_MOREITER))                            // ITER NEXTITERVAL?
        return false;
    if (!emit1(JSOP_ISNOITER))                            // ITER NEXTITERVAL? ISNOITER
        return false;

    JumpList beq;
    JumpTarget breakTarget{ -1 };
    if (!emitBackwardJump(JSOP_IFEQ, top, &beq, &breakTarget))
        return false;                                     // ITER NEXTITERVAL

    // Set the srcnote offset so we can find the closing jump.
    if (!setSrcNoteOffset(noteIndex, 0, beq.offset - initialJump.offset))
        return false;

    if (!loopInfo.patchBreaksAndContinues(this))
        return false;

    // Pop the enumeration value.
    if (!emit1(JSOP_POP))                                 // ITER
        return false;

    if (!tryNoteList.append(JSTRY_FOR_IN, this->stackDepth, top.offset, offset()))
        return false;

    return emit1(JSOP_ENDITER);                           //
}

/* C-style `for (init; cond; update) ...` loop. */
bool
BytecodeEmitter::emitCStyleFor(ParseNode* pn, EmitterScope* headLexicalEmitterScope)
{
    LoopControl loopInfo(this, StatementKind::ForLoop);

    ParseNode* forHead = pn->pn_left;
    ParseNode* forBody = pn->pn_right;

    // If the head of this for-loop declared any lexical variables, the parser
    // wrapped this ParseNodeKind::For node in a ParseNodeKind::LexicalScope
    // representing the implicit scope of those variables. By the time we get here,
    // we have already entered that scope. So far, so good.
    //
    // ### Scope freshening
    //
    // Each iteration of a `for (let V...)` loop creates a fresh loop variable
    // binding for V, even if the loop is a C-style `for(;;)` loop:
    //
    //     var funcs = [];
    //     for (let i = 0; i < 2; i++)
    //         funcs.push(function() { return i; });
    //     assertEq(funcs[0](), 0);  // the two closures capture...
    //     assertEq(funcs[1](), 1);  // ...two different `i` bindings
    //
    // This is implemented by "freshening" the implicit block -- changing the
    // scope chain to a fresh clone of the instantaneous block object -- each
    // iteration, just before evaluating the "update" in for(;;) loops.
    //
    // No freshening occurs in `for (const ...;;)` as there's no point: you
    // can't reassign consts. This is observable through the Debugger API. (The
    // ES6 spec also skips cloning the environment in this case.)
    bool forLoopRequiresFreshening = false;
    if (ParseNode* init = forHead->pn_kid1) {
        // Emit the `init` clause, whether it's an expression or a variable
        // declaration. (The loop variables were hoisted into an enclosing
        // scope, but we still need to emit code for the initializers.)
        if (!updateSourceCoordNotes(init->pn_pos.begin))
            return false;
        if (init->isForLoopDeclaration()) {
            if (!emitTree(init))
                return false;
        } else {
            // 'init' is an expression, not a declaration. emitTree left its
            // value on the stack.
            if (!emitTree(init, ValueUsage::IgnoreValue))
                return false;
            if (!emit1(JSOP_POP))
                return false;
        }

        // ES 13.7.4.8 step 2. The initial freshening.
        //
        // If an initializer let-declaration may be captured during loop iteration,
        // the current scope has an environment.  If so, freshen the current
        // environment to expose distinct bindings for each loop iteration.
        forLoopRequiresFreshening = init->isKind(ParseNodeKind::Let) && headLexicalEmitterScope;
        if (forLoopRequiresFreshening) {
            // The environment chain only includes an environment for the for(;;)
            // loop head's let-declaration *if* a scope binding is captured, thus
            // requiring a fresh environment each iteration. If a lexical scope
            // exists for the head, it must be the innermost one. If that scope
            // has closed-over bindings inducing an environment, recreate the
            // current environment.
            MOZ_ASSERT(headLexicalEmitterScope == innermostEmitterScope());
            MOZ_ASSERT(headLexicalEmitterScope->scope(this)->kind() == ScopeKind::Lexical);

            if (headLexicalEmitterScope->hasEnvironment()) {
                if (!emit1(JSOP_FRESHENLEXICALENV))
                    return false;
            }
        }
    }

    /*
     * NB: the SRC_FOR note has offsetBias 1 (JSOP_NOP_LENGTH).
     * Use tmp to hold the biased srcnote "top" offset, which differs
     * from the top local variable by the length of the JSOP_GOTO
     * emitted in between tmp and top if this loop has a condition.
     */
    unsigned noteIndex;
    if (!newSrcNote(SRC_FOR, &noteIndex))
        return false;
    if (!emit1(JSOP_NOP))
        return false;
    ptrdiff_t tmp = offset();

    JumpList jmp;
    if (forHead->pn_kid2) {
        /* Goto the loop condition, which branches back to iterate. */
        if (!emitJump(JSOP_GOTO, &jmp))
            return false;
    }

    /* Emit code for the loop body. */
    JumpTarget top{ -1 };
    if (!emitLoopHead(forBody, &top))
        return false;
    if (jmp.offset == -1 && !emitLoopEntry(forBody, jmp))
        return false;

    if (!emitTreeInBranch(forBody))
        return false;

    // Set loop and enclosing "update" offsets, for continue.  Note that we
    // continue to immediately *before* the block-freshening: continuing must
    // refresh the block.
    if (!emitJumpTarget(&loopInfo.continueTarget))
        return false;

    // ES 13.7.4.8 step 3.e. The per-iteration freshening.
    if (forLoopRequiresFreshening) {
        MOZ_ASSERT(headLexicalEmitterScope == innermostEmitterScope());
        MOZ_ASSERT(headLexicalEmitterScope->scope(this)->kind() == ScopeKind::Lexical);

        if (headLexicalEmitterScope->hasEnvironment()) {
            if (!emit1(JSOP_FRESHENLEXICALENV))
                return false;
        }
    }

    // Check for update code to do before the condition (if any).
    // The update code may not be executed at all; it needs its own TDZ cache.
    if (ParseNode* update = forHead->pn_kid3) {
        TDZCheckCache tdzCache(this);

        if (!updateSourceCoordNotes(update->pn_pos.begin))
            return false;
        if (!emitTree(update, ValueUsage::IgnoreValue))
            return false;
        if (!emit1(JSOP_POP))
            return false;

        /* Restore the absolute line number for source note readers. */
        uint32_t lineNum = parser.tokenStream().srcCoords.lineNum(pn->pn_pos.end);
        if (currentLine() != lineNum) {
            if (!newSrcNote2(SRC_SETLINE, ptrdiff_t(lineNum)))
                return false;
            current->currentLine = lineNum;
            current->lastColumn = 0;
        }
    }

    ptrdiff_t tmp3 = offset();

    if (forHead->pn_kid2) {
        /* Fix up the goto from top to target the loop condition. */
        MOZ_ASSERT(jmp.offset >= 0);
        if (!emitLoopEntry(forHead->pn_kid2, jmp))
            return false;

        if (!emitTree(forHead->pn_kid2))
            return false;
    } else if (!forHead->pn_kid3) {
        // If there is no condition clause and no update clause, mark
        // the loop-ending "goto" with the location of the "for".
        // This ensures that the debugger will stop on each loop
        // iteration.
        if (!updateSourceCoordNotes(pn->pn_pos.begin))
            return false;
    }

    /* Set the first note offset so we can find the loop condition. */
    if (!setSrcNoteOffset(noteIndex, 0, tmp3 - tmp))
        return false;
    if (!setSrcNoteOffset(noteIndex, 1, loopInfo.continueTarget.offset - tmp))
        return false;

    /* If no loop condition, just emit a loop-closing jump. */
    JumpList beq;
    JumpTarget breakTarget{ -1 };
    if (!emitBackwardJump(forHead->pn_kid2 ? JSOP_IFNE : JSOP_GOTO, top, &beq, &breakTarget))
        return false;

    /* The third note offset helps us find the loop-closing jump. */
    if (!setSrcNoteOffset(noteIndex, 2, beq.offset - tmp))
        return false;

    if (!tryNoteList.append(JSTRY_LOOP, stackDepth, top.offset, breakTarget.offset))
        return false;

    if (!loopInfo.patchBreaksAndContinues(this))
        return false;

    return true;
}

bool
BytecodeEmitter::emitFor(ParseNode* pn, EmitterScope* headLexicalEmitterScope)
{
    MOZ_ASSERT(pn->isKind(ParseNodeKind::For));

    if (pn->pn_left->isKind(ParseNodeKind::ForHead))
        return emitCStyleFor(pn, headLexicalEmitterScope);

    if (!updateLineNumberNotes(pn->pn_pos.begin))
        return false;

    if (pn->pn_left->isKind(ParseNodeKind::ForIn))
        return emitForIn(pn, headLexicalEmitterScope);

    MOZ_ASSERT(pn->pn_left->isKind(ParseNodeKind::ForOf));
    return emitForOf(pn, headLexicalEmitterScope);
}

MOZ_NEVER_INLINE bool
BytecodeEmitter::emitFunction(ParseNode* pn, bool needsProto)
{
    FunctionBox* funbox = pn->pn_funbox;
    RootedFunction fun(cx, funbox->function());
    RootedAtom name(cx, fun->explicitName());
    MOZ_ASSERT_IF(fun->isInterpretedLazy(), fun->lazyScript());

    // Set the |wasEmitted| flag in the funbox once the function has been
    // emitted. Function definitions that need hoisting to the top of the
    // function will be seen by emitFunction in two places.
    if (funbox->wasEmitted) {
        // Annex B block-scoped functions are hoisted like any other
        // block-scoped function to the top of their scope. When their
        // definitions are seen for the second time, we need to emit the
        // assignment that assigns the function to the outer 'var' binding.
        if (funbox->isAnnexB) {
            auto emitRhs = [&name](BytecodeEmitter* bce, const NameLocation&, bool) {
                // The RHS is the value of the lexically bound name in the
                // innermost scope.
                return bce->emitGetName(name);
            };

            // Get the location of the 'var' binding in the body scope. The
            // name must be found, else there is a bug in the Annex B handling
            // in Parser.
            //
            // In sloppy eval contexts, this location is dynamic.
            Maybe<NameLocation> lhsLoc = locationOfNameBoundInScope(name, varEmitterScope);

            // If there are parameter expressions, the var name could be a
            // parameter.
            if (!lhsLoc && sc->isFunctionBox() && sc->asFunctionBox()->hasExtraBodyVarScope())
                lhsLoc = locationOfNameBoundInScope(name, varEmitterScope->enclosingInFrame());

            if (!lhsLoc) {
                lhsLoc = Some(NameLocation::DynamicAnnexBVar());
            } else {
                MOZ_ASSERT(lhsLoc->bindingKind() == BindingKind::Var ||
                           lhsLoc->bindingKind() == BindingKind::FormalParameter ||
                           (lhsLoc->bindingKind() == BindingKind::Let &&
                            sc->asFunctionBox()->hasParameterExprs));
            }

            if (!emitSetOrInitializeNameAtLocation(name, *lhsLoc, emitRhs, false))
                return false;
            if (!emit1(JSOP_POP))
                return false;
        }

        MOZ_ASSERT_IF(fun->hasScript(), fun->nonLazyScript());
        MOZ_ASSERT(pn->functionIsHoisted());
        return true;
    }

    funbox->wasEmitted = true;

    // Mark as singletons any function which will only be executed once, or
    // which is inner to a lambda we only expect to run once. In the latter
    // case, if the lambda runs multiple times then CloneFunctionObject will
    // make a deep clone of its contents.
    if (fun->isInterpreted()) {
        bool singleton = checkRunOnceContext();
        if (!JSFunction::setTypeForScriptedFunction(cx, fun, singleton))
            return false;

        SharedContext* outersc = sc;
        if (fun->isInterpretedLazy()) {
            // We need to update the static scope chain regardless of whether
            // the LazyScript has already been initialized, due to the case
            // where we previously successfully compiled an inner function's
            // lazy script but failed to compile the outer script after the
            // fact. If we attempt to compile the outer script again, the
            // static scope chain will be newly allocated and will mismatch
            // the previously compiled LazyScript's.
            ScriptSourceObject* source = &script->sourceObject()->as<ScriptSourceObject>();
            fun->lazyScript()->setEnclosingScopeAndSource(innermostScope(), source);
            if (emittingRunOnceLambda)
                fun->lazyScript()->setTreatAsRunOnce();
        } else {
            MOZ_ASSERT_IF(outersc->strict(), funbox->strictScript);

            // Inherit most things (principals, version, etc) from the
            // parent.  Use default values for the rest.
            Rooted<JSScript*> parent(cx, script);
            MOZ_ASSERT(parent->mutedErrors() == parser.options().mutedErrors());
            const TransitiveCompileOptions& transitiveOptions = parser.options();
            CompileOptions options(cx, transitiveOptions);

            Rooted<JSObject*> sourceObject(cx, script->sourceObject());
            Rooted<JSScript*> script(cx, JSScript::Create(cx, options, sourceObject,
                                                          funbox->bufStart, funbox->bufEnd,
                                                          funbox->toStringStart,
                                                          funbox->toStringEnd));
            if (!script)
                return false;

            BytecodeEmitter bce2(this, parser, funbox, script, /* lazyScript = */ nullptr,
                                 pn->pn_pos, emitterMode);
            if (!bce2.init())
                return false;

            /* We measured the max scope depth when we parsed the function. */
            if (!bce2.emitFunctionScript(pn->pn_body))
                return false;

            if (funbox->isLikelyConstructorWrapper())
                script->setLikelyConstructorWrapper();
        }

        if (outersc->isFunctionBox())
            outersc->asFunctionBox()->setHasInnerFunctions();
    } else {
        MOZ_ASSERT(IsAsmJSModule(fun));
    }

    // Make the function object a literal in the outer script's pool.
    unsigned index = objectList.add(pn->pn_funbox);

    // Non-hoisted functions simply emit their respective op.
    if (!pn->functionIsHoisted()) {
        // JSOP_LAMBDA_ARROW is always preceded by a new.target
        MOZ_ASSERT(fun->isArrow() == (pn->getOp() == JSOP_LAMBDA_ARROW));
        if (funbox->isAsync()) {
            MOZ_ASSERT(!needsProto);
            return emitAsyncWrapper(index, funbox->needsHomeObject(), fun->isArrow(),
                                    fun->isGenerator());
        }

        if (fun->isArrow()) {
            if (sc->allowNewTarget()) {
                if (!emit1(JSOP_NEWTARGET))
                    return false;
            } else {
                if (!emit1(JSOP_NULL))
                    return false;
            }
        }

        if (needsProto) {
            MOZ_ASSERT(pn->getOp() == JSOP_LAMBDA);
            pn->setOp(JSOP_FUNWITHPROTO);
        }

        if (pn->getOp() == JSOP_DEFFUN) {
            if (!emitIndex32(JSOP_LAMBDA, index))
                return false;
            return emit1(JSOP_DEFFUN);
        }

        // This is a FunctionExpression, ArrowFunctionExpression, or class
        // constructor. Emit the single instruction (without location info).
        return emitIndex32(pn->getOp(), index);
    }

    MOZ_ASSERT(!needsProto);

    bool topLevelFunction;
    if (sc->isFunctionBox() || (sc->isEvalContext() && sc->strict())) {
        // No nested functions inside other functions are top-level.
        topLevelFunction = false;
    } else {
        // In sloppy eval scripts, top-level functions in are accessed
        // dynamically. In global and module scripts, top-level functions are
        // those bound in the var scope.
        NameLocation loc = lookupName(name);
        topLevelFunction = loc.kind() == NameLocation::Kind::Dynamic ||
                           loc.bindingKind() == BindingKind::Var;
    }

    if (topLevelFunction) {
        if (sc->isModuleContext()) {
            // For modules, we record the function and instantiate the binding
            // during ModuleInstantiate(), before the script is run.

            RootedModuleObject module(cx, sc->asModuleContext()->module());
            if (!module->noteFunctionDeclaration(cx, name, fun))
                return false;
        } else {
            MOZ_ASSERT(sc->isGlobalContext() || sc->isEvalContext());
            MOZ_ASSERT(pn->getOp() == JSOP_NOP);
            switchToPrologue();
            if (funbox->isAsync()) {
                if (!emitAsyncWrapper(index, fun->isMethod(), fun->isArrow(),
                                      fun->isGenerator()))
                {
                    return false;
                }
            } else {
                if (!emitIndex32(JSOP_LAMBDA, index))
                    return false;
            }
            if (!emit1(JSOP_DEFFUN))
                return false;
            switchToMain();
        }
    } else {
        // For functions nested within functions and blocks, make a lambda and
        // initialize the binding name of the function in the current scope.

        bool isAsync = funbox->isAsync();
        bool isGenerator = funbox->isGenerator();
        auto emitLambda = [index, isAsync, isGenerator](BytecodeEmitter* bce,
                                                        const NameLocation&, bool) {
            if (isAsync) {
                return bce->emitAsyncWrapper(index, /* needsHomeObject = */ false,
                                             /* isArrow = */ false, isGenerator);
            }
            return bce->emitIndexOp(JSOP_LAMBDA, index);
        };

        if (!emitInitializeName(name, emitLambda))
            return false;
        if (!emit1(JSOP_POP))
            return false;
    }

    return true;
}

bool
BytecodeEmitter::emitAsyncWrapperLambda(unsigned index, bool isArrow) {
    if (isArrow) {
        if (sc->allowNewTarget()) {
            if (!emit1(JSOP_NEWTARGET))
                return false;
        } else {
            if (!emit1(JSOP_NULL))
                return false;
        }
        if (!emitIndex32(JSOP_LAMBDA_ARROW, index))
            return false;
    } else {
        if (!emitIndex32(JSOP_LAMBDA, index))
            return false;
    }

    return true;
}

bool
BytecodeEmitter::emitAsyncWrapper(unsigned index, bool needsHomeObject, bool isArrow,
                                  bool isGenerator)
{
    // needsHomeObject can be true for propertyList for extended class.
    // In that case push both unwrapped and wrapped function, in order to
    // initialize home object of unwrapped function, and set wrapped function
    // as a property.
    //
    //   lambda       // unwrapped
    //   dup          // unwrapped unwrapped
    //   toasync      // unwrapped wrapped
    //
    // Emitted code is surrounded by the following code.
    //
    //                    // classObj classCtor classProto
    //   (emitted code)   // classObj classCtor classProto unwrapped wrapped
    //   swap             // classObj classCtor classProto wrapped unwrapped
    //   inithomeobject 1 // classObj classCtor classProto wrapped unwrapped
    //                    //   initialize the home object of unwrapped
    //                    //   with classProto here
    //   pop              // classObj classCtor classProto wrapped
    //   inithiddenprop   // classObj classCtor classProto wrapped
    //                    //   initialize the property of the classProto
    //                    //   with wrapped function here
    //   pop              // classObj classCtor classProto
    //
    // needsHomeObject is false for other cases, push wrapped function only.
    if (!emitAsyncWrapperLambda(index, isArrow))
        return false;
    if (needsHomeObject) {
        if (!emit1(JSOP_DUP))
            return false;
    }
    if (isGenerator) {
        if (!emit1(JSOP_TOASYNCGEN))
            return false;
    } else {
        if (!emit1(JSOP_TOASYNC))
            return false;
    }
    return true;
}

bool
BytecodeEmitter::emitDo(ParseNode* pn)
{
    /* Emit an annotated nop so IonBuilder can recognize the 'do' loop. */
    unsigned noteIndex;
    if (!newSrcNote(SRC_WHILE, &noteIndex))
        return false;
    if (!emit1(JSOP_NOP))
        return false;

    unsigned noteIndex2;
    if (!newSrcNote(SRC_WHILE, &noteIndex2))
        return false;

    /* Compile the loop body. */
    JumpTarget top;
    if (!emitLoopHead(pn->pn_left, &top))
        return false;

    LoopControl loopInfo(this, StatementKind::DoLoop);

    JumpList empty;
    if (!emitLoopEntry(nullptr, empty))
        return false;

    if (!emitTree(pn->pn_left))
        return false;

    // Set the offset for continues.
    if (!emitJumpTarget(&loopInfo.continueTarget))
        return false;

    /* Compile the loop condition, now that continues know where to go. */
    if (!emitTree(pn->pn_right))
        return false;

    JumpList beq;
    JumpTarget breakTarget{ -1 };
    if (!emitBackwardJump(JSOP_IFNE, top, &beq, &breakTarget))
        return false;

    if (!tryNoteList.append(JSTRY_LOOP, stackDepth, top.offset, breakTarget.offset))
        return false;

    /*
     * Update the annotations with the update and back edge positions, for
     * IonBuilder.
     *
     * Be careful: We must set noteIndex2 before noteIndex in case the noteIndex
     * note gets bigger.
     */
    if (!setSrcNoteOffset(noteIndex2, 0, beq.offset - top.offset))
        return false;
    if (!setSrcNoteOffset(noteIndex, 0, 1 + (loopInfo.continueTarget.offset - top.offset)))
        return false;

    if (!loopInfo.patchBreaksAndContinues(this))
        return false;

    return true;
}

bool
BytecodeEmitter::emitWhile(ParseNode* pn)
{
    /*
     * Minimize bytecodes issued for one or more iterations by jumping to
     * the condition below the body and closing the loop if the condition
     * is true with a backward branch. For iteration count i:
     *
     *  i    test at the top                 test at the bottom
     *  =    ===============                 ==================
     *  0    ifeq-pass                       goto; ifne-fail
     *  1    ifeq-fail; goto; ifne-pass      goto; ifne-pass; ifne-fail
     *  2    2*(ifeq-fail; goto); ifeq-pass  goto; 2*ifne-pass; ifne-fail
     *  . . .
     *  N    N*(ifeq-fail; goto); ifeq-pass  goto; N*ifne-pass; ifne-fail
     */

    // If we have a single-line while, like "while (x) ;", we want to
    // emit the line note before the initial goto, so that the
    // debugger sees a single entry point.  This way, if there is a
    // breakpoint on the line, it will only fire once; and "next"ing
    // will skip the whole loop.  However, for the multi-line case we
    // want to emit the line note after the initial goto, so that
    // "cont" stops on each iteration -- but without a stop before the
    // first iteration.
    if (parser.tokenStream().srcCoords.lineNum(pn->pn_pos.begin) ==
        parser.tokenStream().srcCoords.lineNum(pn->pn_pos.end))
    {
        if (!updateSourceCoordNotes(pn->pn_pos.begin))
            return false;
    }

    JumpTarget top{ -1 };
    if (!emitJumpTarget(&top))
        return false;

    LoopControl loopInfo(this, StatementKind::WhileLoop);
    loopInfo.continueTarget = top;

    unsigned noteIndex;
    if (!newSrcNote(SRC_WHILE, &noteIndex))
        return false;

    JumpList jmp;
    if (!emitJump(JSOP_GOTO, &jmp))
        return false;

    if (!emitLoopHead(pn->pn_right, &top))
        return false;

    if (!emitTreeInBranch(pn->pn_right))
        return false;

    if (!emitLoopEntry(pn->pn_left, jmp))
        return false;
    if (!emitTree(pn->pn_left))
        return false;

    JumpList beq;
    JumpTarget breakTarget{ -1 };
    if (!emitBackwardJump(JSOP_IFNE, top, &beq, &breakTarget))
        return false;

    if (!tryNoteList.append(JSTRY_LOOP, stackDepth, top.offset, breakTarget.offset))
        return false;

    if (!setSrcNoteOffset(noteIndex, 0, beq.offset - jmp.offset))
        return false;

    if (!loopInfo.patchBreaksAndContinues(this))
        return false;

    return true;
}

bool
BytecodeEmitter::emitBreak(PropertyName* label)
{
    BreakableControl* target;
    SrcNoteType noteType;
    if (label) {
        // Any statement with the matching label may be the break target.
        auto hasSameLabel = [label](LabelControl* labelControl) {
            return labelControl->label() == label;
        };
        target = findInnermostNestableControl<LabelControl>(hasSameLabel);
        noteType = SRC_BREAK2LABEL;
    } else {
        auto isNotLabel = [](BreakableControl* control) {
            return !control->is<LabelControl>();
        };
        target = findInnermostNestableControl<BreakableControl>(isNotLabel);
        noteType = (target->kind() == StatementKind::Switch) ? SRC_SWITCHBREAK : SRC_BREAK;
    }

    return emitGoto(target, &target->breaks, noteType);
}

bool
BytecodeEmitter::emitContinue(PropertyName* label)
{
    LoopControl* target = nullptr;
    if (label) {
        // Find the loop statement enclosed by the matching label.
        NestableControl* control = innermostNestableControl;
        while (!control->is<LabelControl>() || control->as<LabelControl>().label() != label) {
            if (control->is<LoopControl>())
                target = &control->as<LoopControl>();
            control = control->enclosing();
        }
    } else {
        target = findInnermostNestableControl<LoopControl>();
    }
    return emitGoto(target, &target->continues, SRC_CONTINUE);
}

bool
BytecodeEmitter::emitGetFunctionThis(ParseNode* pn)
{
    MOZ_ASSERT(sc->thisBinding() == ThisBinding::Function);
    MOZ_ASSERT(pn->isKind(ParseNodeKind::Name));
    MOZ_ASSERT(pn->name() == cx->names().dotThis);

    if (!emitTree(pn))
        return false;
    if (sc->needsThisTDZChecks() && !emit1(JSOP_CHECKTHIS))
        return false;

    return true;
}

bool
BytecodeEmitter::emitGetThisForSuperBase(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(ParseNodeKind::SuperBase));
    return emitGetFunctionThis(pn->pn_kid);
}

bool
BytecodeEmitter::emitThisLiteral(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(ParseNodeKind::This));

    if (ParseNode* thisName = pn->pn_kid)
        return emitGetFunctionThis(thisName);

    if (sc->thisBinding() == ThisBinding::Module)
        return emit1(JSOP_UNDEFINED);

    MOZ_ASSERT(sc->thisBinding() == ThisBinding::Global);
    return emit1(JSOP_GLOBALTHIS);
}

bool
BytecodeEmitter::emitCheckDerivedClassConstructorReturn()
{
    MOZ_ASSERT(lookupName(cx->names().dotThis).hasKnownSlot());
    if (!emitGetName(cx->names().dotThis))
        return false;
    if (!emit1(JSOP_CHECKRETURN))
        return false;
    return true;
}

bool
BytecodeEmitter::emitReturn(ParseNode* pn)
{
    if (!updateSourceCoordNotes(pn->pn_pos.begin))
        return false;

    bool needsIteratorResult = sc->isFunctionBox() && sc->asFunctionBox()->needsIteratorResult();
    if (needsIteratorResult) {
        if (!emitPrepareIteratorResult())
            return false;
    }

    /* Push a return value */
    if (ParseNode* pn2 = pn->pn_kid) {
        if (!emitTree(pn2))
            return false;

        bool isAsyncGenerator = sc->asFunctionBox()->isAsync() &&
                                sc->asFunctionBox()->isGenerator();
        if (isAsyncGenerator) {
            if (!emitAwaitInInnermostScope())
                return false;
        }
    } else {
        /* No explicit return value provided */
        if (!emit1(JSOP_UNDEFINED))
            return false;
    }

    if (needsIteratorResult) {
        if (!emitFinishIteratorResult(true))
            return false;
    }

    // We know functionBodyEndPos is set because "return" is only
    // valid in a function, and so we've passed through
    // emitFunctionScript.
    MOZ_ASSERT(functionBodyEndPosSet);
    if (!updateSourceCoordNotes(functionBodyEndPos))
        return false;

    /*
     * EmitNonLocalJumpFixup may add fixup bytecode to close open try
     * blocks having finally clauses and to exit intermingled let blocks.
     * We can't simply transfer control flow to our caller in that case,
     * because we must gosub to those finally clauses from inner to outer,
     * with the correct stack pointer (i.e., after popping any with,
     * for/in, etc., slots nested inside the finally's try).
     *
     * In this case we mutate JSOP_RETURN into JSOP_SETRVAL and add an
     * extra JSOP_RETRVAL after the fixups.
     */
    ptrdiff_t top = offset();

    bool needsFinalYield = sc->isFunctionBox() && sc->asFunctionBox()->needsFinalYield();
    bool isDerivedClassConstructor =
        sc->isFunctionBox() && sc->asFunctionBox()->isDerivedClassConstructor();

    if (!emit1((needsFinalYield || isDerivedClassConstructor) ? JSOP_SETRVAL : JSOP_RETURN))
        return false;

    // Make sure that we emit this before popping the blocks in prepareForNonLocalJump,
    // to ensure that the error is thrown while the scope-chain is still intact.
    if (isDerivedClassConstructor) {
        if (!emitCheckDerivedClassConstructorReturn())
            return false;
    }

    NonLocalExitControl nle(this, NonLocalExitControl::Return);

    if (!nle.prepareForNonLocalJumpToOutermost())
        return false;

    if (needsFinalYield) {
        // We know that .generator is on the function scope, as we just exited
        // all nested scopes.
        NameLocation loc =
            *locationOfNameBoundInFunctionScope(cx->names().dotGenerator, varEmitterScope);
        if (!emitGetNameAtLocation(cx->names().dotGenerator, loc))
            return false;
        if (!emitYieldOp(JSOP_FINALYIELDRVAL))
            return false;
    } else if (isDerivedClassConstructor) {
        MOZ_ASSERT(code()[top] == JSOP_SETRVAL);
        if (!emit1(JSOP_RETRVAL))
            return false;
    } else if (top + static_cast<ptrdiff_t>(JSOP_RETURN_LENGTH) != offset()) {
        code()[top] = JSOP_SETRVAL;
        if (!emit1(JSOP_RETRVAL))
            return false;
    }

    return true;
}

bool
BytecodeEmitter::emitGetDotGeneratorInScope(EmitterScope& currentScope)
{
    NameLocation loc = *locationOfNameBoundInFunctionScope(cx->names().dotGenerator, &currentScope);
    return emitGetNameAtLocation(cx->names().dotGenerator, loc);
}

bool
BytecodeEmitter::emitInitialYield(ParseNode* pn)
{
    if (!emitTree(pn->pn_kid))
        return false;

    if (!emitYieldOp(JSOP_INITIALYIELD))
        return false;

    if (!emit1(JSOP_POP))
        return false;

    return true;
}

bool
BytecodeEmitter::emitYield(ParseNode* pn)
{
    MOZ_ASSERT(sc->isFunctionBox());
    MOZ_ASSERT(pn->isKind(ParseNodeKind::Yield));

    bool needsIteratorResult = sc->asFunctionBox()->needsIteratorResult();
    if (needsIteratorResult) {
        if (!emitPrepareIteratorResult())
            return false;
    }
    if (pn->pn_kid) {
        if (!emitTree(pn->pn_kid))
            return false;
    } else {
        if (!emit1(JSOP_UNDEFINED))
            return false;
    }

    // 11.4.3.7 AsyncGeneratorYield step 5.
    bool isAsyncGenerator = sc->asFunctionBox()->isAsync();
    if (isAsyncGenerator) {
        if (!emitAwaitInInnermostScope())                 // RESULT
            return false;
    }

    if (needsIteratorResult) {
        if (!emitFinishIteratorResult(false))
            return false;
    }

    if (!emitGetDotGeneratorInInnermostScope())
        return false;

    if (!emitYieldOp(JSOP_YIELD))
        return false;

    return true;
}

bool
BytecodeEmitter::emitAwaitInInnermostScope(ParseNode* pn)
{
    MOZ_ASSERT(sc->isFunctionBox());
    MOZ_ASSERT(pn->isKind(ParseNodeKind::Await));

    if (!emitTree(pn->pn_kid))
        return false;
    return emitAwaitInInnermostScope();
}

bool
BytecodeEmitter::emitAwaitInScope(EmitterScope& currentScope)
{
    if (!emitGetDotGeneratorInScope(currentScope))
        return false;
    if (!emitYieldOp(JSOP_AWAIT))
        return false;
    return true;
}

bool
BytecodeEmitter::emitYieldStar(ParseNode* iter)
{
    MOZ_ASSERT(sc->isFunctionBox());
    MOZ_ASSERT(sc->asFunctionBox()->isGenerator());

    bool isAsyncGenerator = sc->asFunctionBox()->isAsync();

    if (!emitTree(iter))                                  // ITERABLE
        return false;
    if (isAsyncGenerator) {
        if (!emitAsyncIterator())                         // NEXT ITER
            return false;
    } else {
        if (!emitIterator())                              // NEXT ITER
            return false;
    }

    // Initial send value is undefined.
    if (!emit1(JSOP_UNDEFINED))                           // NEXT ITER RECEIVED
        return false;

    int32_t savedDepthTemp;
    int32_t startDepth = stackDepth;
    MOZ_ASSERT(startDepth >= 3);

    TryEmitter tryCatch(this, TryEmitter::TryCatchFinally, TryEmitter::DontUseRetVal,
                        TryEmitter::DontUseControl);
    if (!tryCatch.emitJumpOverCatchAndFinally())          // NEXT ITER RESULT
        return false;

    JumpTarget tryStart{ offset() };
    if (!tryCatch.emitTry())                              // NEXT ITER RESULT
        return false;

    MOZ_ASSERT(this->stackDepth == startDepth);

    // 11.4.3.7 AsyncGeneratorYield step 5.
    if (isAsyncGenerator) {
        if (!emitAwaitInInnermostScope())                 // NEXT ITER RESULT
            return false;
    }

    // Load the generator object.
    if (!emitGetDotGeneratorInInnermostScope())           // NEXT ITER RESULT GENOBJ
        return false;

    // Yield RESULT as-is, without re-boxing.
    if (!emitYieldOp(JSOP_YIELD))                         // NEXT ITER RECEIVED
        return false;

    if (!tryCatch.emitCatch())                            // NEXT ITER RESULT
        return false;

    stackDepth = startDepth;                              // NEXT ITER RESULT
    if (!emit1(JSOP_EXCEPTION))                           // NEXT ITER RESULT EXCEPTION
        return false;
    if (!emitDupAt(2))                                    // NEXT ITER RESULT EXCEPTION ITER
        return false;
    if (!emit1(JSOP_DUP))                                 // NEXT ITER RESULT EXCEPTION ITER ITER
        return false;
    if (!emitAtomOp(cx->names().throw_, JSOP_CALLPROP))   // NEXT ITER RESULT EXCEPTION ITER THROW
        return false;
    if (!emit1(JSOP_DUP))                                 // NEXT ITER RESULT EXCEPTION ITER THROW THROW
        return false;
    if (!emit1(JSOP_UNDEFINED))                           // NEXT ITER RESULT EXCEPTION ITER THROW THROW UNDEFINED
        return false;
    if (!emit1(JSOP_EQ))                                  // NEXT ITER RESULT EXCEPTION ITER THROW ?EQL
        return false;

    IfThenElseEmitter ifThrowMethodIsNotDefined(this);
    if (!ifThrowMethodIsNotDefined.emitIf())              // NEXT ITER RESULT EXCEPTION ITER THROW
        return false;
    savedDepthTemp = stackDepth;
    if (!emit1(JSOP_POP))                                 // NEXT ITER RESULT EXCEPTION ITER
        return false;
    // ES 14.4.13, YieldExpression : yield * AssignmentExpression, step 5.b.iii.2
    //
    // If the iterator does not have a "throw" method, it calls IteratorClose
    // and then throws a TypeError.
    IteratorKind iterKind = isAsyncGenerator ? IteratorKind::Async : IteratorKind::Sync;
    if (!emitIteratorCloseInInnermostScope(iterKind))     // NEXT ITER RESULT EXCEPTION
        return false;
    if (!emitUint16Operand(JSOP_THROWMSG, JSMSG_ITERATOR_NO_THROW)) // throw
        return false;
    stackDepth = savedDepthTemp;
    if (!ifThrowMethodIsNotDefined.emitEnd())             // NEXT ITER OLDRESULT EXCEPTION ITER THROW
        return false;
    // ES 14.4.13, YieldExpression : yield * AssignmentExpression, step 5.b.iii.4.
    // RESULT = ITER.throw(EXCEPTION)                     // NEXT ITER OLDRESULT EXCEPTION ITER THROW
    if (!emit1(JSOP_SWAP))                                // NEXT ITER OLDRESULT EXCEPTION THROW ITER
        return false;
    if (!emit2(JSOP_PICK, 2))                             // NEXT ITER OLDRESULT THROW ITER EXCEPTION
        return false;
    if (!emitCall(JSOP_CALL, 1, iter))                    // NEXT ITER OLDRESULT RESULT
        return false;
    checkTypeSet(JSOP_CALL);

    if (isAsyncGenerator) {
        if (!emitAwaitInInnermostScope())                 // NEXT ITER OLDRESULT RESULT
            return false;
    }

    if (!emitCheckIsObj(CheckIsObjectKind::IteratorThrow)) // NEXT ITER OLDRESULT RESULT
        return false;
    if (!emit1(JSOP_SWAP))                                // NEXT ITER RESULT OLDRESULT
        return false;
    if (!emit1(JSOP_POP))                                 // NEXT ITER RESULT
        return false;
    MOZ_ASSERT(this->stackDepth == startDepth);
    JumpList checkResult;
    // ES 14.4.13, YieldExpression : yield * AssignmentExpression, step 5.b.ii.
    //
    // Note that there is no GOSUB to the finally block here. If the iterator has a
    // "throw" method, it does not perform IteratorClose.
    if (!emitJump(JSOP_GOTO, &checkResult))               // goto checkResult
        return false;

    if (!tryCatch.emitFinally())
         return false;

    // ES 14.4.13, yield * AssignmentExpression, step 5.c
    //
    // Call iterator.return() for receiving a "forced return" completion from
    // the generator.

    IfThenElseEmitter ifGeneratorClosing(this);
    if (!emit1(JSOP_ISGENCLOSING))                        // NEXT ITER RESULT FTYPE FVALUE CLOSING
        return false;
    if (!ifGeneratorClosing.emitIf())                     // NEXT ITER RESULT FTYPE FVALUE
        return false;

    // Step ii.
    //
    // Get the "return" method.
    if (!emitDupAt(3))                                    // NEXT ITER RESULT FTYPE FVALUE ITER
        return false;
    if (!emit1(JSOP_DUP))                                 // NEXT ITER RESULT FTYPE FVALUE ITER ITER
        return false;
    if (!emitAtomOp(cx->names().return_, JSOP_CALLPROP))  // NEXT ITER RESULT FTYPE FVALUE ITER RET
        return false;

    // Step iii.
    //
    // Do nothing if "return" is undefined or null.
    IfThenElseEmitter ifReturnMethodIsDefined(this);
    if (!emitPushNotUndefinedOrNull())                    // NEXT ITER RESULT FTYPE FVALUE ITER RET NOT-UNDEF-OR-NULL
        return false;

    // Step iv.
    //
    // Call "return" with the argument passed to Generator.prototype.return,
    // which is currently in rval.value.
    if (!ifReturnMethodIsDefined.emitIfElse())            // NEXT ITER OLDRESULT FTYPE FVALUE ITER RET
        return false;
    if (!emit1(JSOP_SWAP))                                // NEXT ITER OLDRESULT FTYPE FVALUE RET ITER
        return false;
    if (!emit1(JSOP_GETRVAL))                             // NEXT ITER OLDRESULT FTYPE FVALUE RET ITER RVAL
        return false;
    if (!emitAtomOp(cx->names().value, JSOP_GETPROP))     // NEXT ITER OLDRESULT FTYPE FVALUE RET ITER VALUE
        return false;
    if (!emitCall(JSOP_CALL, 1))                          // NEXT ITER OLDRESULT FTYPE FVALUE RESULT
        return false;
    checkTypeSet(JSOP_CALL);

    if (iterKind == IteratorKind::Async) {
        if (!emitAwaitInInnermostScope())                 // ... FTYPE FVALUE RESULT
            return false;
    }

    // Step v.
    if (!emitCheckIsObj(CheckIsObjectKind::IteratorReturn)) // NEXT ITER OLDRESULT FTYPE FVALUE RESULT
        return false;

    // Steps vi-viii.
    //
    // Check if the returned object from iterator.return() is done. If not,
    // continuing yielding.
    IfThenElseEmitter ifReturnDone(this);
    if (!emit1(JSOP_DUP))                                 // NEXT ITER OLDRESULT FTYPE FVALUE RESULT RESULT
        return false;
    if (!emitAtomOp(cx->names().done, JSOP_GETPROP))      // NEXT ITER OLDRESULT FTYPE FVALUE RESULT DONE
        return false;
    if (!ifReturnDone.emitIfElse())                       // NEXT ITER OLDRESULT FTYPE FVALUE RESULT
        return false;
    if (!emitAtomOp(cx->names().value, JSOP_GETPROP))     // NEXT ITER OLDRESULT FTYPE FVALUE VALUE
        return false;

    if (!emitPrepareIteratorResult())                     // NEXT ITER OLDRESULT FTYPE FVALUE VALUE RESULT
        return false;
    if (!emit1(JSOP_SWAP))                                // NEXT ITER OLDRESULT FTYPE FVALUE RESULT VALUE
        return false;
    if (!emitFinishIteratorResult(true))                  // NEXT ITER OLDRESULT FTYPE FVALUE RESULT
        return false;
    if (!emit1(JSOP_SETRVAL))                             // NEXT ITER OLDRESULT FTYPE FVALUE
        return false;
    savedDepthTemp = this->stackDepth;
    if (!ifReturnDone.emitElse())                         // NEXT ITER OLDRESULT FTYPE FVALUE RESULT
        return false;
    if (!emit2(JSOP_UNPICK, 3))                           // NEXT ITER RESULT OLDRESULT FTYPE FVALUE
        return false;
    if (!emitPopN(3))                                     // NEXT ITER RESULT
        return false;
    {
        // goto tryStart;
        JumpList beq;
        JumpTarget breakTarget{ -1 };
        if (!emitBackwardJump(JSOP_GOTO, tryStart, &beq, &breakTarget)) // NEXT ITER RESULT
            return false;
    }
    this->stackDepth = savedDepthTemp;
    if (!ifReturnDone.emitEnd())
        return false;

    if (!ifReturnMethodIsDefined.emitElse())              // NEXT ITER RESULT FTYPE FVALUE ITER RET
        return false;
    if (!emitPopN(2))                                     // NEXT ITER RESULT FTYPE FVALUE
        return false;
    if (!ifReturnMethodIsDefined.emitEnd())
        return false;

    if (!ifGeneratorClosing.emitEnd())
        return false;

    if (!tryCatch.emitEnd())
        return false;

    // After the try-catch-finally block: send the received value to the iterator.
    // result = iter.next(received)                              // NEXT ITER RECEIVED
    if (!emit2(JSOP_UNPICK, 2))                                  // RECEIVED NEXT ITER
        return false;
    if (!emit1(JSOP_DUP2))                                       // RECEIVED NEXT ITER NEXT ITER
        return false;
    if (!emit2(JSOP_PICK, 4))                                    // NEXT ITER NEXT ITER RECEIVED
        return false;
    if (!emitCall(JSOP_CALL, 1, iter))                           // NEXT ITER RESULT
        return false;
    checkTypeSet(JSOP_CALL);

    if (isAsyncGenerator) {
        if (!emitAwaitInInnermostScope())                        // NEXT ITER RESULT RESULT
            return false;
    }

    if (!emitCheckIsObj(CheckIsObjectKind::IteratorNext))        // NEXT ITER RESULT
        return false;
    MOZ_ASSERT(this->stackDepth == startDepth);

    if (!emitJumpTargetAndPatch(checkResult))                    // checkResult:
        return false;

    // if (!result.done) goto tryStart;                          // NEXT ITER RESULT
    if (!emit1(JSOP_DUP))                                        // NEXT ITER RESULT RESULT
        return false;
    if (!emitAtomOp(cx->names().done, JSOP_GETPROP))             // NEXT ITER RESULT DONE
        return false;
    // if (!DONE) goto tryStart;
    {
        JumpList beq;
        JumpTarget breakTarget{ -1 };
        if (!emitBackwardJump(JSOP_IFEQ, tryStart, &beq, &breakTarget)) // NEXT ITER RESULT
            return false;
    }

    // result.value
    if (!emit2(JSOP_UNPICK, 2))                                  // RESULT NEXT ITER
        return false;
    if (!emitPopN(2))                                            // RESULT
        return false;
    if (!emitAtomOp(cx->names().value, JSOP_GETPROP))            // VALUE
        return false;

    MOZ_ASSERT(this->stackDepth == startDepth - 2);

    return true;
}

bool
BytecodeEmitter::emitStatementList(ParseNode* pn)
{
    MOZ_ASSERT(pn->isArity(PN_LIST));
    for (ParseNode* pn2 = pn->pn_head; pn2; pn2 = pn2->pn_next) {
        if (!emitTree(pn2))
            return false;
    }
    return true;
}

bool
BytecodeEmitter::emitExpressionStatement(ParseNode* pn)
{
    MOZ_ASSERT(pn->isKind(ParseNodeKind::ExpressionStatement));

    if (!updateSourceCoordNotes(pn->pn_pos.begin))
        return false;

    /*
     * Top-level or called-from-a-native JS_Execute/EvaluateScript,
     * debugger, and eval frames may need the value of the ultimate
     * expression statement as the script's result, despite the fact
     * that it appears useless to the compiler.
     *
     * API users may also set the JSOPTION_NO_SCRIPT_RVAL option when
     * calling JS_Compile* to suppress JSOP_SETRVAL.
     */
    bool wantval = false;
    bool useful = false;
    if (sc->isFunctionBox())
        MOZ_ASSERT(!script->noScriptRval());
    else
        useful = wantval = !script->noScriptRval();

    /* Don't eliminate expressions with side effects. */
    ParseNode* expr = pn->pn_kid;
    if (!useful) {
        if (!checkSideEffects(expr, &useful))
            return false;

        /*
         * Don't eliminate apparently useless expressions if they are labeled
         * expression statements. The startOffset() test catches the case
         * where we are nesting in emitTree for a labeled compound statement.
         */
        if (innermostNestableControl &&
            innermostNestableControl->is<LabelControl>() &&
            innermostNestableControl->as<LabelControl>().startOffset() >= offset())
        {
            useful = true;
        }
    }

    if (useful) {
        JSOp op = wantval ? JSOP_SETRVAL : JSOP_POP;
        ValueUsage valueUsage = wantval ? ValueUsage::WantValue : ValueUsage::IgnoreValue;
        MOZ_ASSERT_IF(expr->isKind(ParseNodeKind::Assign), expr->isOp(JSOP_NOP));
        if (!emitTree(expr, valueUsage))
            return false;
        if (!emit1(op))
            return false;
    } else if (pn->isDirectivePrologueMember()) {
        // Don't complain about directive prologue members; just don't emit
        // their code.
    } else {
        if (JSAtom* atom = pn->isStringExprStatement()) {
            // Warn if encountering a non-directive prologue member string
            // expression statement, that is inconsistent with the current
            // directive prologue.  That is, a script *not* starting with
            // "use strict" should warn for any "use strict" statements seen
            // later in the script, because such statements are misleading.
            const char* directive = nullptr;
            if (atom == cx->names().useStrict) {
                if (!sc->strictScript)
                    directive = js_useStrict_str;
            } else if (atom == cx->names().useAsm) {
                if (sc->isFunctionBox()) {
                    if (IsAsmJSModule(sc->asFunctionBox()->function()))
                        directive = js_useAsm_str;
                }
            }

            if (directive) {
                if (!reportExtraWarning(expr, JSMSG_CONTRARY_NONDIRECTIVE, directive))
                    return false;
            }
        } else {
            if (!reportExtraWarning(expr, JSMSG_USELESS_EXPR))
                return false;
        }
    }

    return true;
}

bool
BytecodeEmitter::emitDeleteName(ParseNode* node)
{
    MOZ_ASSERT(node->isKind(ParseNodeKind::DeleteName));
    MOZ_ASSERT(node->isArity(PN_UNARY));

    ParseNode* nameExpr = node->pn_kid;
    MOZ_ASSERT(nameExpr->isKind(ParseNodeKind::Name));

    return emitAtomOp(nameExpr, JSOP_DELNAME);
}

bool
BytecodeEmitter::emitDeleteProperty(ParseNode* node)
{
    MOZ_ASSERT(node->isKind(ParseNodeKind::DeleteProp));
    MOZ_ASSERT(node->isArity(PN_UNARY));

    ParseNode* propExpr = node->pn_kid;
    MOZ_ASSERT(propExpr->isKind(ParseNodeKind::Dot));

    if (propExpr->as<PropertyAccess>().isSuper()) {
        // Still have to calculate the base, even though we are are going
        // to throw unconditionally, as calculating the base could also
        // throw.
        if (!emit1(JSOP_SUPERBASE))
            return false;

        return emitUint16Operand(JSOP_THROWMSG, JSMSG_CANT_DELETE_SUPER);
    }

    JSOp delOp = sc->strict() ? JSOP_STRICTDELPROP : JSOP_DELPROP;
    return emitPropOp(propExpr, delOp);
}

bool
BytecodeEmitter::emitDeleteElement(ParseNode* node)
{
    MOZ_ASSERT(node->isKind(ParseNodeKind::DeleteElem));
    MOZ_ASSERT(node->isArity(PN_UNARY));

    ParseNode* elemExpr = node->pn_kid;
    MOZ_ASSERT(elemExpr->isKind(ParseNodeKind::Elem));

    if (elemExpr->as<PropertyByValue>().isSuper()) {
        // Still have to calculate everything, even though we're gonna throw
        // since it may have side effects
        if (!emitTree(elemExpr->pn_right))
            return false;

        if (!emit1(JSOP_SUPERBASE))
            return false;
        if (!emitUint16Operand(JSOP_THROWMSG, JSMSG_CANT_DELETE_SUPER))
            return false;

        // Another wrinkle: Balance the stack from the emitter's point of view.
        // Execution will not reach here, as the last bytecode threw.
        return emit1(JSOP_POP);
    }

    JSOp delOp = sc->strict() ? JSOP_STRICTDELELEM : JSOP_DELELEM;
    return emitElemOp(elemExpr, delOp);
}

bool
BytecodeEmitter::emitDeleteExpression(ParseNode* node)
{
    MOZ_ASSERT(node->isKind(ParseNodeKind::DeleteExpr));
    MOZ_ASSERT(node->isArity(PN_UNARY));

    ParseNode* expression = node->pn_kid;

    // If useless, just emit JSOP_TRUE; otherwise convert |delete <expr>| to
    // effectively |<expr>, true|.
    bool useful = false;
    if (!checkSideEffects(expression, &useful))
        return false;

    if (useful) {
        if (!emitTree(expression))
            return false;
        if (!emit1(JSOP_POP))
            return false;
    }

    return emit1(JSOP_TRUE);
}

static const char *
SelfHostedCallFunctionName(JSAtom* name, JSContext* cx)
{
    if (name == cx->names().callFunction)
        return "callFunction";
    if (name == cx->names().callContentFunction)
        return "callContentFunction";
    if (name == cx->names().constructContentFunction)
        return "constructContentFunction";

    MOZ_CRASH("Unknown self-hosted call function name");
}

bool
BytecodeEmitter::emitSelfHostedCallFunction(ParseNode* pn)
{
    // Special-casing of callFunction to emit bytecode that directly
    // invokes the callee with the correct |this| object and arguments.
    // callFunction(fun, thisArg, arg0, arg1) thus becomes:
    // - emit lookup for fun
    // - emit lookup for thisArg
    // - emit lookups for arg0, arg1
    //
    // argc is set to the amount of actually emitted args and the
    // emitting of args below is disabled by setting emitArgs to false.
    ParseNode* pn2 = pn->pn_head;
    const char* errorName = SelfHostedCallFunctionName(pn2->name(), cx);

    if (pn->pn_count < 3) {
        reportError(pn, JSMSG_MORE_ARGS_NEEDED, errorName, "2", "s");
        return false;
    }

    JSOp callOp = pn->getOp();
    if (callOp != JSOP_CALL) {
        reportError(pn, JSMSG_NOT_CONSTRUCTOR, errorName);
        return false;
    }

    bool constructing = pn2->name() == cx->names().constructContentFunction;
    ParseNode* funNode = pn2->pn_next;
    if (constructing) {
        callOp = JSOP_NEW;
    } else if (funNode->getKind() == ParseNodeKind::Name &&
               funNode->name() == cx->names().std_Function_apply) {
        callOp = JSOP_FUNAPPLY;
    }

    if (!emitTree(funNode))
        return false;

#ifdef DEBUG
    if (emitterMode == BytecodeEmitter::SelfHosting &&
        pn2->name() == cx->names().callFunction)
    {
        if (!emit1(JSOP_DEBUGCHECKSELFHOSTED))
            return false;
    }
#endif

    ParseNode* thisOrNewTarget = funNode->pn_next;
    if (constructing) {
        // Save off the new.target value, but here emit a proper |this| for a
        // constructing call.
        if (!emit1(JSOP_IS_CONSTRUCTING))
            return false;
    } else {
        // It's |this|, emit it.
        if (!emitTree(thisOrNewTarget))
            return false;
    }

    for (ParseNode* argpn = thisOrNewTarget->pn_next; argpn; argpn = argpn->pn_next) {
        if (!emitTree(argpn))
            return false;
    }

    if (constructing) {
        if (!emitTree(thisOrNewTarget))
            return false;
    }

    uint32_t argc = pn->pn_count - 3;
    if (!emitCall(callOp, argc))
        return false;

    checkTypeSet(callOp);
    return true;
}

bool
BytecodeEmitter::emitSelfHostedResumeGenerator(ParseNode* pn)
{
    // Syntax: resumeGenerator(gen, value, 'next'|'throw'|'return')
    if (pn->pn_count != 4) {
        reportError(pn, JSMSG_MORE_ARGS_NEEDED, "resumeGenerator", "1", "s");
        return false;
    }

    ParseNode* funNode = pn->pn_head;  // The resumeGenerator node.

    ParseNode* genNode = funNode->pn_next;
    if (!emitTree(genNode))
        return false;

    ParseNode* valNode = genNode->pn_next;
    if (!emitTree(valNode))
        return false;

    ParseNode* kindNode = valNode->pn_next;
    MOZ_ASSERT(kindNode->isKind(ParseNodeKind::String));
    uint16_t operand = GeneratorObject::getResumeKind(cx, kindNode->pn_atom);
    MOZ_ASSERT(!kindNode->pn_next);

    if (!emitCall(JSOP_RESUME, operand))
        return false;

    return true;
}

bool
BytecodeEmitter::emitSelfHostedForceInterpreter()
{
    if (!emit1(JSOP_FORCEINTERPRETER))
        return false;
    if (!emit1(JSOP_UNDEFINED))
        return false;
    return true;
}

bool
BytecodeEmitter::emitSelfHostedAllowContentIter(ParseNode* pn)
{
    if (pn->pn_count != 2) {
        reportError(pn, JSMSG_MORE_ARGS_NEEDED, "allowContentIter", "1", "");
        return false;
    }

    // We're just here as a sentinel. Pass the value through directly.
    return emitTree(pn->pn_head->pn_next);
}

bool
BytecodeEmitter::emitSelfHostedDefineDataProperty(ParseNode* pn)
{
    // Only optimize when 3 arguments are passed (we use 4 to include |this|).
    MOZ_ASSERT(pn->pn_count == 4);

    ParseNode* funNode = pn->pn_head;  // The _DefineDataProperty node.

    ParseNode* objNode = funNode->pn_next;
    if (!emitTree(objNode))
        return false;

    ParseNode* idNode = objNode->pn_next;
    if (!emitTree(idNode))
        return false;

    ParseNode* valNode = idNode->pn_next;
    if (!emitTree(valNode))
        return false;

    // This will leave the object on the stack instead of pushing |undefined|,
    // but that's fine because the self-hosted code doesn't use the return
    // value.
    return emit1(JSOP_INITELEM);
}

bool
BytecodeEmitter::emitSelfHostedHasOwn(ParseNode* pn)
{
    if (pn->pn_count != 3) {
        reportError(pn, JSMSG_MORE_ARGS_NEEDED, "hasOwn", "2", "");
        return false;
    }

    ParseNode* funNode = pn->pn_head;  // The hasOwn node.

    ParseNode* idNode = funNode->pn_next;
    if (!emitTree(idNode))
        return false;

    ParseNode* objNode = idNode->pn_next;
    if (!emitTree(objNode))
        return false;

    return emit1(JSOP_HASOWN);
}

bool
BytecodeEmitter::emitSelfHostedGetPropertySuper(ParseNode* pn)
{
    if (pn->pn_count != 4) {
        reportError(pn, JSMSG_MORE_ARGS_NEEDED, "getPropertySuper", "3", "");
        return false;
    }

    ParseNode* funNode = pn->pn_head;  // The getPropertySuper node.

    ParseNode* objNode = funNode->pn_next;
    ParseNode* idNode = objNode->pn_next;
    ParseNode* receiverNode = idNode->pn_next;

    if (!emitTree(idNode))
        return false;

    if (!emitTree(receiverNode))
        return false;

    if (!emitTree(objNode))
        return false;

    return emitElemOpBase(JSOP_GETELEM_SUPER);
}

bool
BytecodeEmitter::isRestParameter(ParseNode* pn)
{
    if (!sc->isFunctionBox())
        return false;

    FunctionBox* funbox = sc->asFunctionBox();
    RootedFunction fun(cx, funbox->function());
    if (!funbox->hasRest())
        return false;

    if (!pn->isKind(ParseNodeKind::Name)) {
        if (emitterMode == BytecodeEmitter::SelfHosting && pn->isKind(ParseNodeKind::Call)) {
            ParseNode* pn2 = pn->pn_head;
            if (pn2->getKind() == ParseNodeKind::Name &&
                pn2->name() == cx->names().allowContentIter)
            {
                return isRestParameter(pn2->pn_next);
            }
        }
        return false;
    }

    JSAtom* name = pn->name();
    Maybe<NameLocation> paramLoc = locationOfNameBoundInFunctionScope(name);
    if (paramLoc && lookupName(name) == *paramLoc) {
        FunctionScope::Data* bindings = funbox->functionScopeBindings();
        if (bindings->nonPositionalFormalStart > 0) {
            // |paramName| can be nullptr when the rest destructuring syntax is
            // used: `function f(...[]) {}`.
            JSAtom* paramName = bindings->names[bindings->nonPositionalFormalStart - 1].name();
            return paramName && name == paramName;
        }
    }

    return false;
}

bool
BytecodeEmitter::emitCallee(ParseNode* callee, ParseNode* call, bool* callop)
{
    switch (callee->getKind()) {
      case ParseNodeKind::Name:
        if (!emitGetName(callee, *callop))
            return false;
        break;
      case ParseNodeKind::Dot:
        MOZ_ASSERT(emitterMode != BytecodeEmitter::SelfHosting);
        if (callee->as<PropertyAccess>().isSuper()) {
            if (!emitSuperPropOp(callee, JSOP_GETPROP_SUPER, /* isCall = */ *callop))
                return false;
        } else {
            if (!emitPropOp(callee, *callop ? JSOP_CALLPROP : JSOP_GETPROP))
                return false;
        }

        break;
      case ParseNodeKind::Elem:
        MOZ_ASSERT(emitterMode != BytecodeEmitter::SelfHosting);
        if (callee->as<PropertyByValue>().isSuper()) {
            if (!emitSuperElemOp(callee, JSOP_GETELEM_SUPER, /* isCall = */ *callop))
                return false;
        } else {
            if (!emitElemOp(callee, *callop ? JSOP_CALLELEM : JSOP_GETELEM))
                return false;
            if (*callop) {
                if (!emit1(JSOP_SWAP))
                    return false;
            }
        }

        break;
      case ParseNodeKind::Function:
        /*
         * Top level lambdas which are immediately invoked should be
         * treated as only running once. Every time they execute we will
         * create new types and scripts for their contents, to increase
         * the quality of type information within them and enable more
         * backend optimizations. Note that this does not depend on the
         * lambda being invoked at most once (it may be named or be
         * accessed via foo.caller indirection), as multiple executions
         * will just cause the inner scripts to be repeatedly cloned.
         */
        MOZ_ASSERT(!emittingRunOnceLambda);
        if (checkRunOnceContext()) {
            emittingRunOnceLambda = true;
            if (!emitTree(callee))
                return false;
            emittingRunOnceLambda = false;
        } else {
            if (!emitTree(callee))
                return false;
        }
        *callop = false;
        break;
      case ParseNodeKind::SuperBase:
        MOZ_ASSERT(call->isKind(ParseNodeKind::SuperCall));
        MOZ_ASSERT(parser.isSuperBase(callee));
        if (!emit1(JSOP_SUPERFUN))
            return false;
        break;
      default:
        if (!emitTree(callee))
            return false;
        *callop = false;             /* trigger JSOP_UNDEFINED after */
        break;
    }

    return true;
}

bool
BytecodeEmitter::emitPipeline(ParseNode* pn)
{
    MOZ_ASSERT(pn->isArity(PN_LIST));
    MOZ_ASSERT(pn->pn_count >= 2);

    if (!emitTree(pn->pn_head))
        return false;

    ParseNode* callee = pn->pn_head->pn_next;

    do {
        bool callop = true;
        if (!emitCallee(callee, pn, &callop))
            return false;

        // Emit room for |this|
        if (!callop) {
            if (!emit1(JSOP_UNDEFINED))
                return false;
        }

        if (!emit2(JSOP_PICK, 2))
            return false;

        if (!emitCall(JSOP_CALL, 1, pn))
            return false;

        checkTypeSet(JSOP_CALL);
    } while ((callee = callee->pn_next));

    return true;
}

bool
BytecodeEmitter::emitCallOrNew(ParseNode* pn, ValueUsage valueUsage /* = ValueUsage::WantValue */)
{
    bool callop =
        pn->isKind(ParseNodeKind::Call) || pn->isKind(ParseNodeKind::TaggedTemplate);
    /*
     * Emit callable invocation or operator new (constructor call) code.
     * First, emit code for the left operand to evaluate the callable or
     * constructable object expression.
     *
     * For operator new, we emit JSOP_GETPROP instead of JSOP_CALLPROP, etc.
     * This is necessary to interpose the lambda-initialized method read
     * barrier -- see the code in jsinterp.cpp for JSOP_LAMBDA followed by
     * JSOP_{SET,INIT}PROP.
     *
     * Then (or in a call case that has no explicit reference-base
     * object) we emit JSOP_UNDEFINED to produce the undefined |this|
     * value required for calls (which non-strict mode functions
     * will box into the global object).
     */
    uint32_t argc = pn->pn_count - 1;

    if (argc >= ARGC_LIMIT) {
        parser.reportError(callop ? JSMSG_TOO_MANY_FUN_ARGS : JSMSG_TOO_MANY_CON_ARGS);
        return false;
    }

    ParseNode* pn2 = pn->pn_head;
    bool spread = JOF_OPTYPE(pn->getOp()) == JOF_BYTE;

    if (pn2->isKind(ParseNodeKind::Name) && emitterMode == BytecodeEmitter::SelfHosting && !spread) {
        // Calls to "forceInterpreter", "callFunction",
        // "callContentFunction", or "resumeGenerator" in self-hosted
        // code generate inline bytecode.
        if (pn2->name() == cx->names().callFunction ||
            pn2->name() == cx->names().callContentFunction ||
            pn2->name() == cx->names().constructContentFunction)
        {
            return emitSelfHostedCallFunction(pn);
        }
        if (pn2->name() == cx->names().resumeGenerator)
            return emitSelfHostedResumeGenerator(pn);
        if (pn2->name() == cx->names().forceInterpreter)
            return emitSelfHostedForceInterpreter();
        if (pn2->name() == cx->names().allowContentIter)
            return emitSelfHostedAllowContentIter(pn);
        if (pn2->name() == cx->names().defineDataPropertyIntrinsic && pn->pn_count == 4)
            return emitSelfHostedDefineDataProperty(pn);
        if (pn2->name() == cx->names().hasOwn)
            return emitSelfHostedHasOwn(pn);
        if (pn2->name() == cx->names().getPropertySuper)
            return emitSelfHostedGetPropertySuper(pn);
        // Fall through
    }

    if (!emitCallee(pn2, pn, &callop))
        return false;

    bool isNewOp = pn->getOp() == JSOP_NEW || pn->getOp() == JSOP_SPREADNEW ||
                   pn->getOp() == JSOP_SUPERCALL || pn->getOp() == JSOP_SPREADSUPERCALL;


    // Emit room for |this|.
    if (!callop) {
        if (isNewOp) {
            if (!emit1(JSOP_IS_CONSTRUCTING))
                return false;
        } else {
            if (!emit1(JSOP_UNDEFINED))
                return false;
        }
    }

    /*
     * Emit code for each argument in order, then emit the JSOP_*CALL or
     * JSOP_NEW bytecode with a two-byte immediate telling how many args
     * were pushed on the operand stack.
     */
    if (!spread) {
        for (ParseNode* pn3 = pn2->pn_next; pn3; pn3 = pn3->pn_next) {
            if (!emitTree(pn3))
                return false;
        }

        if (isNewOp) {
            if (pn->isKind(ParseNodeKind::SuperCall)) {
                if (!emit1(JSOP_NEWTARGET))
                    return false;
            } else {
                // Repush the callee as new.target
                if (!emitDupAt(argc + 1))
                    return false;
            }
        }
    } else {
        ParseNode* args = pn2->pn_next;
        bool emitOptCode = (argc == 1) && isRestParameter(args->pn_kid);
        IfThenElseEmitter ifNotOptimizable(this);

        if (emitOptCode) {
            // Emit a preparation code to optimize the spread call with a rest
            // parameter:
            //
            //   function f(...args) {
            //     g(...args);
            //   }
            //
            // If the spread operand is a rest parameter and it's optimizable
            // array, skip spread operation and pass it directly to spread call
            // operation.  See the comment in OptimizeSpreadCall in
            // Interpreter.cpp for the optimizable conditons.

            if (!emitTree(args->pn_kid))
                return false;

            if (!emit1(JSOP_OPTIMIZE_SPREADCALL))
                return false;

            if (!emit1(JSOP_NOT))
                return false;

            if (!ifNotOptimizable.emitIf())
                return false;

            if (!emit1(JSOP_POP))
                return false;
        }

        if (!emitArray(args, argc))
            return false;

        if (emitOptCode) {
            if (!ifNotOptimizable.emitEnd())
                return false;
        }

        if (isNewOp) {
            if (pn->isKind(ParseNodeKind::SuperCall)) {
                if (!emit1(JSOP_NEWTARGET))
                    return false;
            } else {
                if (!emitDupAt(2))
                    return false;
            }
        }
    }

    if (!spread) {
        if (pn->getOp() == JSOP_CALL && valueUsage == ValueUsage::IgnoreValue) {
            if (!emitCall(JSOP_CALL_IGNORES_RV, argc, pn))
                return false;
            checkTypeSet(JSOP_CALL_IGNORES_RV);
        } else {
            if (!emitCall(pn->getOp(), argc, pn))
                return false;
            checkTypeSet(pn->getOp());
        }
    } else {
        if (!emit1(pn->getOp()))
            return false;
        checkTypeSet(pn->getOp());
    }
    if (pn->isOp(JSOP_EVAL) ||
        pn->isOp(JSOP_STRICTEVAL) ||
        pn->isOp(JSOP_SPREADEVAL) ||
        pn->isOp(JSOP_STRICTSPREADEVAL))
    {
        uint32_t lineNum = parser.tokenStream().srcCoords.lineNum(pn->pn_pos.begin);
        if (!emitUint32Operand(JSOP_LINENO, lineNum))
            return false;
    }

    return true;
}

static const JSOp ParseNodeKindToJSOp[] = {
    // JSOP_NOP is for pipeline operator which does not emit its own JSOp
    // but has highest precedence in binary operators
    JSOP_NOP,
    JSOP_OR,
    JSOP_AND,
    JSOP_BITOR,
    JSOP_BITXOR,
    JSOP_BITAND,
    JSOP_STRICTEQ,
    JSOP_EQ,
    JSOP_STRICTNE,
    JSOP_NE,
    JSOP_LT,
    JSOP_LE,
    JSOP_GT,
    JSOP_GE,
    JSOP_INSTANCEOF,
    JSOP_IN,
    JSOP_LSH,
    JSOP_RSH,
    JSOP_URSH,
    JSOP_ADD,
    JSOP_SUB,
    JSOP_MUL,
    JSOP_DIV,
    JSOP_MOD,
    JSOP_POW
};

static inline JSOp
BinaryOpParseNodeKindToJSOp(ParseNodeKind pnk)
{
    MOZ_ASSERT(pnk >= ParseNodeKind::BinOpFirst);
    MOZ_ASSERT(pnk <= ParseNodeKind::BinOpLast);
    return ParseNodeKindToJSOp[size_t(pnk) - size_t(ParseNodeKind::BinOpFirst)];
}

bool
BytecodeEmitter::emitRightAssociative(ParseNode* pn)
{
    // ** is the only right-associative operator.
    MOZ_ASSERT(pn->isKind(ParseNodeKind::Pow));
    MOZ_ASSERT(pn->isArity(PN_LIST));

    // Right-associative operator chain.
    for (ParseNode* subexpr = pn->pn_head; subexpr; subexpr = subexpr->pn_next) {
        if (!emitTree(subexpr))
            return false;
    }
    for (uint32_t i = 0; i < pn->pn_count - 1; i++) {
        if (!emit1(JSOP_POW))
            return false;
    }
    return true;
}

bool
BytecodeEmitter::emitLeftAssociative(ParseNode* pn)
{
    MOZ_ASSERT(pn->isArity(PN_LIST));

    // Left-associative operator chain.
    if (!emitTree(pn->pn_head))
        return false;
    JSOp op = BinaryOpParseNodeKindToJSOp(pn->getKind());
    ParseNode* nextExpr = pn->pn_head->pn_next;
    do {
        if (!emitTree(nextExpr))
            return false;
        if (!emit1(op))
            return false;
    } while ((nextExpr = nextExpr->pn_next));
    return true;
}

bool
BytecodeEmitter::emitLogical(ParseNode* pn)
{
    MOZ_ASSERT(pn->isArity(PN_LIST));
    MOZ_ASSERT(pn->isKind(ParseNodeKind::Or) || pn->isKind(ParseNodeKind::And));

    /*
     * JSOP_OR converts the operand on the stack to boolean, leaves the original
     * value on the stack and jumps if true; otherwise it falls into the next
     * bytecode, which pops the left operand and then evaluates the right operand.
     * The jump goes around the right operand evaluation.
     *
     * JSOP_AND converts the operand on the stack to boolean and jumps if false;
     * otherwise it falls into the right operand's bytecode.
     */

    TDZCheckCache tdzCache(this);

    /* Left-associative operator chain: avoid too much recursion. */
    ParseNode* pn2 = pn->pn_head;
    if (!emitTree(pn2))
        return false;
    JSOp op = pn->isKind(ParseNodeKind::Or) ? JSOP_OR : JSOP_AND;
    JumpList jump;
    if (!emitJump(op, &jump))
        return false;
    if (!emit1(JSOP_POP))
        return false;

    /* Emit nodes between the head and the tail. */
    while ((pn2 = pn2->pn_next)->pn_next) {
        if (!emitTree(pn2))
            return false;
        if (!emitJump(op, &jump))
            return false;
        if (!emit1(JSOP_POP))
            return false;
    }
    if (!emitTree(pn2))
        return false;

    if (!emitJumpTargetAndPatch(jump))
        return false;
    return true;
}

bool
BytecodeEmitter::emitSequenceExpr(ParseNode* pn,
                                  ValueUsage valueUsage /* = ValueUsage::WantValue */)
{
    for (ParseNode* child = pn->pn_head; ; child = child->pn_next) {
        if (!updateSourceCoordNotes(child->pn_pos.begin))
            return false;
        if (!emitTree(child, child->pn_next ? ValueUsage::IgnoreValue : valueUsage))
            return false;
        if (!child->pn_next)
            break;
        if (!emit1(JSOP_POP))
            return false;
    }
    return true;
}

// Using MOZ_NEVER_INLINE in here is a workaround for llvm.org/pr14047. See
// the comment on emitSwitch.
MOZ_NEVER_INLINE bool
BytecodeEmitter::emitIncOrDec(ParseNode* pn)
{
    switch (pn->pn_kid->getKind()) {
      case ParseNodeKind::Dot:
        return emitPropIncDec(pn);
      case ParseNodeKind::Elem:
        return emitElemIncDec(pn);
      case ParseNodeKind::Call:
        return emitCallIncDec(pn);
      default:
        return emitNameIncDec(pn);
    }
}

// Using MOZ_NEVER_INLINE in here is a workaround for llvm.org/pr14047. See
// the comment on emitSwitch.
MOZ_NEVER_INLINE bool
BytecodeEmitter::emitLabeledStatement(const LabeledStatement* pn)
{
    /*
     * Emit a JSOP_LABEL instruction. The argument is the offset to the statement
     * following the labeled statement.
     */
    uint32_t index;
    if (!makeAtomIndex(pn->label(), &index))
        return false;

    JumpList top;
    if (!emitJump(JSOP_LABEL, &top))
        return false;

    /* Emit code for the labeled statement. */
    LabelControl controlInfo(this, pn->label(), offset());

    if (!emitTree(pn->statement()))
        return false;

    /* Patch the JSOP_LABEL offset. */
    JumpTarget brk{ lastNonJumpTargetOffset() };
    patchJumpsToTarget(top, brk);

    if (!controlInfo.patchBreaks(this))
        return false;

    return true;
}

bool
BytecodeEmitter::emitConditionalExpression(ConditionalExpression& conditional,
                                           ValueUsage valueUsage /* = ValueUsage::WantValue */)
{
    /* Emit the condition, then branch if false to the else part. */
    if (!emitTree(&conditional.condition()))
        return false;

    IfThenElseEmitter ifThenElse(this);
    if (!ifThenElse.emitCond())
        return false;

    if (!emitTreeInBranch(&conditional.thenExpression(), valueUsage))
        return false;

    if (!ifThenElse.emitElse())
        return false;

    if (!emitTreeInBranch(&conditional.elseExpression(), valueUsage))
        return false;

    if (!ifThenElse.emitEnd())
        return false;
    MOZ_ASSERT(ifThenElse.pushed() == 1);

    return true;
}

bool
BytecodeEmitter::emitPropertyList(ParseNode* pn, MutableHandlePlainObject objp, PropListType type)
{
    for (ParseNode* propdef = pn->pn_head; propdef; propdef = propdef->pn_next) {
        if (!updateSourceCoordNotes(propdef->pn_pos.begin))
            return false;

        // Handle __proto__: v specially because *only* this form, and no other
        // involving "__proto__", performs [[Prototype]] mutation.
        if (propdef->isKind(ParseNodeKind::MutateProto)) {
            MOZ_ASSERT(type == ObjectLiteral);
            if (!emitTree(propdef->pn_kid))
                return false;
            objp.set(nullptr);
            if (!emit1(JSOP_MUTATEPROTO))
                return false;
            continue;
        }

        if (propdef->isKind(ParseNodeKind::Spread)) {
            MOZ_ASSERT(type == ObjectLiteral);

            if (!emit1(JSOP_DUP))
                return false;

            if (!emitTree(propdef->pn_kid))
                return false;

            if (!emitCopyDataProperties(CopyOption::Unfiltered))
                return false;

            objp.set(nullptr);
            continue;
        }

        bool extraPop = false;
        if (type == ClassBody && propdef->as<ClassMethod>().isStatic()) {
            extraPop = true;
            if (!emit1(JSOP_DUP2))
                return false;
            if (!emit1(JSOP_POP))
                return false;
        }

        /* Emit an index for t[2] for later consumption by JSOP_INITELEM. */
        ParseNode* key = propdef->pn_left;
        bool isIndex = false;
        if (key->isKind(ParseNodeKind::Number)) {
            if (!emitNumberOp(key->pn_dval))
                return false;
            isIndex = true;
        } else if (key->isKind(ParseNodeKind::ObjectPropertyName) ||
                   key->isKind(ParseNodeKind::String))
        {
            // EmitClass took care of constructor already.
            if (type == ClassBody && key->pn_atom == cx->names().constructor &&
                !propdef->as<ClassMethod>().isStatic())
            {
                continue;
            }
        } else {
            if (!emitComputedPropertyName(key))
                return false;
            isIndex = true;
        }

        /* Emit code for the property initializer. */
        if (!emitTree(propdef->pn_right))
            return false;

        JSOp op = propdef->getOp();
        MOZ_ASSERT(op == JSOP_INITPROP ||
                   op == JSOP_INITPROP_GETTER ||
                   op == JSOP_INITPROP_SETTER);

        FunctionPrefixKind prefixKind = op == JSOP_INITPROP_GETTER ? FunctionPrefixKind::Get
                                        : op == JSOP_INITPROP_SETTER ? FunctionPrefixKind::Set
                                        : FunctionPrefixKind::None;

        if (op == JSOP_INITPROP_GETTER || op == JSOP_INITPROP_SETTER)
            objp.set(nullptr);

        if (propdef->pn_right->isKind(ParseNodeKind::Function) &&
            propdef->pn_right->pn_funbox->needsHomeObject())
        {
            MOZ_ASSERT(propdef->pn_right->pn_funbox->function()->allowSuperProperty());
            bool isAsync = propdef->pn_right->pn_funbox->isAsync();
            if (isAsync) {
                if (!emit1(JSOP_SWAP))
                    return false;
            }
            if (!emit2(JSOP_INITHOMEOBJECT, isIndex + isAsync))
                return false;
            if (isAsync) {
                if (!emit1(JSOP_POP))
                    return false;
            }
        }

        // Class methods are not enumerable.
        if (type == ClassBody) {
            switch (op) {
              case JSOP_INITPROP:        op = JSOP_INITHIDDENPROP;          break;
              case JSOP_INITPROP_GETTER: op = JSOP_INITHIDDENPROP_GETTER;   break;
              case JSOP_INITPROP_SETTER: op = JSOP_INITHIDDENPROP_SETTER;   break;
              default: MOZ_CRASH("Invalid op");
            }
        }

        if (isIndex) {
            objp.set(nullptr);
            switch (op) {
              case JSOP_INITPROP:               op = JSOP_INITELEM;              break;
              case JSOP_INITHIDDENPROP:         op = JSOP_INITHIDDENELEM;        break;
              case JSOP_INITPROP_GETTER:        op = JSOP_INITELEM_GETTER;       break;
              case JSOP_INITHIDDENPROP_GETTER:  op = JSOP_INITHIDDENELEM_GETTER; break;
              case JSOP_INITPROP_SETTER:        op = JSOP_INITELEM_SETTER;       break;
              case JSOP_INITHIDDENPROP_SETTER:  op = JSOP_INITHIDDENELEM_SETTER; break;
              default: MOZ_CRASH("Invalid op");
            }
            if (propdef->pn_right->isDirectRHSAnonFunction()) {
                if (!emitDupAt(1))
                    return false;
                if (!emit2(JSOP_SETFUNNAME, uint8_t(prefixKind)))
                    return false;
            }
            if (!emit1(op))
                return false;
        } else {
            MOZ_ASSERT(key->isKind(ParseNodeKind::ObjectPropertyName) ||
                       key->isKind(ParseNodeKind::String));

            uint32_t index;
            if (!makeAtomIndex(key->pn_atom, &index))
                return false;

            if (objp) {
                MOZ_ASSERT(type == ObjectLiteral);
                MOZ_ASSERT(!IsHiddenInitOp(op));
                MOZ_ASSERT(!objp->inDictionaryMode());
                Rooted<jsid> id(cx, AtomToId(key->pn_atom));
                if (!NativeDefineDataProperty(cx, objp, id, UndefinedHandleValue,
                                              JSPROP_ENUMERATE))
                {
                    return false;
                }
                if (objp->inDictionaryMode())
                    objp.set(nullptr);
            }

            if (propdef->pn_right->isDirectRHSAnonFunction()) {
                RootedAtom keyName(cx, key->pn_atom);
                if (!setOrEmitSetFunName(propdef->pn_right, keyName, prefixKind))
                    return false;
            }
            if (!emitIndex32(op, index))
                return false;
        }

        if (extraPop) {
            if (!emit1(JSOP_POP))
                return false;
        }
    }
    return true;
}

// Using MOZ_NEVER_INLINE in here is a workaround for llvm.org/pr14047. See
// the comment on emitSwitch.
MOZ_NEVER_INLINE bool
BytecodeEmitter::emitObject(ParseNode* pn)
{
    if (!(pn->pn_xflags & PNX_NONCONST) && pn->pn_head && checkSingletonContext())
        return emitSingletonInitialiser(pn);

    /*
     * Emit code for {p:a, '%q':b, 2:c} that is equivalent to constructing
     * a new object and defining (in source order) each property on the object
     * (or mutating the object's [[Prototype]], in the case of __proto__).
     */
    ptrdiff_t offset = this->offset();
    if (!emitNewInit(JSProto_Object))
        return false;

    // Try to construct the shape of the object as we go, so we can emit a
    // JSOP_NEWOBJECT with the final shape instead.
    // In the case of computed property names and indices, we cannot fix the
    // shape at bytecode compile time. When the shape cannot be determined,
    // |obj| is nulled out.

    // No need to do any guessing for the object kind, since we know the upper
    // bound of how many properties we plan to have.
    gc::AllocKind kind = gc::GetGCObjectKind(pn->pn_count);
    RootedPlainObject obj(cx, NewBuiltinClassInstance<PlainObject>(cx, kind, TenuredObject));
    if (!obj)
        return false;

    if (!emitPropertyList(pn, &obj, ObjectLiteral))
        return false;

    if (obj) {
        // The object survived and has a predictable shape: update the original
        // bytecode.
        if (!replaceNewInitWithNewObject(obj, offset))
            return false;
    }

    return true;
}

bool
BytecodeEmitter::replaceNewInitWithNewObject(JSObject* obj, ptrdiff_t offset)
{
    ObjectBox* objbox = parser.newObjectBox(obj);
    if (!objbox)
        return false;

    static_assert(JSOP_NEWINIT_LENGTH == JSOP_NEWOBJECT_LENGTH,
                  "newinit and newobject must have equal length to edit in-place");

    uint32_t index = objectList.add(objbox);
    jsbytecode* code = this->code(offset);

    MOZ_ASSERT(code[0] == JSOP_NEWINIT);
    code[0] = JSOP_NEWOBJECT;
    SET_UINT32(code, index);

    return true;
}

bool
BytecodeEmitter::emitArrayLiteral(ParseNode* pn)
{
    if (!(pn->pn_xflags & PNX_NONCONST) && pn->pn_head) {
        if (checkSingletonContext()) {
            // Bake in the object entirely if it will only be created once.
            return emitSingletonInitialiser(pn);
        }

        // If the array consists entirely of primitive values, make a
        // template object with copy on write elements that can be reused
        // every time the initializer executes. Don't do this if the array is
        // small: copying the elements lazily is not worth it in that case.
        static const size_t MinElementsForCopyOnWrite = 5;
        if (emitterMode != BytecodeEmitter::SelfHosting &&
            pn->pn_count >= MinElementsForCopyOnWrite)
        {
            RootedValue value(cx);
            if (!pn->getConstantValue(cx, ParseNode::ForCopyOnWriteArray, &value))
                return false;
            if (!value.isMagic(JS_GENERIC_MAGIC)) {
                // Note: the group of the template object might not yet reflect
                // that the object has copy on write elements. When the
                // interpreter or JIT compiler fetches the template, it should
                // use ObjectGroup::getOrFixupCopyOnWriteObject to make sure the
                // group for the template is accurate. We don't do this here as we
                // want to use ObjectGroup::allocationSiteGroup, which requires a
                // finished script.
                JSObject* obj = &value.toObject();
                MOZ_ASSERT(obj->is<ArrayObject>() &&
                           obj->as<ArrayObject>().denseElementsAreCopyOnWrite());

                ObjectBox* objbox = parser.newObjectBox(obj);
                if (!objbox)
                    return false;

                return emitObjectOp(objbox, JSOP_NEWARRAY_COPYONWRITE);
            }
        }
    }

    return emitArray(pn->pn_head, pn->pn_count);
}

bool
BytecodeEmitter::emitArray(ParseNode* pn, uint32_t count)
{

    /*
     * Emit code for [a, b, c] that is equivalent to constructing a new
     * array and in source order evaluating each element value and adding
     * it to the array, without invoking latent setters.  We use the
     * JSOP_NEWINIT and JSOP_INITELEM_ARRAY bytecodes to ignore setters and
     * to avoid dup'ing and popping the array as each element is added, as
     * JSOP_SETELEM/JSOP_SETPROP would do.
     */

    uint32_t nspread = 0;
    for (ParseNode* elt = pn; elt; elt = elt->pn_next) {
        if (elt->isKind(ParseNodeKind::Spread))
            nspread++;
    }

    // Array literal's length is limited to NELEMENTS_LIMIT in parser.
    static_assert(NativeObject::MAX_DENSE_ELEMENTS_COUNT <= INT32_MAX,
                  "array literals' maximum length must not exceed limits "
                  "required by BaselineCompiler::emit_JSOP_NEWARRAY, "
                  "BaselineCompiler::emit_JSOP_INITELEM_ARRAY, "
                  "and DoSetElemFallback's handling of JSOP_INITELEM_ARRAY");
    MOZ_ASSERT(count >= nspread);
    MOZ_ASSERT(count <= NativeObject::MAX_DENSE_ELEMENTS_COUNT,
               "the parser must throw an error if the array exceeds maximum "
               "length");

    // For arrays with spread, this is a very pessimistic allocation, the
    // minimum possible final size.
    if (!emitUint32Operand(JSOP_NEWARRAY, count - nspread))         // ARRAY
        return false;

    ParseNode* pn2 = pn;
    uint32_t index;
    bool afterSpread = false;
    for (index = 0; pn2; index++, pn2 = pn2->pn_next) {
        if (!afterSpread && pn2->isKind(ParseNodeKind::Spread)) {
            afterSpread = true;
            if (!emitNumberOp(index))                               // ARRAY INDEX
                return false;
        }
        if (!updateSourceCoordNotes(pn2->pn_pos.begin))
            return false;

        bool allowSelfHostedIter = false;
        if (pn2->isKind(ParseNodeKind::Elision)) {
            if (!emit1(JSOP_HOLE))
                return false;
        } else {
            ParseNode* expr;
            if (pn2->isKind(ParseNodeKind::Spread)) {
                expr = pn2->pn_kid;

                if (emitterMode == BytecodeEmitter::SelfHosting &&
                    expr->isKind(ParseNodeKind::Call) &&
                    expr->pn_head->name() == cx->names().allowContentIter)
                {
                    allowSelfHostedIter = true;
                }
            } else {
                expr = pn2;
            }
            if (!emitTree(expr))                                         // ARRAY INDEX? VALUE
                return false;
        }
        if (pn2->isKind(ParseNodeKind::Spread)) {
            if (!emitIterator())                                         // ARRAY INDEX NEXT ITER
                return false;
            if (!emit2(JSOP_PICK, 3))                                    // INDEX NEXT ITER ARRAY
                return false;
            if (!emit2(JSOP_PICK, 3))                                    // NEXT ITER ARRAY INDEX
                return false;
            if (!emitSpread(allowSelfHostedIter))                        // ARRAY INDEX
                return false;
        } else if (afterSpread) {
            if (!emit1(JSOP_INITELEM_INC))
                return false;
        } else {
            if (!emitUint32Operand(JSOP_INITELEM_ARRAY, index))
                return false;
        }
    }
    MOZ_ASSERT(index == count);
    if (afterSpread) {
        if (!emit1(JSOP_POP))                                            // ARRAY
            return false;
    }
    return true;
}

static inline JSOp
UnaryOpParseNodeKindToJSOp(ParseNodeKind pnk)
{
    switch (pnk) {
      case ParseNodeKind::Throw: return JSOP_THROW;
      case ParseNodeKind::Void: return JSOP_VOID;
      case ParseNodeKind::Not: return JSOP_NOT;
      case ParseNodeKind::BitNot: return JSOP_BITNOT;
      case ParseNodeKind::Pos: return JSOP_POS;
      case ParseNodeKind::Neg: return JSOP_NEG;
      default: MOZ_CRASH("unexpected unary op");
    }
}

bool
BytecodeEmitter::emitUnary(ParseNode* pn)
{
    if (!updateSourceCoordNotes(pn->pn_pos.begin))
        return false;
    if (!emitTree(pn->pn_kid))
        return false;
    return emit1(UnaryOpParseNodeKindToJSOp(pn->getKind()));
}

bool
BytecodeEmitter::emitTypeof(ParseNode* node, JSOp op)
{
    MOZ_ASSERT(op == JSOP_TYPEOF || op == JSOP_TYPEOFEXPR);

    if (!updateSourceCoordNotes(node->pn_pos.begin))
        return false;

    if (!emitTree(node->pn_kid))
        return false;

    return emit1(op);
}

bool
BytecodeEmitter::emitFunctionFormalParametersAndBody(ParseNode *pn)
{
    MOZ_ASSERT(pn->isKind(ParseNodeKind::ParamsBody));

    ParseNode* funBody = pn->last();
    FunctionBox* funbox = sc->asFunctionBox();

    TDZCheckCache tdzCache(this);

    if (funbox->hasParameterExprs) {
        EmitterScope funEmitterScope(this);
        if (!funEmitterScope.enterFunction(this, funbox))
            return false;

        if (!emitInitializeFunctionSpecialNames())
            return false;

        if (!emitFunctionFormalParameters(pn))
            return false;

        {
            Maybe<EmitterScope> extraVarEmitterScope;

            if (funbox->hasExtraBodyVarScope()) {
                extraVarEmitterScope.emplace(this);
                if (!extraVarEmitterScope->enterFunctionExtraBodyVar(this, funbox))
                    return false;

                // After emitting expressions for all parameters, copy over any
                // formal parameters which have been redeclared as vars. For
                // example, in the following, the var y in the body scope is 42:
                //
                //   function f(x, y = 42) { var y; }
                //
                RootedAtom name(cx);
                if (funbox->extraVarScopeBindings() && funbox->functionScopeBindings()) {
                    for (BindingIter bi(*funbox->functionScopeBindings(), true); bi; bi++) {
                        name = bi.name();

                        // There may not be a var binding of the same name.
                        if (!locationOfNameBoundInScope(name, extraVarEmitterScope.ptr()))
                            continue;

                        // The '.this' and '.generator' function special
                        // bindings should never appear in the extra var
                        // scope. 'arguments', however, may.
                        MOZ_ASSERT(name != cx->names().dotThis &&
                                   name != cx->names().dotGenerator);

                        NameLocation paramLoc = *locationOfNameBoundInScope(name, &funEmitterScope);
                        auto emitRhs = [&name, &paramLoc](BytecodeEmitter* bce,
                                                          const NameLocation&, bool)
                        {
                            return bce->emitGetNameAtLocation(name, paramLoc);
                        };

                        if (!emitInitializeName(name, emitRhs))
                            return false;
                        if (!emit1(JSOP_POP))
                            return false;
                    }
                }
            }

            if (!emitFunctionBody(funBody))
                return false;

            if (extraVarEmitterScope && !extraVarEmitterScope->leave(this))
                return false;
        }

        return funEmitterScope.leave(this);
    }

    // No parameter expressions. Enter the function body scope and emit
    // everything.
    //
    // One caveat is that Debugger considers ops in the prologue to be
    // unreachable (i.e. cannot set a breakpoint on it). If there are no
    // parameter exprs, any unobservable environment ops (like pushing the
    // call object, setting '.this', etc) need to go in the prologue, else it
    // messes up breakpoint tests.
    EmitterScope emitterScope(this);

    switchToPrologue();
    if (!emitterScope.enterFunction(this, funbox))
        return false;

    if (!emitInitializeFunctionSpecialNames())
        return false;
    switchToMain();

    if (!emitFunctionFormalParameters(pn))
        return false;

    if (!emitFunctionBody(funBody))
        return false;

    return emitterScope.leave(this);
}

bool
BytecodeEmitter::emitFunctionFormalParameters(ParseNode* pn)
{
    ParseNode* funBody = pn->last();
    FunctionBox* funbox = sc->asFunctionBox();
    EmitterScope* funScope = innermostEmitterScope();

    bool hasParameterExprs = funbox->hasParameterExprs;
    bool hasRest = funbox->hasRest();

    uint16_t argSlot = 0;
    for (ParseNode* arg = pn->pn_head; arg != funBody; arg = arg->pn_next, argSlot++) {
        ParseNode* bindingElement = arg;
        ParseNode* initializer = nullptr;
        if (arg->isKind(ParseNodeKind::Assign)) {
            bindingElement = arg->pn_left;
            initializer = arg->pn_right;
        }

        // Left-hand sides are either simple names or destructuring patterns.
        MOZ_ASSERT(bindingElement->isKind(ParseNodeKind::Name) ||
                   bindingElement->isKind(ParseNodeKind::Array) ||
                   bindingElement->isKind(ParseNodeKind::Object));

        // The rest parameter doesn't have an initializer.
        bool isRest = hasRest && arg->pn_next == funBody;
        MOZ_ASSERT_IF(isRest, !initializer);

        bool isDestructuring = !bindingElement->isKind(ParseNodeKind::Name);

        // ES 14.1.19 says if BindingElement contains an expression in the
        // production FormalParameter : BindingElement, it is evaluated in a
        // new var environment. This is needed to prevent vars from escaping
        // direct eval in parameter expressions.
        Maybe<EmitterScope> paramExprVarScope;
        if (funbox->hasDirectEvalInParameterExpr && (isDestructuring || initializer)) {
            paramExprVarScope.emplace(this);
            if (!paramExprVarScope->enterParameterExpressionVar(this))
                return false;
        }

        // First push the RHS if there is a default expression or if it is
        // rest.

        if (initializer) {
            // If we have an initializer, emit the initializer and assign it
            // to the argument slot. TDZ is taken care of afterwards.
            MOZ_ASSERT(hasParameterExprs);
            if (!emitArgOp(JSOP_GETARG, argSlot))
                return false;
            if (!emit1(JSOP_DUP))
                return false;
            if (!emit1(JSOP_UNDEFINED))
                return false;
            if (!emit1(JSOP_STRICTEQ))
                return false;
            // Emit source note to enable Ion compilation.
            if (!newSrcNote(SRC_IF))
                return false;
            JumpList jump;
            if (!emitJump(JSOP_IFEQ, &jump))
                return false;
            if (!emit1(JSOP_POP))
                return false;
            if (!emitInitializerInBranch(initializer, bindingElement))
                return false;
            if (!emitJumpTargetAndPatch(jump))
                return false;
        } else if (isRest) {
            if (!emit1(JSOP_REST))
                return false;
            checkTypeSet(JSOP_REST);
        }

        // Initialize the parameter name.

        if (isDestructuring) {
            // If we had an initializer or the rest parameter, the value is
            // already on the stack.
            if (!initializer && !isRest && !emitArgOp(JSOP_GETARG, argSlot))
                return false;

            // If there's an parameter expression var scope, the destructuring
            // declaration needs to initialize the name in the function scope,
            // which is not the innermost scope.
            if (!emitDestructuringOps(bindingElement,
                                      paramExprVarScope
                                      ? DestructuringFormalParameterInVarScope
                                      : DestructuringDeclaration))
            {
                return false;
            }

            if (!emit1(JSOP_POP))
                return false;
        } else {
            RootedAtom paramName(cx, bindingElement->name());
            NameLocation paramLoc = *locationOfNameBoundInScope(paramName, funScope);

            if (hasParameterExprs) {
                auto emitRhs = [argSlot, initializer, isRest](BytecodeEmitter* bce,
                                                              const NameLocation&, bool)
                {
                    // If we had an initializer or a rest parameter, the value is
                    // already on the stack.
                    if (!initializer && !isRest)
                        return bce->emitArgOp(JSOP_GETARG, argSlot);
                    return true;
                };

                if (!emitSetOrInitializeNameAtLocation(paramName, paramLoc, emitRhs, true))
                    return false;
                if (!emit1(JSOP_POP))
                    return false;
            } else if (isRest) {
                // The rest value is already on top of the stack.
                auto nop = [](BytecodeEmitter*, const NameLocation&, bool) {
                    return true;
                };

                if (!emitSetOrInitializeNameAtLocation(paramName, paramLoc, nop, true))
                    return false;
                if (!emit1(JSOP_POP))
                    return false;
            }
        }

        if (paramExprVarScope) {
            if (!paramExprVarScope->leave(this))
                return false;
        }
    }

    return true;
}

bool
BytecodeEmitter::emitInitializeFunctionSpecialNames()
{
    FunctionBox* funbox = sc->asFunctionBox();

    auto emitInitializeFunctionSpecialName = [](BytecodeEmitter* bce, HandlePropertyName name,
                                                JSOp op)
    {
        // A special name must be slotful, either on the frame or on the
        // call environment.
        MOZ_ASSERT(bce->lookupName(name).hasKnownSlot());

        auto emitInitial = [op](BytecodeEmitter* bce, const NameLocation&, bool) {
            return bce->emit1(op);
        };

        if (!bce->emitInitializeName(name, emitInitial))
            return false;
        if (!bce->emit1(JSOP_POP))
            return false;

        return true;
    };

    // Do nothing if the function doesn't have an arguments binding.
    if (funbox->argumentsHasLocalBinding()) {
        if (!emitInitializeFunctionSpecialName(this, cx->names().arguments, JSOP_ARGUMENTS))
            return false;
    }

    // Do nothing if the function doesn't have a this-binding (this
    // happens for instance if it doesn't use this/eval or if it's an
    // arrow function).
    if (funbox->hasThisBinding()) {
        if (!emitInitializeFunctionSpecialName(this, cx->names().dotThis, JSOP_FUNCTIONTHIS))
            return false;
    }

    return true;
}

bool
BytecodeEmitter::emitFunctionBody(ParseNode* funBody)
{
    FunctionBox* funbox = sc->asFunctionBox();

    if (!emitTree(funBody))
        return false;

    if (funbox->needsFinalYield()) {
        // If we fall off the end of a generator, do a final yield.
        bool needsIteratorResult = funbox->needsIteratorResult();
        if (needsIteratorResult) {
            if (!emitPrepareIteratorResult())
                return false;
        }

        if (!emit1(JSOP_UNDEFINED))
            return false;

        if (needsIteratorResult) {
            if (!emitFinishIteratorResult(true))
                return false;
        }

        if (!emit1(JSOP_SETRVAL))
            return false;

        if (!emitGetDotGeneratorInInnermostScope())
            return false;

        // No need to check for finally blocks, etc as in EmitReturn.
        if (!emitYieldOp(JSOP_FINALYIELDRVAL))
            return false;
    } else {
        // Non-generator functions just return |undefined|. The
        // JSOP_RETRVAL emitted below will do that, except if the
        // script has a finally block: there can be a non-undefined
        // value in the return value slot. Make sure the return value
        // is |undefined|.
        if (hasTryFinally) {
            if (!emit1(JSOP_UNDEFINED))
                return false;
            if (!emit1(JSOP_SETRVAL))
                return false;
        }
    }

    if (funbox->isDerivedClassConstructor()) {
        if (!emitCheckDerivedClassConstructorReturn())
            return false;
    }

    return true;
}

bool
BytecodeEmitter::emitLexicalInitialization(ParseNode* pn)
{
    // The caller has pushed the RHS to the top of the stack. Assert that the
    // name is lexical and no BIND[G]NAME ops were emitted.
    auto assertLexical = [](BytecodeEmitter*, const NameLocation& loc, bool emittedBindOp) {
        MOZ_ASSERT(loc.isLexical());
        MOZ_ASSERT(!emittedBindOp);
        return true;
    };
    return emitInitializeName(pn, assertLexical);
}

// This follows ES6 14.5.14 (ClassDefinitionEvaluation) and ES6 14.5.15
// (BindingClassDeclarationEvaluation).
bool
BytecodeEmitter::emitClass(ParseNode* pn)
{
    ClassNode& classNode = pn->as<ClassNode>();

    ClassNames* names = classNode.names();

    ParseNode* heritageExpression = classNode.heritage();

    ParseNode* classMethods = classNode.methodList();
    ParseNode* constructor = nullptr;
    for (ParseNode* mn = classMethods->pn_head; mn; mn = mn->pn_next) {
        ClassMethod& method = mn->as<ClassMethod>();
        ParseNode& methodName = method.name();
        if (!method.isStatic() &&
            (methodName.isKind(ParseNodeKind::ObjectPropertyName) ||
             methodName.isKind(ParseNodeKind::String)) &&
            methodName.pn_atom == cx->names().constructor)
        {
            constructor = &method.method();
            break;
        }
    }

    bool savedStrictness = sc->setLocalStrictMode(true);

    Maybe<TDZCheckCache> tdzCache;
    Maybe<EmitterScope> emitterScope;
    if (names) {
        tdzCache.emplace(this);
        emitterScope.emplace(this);
        if (!emitterScope->enterLexical(this, ScopeKind::Lexical, classNode.scopeBindings()))
            return false;
    }

    // Pseudocode for class declarations:
    //
    //     class extends BaseExpression {
    //       constructor() { ... }
    //       ...
    //       }
    //
    //
    //   if defined <BaseExpression> {
    //     let heritage = BaseExpression;
    //
    //     if (heritage !== null) {
    //       funProto = heritage;
    //       objProto = heritage.prototype;
    //     } else {
    //       funProto = %FunctionPrototype%;
    //       objProto = null;
    //     }
    //   } else {
    //     objProto = %ObjectPrototype%;
    //   }
    //
    //   let homeObject = ObjectCreate(objProto);
    //
    //   if defined <constructor> {
    //     if defined <BaseExpression> {
    //       cons = DefineMethod(<constructor>, proto=homeObject, funProto=funProto);
    //     } else {
    //       cons = DefineMethod(<constructor>, proto=homeObject);
    //     }
    //   } else {
    //     if defined <BaseExpression> {
    //       cons = DefaultDerivedConstructor(proto=homeObject, funProto=funProto);
    //     } else {
    //       cons = DefaultConstructor(proto=homeObject);
    //     }
    //   }
    //
    //   cons.prototype = homeObject;
    //   homeObject.constructor = cons;
    //
    //   EmitPropertyList(...)

    // This is kind of silly. In order to the get the home object defined on
    // the constructor, we have to make it second, but we want the prototype
    // on top for EmitPropertyList, because we expect static properties to be
    // rarer. The result is a few more swaps than we would like. Such is life.
    if (heritageExpression) {
        IfThenElseEmitter ifThenElse(this);

        if (!emitTree(heritageExpression))                      // ... HERITAGE
            return false;

        // Heritage must be null or a non-generator constructor
        if (!emit1(JSOP_CHECKCLASSHERITAGE))                    // ... HERITAGE
            return false;

        // [IF] (heritage !== null)
        if (!emit1(JSOP_DUP))                                   // ... HERITAGE HERITAGE
            return false;
        if (!emit1(JSOP_NULL))                                  // ... HERITAGE HERITAGE NULL
            return false;
        if (!emit1(JSOP_STRICTNE))                              // ... HERITAGE NE
            return false;

        // [THEN] funProto = heritage, objProto = heritage.prototype
        if (!ifThenElse.emitIfElse())
            return false;
        if (!emit1(JSOP_DUP))                                   // ... HERITAGE HERITAGE
            return false;
        if (!emitAtomOp(cx->names().prototype, JSOP_GETPROP))   // ... HERITAGE PROTO
            return false;

        // [ELSE] funProto = %FunctionPrototype%, objProto = null
        if (!ifThenElse.emitElse())
            return false;
        if (!emit1(JSOP_POP))                                   // ...
            return false;
        if (!emit2(JSOP_BUILTINPROTO, JSProto_Function))        // ... PROTO
            return false;
        if (!emit1(JSOP_NULL))                                  // ... PROTO NULL
            return false;

        // [ENDIF]
        if (!ifThenElse.emitEnd())
            return false;

        if (!emit1(JSOP_OBJWITHPROTO))                          // ... HERITAGE HOMEOBJ
            return false;
        if (!emit1(JSOP_SWAP))                                  // ... HOMEOBJ HERITAGE
            return false;
    } else {
        if (!emitNewInit(JSProto_Object))                       // ... HOMEOBJ
            return false;
    }

    // Stack currently has HOMEOBJ followed by optional HERITAGE. When HERITAGE
    // is not used, an implicit value of %FunctionPrototype% is implied.

    if (constructor) {
        if (!emitFunction(constructor, !!heritageExpression))   // ... HOMEOBJ CONSTRUCTOR
            return false;
        if (constructor->pn_funbox->needsHomeObject()) {
            if (!emit2(JSOP_INITHOMEOBJECT, 0))                 // ... HOMEOBJ CONSTRUCTOR
                return false;
        }
    } else {
        // In the case of default class constructors, emit the start and end
        // offsets in the source buffer as source notes so that when we
        // actually make the constructor during execution, we can give it the
        // correct toString output.
        ptrdiff_t classStart = ptrdiff_t(pn->pn_pos.begin);
        ptrdiff_t classEnd = ptrdiff_t(pn->pn_pos.end);
        if (!newSrcNote3(SRC_CLASS_SPAN, classStart, classEnd))
            return false;

        JSAtom *name = names ? names->innerBinding()->pn_atom : cx->names().empty;
        if (heritageExpression) {
            if (!emitAtomOp(name, JSOP_DERIVEDCONSTRUCTOR))     // ... HOMEOBJ CONSTRUCTOR
                return false;
        } else {
            if (!emitAtomOp(name, JSOP_CLASSCONSTRUCTOR))       // ... HOMEOBJ CONSTRUCTOR
                return false;
        }
    }

    if (!emit1(JSOP_SWAP))                                      // ... CONSTRUCTOR HOMEOBJ
        return false;

    if (!emit1(JSOP_DUP2))                                          // ... CONSTRUCTOR HOMEOBJ CONSTRUCTOR HOMEOBJ
        return false;
    if (!emitAtomOp(cx->names().prototype, JSOP_INITLOCKEDPROP))    // ... CONSTRUCTOR HOMEOBJ CONSTRUCTOR
        return false;
    if (!emitAtomOp(cx->names().constructor, JSOP_INITHIDDENPROP))  // ... CONSTRUCTOR HOMEOBJ
        return false;

    RootedPlainObject obj(cx);
    if (!emitPropertyList(classMethods, &obj, ClassBody))       // ... CONSTRUCTOR HOMEOBJ
        return false;

    if (!emit1(JSOP_POP))                                       // ... CONSTRUCTOR
        return false;

    if (names) {
        ParseNode* innerName = names->innerBinding();
        if (!emitLexicalInitialization(innerName))              // ... CONSTRUCTOR
            return false;

        // Pop the inner scope.
        if (!emitterScope->leave(this))
            return false;
        emitterScope.reset();

        ParseNode* outerName = names->outerBinding();
        if (outerName) {
            if (!emitLexicalInitialization(outerName))          // ... CONSTRUCTOR
                return false;
            // Only class statements make outer bindings, and they do not leave
            // themselves on the stack.
            if (!emit1(JSOP_POP))                               // ...
                return false;
        }
    }

    // The CONSTRUCTOR is left on stack if this is an expression.

    MOZ_ALWAYS_TRUE(sc->setLocalStrictMode(savedStrictness));

    return true;
}

bool
BytecodeEmitter::emitExportDefault(ParseNode* pn)
{
    if (!emitTree(pn->pn_left))
        return false;

    if (pn->pn_right) {
        if (!emitLexicalInitialization(pn->pn_right))
            return false;

        if (pn->pn_left->isDirectRHSAnonFunction()) {
            HandlePropertyName name = cx->names().default_;
            if (!setOrEmitSetFunName(pn->pn_left, name, FunctionPrefixKind::None))
                return false;
        }

        if (!emit1(JSOP_POP))
            return false;
    }

    return true;
}

bool
BytecodeEmitter::emitTree(ParseNode* pn, ValueUsage valueUsage /* = ValueUsage::WantValue */,
                          EmitLineNumberNote emitLineNote /* = EMIT_LINENOTE */)
{
    if (!CheckRecursionLimit(cx))
        return false;

    EmitLevelManager elm(this);

    /* Emit notes to tell the current bytecode's source line number.
       However, a couple trees require special treatment; see the
       relevant emitter functions for details. */
    if (emitLineNote == EMIT_LINENOTE && !ParseNodeRequiresSpecialLineNumberNotes(pn)) {
        if (!updateLineNumberNotes(pn->pn_pos.begin))
            return false;
    }

    switch (pn->getKind()) {
      case ParseNodeKind::Function:
        if (!emitFunction(pn))
            return false;
        break;

      case ParseNodeKind::ParamsBody:
        if (!emitFunctionFormalParametersAndBody(pn))
            return false;
        break;

      case ParseNodeKind::If:
        if (!emitIf(pn))
            return false;
        break;

      case ParseNodeKind::Switch:
        if (!emitSwitch(pn))
            return false;
        break;

      case ParseNodeKind::While:
        if (!emitWhile(pn))
            return false;
        break;

      case ParseNodeKind::DoWhile:
        if (!emitDo(pn))
            return false;
        break;

      case ParseNodeKind::For:
        if (!emitFor(pn))
            return false;
        break;

      case ParseNodeKind::Break:
        if (!emitBreak(pn->as<BreakStatement>().label()))
            return false;
        break;

      case ParseNodeKind::Continue:
        if (!emitContinue(pn->as<ContinueStatement>().label()))
            return false;
        break;

      case ParseNodeKind::With:
        if (!emitWith(pn))
            return false;
        break;

      case ParseNodeKind::Try:
        if (!emitTry(pn))
            return false;
        break;

      case ParseNodeKind::Catch:
        if (!emitCatch(pn))
            return false;
        break;

      case ParseNodeKind::Var:
        if (!emitDeclarationList(pn))
            return false;
        break;

      case ParseNodeKind::Return:
        if (!emitReturn(pn))
            return false;
        break;

      case ParseNodeKind::YieldStar:
        if (!emitYieldStar(pn->pn_kid))
            return false;
        break;

      case ParseNodeKind::Generator:
        if (!emit1(JSOP_GENERATOR))
            return false;
        break;

      case ParseNodeKind::InitialYield:
        if (!emitInitialYield(pn))
            return false;
        break;

      case ParseNodeKind::Yield:
        if (!emitYield(pn))
            return false;
        break;

      case ParseNodeKind::Await:
        if (!emitAwaitInInnermostScope(pn))
            return false;
        break;

      case ParseNodeKind::StatementList:
        if (!emitStatementList(pn))
            return false;
        break;

      case ParseNodeKind::EmptyStatement:
        break;

      case ParseNodeKind::ExpressionStatement:
        if (!emitExpressionStatement(pn))
            return false;
        break;

      case ParseNodeKind::Label:
        if (!emitLabeledStatement(&pn->as<LabeledStatement>()))
            return false;
        break;

      case ParseNodeKind::Comma:
        if (!emitSequenceExpr(pn, valueUsage))
            return false;
        break;

      case ParseNodeKind::Assign:
      case ParseNodeKind::AddAssign:
      case ParseNodeKind::SubAssign:
      case ParseNodeKind::BitOrAssign:
      case ParseNodeKind::BitXorAssign:
      case ParseNodeKind::BitAndAssign:
      case ParseNodeKind::LshAssign:
      case ParseNodeKind::RshAssign:
      case ParseNodeKind::UrshAssign:
      case ParseNodeKind::MulAssign:
      case ParseNodeKind::DivAssign:
      case ParseNodeKind::ModAssign:
      case ParseNodeKind::PowAssign:
        if (!emitAssignment(pn->pn_left, pn->getKind(), pn->pn_right))
            return false;
        break;

      case ParseNodeKind::Conditional:
        if (!emitConditionalExpression(pn->as<ConditionalExpression>(), valueUsage))
            return false;
        break;

      case ParseNodeKind::Or:
      case ParseNodeKind::And:
        if (!emitLogical(pn))
            return false;
        break;

      case ParseNodeKind::Add:
      case ParseNodeKind::Sub:
      case ParseNodeKind::BitOr:
      case ParseNodeKind::BitXor:
      case ParseNodeKind::BitAnd:
      case ParseNodeKind::StrictEq:
      case ParseNodeKind::Eq:
      case ParseNodeKind::StrictNe:
      case ParseNodeKind::Ne:
      case ParseNodeKind::Lt:
      case ParseNodeKind::Le:
      case ParseNodeKind::Gt:
      case ParseNodeKind::Ge:
      case ParseNodeKind::In:
      case ParseNodeKind::InstanceOf:
      case ParseNodeKind::Lsh:
      case ParseNodeKind::Rsh:
      case ParseNodeKind::Ursh:
      case ParseNodeKind::Star:
      case ParseNodeKind::Div:
      case ParseNodeKind::Mod:
        if (!emitLeftAssociative(pn))
            return false;
        break;

      case ParseNodeKind::Pow:
        if (!emitRightAssociative(pn))
            return false;
        break;

      case ParseNodeKind::Pipeline:
        if (!emitPipeline(pn))
            return false;
        break;

      case ParseNodeKind::TypeOfName:
        if (!emitTypeof(pn, JSOP_TYPEOF))
            return false;
        break;

      case ParseNodeKind::TypeOfExpr:
        if (!emitTypeof(pn, JSOP_TYPEOFEXPR))
            return false;
        break;

      case ParseNodeKind::Throw:
      case ParseNodeKind::Void:
      case ParseNodeKind::Not:
      case ParseNodeKind::BitNot:
      case ParseNodeKind::Pos:
      case ParseNodeKind::Neg:
        if (!emitUnary(pn))
            return false;
        break;

      case ParseNodeKind::PreIncrement:
      case ParseNodeKind::PreDecrement:
      case ParseNodeKind::PostIncrement:
      case ParseNodeKind::PostDecrement:
        if (!emitIncOrDec(pn))
            return false;
        break;

      case ParseNodeKind::DeleteName:
        if (!emitDeleteName(pn))
            return false;
        break;

      case ParseNodeKind::DeleteProp:
        if (!emitDeleteProperty(pn))
            return false;
        break;

      case ParseNodeKind::DeleteElem:
        if (!emitDeleteElement(pn))
            return false;
        break;

      case ParseNodeKind::DeleteExpr:
        if (!emitDeleteExpression(pn))
            return false;
        break;

      case ParseNodeKind::Dot:
        if (pn->as<PropertyAccess>().isSuper()) {
            if (!emitSuperPropOp(pn, JSOP_GETPROP_SUPER))
                return false;
        } else {
            if (!emitPropOp(pn, JSOP_GETPROP))
                return false;
        }
        break;

      case ParseNodeKind::Elem:
        if (pn->as<PropertyByValue>().isSuper()) {
            if (!emitSuperElemOp(pn, JSOP_GETELEM_SUPER))
                return false;
        } else {
            if (!emitElemOp(pn, JSOP_GETELEM))
                return false;
        }
        break;

      case ParseNodeKind::New:
      case ParseNodeKind::TaggedTemplate:
      case ParseNodeKind::Call:
      case ParseNodeKind::SuperCall:
        if (!emitCallOrNew(pn, valueUsage))
            return false;
        break;

      case ParseNodeKind::LexicalScope:
        if (!emitLexicalScope(pn))
            return false;
        break;

      case ParseNodeKind::Const:
      case ParseNodeKind::Let:
        if (!emitDeclarationList(pn))
            return false;
        break;

      case ParseNodeKind::Import:
        MOZ_ASSERT(sc->isModuleContext());
        break;

      case ParseNodeKind::Export:
        MOZ_ASSERT(sc->isModuleContext());
        if (pn->pn_kid->getKind() != ParseNodeKind::ExportSpecList) {
            if (!emitTree(pn->pn_kid))
                return false;
        }
        break;

      case ParseNodeKind::ExportDefault:
        MOZ_ASSERT(sc->isModuleContext());
        if (!emitExportDefault(pn))
            return false;
        break;

      case ParseNodeKind::ExportFrom:
        MOZ_ASSERT(sc->isModuleContext());
        break;

      case ParseNodeKind::CallSiteObj:
        if (!emitCallSiteObject(pn))
            return false;
        break;

      case ParseNodeKind::Array:
        if (!emitArrayLiteral(pn))
            return false;
        break;

      case ParseNodeKind::Object:
        if (!emitObject(pn))
            return false;
        break;

      case ParseNodeKind::Name:
        if (!emitGetName(pn))
            return false;
        break;

      case ParseNodeKind::TemplateStringList:
        if (!emitTemplateString(pn))
            return false;
        break;

      case ParseNodeKind::TemplateString:
      case ParseNodeKind::String:
        if (!emitAtomOp(pn, JSOP_STRING))
            return false;
        break;

      case ParseNodeKind::Number:
        if (!emitNumberOp(pn->pn_dval))
            return false;
        break;

      case ParseNodeKind::RegExp:
        if (!emitRegExp(objectList.add(pn->as<RegExpLiteral>().objbox())))
            return false;
        break;

      case ParseNodeKind::True:
      case ParseNodeKind::False:
      case ParseNodeKind::Null:
      case ParseNodeKind::RawUndefined:
        if (!emit1(pn->getOp()))
            return false;
        break;

      case ParseNodeKind::This:
        if (!emitThisLiteral(pn))
            return false;
        break;

      case ParseNodeKind::Debugger:
        if (!updateSourceCoordNotes(pn->pn_pos.begin))
            return false;
        if (!emit1(JSOP_DEBUGGER))
            return false;
        break;

      case ParseNodeKind::Class:
        if (!emitClass(pn))
            return false;
        break;

      case ParseNodeKind::NewTarget:
        if (!emit1(JSOP_NEWTARGET))
            return false;
        break;

      case ParseNodeKind::SetThis:
        if (!emitSetThis(pn))
            return false;
        break;

      case ParseNodeKind::PosHolder:
        MOZ_FALLTHROUGH_ASSERT("Should never try to emit ParseNodeKind::PosHolder");

      default:
        MOZ_ASSERT(0);
    }

    /* bce->emitLevel == 1 means we're last on the stack, so finish up. */
    if (emitLevel == 1) {
        if (!updateSourceCoordNotes(pn->pn_pos.end))
            return false;
    }
    return true;
}

bool
BytecodeEmitter::emitTreeInBranch(ParseNode* pn,
                                  ValueUsage valueUsage /* = ValueUsage::WantValue */)
{
    // Code that may be conditionally executed always need their own TDZ
    // cache.
    TDZCheckCache tdzCache(this);
    return emitTree(pn, valueUsage);
}

static bool
AllocSrcNote(JSContext* cx, SrcNotesVector& notes, unsigned* index)
{
    if (!notes.growBy(1)) {
        ReportOutOfMemory(cx);
        return false;
    }

    *index = notes.length() - 1;
    return true;
}

bool
BytecodeEmitter::newSrcNote(SrcNoteType type, unsigned* indexp)
{
    SrcNotesVector& notes = this->notes();
    unsigned index;
    if (!AllocSrcNote(cx, notes, &index))
        return false;

    /*
     * Compute delta from the last annotated bytecode's offset.  If it's too
     * big to fit in sn, allocate one or more xdelta notes and reset sn.
     */
    ptrdiff_t offset = this->offset();
    ptrdiff_t delta = offset - lastNoteOffset();
    current->lastNoteOffset = offset;
    if (delta >= SN_DELTA_LIMIT) {
        do {
            ptrdiff_t xdelta = Min(delta, SN_XDELTA_MASK);
            SN_MAKE_XDELTA(&notes[index], xdelta);
            delta -= xdelta;
            if (!AllocSrcNote(cx, notes, &index))
                return false;
        } while (delta >= SN_DELTA_LIMIT);
    }

    /*
     * Initialize type and delta, then allocate the minimum number of notes
     * needed for type's arity.  Usually, we won't need more, but if an offset
     * does take two bytes, setSrcNoteOffset will grow notes.
     */
    SN_MAKE_NOTE(&notes[index], type, delta);
    for (int n = (int)js_SrcNoteSpec[type].arity; n > 0; n--) {
        if (!newSrcNote(SRC_NULL))
            return false;
    }

    if (indexp)
        *indexp = index;
    return true;
}

bool
BytecodeEmitter::newSrcNote2(SrcNoteType type, ptrdiff_t offset, unsigned* indexp)
{
    unsigned index;
    if (!newSrcNote(type, &index))
        return false;
    if (!setSrcNoteOffset(index, 0, offset))
        return false;
    if (indexp)
        *indexp = index;
    return true;
}

bool
BytecodeEmitter::newSrcNote3(SrcNoteType type, ptrdiff_t offset1, ptrdiff_t offset2,
                             unsigned* indexp)
{
    unsigned index;
    if (!newSrcNote(type, &index))
        return false;
    if (!setSrcNoteOffset(index, 0, offset1))
        return false;
    if (!setSrcNoteOffset(index, 1, offset2))
        return false;
    if (indexp)
        *indexp = index;
    return true;
}

bool
BytecodeEmitter::addToSrcNoteDelta(jssrcnote* sn, ptrdiff_t delta)
{
    /*
     * Called only from finishTakingSrcNotes to add to main script note
     * deltas, and only by a small positive amount.
     */
    MOZ_ASSERT(current == &main);
    MOZ_ASSERT((unsigned) delta < (unsigned) SN_XDELTA_LIMIT);

    ptrdiff_t base = SN_DELTA(sn);
    ptrdiff_t limit = SN_IS_XDELTA(sn) ? SN_XDELTA_LIMIT : SN_DELTA_LIMIT;
    ptrdiff_t newdelta = base + delta;
    if (newdelta < limit) {
        SN_SET_DELTA(sn, newdelta);
    } else {
        jssrcnote xdelta;
        SN_MAKE_XDELTA(&xdelta, delta);
        if (!main.notes.insert(sn, xdelta))
            return false;
    }
    return true;
}

bool
BytecodeEmitter::setSrcNoteOffset(unsigned index, unsigned which, ptrdiff_t offset)
{
    if (!SN_REPRESENTABLE_OFFSET(offset)) {
        parser.reportError(JSMSG_NEED_DIET, js_script_str);
        return false;
    }

    SrcNotesVector& notes = this->notes();

    /* Find the offset numbered which (i.e., skip exactly which offsets). */
    jssrcnote* sn = &notes[index];
    MOZ_ASSERT(SN_TYPE(sn) != SRC_XDELTA);
    MOZ_ASSERT((int) which < js_SrcNoteSpec[SN_TYPE(sn)].arity);
    for (sn++; which; sn++, which--) {
        if (*sn & SN_4BYTE_OFFSET_FLAG)
            sn += 3;
    }

    /*
     * See if the new offset requires four bytes either by being too big or if
     * the offset has already been inflated (in which case, we need to stay big
     * to not break the srcnote encoding if this isn't the last srcnote).
     */
    if (offset > (ptrdiff_t)SN_4BYTE_OFFSET_MASK || (*sn & SN_4BYTE_OFFSET_FLAG)) {
        /* Maybe this offset was already set to a four-byte value. */
        if (!(*sn & SN_4BYTE_OFFSET_FLAG)) {
            /* Insert three dummy bytes that will be overwritten shortly. */
            jssrcnote dummy = 0;
            if (!(sn = notes.insert(sn, dummy)) ||
                !(sn = notes.insert(sn, dummy)) ||
                !(sn = notes.insert(sn, dummy)))
            {
                ReportOutOfMemory(cx);
                return false;
            }
        }
        *sn++ = (jssrcnote)(SN_4BYTE_OFFSET_FLAG | (offset >> 24));
        *sn++ = (jssrcnote)(offset >> 16);
        *sn++ = (jssrcnote)(offset >> 8);
    }
    *sn = (jssrcnote)offset;
    return true;
}

bool
BytecodeEmitter::finishTakingSrcNotes(uint32_t* out)
{
    MOZ_ASSERT(current == &main);

    unsigned prologueCount = prologue.notes.length();
    if (prologueCount && prologue.currentLine != firstLine) {
        switchToPrologue();
        if (!newSrcNote2(SRC_SETLINE, ptrdiff_t(firstLine)))
            return false;
        switchToMain();
    } else {
        /*
         * Either no prologue srcnotes, or no line number change over prologue.
         * We don't need a SRC_SETLINE, but we may need to adjust the offset
         * of the first main note, by adding to its delta and possibly even
         * prepending SRC_XDELTA notes to it to account for prologue bytecodes
         * that came at and after the last annotated bytecode.
         */
        ptrdiff_t offset = prologueOffset() - prologue.lastNoteOffset;
        MOZ_ASSERT(offset >= 0);
        if (offset > 0 && main.notes.length() != 0) {
            /* NB: Use as much of the first main note's delta as we can. */
            jssrcnote* sn = main.notes.begin();
            ptrdiff_t delta = SN_IS_XDELTA(sn)
                            ? SN_XDELTA_MASK - (*sn & SN_XDELTA_MASK)
                            : SN_DELTA_MASK - (*sn & SN_DELTA_MASK);
            if (offset < delta)
                delta = offset;
            for (;;) {
                if (!addToSrcNoteDelta(sn, delta))
                    return false;
                offset -= delta;
                if (offset == 0)
                    break;
                delta = Min(offset, SN_XDELTA_MASK);
                sn = main.notes.begin();
            }
        }
    }

    // The prologue count might have changed, so we can't reuse prologueCount.
    // The + 1 is to account for the final SN_MAKE_TERMINATOR that is appended
    // when the notes are copied to their final destination by copySrcNotes.
    *out = prologue.notes.length() + main.notes.length() + 1;
    return true;
}

void
BytecodeEmitter::copySrcNotes(jssrcnote* destination, uint32_t nsrcnotes)
{
    unsigned prologueCount = prologue.notes.length();
    unsigned mainCount = main.notes.length();
    unsigned totalCount = prologueCount + mainCount;
    MOZ_ASSERT(totalCount == nsrcnotes - 1);
    if (prologueCount)
        PodCopy(destination, prologue.notes.begin(), prologueCount);
    PodCopy(destination + prologueCount, main.notes.begin(), mainCount);
    SN_MAKE_TERMINATOR(&destination[totalCount]);
}

void
CGConstList::finish(ConstArray* array)
{
    MOZ_ASSERT(length() == array->length);

    for (unsigned i = 0; i < length(); i++)
        array->vector[i] = list[i];
}

/*
 * Find the index of the given object for code generator.
 *
 * Since the emitter refers to each parsed object only once, for the index we
 * use the number of already indexed objects. We also add the object to a list
 * to convert the list to a fixed-size array when we complete code generation,
 * see js::CGObjectList::finish below.
 */
unsigned
CGObjectList::add(ObjectBox* objbox)
{
    MOZ_ASSERT(!objbox->emitLink);
    objbox->emitLink = lastbox;
    lastbox = objbox;
    return length++;
}

unsigned
CGObjectList::indexOf(JSObject* obj)
{
    MOZ_ASSERT(length > 0);
    unsigned index = length - 1;
    for (ObjectBox* box = lastbox; box->object != obj; box = box->emitLink)
        index--;
    return index;
}

void
CGObjectList::finish(ObjectArray* array)
{
    MOZ_ASSERT(length <= INDEX_LIMIT);
    MOZ_ASSERT(length == array->length);

    js::GCPtrObject* cursor = array->vector + array->length;
    ObjectBox* objbox = lastbox;
    do {
        --cursor;
        MOZ_ASSERT(!*cursor);
        MOZ_ASSERT(objbox->object->isTenured());
        *cursor = objbox->object;
    } while ((objbox = objbox->emitLink) != nullptr);
    MOZ_ASSERT(cursor == array->vector);
}

ObjectBox*
CGObjectList::find(uint32_t index)
{
    MOZ_ASSERT(index < length);
    ObjectBox* box = lastbox;
    for (unsigned n = length - 1; n > index; n--)
        box = box->emitLink;
    return box;
}

void
CGScopeList::finish(ScopeArray* array)
{
    MOZ_ASSERT(length() <= INDEX_LIMIT);
    MOZ_ASSERT(length() == array->length);
    for (uint32_t i = 0; i < length(); i++)
        array->vector[i].init(vector[i]);
}

bool
CGTryNoteList::append(JSTryNoteKind kind, uint32_t stackDepth, size_t start, size_t end)
{
    MOZ_ASSERT(start <= end);
    MOZ_ASSERT(size_t(uint32_t(start)) == start);
    MOZ_ASSERT(size_t(uint32_t(end)) == end);

    JSTryNote note;
    note.kind = kind;
    note.stackDepth = stackDepth;
    note.start = uint32_t(start);
    note.length = uint32_t(end - start);

    return list.append(note);
}

void
CGTryNoteList::finish(TryNoteArray* array)
{
    MOZ_ASSERT(length() == array->length);

    for (unsigned i = 0; i < length(); i++)
        array->vector[i] = list[i];
}

bool
CGScopeNoteList::append(uint32_t scopeIndex, uint32_t offset, bool inPrologue,
                        uint32_t parent)
{
    CGScopeNote note;
    mozilla::PodZero(&note);

    note.index = scopeIndex;
    note.start = offset;
    note.parent = parent;
    note.startInPrologue = inPrologue;

    return list.append(note);
}

void
CGScopeNoteList::recordEnd(uint32_t index, uint32_t offset, bool inPrologue)
{
    MOZ_ASSERT(index < length());
    MOZ_ASSERT(list[index].length == 0);
    list[index].end = offset;
    list[index].endInPrologue = inPrologue;
}

void
CGScopeNoteList::finish(ScopeNoteArray* array, uint32_t prologueLength)
{
    MOZ_ASSERT(length() == array->length);

    for (unsigned i = 0; i < length(); i++) {
        if (!list[i].startInPrologue)
            list[i].start += prologueLength;
        if (!list[i].endInPrologue && list[i].end != UINT32_MAX)
            list[i].end += prologueLength;
        MOZ_ASSERT(list[i].end >= list[i].start);
        list[i].length = list[i].end - list[i].start;
        array->vector[i] = list[i];
    }
}

void
CGYieldAndAwaitOffsetList::finish(YieldAndAwaitOffsetArray& array, uint32_t prologueLength)
{
    MOZ_ASSERT(length() == array.length());

    for (unsigned i = 0; i < length(); i++)
        array[i] = prologueLength + list[i];
}

/*
 * We should try to get rid of offsetBias (always 0 or 1, where 1 is
 * JSOP_{NOP,POP}_LENGTH), which is used only by SRC_FOR.
 */
const JSSrcNoteSpec js_SrcNoteSpec[] = {
#define DEFINE_SRC_NOTE_SPEC(sym, name, arity) { name, arity },
    FOR_EACH_SRC_NOTE_TYPE(DEFINE_SRC_NOTE_SPEC)
#undef DEFINE_SRC_NOTE_SPEC
};

static int
SrcNoteArity(jssrcnote* sn)
{
    MOZ_ASSERT(SN_TYPE(sn) < SRC_LAST);
    return js_SrcNoteSpec[SN_TYPE(sn)].arity;
}

JS_FRIEND_API(unsigned)
js::SrcNoteLength(jssrcnote* sn)
{
    unsigned arity;
    jssrcnote* base;

    arity = SrcNoteArity(sn);
    for (base = sn++; arity; sn++, arity--) {
        if (*sn & SN_4BYTE_OFFSET_FLAG)
            sn += 3;
    }
    return sn - base;
}

JS_FRIEND_API(ptrdiff_t)
js::GetSrcNoteOffset(jssrcnote* sn, unsigned which)
{
    /* Find the offset numbered which (i.e., skip exactly which offsets). */
    MOZ_ASSERT(SN_TYPE(sn) != SRC_XDELTA);
    MOZ_ASSERT((int) which < SrcNoteArity(sn));
    for (sn++; which; sn++, which--) {
        if (*sn & SN_4BYTE_OFFSET_FLAG)
            sn += 3;
    }
    if (*sn & SN_4BYTE_OFFSET_FLAG) {
        return (ptrdiff_t)(((uint32_t)(sn[0] & SN_4BYTE_OFFSET_MASK) << 24)
                           | (sn[1] << 16)
                           | (sn[2] << 8)
                           | sn[3]);
    }
    return (ptrdiff_t)*sn;
}
