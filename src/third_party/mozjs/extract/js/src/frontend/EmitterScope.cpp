/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/EmitterScope.h"

#include "frontend/AbstractScopePtr.h"
#include "frontend/BytecodeEmitter.h"
#include "frontend/ModuleSharedContext.h"
#include "frontend/TDZCheckCache.h"
#include "js/friend/ErrorMessages.h"  // JSMSG_*
#include "vm/EnvironmentObject.h"     // ClassBodyLexicalEnvironmentObject
#include "vm/WellKnownAtom.h"         // js_*_str

using namespace js;
using namespace js::frontend;

using mozilla::DebugOnly;
using mozilla::Maybe;
using mozilla::Nothing;
using mozilla::Some;

EmitterScope::EmitterScope(BytecodeEmitter* bce)
    : Nestable<EmitterScope>(&bce->innermostEmitterScope_),
      nameCache_(bce->fc->nameCollectionPool()),
      hasEnvironment_(false),
      environmentChainLength_(0),
      nextFrameSlot_(0),
      scopeIndex_(ScopeNote::NoScopeIndex),
      noteIndex_(ScopeNote::NoScopeNoteIndex) {}

bool EmitterScope::ensureCache(BytecodeEmitter* bce) {
  return nameCache_.acquire(bce->fc);
}

bool EmitterScope::checkSlotLimits(BytecodeEmitter* bce,
                                   const ParserBindingIter& bi) {
  if (bi.nextFrameSlot() >= LOCALNO_LIMIT ||
      bi.nextEnvironmentSlot() >= ENVCOORD_SLOT_LIMIT) {
    bce->reportError(nullptr, JSMSG_TOO_MANY_LOCALS);
    return false;
  }
  return true;
}

bool EmitterScope::checkEnvironmentChainLength(BytecodeEmitter* bce) {
  uint32_t hops;
  if (EmitterScope* emitterScope = enclosing(&bce)) {
    hops = emitterScope->environmentChainLength_;
  } else if (!bce->compilationState.input.enclosingScope.isNull()) {
    hops =
        bce->compilationState.scopeContext.enclosingScopeEnvironmentChainLength;
  } else {
    // If we're compiling module, enclosingScope is nullptr and it means empty
    // global scope.
    // See also the assertion in CompilationStencil::instantiateStencils.
    //
    // Global script also uses enclosingScope == nullptr, but it shouldn't call
    // checkEnvironmentChainLength.
    MOZ_ASSERT(bce->sc->isModule());
    hops = ModuleScope::EnclosingEnvironmentChainLength;
  }

  if (hops >= ENVCOORD_HOPS_LIMIT - 1) {
    bce->reportError(nullptr, JSMSG_TOO_DEEP, js_function_str);
    return false;
  }

  environmentChainLength_ = mozilla::AssertedCast<uint8_t>(hops + 1);
  return true;
}

void EmitterScope::updateFrameFixedSlots(BytecodeEmitter* bce,
                                         const ParserBindingIter& bi) {
  nextFrameSlot_ = bi.nextFrameSlot();
  if (nextFrameSlot_ > bce->maxFixedSlots) {
    bce->maxFixedSlots = nextFrameSlot_;
  }
}

bool EmitterScope::putNameInCache(BytecodeEmitter* bce,
                                  TaggedParserAtomIndex name,
                                  NameLocation loc) {
  NameLocationMap& cache = *nameCache_;
  NameLocationMap::AddPtr p = cache.lookupForAdd(name);
  MOZ_ASSERT(!p);
  if (!cache.add(p, name, loc)) {
    ReportOutOfMemory(bce->fc);
    return false;
  }
  return true;
}

Maybe<NameLocation> EmitterScope::lookupInCache(BytecodeEmitter* bce,
                                                TaggedParserAtomIndex name) {
  if (NameLocationMap::Ptr p = nameCache_->lookup(name)) {
    return Some(p->value().wrapped);
  }
  if (fallbackFreeNameLocation_ && nameCanBeFree(bce, name)) {
    return fallbackFreeNameLocation_;
  }
  return Nothing();
}

EmitterScope* EmitterScope::enclosing(BytecodeEmitter** bce) const {
  // There is an enclosing scope with access to the same frame.
  if (EmitterScope* inFrame = enclosingInFrame()) {
    return inFrame;
  }

  // We are currently compiling the enclosing script, look in the
  // enclosing BCE.
  if ((*bce)->parent) {
    *bce = (*bce)->parent;
    return (*bce)->innermostEmitterScopeNoCheck();
  }

  return nullptr;
}

mozilla::Maybe<ScopeIndex> EmitterScope::enclosingScopeIndex(
    BytecodeEmitter* bce) const {
  if (EmitterScope* es = enclosing(&bce)) {
    // NOTE: A value of Nothing for the ScopeIndex will occur when the enclosing
    // scope is the empty-global-scope. This is only allowed for self-hosting
    // code.
    MOZ_ASSERT_IF(es->scopeIndex(bce).isNothing(),
                  bce->emitterMode == BytecodeEmitter::SelfHosting);
    return es->scopeIndex(bce);
  }

  // The enclosing script is already compiled or the current script is the
  // global script.
  return mozilla::Nothing();
}

/* static */
bool EmitterScope::nameCanBeFree(BytecodeEmitter* bce,
                                 TaggedParserAtomIndex name) {
  // '.generator' cannot be accessed by name.
  return name != TaggedParserAtomIndex::WellKnown::dotGenerator();
}

NameLocation EmitterScope::searchAndCache(BytecodeEmitter* bce,
                                          TaggedParserAtomIndex name) {
  Maybe<NameLocation> loc;
  uint8_t hops = hasEnvironment() ? 1 : 0;
  DebugOnly<bool> inCurrentScript = enclosingInFrame();

  // Start searching in the current compilation.
  for (EmitterScope* es = enclosing(&bce); es; es = es->enclosing(&bce)) {
    loc = es->lookupInCache(bce, name);
    if (loc) {
      if (loc->kind() == NameLocation::Kind::EnvironmentCoordinate) {
        *loc = loc->addHops(hops);
      }
      break;
    }

    if (es->hasEnvironment()) {
      hops++;
    }

#ifdef DEBUG
    if (!es->enclosingInFrame()) {
      inCurrentScript = false;
    }
#endif
  }

  // If the name is not found in the current compilation, walk the Scope
  // chain encompassing the compilation.
  if (!loc) {
    MOZ_ASSERT(bce->compilationState.input.target ==
                   CompilationInput::CompilationTarget::Delazification ||
               bce->compilationState.input.target ==
                   CompilationInput::CompilationTarget::Eval);
    inCurrentScript = false;
    loc = Some(bce->compilationState.scopeContext.searchInEnclosingScope(
        bce->fc, bce->compilationState.input, bce->parserAtoms(), name));
    if (loc->kind() == NameLocation::Kind::EnvironmentCoordinate) {
      *loc = loc->addHops(hops);
    }
  }

  // Each script has its own frame. A free name that is accessed
  // from an inner script must not be a frame slot access. If this
  // assertion is hit, it is a bug in the free name analysis in the
  // parser.
  MOZ_ASSERT_IF(!inCurrentScript, loc->kind() != NameLocation::Kind::FrameSlot);

  // It is always correct to not cache the location. Ignore OOMs to make
  // lookups infallible.
  if (!putNameInCache(bce, name, *loc)) {
    bce->fc->recoverFromOutOfMemory();
  }

  return *loc;
}

bool EmitterScope::internEmptyGlobalScopeAsBody(BytecodeEmitter* bce) {
  // Only the self-hosted top-level script uses this. If this changes, you must
  // update ScopeStencil::enclosing.
  MOZ_ASSERT(bce->emitterMode == BytecodeEmitter::SelfHosting);

  hasEnvironment_ = Scope::hasEnvironment(ScopeKind::Global);

  bce->bodyScopeIndex =
      GCThingIndex(bce->perScriptData().gcThingList().length());
  return bce->perScriptData().gcThingList().appendEmptyGlobalScope(
      &scopeIndex_);
}

bool EmitterScope::internScopeStencil(BytecodeEmitter* bce,
                                      ScopeIndex scopeIndex) {
  ScopeStencil& scope = bce->compilationState.scopeData[scopeIndex.index];
  hasEnvironment_ = scope.hasEnvironment();
  return bce->perScriptData().gcThingList().append(scopeIndex, &scopeIndex_);
}

bool EmitterScope::internBodyScopeStencil(BytecodeEmitter* bce,
                                          ScopeIndex scopeIndex) {
  MOZ_ASSERT(bce->bodyScopeIndex == ScopeNote::NoScopeIndex,
             "There can be only one body scope");
  bce->bodyScopeIndex =
      GCThingIndex(bce->perScriptData().gcThingList().length());
  return internScopeStencil(bce, scopeIndex);
}

bool EmitterScope::appendScopeNote(BytecodeEmitter* bce) {
  MOZ_ASSERT(ScopeKindIsInBody(scope(bce).kind()) && enclosingInFrame(),
             "Scope notes are not needed for body-level scopes.");
  noteIndex_ = bce->bytecodeSection().scopeNoteList().length();
  return bce->bytecodeSection().scopeNoteList().append(
      index(), bce->bytecodeSection().offset(),
      enclosingInFrame() ? enclosingInFrame()->noteIndex()
                         : ScopeNote::NoScopeNoteIndex);
}

bool EmitterScope::clearFrameSlotRange(BytecodeEmitter* bce, JSOp opcode,
                                       uint32_t slotStart,
                                       uint32_t slotEnd) const {
  MOZ_ASSERT(opcode == JSOp::Uninitialized || opcode == JSOp::Undefined);

  // Lexical bindings throw ReferenceErrors if they are used before
  // initialization. See ES6 8.1.1.1.6.
  //
  // For completeness, lexical bindings are initialized in ES6 by calling
  // InitializeBinding, after which touching the binding will no longer
  // throw reference errors. See 13.1.11, 9.2.13, 13.6.3.4, 13.6.4.6,
  // 13.6.4.8, 13.14.5, 15.1.8, and 15.2.0.15.
  //
  // This code is also used to reset `var`s to `undefined` when entering an
  // extra body var scope; and to clear slots when leaving a block, in
  // generators and async functions, to avoid keeping garbage alive
  // indefinitely.
  if (slotStart != slotEnd) {
    if (!bce->emit1(opcode)) {
      return false;
    }
    for (uint32_t slot = slotStart; slot < slotEnd; slot++) {
      if (!bce->emitLocalOp(JSOp::InitLexical, slot)) {
        return false;
      }
    }
    if (!bce->emit1(JSOp::Pop)) {
      return false;
    }
  }

  return true;
}

void EmitterScope::dump(BytecodeEmitter* bce) {
  fprintf(stdout, "EmitterScope [%s] %p\n", ScopeKindString(scope(bce).kind()),
          this);

  for (NameLocationMap::Range r = nameCache_->all(); !r.empty(); r.popFront()) {
    const NameLocation& l = r.front().value();

    auto atom = r.front().key();
    UniqueChars bytes = bce->parserAtoms().toPrintableString(atom);
    if (!bytes) {
      ReportOutOfMemory(bce->fc);
      return;
    }
    if (l.kind() != NameLocation::Kind::Dynamic) {
      fprintf(stdout, "  %s %s ", BindingKindString(l.bindingKind()),
              bytes.get());
    } else {
      fprintf(stdout, "  %s ", bytes.get());
    }

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
                l.environmentCoordinate().hops(),
                l.environmentCoordinate().slot());
        break;
      case NameLocation::Kind::DebugEnvironmentCoordinate:
        fprintf(stdout, "debugEnvironment hops=%u slot=%u\n",
                l.environmentCoordinate().hops(),
                l.environmentCoordinate().slot());
        break;
      case NameLocation::Kind::DynamicAnnexBVar:
        fprintf(stdout, "dynamic annex b var\n");
        break;
    }
  }

  fprintf(stdout, "\n");
}

bool EmitterScope::enterLexical(BytecodeEmitter* bce, ScopeKind kind,
                                LexicalScope::ParserData* bindings) {
  MOZ_ASSERT(kind != ScopeKind::NamedLambda &&
             kind != ScopeKind::StrictNamedLambda);
  MOZ_ASSERT(this == bce->innermostEmitterScopeNoCheck());

  if (!ensureCache(bce)) {
    return false;
  }

  // Resolve bindings.
  TDZCheckCache* tdzCache = bce->innermostTDZCheckCache;
  uint32_t firstFrameSlot = frameSlotStart();
  ParserBindingIter bi(*bindings, firstFrameSlot, /* isNamedLambda = */ false);
  for (; bi; bi++) {
    if (!checkSlotLimits(bce, bi)) {
      return false;
    }

    NameLocation loc = bi.nameLocation();
    if (!putNameInCache(bce, bi.name(), loc)) {
      return false;
    }

    if (!tdzCache->noteTDZCheck(bce, bi.name(), CheckTDZ)) {
      return false;
    }
  }

  updateFrameFixedSlots(bce, bi);

  ScopeIndex scopeIndex;
  if (!ScopeStencil::createForLexicalScope(
          bce->fc, bce->compilationState, kind, bindings, firstFrameSlot,
          enclosingScopeIndex(bce), &scopeIndex)) {
    return false;
  }
  if (!internScopeStencil(bce, scopeIndex)) {
    return false;
  }

  if (ScopeKindIsInBody(kind) && hasEnvironment()) {
    // After interning the VM scope we can get the scope index.
    if (!bce->emitInternedScopeOp(index(), JSOp::PushLexicalEnv)) {
      return false;
    }
  }

  // Lexical scopes need notes to be mapped from a pc.
  if (!appendScopeNote(bce)) {
    return false;
  }

  // Put frame slots in TDZ. Environment slots are poisoned during
  // environment creation.
  //
  // This must be done after appendScopeNote to be considered in the extent
  // of the scope.
  if (!deadZoneFrameSlotRange(bce, firstFrameSlot, frameSlotEnd())) {
    return false;
  }

  return checkEnvironmentChainLength(bce);
}

bool EmitterScope::enterClassBody(BytecodeEmitter* bce, ScopeKind kind,
                                  ClassBodyScope::ParserData* bindings) {
  MOZ_ASSERT(kind == ScopeKind::ClassBody);
  MOZ_ASSERT(this == bce->innermostEmitterScopeNoCheck());

  if (!ensureCache(bce)) {
    return false;
  }

  // Resolve bindings.
  TDZCheckCache* tdzCache = bce->innermostTDZCheckCache;
  uint32_t firstFrameSlot = frameSlotStart();
  ParserBindingIter bi(*bindings, firstFrameSlot);
  for (; bi; bi++) {
    if (!checkSlotLimits(bce, bi)) {
      return false;
    }

    NameLocation loc = bi.nameLocation();
    if (!putNameInCache(bce, bi.name(), loc)) {
      return false;
    }

    if (!tdzCache->noteTDZCheck(bce, bi.name(), CheckTDZ)) {
      return false;
    }
  }

  updateFrameFixedSlots(bce, bi);

  ScopeIndex scopeIndex;
  if (!ScopeStencil::createForClassBodyScope(
          bce->fc, bce->compilationState, kind, bindings, firstFrameSlot,
          enclosingScopeIndex(bce), &scopeIndex)) {
    return false;
  }
  if (!internScopeStencil(bce, scopeIndex)) {
    return false;
  }

  if (ScopeKindIsInBody(kind) && hasEnvironment()) {
    // After interning the VM scope we can get the scope index.
    //
    // ClassBody uses PushClassBodyEnv, however, PopLexicalEnv supports both
    // cases and doesn't need extra specialization.
    if (!bce->emitInternedScopeOp(index(), JSOp::PushClassBodyEnv)) {
      return false;
    }
  }

  // Lexical scopes need notes to be mapped from a pc.
  if (!appendScopeNote(bce)) {
    return false;
  }

  return checkEnvironmentChainLength(bce);
}

bool EmitterScope::enterNamedLambda(BytecodeEmitter* bce, FunctionBox* funbox) {
  MOZ_ASSERT(this == bce->innermostEmitterScopeNoCheck());
  MOZ_ASSERT(funbox->namedLambdaBindings());

  if (!ensureCache(bce)) {
    return false;
  }

  ParserBindingIter bi(*funbox->namedLambdaBindings(), LOCALNO_LIMIT,
                       /* isNamedLambda = */ true);
  MOZ_ASSERT(bi.kind() == BindingKind::NamedLambdaCallee);

  // The lambda name, if not closed over, is accessed via JSOp::Callee and
  // not a frame slot. Do not update frame slot information.
  NameLocation loc = bi.nameLocation();
  if (!putNameInCache(bce, bi.name(), loc)) {
    return false;
  }

  bi++;
  MOZ_ASSERT(!bi, "There should be exactly one binding in a NamedLambda scope");

  ScopeKind scopeKind =
      funbox->strict() ? ScopeKind::StrictNamedLambda : ScopeKind::NamedLambda;

  ScopeIndex scopeIndex;
  if (!ScopeStencil::createForLexicalScope(
          bce->fc, bce->compilationState, scopeKind,
          funbox->namedLambdaBindings(), LOCALNO_LIMIT,
          enclosingScopeIndex(bce), &scopeIndex)) {
    return false;
  }
  if (!internScopeStencil(bce, scopeIndex)) {
    return false;
  }

  return checkEnvironmentChainLength(bce);
}

bool EmitterScope::enterFunction(BytecodeEmitter* bce, FunctionBox* funbox) {
  MOZ_ASSERT(this == bce->innermostEmitterScopeNoCheck());

  // If there are parameter expressions, there is an extra var scope.
  if (!funbox->functionHasExtraBodyVarScope()) {
    bce->setVarEmitterScope(this);
  }

  if (!ensureCache(bce)) {
    return false;
  }

  // Resolve body-level bindings, if there are any.
  auto bindings = funbox->functionScopeBindings();
  if (bindings) {
    NameLocationMap& cache = *nameCache_;

    ParserBindingIter bi(*bindings, funbox->hasParameterExprs);
    for (; bi; bi++) {
      if (!checkSlotLimits(bce, bi)) {
        return false;
      }

      NameLocation loc = bi.nameLocation();
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
        ReportOutOfMemory(bce->fc);
        return false;
      }
    }

    updateFrameFixedSlots(bce, bi);
  } else {
    nextFrameSlot_ = 0;
  }

  // If the function's scope may be extended at runtime due to sloppy direct
  // eval, any names beyond the function scope must be accessed dynamically as
  // we don't know if the name will become a 'var' binding due to direct eval.
  if (funbox->funHasExtensibleScope()) {
    fallbackFreeNameLocation_ = Some(NameLocation::Dynamic());
  } else if (funbox->isStandalone) {
    // If the function is standalone, the enclosing scope is either an empty
    // global or non-syntactic scope, and there's no static bindings.
    if (bce->compilationState.input.target ==
        CompilationInput::CompilationTarget::
            StandaloneFunctionInNonSyntacticScope) {
      fallbackFreeNameLocation_ = Some(NameLocation::Dynamic());
    } else {
      fallbackFreeNameLocation_ = Some(NameLocation::Global(BindingKind::Var));
    }
  }

  // In case of parameter expressions, the parameters are lexical
  // bindings and have TDZ.
  if (funbox->hasParameterExprs && nextFrameSlot_) {
    uint32_t paramFrameSlotEnd = 0;
    for (ParserBindingIter bi(*bindings, true); bi; bi++) {
      if (!BindingKindIsLexical(bi.kind())) {
        break;
      }

      NameLocation loc = bi.nameLocation();
      if (loc.kind() == NameLocation::Kind::FrameSlot) {
        MOZ_ASSERT(paramFrameSlotEnd <= loc.frameSlot());
        paramFrameSlotEnd = loc.frameSlot() + 1;
      }
    }

    if (!deadZoneFrameSlotRange(bce, 0, paramFrameSlotEnd)) {
      return false;
    }
  }

  ScopeIndex scopeIndex;
  if (!ScopeStencil::createForFunctionScope(
          bce->fc, bce->compilationState, funbox->functionScopeBindings(),
          funbox->hasParameterExprs,
          funbox->needsCallObjectRegardlessOfBindings(), funbox->index(),
          funbox->isArrow(), enclosingScopeIndex(bce), &scopeIndex)) {
    return false;
  }
  if (!internBodyScopeStencil(bce, scopeIndex)) {
    return false;
  }

  return checkEnvironmentChainLength(bce);
}

bool EmitterScope::enterFunctionExtraBodyVar(BytecodeEmitter* bce,
                                             FunctionBox* funbox) {
  MOZ_ASSERT(funbox->hasParameterExprs);
  MOZ_ASSERT(funbox->extraVarScopeBindings() ||
             funbox->needsExtraBodyVarEnvironmentRegardlessOfBindings());
  MOZ_ASSERT(this == bce->innermostEmitterScopeNoCheck());

  // The extra var scope is never popped once it's entered. It replaces the
  // function scope as the var emitter scope.
  bce->setVarEmitterScope(this);

  if (!ensureCache(bce)) {
    return false;
  }

  // Resolve body-level bindings, if there are any.
  uint32_t firstFrameSlot = frameSlotStart();
  if (auto bindings = funbox->extraVarScopeBindings()) {
    ParserBindingIter bi(*bindings, firstFrameSlot);
    for (; bi; bi++) {
      if (!checkSlotLimits(bce, bi)) {
        return false;
      }

      NameLocation loc = bi.nameLocation();
      MOZ_ASSERT(bi.kind() == BindingKind::Var);
      if (!putNameInCache(bce, bi.name(), loc)) {
        return false;
      }
    }

    uint32_t priorEnd = bce->maxFixedSlots;
    updateFrameFixedSlots(bce, bi);

    // If any of the bound slots were previously used, reset them to undefined.
    // This doesn't break TDZ for let/const/class bindings because there aren't
    // any in extra body var scopes. We assert above that bi.kind() is Var.
    uint32_t end = std::min(priorEnd, nextFrameSlot_);
    if (firstFrameSlot < end) {
      if (!clearFrameSlotRange(bce, JSOp::Undefined, firstFrameSlot, end)) {
        return false;
      }
    }
  } else {
    nextFrameSlot_ = firstFrameSlot;
  }

  // If the extra var scope may be extended at runtime due to sloppy
  // direct eval, any names beyond the var scope must be accessed
  // dynamically as we don't know if the name will become a 'var' binding
  // due to direct eval.
  if (funbox->funHasExtensibleScope()) {
    fallbackFreeNameLocation_ = Some(NameLocation::Dynamic());
  }

  // Create and intern the VM scope.
  ScopeIndex scopeIndex;
  if (!ScopeStencil::createForVarScope(
          bce->fc, bce->compilationState, ScopeKind::FunctionBodyVar,
          funbox->extraVarScopeBindings(), firstFrameSlot,
          funbox->needsExtraBodyVarEnvironmentRegardlessOfBindings(),
          enclosingScopeIndex(bce), &scopeIndex)) {
    return false;
  }
  if (!internScopeStencil(bce, scopeIndex)) {
    return false;
  }

  if (hasEnvironment()) {
    if (!bce->emitInternedScopeOp(index(), JSOp::PushVarEnv)) {
      return false;
    }
  }

  // The extra var scope needs a note to be mapped from a pc.
  if (!appendScopeNote(bce)) {
    return false;
  }

  return checkEnvironmentChainLength(bce);
}

bool EmitterScope::enterGlobal(BytecodeEmitter* bce,
                               GlobalSharedContext* globalsc) {
  MOZ_ASSERT(this == bce->innermostEmitterScopeNoCheck());

  // TODO-Stencil
  //   This is another snapshot-sensitive location.
  //   The incoming atoms from the global scope object should be snapshotted.
  //   For now, converting them to ParserAtoms here individually.

  bce->setVarEmitterScope(this);

  if (!ensureCache(bce)) {
    return false;
  }

  if (bce->emitterMode == BytecodeEmitter::SelfHosting) {
    // In self-hosting, it is incorrect to consult the global scope because
    // self-hosted scripts are cloned into their target compartments before
    // they are run. Instead of Global, Intrinsic is used for all names.
    //
    // Intrinsic lookups are redirected to the special intrinsics holder
    // in the global object, into which any missing values are cloned
    // lazily upon first access.
    fallbackFreeNameLocation_ = Some(NameLocation::Intrinsic());

    return internEmptyGlobalScopeAsBody(bce);
  }

  ScopeIndex scopeIndex;
  if (!ScopeStencil::createForGlobalScope(bce->fc, bce->compilationState,
                                          globalsc->scopeKind(),
                                          globalsc->bindings, &scopeIndex)) {
    return false;
  }
  if (!internBodyScopeStencil(bce, scopeIndex)) {
    return false;
  }

  // See: JSScript::outermostScope.
  MOZ_ASSERT(bce->bodyScopeIndex == GCThingIndex::outermostScopeIndex(),
             "Global scope must be index 0");

  // Resolve binding names.
  //
  // NOTE: BytecodeEmitter::emitDeclarationInstantiation will emit the
  //       redeclaration check and initialize these bindings.
  if (globalsc->bindings) {
    for (ParserBindingIter bi(*globalsc->bindings); bi; bi++) {
      NameLocation loc = bi.nameLocation();
      if (!putNameInCache(bce, bi.name(), loc)) {
        return false;
      }
    }
  }

  // Note that to save space, we don't add free names to the cache for
  // global scopes. They are assumed to be global vars in the syntactic
  // global scope, dynamic accesses under non-syntactic global scope.
  if (globalsc->scopeKind() == ScopeKind::Global) {
    fallbackFreeNameLocation_ = Some(NameLocation::Global(BindingKind::Var));
  } else {
    fallbackFreeNameLocation_ = Some(NameLocation::Dynamic());
  }

  return true;
}

bool EmitterScope::enterEval(BytecodeEmitter* bce, EvalSharedContext* evalsc) {
  MOZ_ASSERT(this == bce->innermostEmitterScopeNoCheck());

  bce->setVarEmitterScope(this);

  if (!ensureCache(bce)) {
    return false;
  }

  // Create the `var` scope. Note that there is also a lexical scope, created
  // separately in emitScript().
  ScopeKind scopeKind =
      evalsc->strict() ? ScopeKind::StrictEval : ScopeKind::Eval;

  ScopeIndex scopeIndex;
  if (!ScopeStencil::createForEvalScope(
          bce->fc, bce->compilationState, scopeKind, evalsc->bindings,
          enclosingScopeIndex(bce), &scopeIndex)) {
    return false;
  }
  if (!internBodyScopeStencil(bce, scopeIndex)) {
    return false;
  }

  if (evalsc->strict()) {
    if (evalsc->bindings) {
      ParserBindingIter bi(*evalsc->bindings, true);
      for (; bi; bi++) {
        if (!checkSlotLimits(bce, bi)) {
          return false;
        }

        NameLocation loc = bi.nameLocation();
        if (!putNameInCache(bce, bi.name(), loc)) {
          return false;
        }
      }

      updateFrameFixedSlots(bce, bi);
    }
  } else {
    // For simplicity, treat all free name lookups in nonstrict eval scripts as
    // dynamic.
    fallbackFreeNameLocation_ = Some(NameLocation::Dynamic());
  }

  if (hasEnvironment()) {
    if (!bce->emitInternedScopeOp(index(), JSOp::PushVarEnv)) {
      return false;
    }
  } else {
    // NOTE: BytecodeEmitter::emitDeclarationInstantiation will emit the
    //       redeclaration check and initialize these bindings for sloppy
    //       eval.

    // As an optimization, if the eval does not have its own var
    // environment and is directly enclosed in a global scope, then all
    // free name lookups are global.
    if (scope(bce).enclosing().is<GlobalScope>()) {
      fallbackFreeNameLocation_ = Some(NameLocation::Global(BindingKind::Var));
    }
  }

  return checkEnvironmentChainLength(bce);
}

bool EmitterScope::enterModule(BytecodeEmitter* bce,
                               ModuleSharedContext* modulesc) {
  MOZ_ASSERT(this == bce->innermostEmitterScopeNoCheck());

  bce->setVarEmitterScope(this);

  if (!ensureCache(bce)) {
    return false;
  }

  // Resolve body-level bindings, if there are any.
  TDZCheckCache* tdzCache = bce->innermostTDZCheckCache;
  Maybe<uint32_t> firstLexicalFrameSlot;
  if (ModuleScope::ParserData* bindings = modulesc->bindings) {
    ParserBindingIter bi(*bindings);
    for (; bi; bi++) {
      if (!checkSlotLimits(bce, bi)) {
        return false;
      }

      NameLocation loc = bi.nameLocation();
      if (!putNameInCache(bce, bi.name(), loc)) {
        return false;
      }

      if (BindingKindIsLexical(bi.kind())) {
        if (loc.kind() == NameLocation::Kind::FrameSlot &&
            !firstLexicalFrameSlot) {
          firstLexicalFrameSlot = Some(loc.frameSlot());
        }

        if (!tdzCache->noteTDZCheck(bce, bi.name(), CheckTDZ)) {
          return false;
        }
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
    if (!deadZoneFrameSlotRange(bce, *firstLexicalFrameSlot, frameSlotEnd())) {
      return false;
    }
  }

  // Create and intern the VM scope creation data.
  ScopeIndex scopeIndex;
  if (!ScopeStencil::createForModuleScope(
          bce->fc, bce->compilationState, modulesc->bindings,
          enclosingScopeIndex(bce), &scopeIndex)) {
    return false;
  }
  if (!internBodyScopeStencil(bce, scopeIndex)) {
    return false;
  }

  return checkEnvironmentChainLength(bce);
}

bool EmitterScope::enterWith(BytecodeEmitter* bce) {
  MOZ_ASSERT(this == bce->innermostEmitterScopeNoCheck());

  if (!ensureCache(bce)) {
    return false;
  }

  // 'with' make all accesses dynamic and unanalyzable.
  fallbackFreeNameLocation_ = Some(NameLocation::Dynamic());

  ScopeIndex scopeIndex;
  if (!ScopeStencil::createForWithScope(bce->fc, bce->compilationState,
                                        enclosingScopeIndex(bce),
                                        &scopeIndex)) {
    return false;
  }

  if (!internScopeStencil(bce, scopeIndex)) {
    return false;
  }

  if (!bce->emitInternedScopeOp(index(), JSOp::EnterWith)) {
    return false;
  }

  if (!appendScopeNote(bce)) {
    return false;
  }

  return checkEnvironmentChainLength(bce);
}

bool EmitterScope::deadZoneFrameSlots(BytecodeEmitter* bce) const {
  return deadZoneFrameSlotRange(bce, frameSlotStart(), frameSlotEnd());
}

bool EmitterScope::leave(BytecodeEmitter* bce, bool nonLocal) {
  // If we aren't leaving the scope due to a non-local jump (e.g., break),
  // we must be the innermost scope.
  MOZ_ASSERT_IF(!nonLocal, this == bce->innermostEmitterScopeNoCheck());

  ScopeKind kind = scope(bce).kind();
  switch (kind) {
    case ScopeKind::Lexical:
    case ScopeKind::SimpleCatch:
    case ScopeKind::Catch:
    case ScopeKind::FunctionLexical:
    case ScopeKind::ClassBody:
      if (bce->sc->isFunctionBox() &&
          bce->sc->asFunctionBox()->needsClearSlotsOnExit()) {
        if (!deadZoneFrameSlots(bce)) {
          return false;
        }
      }
      if (!bce->emit1(hasEnvironment() ? JSOp::PopLexicalEnv
                                       : JSOp::DebugLeaveLexicalEnv)) {
        return false;
      }
      break;

    case ScopeKind::With:
      if (!bce->emit1(JSOp::LeaveWith)) {
        return false;
      }
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
      if (kind == ScopeKind::FunctionBodyVar) {
        // The extra function var scope is never popped once it's pushed,
        // so its scope note extends until the end of any possible code.
        bce->bytecodeSection().scopeNoteList().recordEndFunctionBodyVar(
            noteIndex_);
      } else {
        bce->bytecodeSection().scopeNoteList().recordEnd(
            noteIndex_, bce->bytecodeSection().offset());
      }
    }
  }

  return true;
}

AbstractScopePtr EmitterScope::scope(const BytecodeEmitter* bce) const {
  return bce->perScriptData().gcThingList().getScope(index());
}

mozilla::Maybe<ScopeIndex> EmitterScope::scopeIndex(
    const BytecodeEmitter* bce) const {
  return bce->perScriptData().gcThingList().getScopeIndex(index());
}

NameLocation EmitterScope::lookup(BytecodeEmitter* bce,
                                  TaggedParserAtomIndex name) {
  if (Maybe<NameLocation> loc = lookupInCache(bce, name)) {
    return *loc;
  }
  return searchAndCache(bce, name);
}

/* static */
uint32_t EmitterScope::CountEnclosingCompilationEnvironments(
    BytecodeEmitter* bce, EmitterScope* emitterScope) {
  uint32_t environments = emitterScope->hasEnvironment() ? 1 : 0;
  while ((emitterScope = emitterScope->enclosing(&bce))) {
    if (emitterScope->hasEnvironment()) {
      environments++;
    }
  }
  return environments;
}

void EmitterScope::lookupPrivate(BytecodeEmitter* bce,
                                 TaggedParserAtomIndex name, NameLocation& loc,
                                 mozilla::Maybe<NameLocation>& brandLoc) {
  loc = lookup(bce, name);

  // Private Brand checking relies on the ability to construct a new
  // environment coordinate for a name at a fixed offset, which will
  // correspond to the private brand for that class.
  //
  // If our name lookup isn't a fixed location, we must construct a
  // new environment coordinate, using information available from our private
  // field cache. (See cachePrivateFieldsForEval, and Bug 1638309).
  //
  // This typically involves a DebugEnvironmentProxy, so we need to use a
  // DebugEnvironmentCoordinate.
  //
  // See also Bug 793345 which argues that we should remove the
  // DebugEnvironmentProxy.
  if (loc.kind() != NameLocation::Kind::EnvironmentCoordinate &&
      loc.kind() != NameLocation::Kind::FrameSlot) {
    MOZ_ASSERT(loc.kind() == NameLocation::Kind::Dynamic ||
               loc.kind() == NameLocation::Kind::Global);
    // Private fields don't require brand checking and can be correctly
    // code-generated with dynamic name lookup bytecode we have today. However,
    // for that to happen we first need to figure out if we have a Private
    // method or private field, which we cannot disambiguate based on the
    // dynamic lookup.
    //
    // However, this is precisely the case that the private field eval case can
    // help us handle. It knows the truth about these private bindings.
    mozilla::Maybe<NameLocation> cacheEntry =
        bce->compilationState.scopeContext.getPrivateFieldLocation(name);
    MOZ_ASSERT(cacheEntry);

    if (cacheEntry->bindingKind() == BindingKind::PrivateMethod) {
      MOZ_ASSERT(cacheEntry->kind() ==
                 NameLocation::Kind::DebugEnvironmentCoordinate);
      // To construct the brand check there are two hop values required:
      //
      // 1. Compilation Hops: The number of environment hops required to get to
      //    the compilation enclosing environment.
      // 2. "external hops", to get from compilation enclosing debug environment
      //     to the environment that actually contains our brand. This is
      //     determined by the cacheEntry. This traversal will bypass a Debug
      //     environment proxy, which is why need to use
      //     DebugEnvironmentCoordinate.

      uint32_t compilation_hops =
          CountEnclosingCompilationEnvironments(bce, this);

      uint32_t external_hops = cacheEntry->environmentCoordinate().hops();

      brandLoc = Some(NameLocation::DebugEnvironmentCoordinate(
          BindingKind::Synthetic, compilation_hops + external_hops,
          ClassBodyLexicalEnvironmentObject::privateBrandSlot()));
    } else {
      brandLoc = Nothing();
    }
    return;
  }

  if (loc.bindingKind() == BindingKind::PrivateMethod) {
    uint32_t hops = 0;
    if (loc.kind() == NameLocation::Kind::EnvironmentCoordinate) {
      hops = loc.environmentCoordinate().hops();
    } else {
      // If we have a FrameSlot, then our innermost emitter scope must be a
      // class body scope, and we can generate an environment coordinate with
      // hops=0 to find the associated brand location.
      MOZ_ASSERT(bce->innermostScope().is<ClassBodyScope>());
    }

    brandLoc = Some(NameLocation::EnvironmentCoordinate(
        BindingKind::Synthetic, hops,
        ClassBodyLexicalEnvironmentObject::privateBrandSlot()));
  } else {
    brandLoc = Nothing();
  }
}

Maybe<NameLocation> EmitterScope::locationBoundInScope(
    TaggedParserAtomIndex name, EmitterScope* target) {
  // The target scope must be an intra-frame enclosing scope of this
  // one. Count the number of extra hops to reach it.
  uint8_t extraHops = 0;
  for (EmitterScope* es = this; es != target; es = es->enclosingInFrame()) {
    if (es->hasEnvironment()) {
      extraHops++;
    }
  }

  // Caches are prepopulated with bound names. So if the name is bound in a
  // particular scope, it must already be in the cache. Furthermore, don't
  // consult the fallback location as we only care about binding names.
  Maybe<NameLocation> loc;
  if (NameLocationMap::Ptr p = target->nameCache_->lookup(name)) {
    NameLocation l = p->value().wrapped;
    if (l.kind() == NameLocation::Kind::EnvironmentCoordinate) {
      loc = Some(l.addHops(extraHops));
    } else {
      loc = Some(l);
    }
  }
  return loc;
}
