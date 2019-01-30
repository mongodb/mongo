/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_CompileInfo_h
#define jit_CompileInfo_h

#include "mozilla/Maybe.h"

#include "jit/JitAllocPolicy.h"
#include "jit/JitFrames.h"
#include "jit/Registers.h"
#include "vm/EnvironmentObject.h"
#include "vm/JSFunction.h"

namespace js {
namespace jit {

class TrackedOptimizations;

inline unsigned
StartArgSlot(JSScript* script)
{
    // Reserved slots:
    // Slot 0: Environment chain.
    // Slot 1: Return value.

    // When needed:
    // Slot 2: Argumentsobject.

    // Note: when updating this, please also update the assert in SnapshotWriter::startFrame
    return 2 + (script->argumentsHasVarBinding() ? 1 : 0);
}

inline unsigned
CountArgSlots(JSScript* script, JSFunction* fun)
{
    // Slot x + 0: This value.
    // Slot x + 1: Argument 1.
    // ...
    // Slot x + n: Argument n.

    // Note: when updating this, please also update the assert in SnapshotWriter::startFrame
    return StartArgSlot(script) + (fun ? fun->nargs() + 1 : 0);
}


// The compiler at various points needs to be able to store references to the
// current inline path (the sequence of scripts and call-pcs that lead to the
// current function being inlined).
//
// To support this, the top-level IonBuilder keeps a tree that records the
// inlinings done during compilation.
class InlineScriptTree {
    // InlineScriptTree for the caller
    InlineScriptTree* caller_;

    // PC in the caller corresponding to this script.
    jsbytecode* callerPc_;

    // Script for this entry.
    JSScript* script_;

    // Child entries (linked together by nextCallee pointer)
    InlineScriptTree* children_;
    InlineScriptTree* nextCallee_;

  public:
    InlineScriptTree(InlineScriptTree* caller, jsbytecode* callerPc, JSScript* script)
      : caller_(caller), callerPc_(callerPc), script_(script),
        children_(nullptr), nextCallee_(nullptr)
    {}

    static InlineScriptTree* New(TempAllocator* allocator, InlineScriptTree* caller,
                                 jsbytecode* callerPc, JSScript* script);

    InlineScriptTree* addCallee(TempAllocator* allocator, jsbytecode* callerPc,
                                JSScript* calleeScript);

    InlineScriptTree* caller() const {
        return caller_;
    }

    bool isOutermostCaller() const {
        return caller_ == nullptr;
    }
    bool hasCaller() const {
        return caller_ != nullptr;
    }
    InlineScriptTree* outermostCaller() {
        if (isOutermostCaller())
            return this;
        return caller_->outermostCaller();
    }

    jsbytecode* callerPc() const {
        return callerPc_;
    }

    JSScript* script() const {
        return script_;
    }

    bool hasChildren() const {
        return children_ != nullptr;
    }
    InlineScriptTree* firstChild() const {
        MOZ_ASSERT(hasChildren());
        return children_;
    }

    bool hasNextCallee() const {
        return nextCallee_ != nullptr;
    }
    InlineScriptTree* nextCallee() const {
        MOZ_ASSERT(hasNextCallee());
        return nextCallee_;
    }

    unsigned depth() const {
        if (isOutermostCaller())
            return 1;
        return 1 + caller_->depth();
    }
};

class BytecodeSite : public TempObject
{
    // InlineScriptTree identifying innermost active function at site.
    InlineScriptTree* tree_;

    // Bytecode address within innermost active function.
    jsbytecode* pc_;

    // Optimization information at the pc.
    TrackedOptimizations* optimizations_;

  public:
    BytecodeSite()
      : tree_(nullptr), pc_(nullptr), optimizations_(nullptr)
    {}

    BytecodeSite(InlineScriptTree* tree, jsbytecode* pc)
      : tree_(tree), pc_(pc), optimizations_(nullptr)
    {
        MOZ_ASSERT(tree_ != nullptr);
        MOZ_ASSERT(pc_ != nullptr);
    }

    InlineScriptTree* tree() const {
        return tree_;
    }

    jsbytecode* pc() const {
        return pc_;
    }

    JSScript* script() const {
        return tree_ ? tree_->script() : nullptr;
    }

    bool hasOptimizations() const {
        return !!optimizations_;
    }

    TrackedOptimizations* optimizations() const {
        MOZ_ASSERT(hasOptimizations());
        return optimizations_;
    }

    void setOptimizations(TrackedOptimizations* optimizations) {
        optimizations_ = optimizations;
    }
};

enum AnalysisMode {
    /* JavaScript execution, not analysis. */
    Analysis_None,

    /*
     * MIR analysis performed when invoking 'new' on a script, to determine
     * definite properties. Used by the optimizing JIT.
     */
    Analysis_DefiniteProperties,

    /*
     * MIR analysis performed when executing a script which uses its arguments,
     * when it is not known whether a lazy arguments value can be used.
     */
    Analysis_ArgumentsUsage
};

// Contains information about the compilation source for IR being generated.
class CompileInfo
{
  public:
    CompileInfo(CompileRuntime* runtime, JSScript* script, JSFunction* fun, jsbytecode* osrPc,
                AnalysisMode analysisMode, bool scriptNeedsArgsObj,
                InlineScriptTree* inlineScriptTree)
      : script_(script), fun_(fun), osrPc_(osrPc),
        analysisMode_(analysisMode), scriptNeedsArgsObj_(scriptNeedsArgsObj),
        hadOverflowBailout_(script->hadOverflowBailout()),
        mayReadFrameArgsDirectly_(script->mayReadFrameArgsDirectly()),
        inlineScriptTree_(inlineScriptTree)
    {
        MOZ_ASSERT_IF(osrPc, JSOp(*osrPc) == JSOP_LOOPENTRY);

        // The function here can flow in from anywhere so look up the canonical
        // function to ensure that we do not try to embed a nursery pointer in
        // jit-code. Precisely because it can flow in from anywhere, it's not
        // guaranteed to be non-lazy. Hence, don't access its script!
        if (fun_) {
            fun_ = fun_->nonLazyScript()->functionNonDelazifying();
            MOZ_ASSERT(fun_->isTenured());
        }

        nimplicit_ = StartArgSlot(script)                   /* env chain and argument obj */
                   + (fun ? 1 : 0);                         /* this */
        nargs_ = fun ? fun->nargs() : 0;
        nlocals_ = script->nfixed();

        // An extra slot is needed for global scopes because INITGLEXICAL (stack
        // depth 1) is compiled as a SETPROP (stack depth 2) on the global lexical
        // scope.
        uint32_t extra = script->isGlobalCode() ? 1 : 0;
        nstack_ = Max<unsigned>(script->nslots() - script->nfixed(), MinJITStackSize) + extra;
        nslots_ = nimplicit_ + nargs_ + nlocals_ + nstack_;

        // For derived class constructors, find and cache the frame slot for
        // the .this binding. This slot is assumed to be always
        // observable. See isObservableFrameSlot.
        if (script->isDerivedClassConstructor()) {
            MOZ_ASSERT(script->functionHasThisBinding());
            for (BindingIter bi(script); bi; bi++) {
                if (bi.name() != runtime->names().dotThis)
                    continue;
                BindingLocation loc = bi.location();
                if (loc.kind() == BindingLocation::Kind::Frame) {
                    thisSlotForDerivedClassConstructor_ = mozilla::Some(localSlot(loc.slot()));
                    break;
                }
            }
        }

        // If the script uses an environment in body, the environment chain
        // will need to be observable.
        needsBodyEnvironmentObject_ = script->needsBodyEnvironment();
    }

    explicit CompileInfo(unsigned nlocals)
      : script_(nullptr), fun_(nullptr), osrPc_(nullptr),
        analysisMode_(Analysis_None), scriptNeedsArgsObj_(false),
        mayReadFrameArgsDirectly_(false), inlineScriptTree_(nullptr),
        needsBodyEnvironmentObject_(false)
    {
        nimplicit_ = 0;
        nargs_ = 0;
        nlocals_ = nlocals;
        nstack_ = 1;  /* For FunctionCompiler::pushPhiInput/popPhiOutput */
        nslots_ = nlocals_ + nstack_;
    }

    JSScript* script() const {
        return script_;
    }
    bool compilingWasm() const {
        return script() == nullptr;
    }
    JSFunction* funMaybeLazy() const {
        return fun_;
    }
    ModuleObject* module() const {
        return script_->module();
    }
    jsbytecode* osrPc() const {
        return osrPc_;
    }
    InlineScriptTree* inlineScriptTree() const {
        return inlineScriptTree_;
    }

    bool hasOsrAt(jsbytecode* pc) const {
        MOZ_ASSERT(JSOp(*pc) == JSOP_LOOPENTRY);
        return pc == osrPc();
    }

    jsbytecode* startPC() const {
        return script_->code();
    }
    jsbytecode* limitPC() const {
        return script_->codeEnd();
    }

    const char* filename() const {
        return script_->filename();
    }

    unsigned lineno() const {
        return script_->lineno();
    }
    unsigned lineno(jsbytecode* pc) const {
        return PCToLineNumber(script_, pc);
    }

    // Script accessors based on PC.

    JSAtom* getAtom(jsbytecode* pc) const {
        return script_->getAtom(GET_UINT32_INDEX(pc));
    }

    PropertyName* getName(jsbytecode* pc) const {
        return script_->getName(GET_UINT32_INDEX(pc));
    }

    inline RegExpObject* getRegExp(jsbytecode* pc) const;

    JSObject* getObject(jsbytecode* pc) const {
        return script_->getObject(GET_UINT32_INDEX(pc));
    }

    inline JSFunction* getFunction(jsbytecode* pc) const;

    const Value& getConst(jsbytecode* pc) const {
        return script_->getConst(GET_UINT32_INDEX(pc));
    }

    jssrcnote* getNote(GSNCache& gsn, jsbytecode* pc) const {
        return GetSrcNote(gsn, script(), pc);
    }

    // Total number of slots: args, locals, and stack.
    unsigned nslots() const {
        return nslots_;
    }

    // Number of slots needed for env chain, return value,
    // maybe argumentsobject and this value.
    unsigned nimplicit() const {
        return nimplicit_;
    }
    // Number of arguments (without counting this value).
    unsigned nargs() const {
        return nargs_;
    }
    // Number of slots needed for all local variables.  This includes "fixed
    // vars" (see above) and also block-scoped locals.
    unsigned nlocals() const {
        return nlocals_;
    }
    unsigned ninvoke() const {
        return nslots_ - nstack_;
    }

    uint32_t environmentChainSlot() const {
        MOZ_ASSERT(script());
        return 0;
    }
    uint32_t returnValueSlot() const {
        MOZ_ASSERT(script());
        return 1;
    }
    uint32_t argsObjSlot() const {
        MOZ_ASSERT(hasArguments());
        return 2;
    }
    uint32_t thisSlot() const {
        MOZ_ASSERT(funMaybeLazy());
        MOZ_ASSERT(nimplicit_ > 0);
        return nimplicit_ - 1;
    }
    uint32_t firstArgSlot() const {
        return nimplicit_;
    }
    uint32_t argSlotUnchecked(uint32_t i) const {
        // During initialization, some routines need to get at arg
        // slots regardless of how regular argument access is done.
        MOZ_ASSERT(i < nargs_);
        return nimplicit_ + i;
    }
    uint32_t argSlot(uint32_t i) const {
        // This should only be accessed when compiling functions for
        // which argument accesses don't need to go through the
        // argument object.
        MOZ_ASSERT(!argsObjAliasesFormals());
        return argSlotUnchecked(i);
    }
    uint32_t firstLocalSlot() const {
        return nimplicit_ + nargs_;
    }
    uint32_t localSlot(uint32_t i) const {
        return firstLocalSlot() + i;
    }
    uint32_t firstStackSlot() const {
        return firstLocalSlot() + nlocals();
    }
    uint32_t stackSlot(uint32_t i) const {
        return firstStackSlot() + i;
    }

    uint32_t startArgSlot() const {
        MOZ_ASSERT(script());
        return StartArgSlot(script());
    }
    uint32_t endArgSlot() const {
        MOZ_ASSERT(script());
        return CountArgSlots(script(), funMaybeLazy());
    }

    uint32_t totalSlots() const {
        MOZ_ASSERT(script() && funMaybeLazy());
        return nimplicit() + nargs() + nlocals();
    }

    bool isSlotAliased(uint32_t index) const {
        MOZ_ASSERT(index >= startArgSlot());
        uint32_t arg = index - firstArgSlot();
        if (arg < nargs())
            return script()->formalIsAliased(arg);
        return false;
    }

    bool hasArguments() const {
        return script()->argumentsHasVarBinding();
    }
    bool argumentsAliasesFormals() const {
        return script()->argumentsAliasesFormals();
    }
    bool needsArgsObj() const {
        return scriptNeedsArgsObj_;
    }
    bool argsObjAliasesFormals() const {
        return scriptNeedsArgsObj_ && script()->hasMappedArgsObj();
    }

    AnalysisMode analysisMode() const {
        return analysisMode_;
    }

    bool isAnalysis() const {
        return analysisMode_ != Analysis_None;
    }

    bool needsBodyEnvironmentObject() const {
        return needsBodyEnvironmentObject_;
    }

    // Returns true if a slot can be observed out-side the current frame while
    // the frame is active on the stack.  This implies that these definitions
    // would have to be executed and that they cannot be removed even if they
    // are unused.
    inline bool isObservableSlot(uint32_t slot) const {
        if (slot >= firstLocalSlot()) {
            // The |this| slot for a derived class constructor is a local slot.
            if (thisSlotForDerivedClassConstructor_)
                return *thisSlotForDerivedClassConstructor_ == slot;
            return false;
        }

        if (slot < firstArgSlot())
            return isObservableFrameSlot(slot);

        return isObservableArgumentSlot(slot);
    }

    bool isObservableFrameSlot(uint32_t slot) const {
        // The |envChain| value must be preserved if environments are added
        // after the prologue.
        if (needsBodyEnvironmentObject() && slot == environmentChainSlot())
            return true;

        if (!funMaybeLazy())
            return false;

        // The |this| value must always be observable.
        if (slot == thisSlot())
            return true;

        // The |this| frame slot in derived class constructors should never be
        // optimized out, as a Debugger might need to perform TDZ checks on it
        // via, e.g., an exceptionUnwind handler. The TDZ check is required
        // for correctness if the handler decides to continue execution.
        if (thisSlotForDerivedClassConstructor_ && *thisSlotForDerivedClassConstructor_ == slot)
            return true;

        if (funMaybeLazy()->needsSomeEnvironmentObject() && slot == environmentChainSlot())
            return true;

        // If the function may need an arguments object, then make sure to
        // preserve the env chain, because it may be needed to construct the
        // arguments object during bailout. If we've already created an
        // arguments object (or got one via OSR), preserve that as well.
        if (hasArguments() && (slot == environmentChainSlot() || slot == argsObjSlot()))
            return true;

        return false;
    }

    bool isObservableArgumentSlot(uint32_t slot) const {
        if (!funMaybeLazy())
            return false;

        // Function.arguments can be used to access all arguments in non-strict
        // scripts, so we can't optimize out any arguments.
        if ((hasArguments() || !script()->strict()) &&
            firstArgSlot() <= slot && slot - firstArgSlot() < nargs())
        {
            return true;
        }

        return false;
    }

    // Returns true if a slot can be recovered before or during a bailout.  A
    // definition which can be observed and recovered, implies that this
    // definition can be optimized away as long as we can compute its values.
    bool isRecoverableOperand(uint32_t slot) const {
        // The |envChain| value cannot be recovered if environments can be
        // added in body (after the prologue).
        if (needsBodyEnvironmentObject() && slot == environmentChainSlot())
            return false;

        if (!funMaybeLazy())
            return true;

        // The |this| and the |envChain| values can be recovered.
        if (slot == thisSlot() || slot == environmentChainSlot())
            return true;

        if (isObservableFrameSlot(slot))
            return false;

        if (needsArgsObj() && isObservableArgumentSlot(slot))
            return false;

        return true;
    }

    // Check previous bailout states to prevent doing the same bailout in the
    // next compilation.
    bool hadOverflowBailout() const {
        return hadOverflowBailout_;
    }
    bool mayReadFrameArgsDirectly() const {
        return mayReadFrameArgsDirectly_;
    }

  private:
    unsigned nimplicit_;
    unsigned nargs_;
    unsigned nlocals_;
    unsigned nstack_;
    unsigned nslots_;
    mozilla::Maybe<unsigned> thisSlotForDerivedClassConstructor_;
    JSScript* script_;
    JSFunction* fun_;
    jsbytecode* osrPc_;
    AnalysisMode analysisMode_;

    // Whether a script needs an arguments object is unstable over compilation
    // since the arguments optimization could be marked as failed on the active
    // thread, so cache a value here and use it throughout for consistency.
    bool scriptNeedsArgsObj_;

    // Record the state of previous bailouts in order to prevent compiling the
    // same function identically the next time.
    bool hadOverflowBailout_;

    bool mayReadFrameArgsDirectly_;

    InlineScriptTree* inlineScriptTree_;

    // Whether a script needs environments within its body. This informs us
    // that the environment chain is not easy to reconstruct.
    bool needsBodyEnvironmentObject_;
};

} // namespace jit
} // namespace js

#endif /* jit_CompileInfo_h */
