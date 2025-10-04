/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/FunctionEmitter.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include "frontend/AsyncEmitter.h"         // AsyncEmitter
#include "frontend/BytecodeEmitter.h"      // BytecodeEmitter
#include "frontend/FunctionSyntaxKind.h"   // FunctionSyntaxKind
#include "frontend/ModuleSharedContext.h"  // ModuleSharedContext
#include "frontend/NameAnalysisTypes.h"    // NameLocation
#include "frontend/NameOpEmitter.h"        // NameOpEmitter
#include "frontend/SharedContext.h"        // SharedContext
#include "vm/ModuleBuilder.h"              // ModuleBuilder
#include "vm/Opcodes.h"                    // JSOp
#include "vm/Scope.h"                      // BindingKind

using namespace js;
using namespace js::frontend;

using mozilla::Maybe;
using mozilla::Some;

FunctionEmitter::FunctionEmitter(BytecodeEmitter* bce, FunctionBox* funbox,
                                 FunctionSyntaxKind syntaxKind,
                                 IsHoisted isHoisted)
    : bce_(bce),
      funbox_(funbox),
      name_(funbox_->explicitName()),
      syntaxKind_(syntaxKind),
      isHoisted_(isHoisted) {}

bool FunctionEmitter::prepareForNonLazy() {
  MOZ_ASSERT(state_ == State::Start);

  MOZ_ASSERT(funbox_->isInterpreted());
  MOZ_ASSERT(funbox_->emitBytecode);
  MOZ_ASSERT(!funbox_->wasEmittedByEnclosingScript());

  //                [stack]

  funbox_->setWasEmittedByEnclosingScript(true);

#ifdef DEBUG
  state_ = State::NonLazy;
#endif
  return true;
}

bool FunctionEmitter::emitNonLazyEnd() {
  MOZ_ASSERT(state_ == State::NonLazy);

  //                [stack]

  if (!emitFunction()) {
    //              [stack] FUN?
    return false;
  }

#ifdef DEBUG
  state_ = State::End;
#endif
  return true;
}

bool FunctionEmitter::emitLazy() {
  MOZ_ASSERT(state_ == State::Start);

  MOZ_ASSERT(funbox_->isInterpreted());
  MOZ_ASSERT(!funbox_->emitBytecode);
  MOZ_ASSERT(!funbox_->wasEmittedByEnclosingScript());

  //                [stack]

  funbox_->setWasEmittedByEnclosingScript(true);

  // Prepare to update the inner lazy script now that it's parent is fully
  // compiled. These updates will be applied in UpdateEmittedInnerFunctions().
  funbox_->setEnclosingScopeForInnerLazyFunction(bce_->innermostScopeIndex());

  if (!emitFunction()) {
    //              [stack] FUN?
    return false;
  }

#ifdef DEBUG
  state_ = State::End;
#endif
  return true;
}

bool FunctionEmitter::emitAgain() {
  MOZ_ASSERT(state_ == State::Start);
  MOZ_ASSERT(funbox_->wasEmittedByEnclosingScript());

  //                [stack]

  // Annex B block-scoped functions are hoisted like any other assignment
  // that assigns the function to the outer 'var' binding.
  if (!funbox_->isAnnexB) {
#ifdef DEBUG
    state_ = State::End;
#endif
    return true;
  }

  // Get the location of the 'var' binding in the body scope. The
  // name must be found, else there is a bug in the Annex B handling
  // in Parser.
  //
  // In sloppy eval contexts, this location is dynamic.
  Maybe<NameLocation> lhsLoc =
      bce_->locationOfNameBoundInScope(name_, bce_->varEmitterScope);

  // If there are parameter expressions, the var name could be a
  // parameter.
  if (!lhsLoc && bce_->sc->isFunctionBox() &&
      bce_->sc->asFunctionBox()->functionHasExtraBodyVarScope()) {
    lhsLoc = bce_->locationOfNameBoundInScope(
        name_, bce_->varEmitterScope->enclosingInFrame());
  }

  if (!lhsLoc) {
    lhsLoc = Some(NameLocation::DynamicAnnexBVar());
  } else {
    MOZ_ASSERT(lhsLoc->bindingKind() == BindingKind::Var ||
               lhsLoc->bindingKind() == BindingKind::FormalParameter ||
               (lhsLoc->bindingKind() == BindingKind::Let &&
                bce_->sc->asFunctionBox()->hasParameterExprs));
  }

  NameOpEmitter noe(bce_, name_, *lhsLoc,
                    NameOpEmitter::Kind::SimpleAssignment);
  if (!noe.prepareForRhs()) {
    //              [stack]
    return false;
  }

  if (!bce_->emitGetName(name_)) {
    //              [stack] FUN
    return false;
  }

  if (!noe.emitAssignment()) {
    //              [stack] FUN
    return false;
  }

  if (!bce_->emit1(JSOp::Pop)) {
    //              [stack]
    return false;
  }

#ifdef DEBUG
  state_ = State::End;
#endif
  return true;
}

bool FunctionEmitter::emitAsmJSModule() {
  MOZ_ASSERT(state_ == State::Start);

  MOZ_ASSERT(!funbox_->wasEmittedByEnclosingScript());
  MOZ_ASSERT(funbox_->isAsmJSModule());

  //                [stack]

  funbox_->setWasEmittedByEnclosingScript(true);

  if (!emitFunction()) {
    //              [stack]
    return false;
  }

#ifdef DEBUG
  state_ = State::End;
#endif
  return true;
}

bool FunctionEmitter::emitFunction() {
  // Make the function object a literal in the outer script's pool.
  GCThingIndex index;
  if (!bce_->perScriptData().gcThingList().append(funbox_, &index)) {
    return false;
  }

  //                [stack]

  if (isHoisted_ == IsHoisted::No) {
    return emitNonHoisted(index);
    //              [stack] FUN?
  }

  bool topLevelFunction;
  if (bce_->sc->isFunctionBox() ||
      (bce_->sc->isEvalContext() && bce_->sc->strict())) {
    // No nested functions inside other functions are top-level.
    topLevelFunction = false;
  } else {
    // In sloppy eval scripts, top-level functions are accessed dynamically.
    // In global and module scripts, top-level functions are those bound in
    // the var scope.
    NameLocation loc = bce_->lookupName(name_);
    topLevelFunction = loc.kind() == NameLocation::Kind::Dynamic ||
                       loc.bindingKind() == BindingKind::Var;
  }

  if (topLevelFunction) {
    return emitTopLevelFunction(index);
    //              [stack]
  }

  return emitHoisted(index);
  //                [stack]
}

bool FunctionEmitter::emitNonHoisted(GCThingIndex index) {
  // Non-hoisted functions simply emit their respective op.

  //                [stack]

  if (syntaxKind_ == FunctionSyntaxKind::DerivedClassConstructor) {
    //              [stack] PROTO
    if (!bce_->emitGCIndexOp(JSOp::FunWithProto, index)) {
      //            [stack] FUN
      return false;
    }
    return true;
  }

  // This is a FunctionExpression, ArrowFunctionExpression, or class
  // constructor. Emit the single instruction (without location info).
  if (!bce_->emitGCIndexOp(JSOp::Lambda, index)) {
    //              [stack] FUN
    return false;
  }

  return true;
}

bool FunctionEmitter::emitHoisted(GCThingIndex index) {
  MOZ_ASSERT(syntaxKind_ == FunctionSyntaxKind::Statement);

  //                [stack]

  // For functions nested within functions and blocks, make a lambda and
  // initialize the binding name of the function in the current scope.

  NameOpEmitter noe(bce_, name_, NameOpEmitter::Kind::Initialize);
  if (!noe.prepareForRhs()) {
    //              [stack]
    return false;
  }

  if (!bce_->emitGCIndexOp(JSOp::Lambda, index)) {
    //              [stack] FUN
    return false;
  }

  if (!noe.emitAssignment()) {
    //              [stack] FUN
    return false;
  }

  if (!bce_->emit1(JSOp::Pop)) {
    //              [stack]
    return false;
  }

  return true;
}

bool FunctionEmitter::emitTopLevelFunction(GCThingIndex index) {
  //                [stack]

  if (bce_->sc->isModuleContext()) {
    // For modules, we record the function and instantiate the binding
    // during ModuleInstantiate(), before the script is run.
    return bce_->sc->asModuleContext()->builder.noteFunctionDeclaration(
        bce_->fc, index);
  }

  MOZ_ASSERT(bce_->sc->isGlobalContext() || bce_->sc->isEvalContext());
  MOZ_ASSERT(syntaxKind_ == FunctionSyntaxKind::Statement);
  MOZ_ASSERT(bce_->inPrologue());

  // NOTE: The `index` is not directly stored as an opcode, but we collect the
  // range of indices in `BytecodeEmitter::emitDeclarationInstantiation` instead
  // of discrete indices.
  (void)index;

  return true;
}

bool FunctionScriptEmitter::prepareForParameters() {
  MOZ_ASSERT(state_ == State::Start);
  MOZ_ASSERT(bce_->inPrologue());

  //                [stack]

  if (paramStart_) {
    bce_->setScriptStartOffsetIfUnset(*paramStart_);
  }

  // The ordering of these EmitterScopes is important. The named lambda
  // scope needs to enclose the function scope needs to enclose the extra
  // var scope.

  if (funbox_->namedLambdaBindings()) {
    namedLambdaEmitterScope_.emplace(bce_);
    if (!namedLambdaEmitterScope_->enterNamedLambda(bce_, funbox_)) {
      return false;
    }
  }

  if (funbox_->needsPromiseResult()) {
    asyncEmitter_.emplace(bce_);
  }

  if (bodyEnd_) {
    bce_->setFunctionBodyEndPos(*bodyEnd_);
  }

  if (paramStart_) {
    if (!bce_->updateLineNumberNotes(*paramStart_)) {
      return false;
    }
  }

  tdzCache_.emplace(bce_);
  functionEmitterScope_.emplace(bce_);

  if (!functionEmitterScope_->enterFunction(bce_, funbox_)) {
    return false;
  }

  if (!emitInitializeClosedOverArgumentBindings()) {
    //              [stack]
    return false;
  }

  if (funbox_->hasParameterExprs) {
    // There's parameter exprs, emit them in the main section.
    //
    // One caveat is that Debugger considers ops in the prologue to be
    // unreachable (i.e. cannot set a breakpoint on it). If there are no
    // parameter exprs, any unobservable environment ops (like pushing the
    // call object, setting '.this', etc) need to go in the prologue, else it
    // messes up breakpoint tests.
    bce_->switchToMain();
  }

  if (!bce_->emitInitializeFunctionSpecialNames()) {
    //              [stack]
    return false;
  }

  if (!funbox_->hasParameterExprs) {
    bce_->switchToMain();
  }

  if (funbox_->needsPromiseResult()) {
    if (funbox_->hasParameterExprs || funbox_->hasDestructuringArgs) {
      if (!asyncEmitter_->prepareForParamsWithExpressionOrDestructuring()) {
        return false;
      }
    } else {
      if (!asyncEmitter_->prepareForParamsWithoutExpressionOrDestructuring()) {
        return false;
      }
    }
  }

  if (funbox_->isClassConstructor()) {
    if (!funbox_->isDerivedClassConstructor()) {
      if (!bce_->emitInitializeInstanceMembers(false)) {
        //          [stack]
        return false;
      }
    }
  }

#ifdef DEBUG
  state_ = State::Parameters;
#endif
  return true;
}

bool FunctionScriptEmitter::emitInitializeClosedOverArgumentBindings() {
  // Initialize CallObject slots for closed-over arguments. If the function has
  // parameter expressions, these are lexical bindings and we initialize the
  // slots to the magic TDZ value. If the function doesn't have parameter
  // expressions, we copy the frame's arguments.

  MOZ_ASSERT(bce_->inPrologue());

  auto* bindings = funbox_->functionScopeBindings();
  if (!bindings) {
    return true;
  }

  const bool hasParameterExprs = funbox_->hasParameterExprs;

  bool pushedUninitialized = false;
  for (ParserPositionalFormalParameterIter fi(*bindings, hasParameterExprs); fi;
       fi++) {
    if (!fi.closedOver()) {
      continue;
    }

    if (hasParameterExprs) {
      NameLocation nameLoc = bce_->lookupName(fi.name());
      if (!pushedUninitialized) {
        if (!bce_->emit1(JSOp::Uninitialized)) {
          //          [stack] UNINITIALIZED
          return false;
        }
        pushedUninitialized = true;
      }
      if (!bce_->emitEnvCoordOp(JSOp::InitAliasedLexical,
                                nameLoc.environmentCoordinate())) {
        //            [stack] UNINITIALIZED
        return false;
      }
    } else {
      NameOpEmitter noe(bce_, fi.name(), NameOpEmitter::Kind::Initialize);
      if (!noe.prepareForRhs()) {
        //            [stack]
        return false;
      }

      if (!bce_->emitArgOp(JSOp::GetFrameArg, fi.argumentSlot())) {
        //            [stack] VAL
        return false;
      }

      if (!noe.emitAssignment()) {
        //            [stack] VAL
        return false;
      }

      if (!bce_->emit1(JSOp::Pop)) {
        //            [stack]
        return false;
      }
    }
  }

  if (pushedUninitialized) {
    MOZ_ASSERT(hasParameterExprs);
    if (!bce_->emit1(JSOp::Pop)) {
      //              [stack]
      return false;
    }
  }

  return true;
}

bool FunctionScriptEmitter::prepareForBody() {
  MOZ_ASSERT(state_ == State::Parameters);

  //                [stack]

  if (funbox_->needsPromiseResult()) {
    if (!asyncEmitter_->emitParamsEpilogue()) {
      return false;
    }
  }

  if (!emitExtraBodyVarScope()) {
    //              [stack]
    return false;
  }

  if (funbox_->needsPromiseResult()) {
    if (!asyncEmitter_->prepareForBody()) {
      return false;
    }
  }

#ifdef DEBUG
  state_ = State::Body;
#endif
  return true;
}

bool FunctionScriptEmitter::emitExtraBodyVarScope() {
  //                [stack]

  if (!funbox_->functionHasExtraBodyVarScope()) {
    return true;
  }

  extraBodyVarEmitterScope_.emplace(bce_);
  if (!extraBodyVarEmitterScope_->enterFunctionExtraBodyVar(bce_, funbox_)) {
    return false;
  }

  if (!funbox_->extraVarScopeBindings() || !funbox_->functionScopeBindings()) {
    return true;
  }

  // After emitting expressions for all parameters, copy over any formal
  // parameters which have been redeclared as vars. For example, in the
  // following, the var y in the body scope is 42:
  //
  //   function f(x, y = 42) { var y; }
  //
  for (ParserBindingIter bi(*funbox_->functionScopeBindings(), true); bi;
       bi++) {
    auto name = bi.name();

    // There may not be a var binding of the same name.
    if (!bce_->locationOfNameBoundInScope(name,
                                          extraBodyVarEmitterScope_.ptr())) {
      continue;
    }

    // The '.this', '.newTarget', and '.generator' function special binding
    // should never appear in the extra var scope. 'arguments', however, may.
    MOZ_ASSERT(name != TaggedParserAtomIndex::WellKnown::dot_this_() &&
               name != TaggedParserAtomIndex::WellKnown::dot_newTarget_() &&
               name != TaggedParserAtomIndex::WellKnown::dot_generator_());

    NameOpEmitter noe(bce_, name, NameOpEmitter::Kind::Initialize);
    if (!noe.prepareForRhs()) {
      //            [stack]
      return false;
    }

    NameLocation paramLoc =
        *bce_->locationOfNameBoundInScope(name, functionEmitterScope_.ptr());
    if (!bce_->emitGetNameAtLocation(name, paramLoc)) {
      //            [stack] VAL
      return false;
    }

    if (!noe.emitAssignment()) {
      //            [stack] VAL
      return false;
    }

    if (!bce_->emit1(JSOp::Pop)) {
      //            [stack]
      return false;
    }
  }

  return true;
}

bool FunctionScriptEmitter::emitEndBody() {
  MOZ_ASSERT(state_ == State::Body);
  //                [stack]

  if (bodyEnd_) {
    if (!bce_->updateSourceCoordNotes(*bodyEnd_)) {
      return false;
    }
  }

  if (funbox_->needsFinalYield()) {
    // If we fall off the end of a generator or async function, we
    // do a final yield with an |undefined| payload. We put all
    // the code to do this in one place, both to reduce bytecode
    // size and to prevent any OOM or debugger exception that occurs
    // at this point from being caught inside the function.
    if (!bce_->emit1(JSOp::Undefined)) {
      //          [stack] UNDEF
      return false;
    }
    if (!bce_->emit1(JSOp::SetRval)) {
      //          [stack]
      return false;
    }

    // Return statements in the body of the function will jump here
    // with the return payload in rval.
    if (!bce_->emitJumpTargetAndPatch(bce_->finalYields)) {
      //          [stack]
      return false;
    }

    if (funbox_->needsIteratorResult()) {
      MOZ_ASSERT(!funbox_->needsPromiseResult());
      // Emit final yield bytecode for generators, for example:
      // function gen * () { ... }
      if (!bce_->emitPrepareIteratorResult()) {
        //          [stack] RESULT
        return false;
      }

      if (!bce_->emit1(JSOp::GetRval)) {
        //          [stack] RESULT RVAL
        return false;
      }

      if (!bce_->emitFinishIteratorResult(true)) {
        //          [stack] RESULT
        return false;
      }

      if (!bce_->emit1(JSOp::SetRval)) {
        //          [stack]
        return false;
      }

    } else if (funbox_->needsPromiseResult()) {
      // Emit final yield bytecode for async functions, for example:
      // async function deferred() { ... }
      if (!bce_->emit1(JSOp::GetRval)) {
        //          [stack] RVAL
        return false;
      }

      if (!bce_->emitGetDotGeneratorInInnermostScope()) {
        //          [stack] RVAL GEN
        return false;
      }

      if (!bce_->emit1(JSOp::AsyncResolve)) {
        //          [stack] PROMISE
        return false;
      }

      if (!bce_->emit1(JSOp::SetRval)) {
        //          [stack]
        return false;
      }
    }

    // Emit the final yield.
    if (!bce_->emitGetDotGeneratorInInnermostScope()) {
      //          [stack] GEN
      return false;
    }

    if (!bce_->emitYieldOp(JSOp::FinalYieldRval)) {
      return false;
    }

    if (funbox_->needsPromiseResult()) {
      // Emit the reject catch block.
      if (!asyncEmitter_->emitEndFunction()) {
        return false;
      }
    }

  } else {
    // Non-generator functions just return |undefined|. The
    // JSOp::RetRval emitted below will do that, except if the
    // script has a finally block: there can be a non-undefined
    // value in the return value slot. Make sure the return value
    // is |undefined|.
    if (bce_->hasTryFinally) {
      if (!bce_->emit1(JSOp::Undefined)) {
        //          [stack] UNDEF
        return false;
      }
      if (!bce_->emit1(JSOp::SetRval)) {
        //          [stack]
        return false;
      }
    }
  }

  // Execute |CheckReturn| right before exiting the class constructor.
  if (funbox_->isDerivedClassConstructor()) {
    if (!bce_->emitJumpTargetAndPatch(bce_->endOfDerivedClassConstructorBody)) {
      return false;
    }

    if (!bce_->emitCheckDerivedClassConstructorReturn()) {
      //            [stack]
      return false;
    }
  }

  if (extraBodyVarEmitterScope_) {
    if (!extraBodyVarEmitterScope_->leave(bce_)) {
      return false;
    }

    extraBodyVarEmitterScope_.reset();
  }

  if (!functionEmitterScope_->leave(bce_)) {
    return false;
  }
  functionEmitterScope_.reset();
  tdzCache_.reset();

  // We only want to mark the end of a function as a breakable position if
  // there is token there that the user can easily associate with the function
  // as a whole. Since arrow function single-expression bodies have no closing
  // curly bracket, we do not place a breakpoint at their end position.
  if (!funbox_->hasExprBody()) {
    if (!bce_->markSimpleBreakpoint()) {
      return false;
    }
  }

  // Emit JSOp::RetRval except for sync arrow function with expression body
  // which always ends with JSOp::Return. Other parts of the codebase depend
  // on these opcodes being the last opcode.
  // See JSScript::lastPC and BaselineCompiler::emitBody.
  if (!funbox_->hasExprBody() || funbox_->isAsync()) {
    if (!bce_->emitReturnRval()) {
      //            [stack]
      return false;
    }
  }

  if (namedLambdaEmitterScope_) {
    if (!namedLambdaEmitterScope_->leave(bce_)) {
      return false;
    }
    namedLambdaEmitterScope_.reset();
  }

#ifdef DEBUG
  state_ = State::EndBody;
#endif
  return true;
}

bool FunctionScriptEmitter::intoStencil() {
  MOZ_ASSERT(state_ == State::EndBody);

  if (!bce_->intoScriptStencil(funbox_->index())) {
    return false;
  }

#ifdef DEBUG
  state_ = State::End;
#endif

  return true;
}

FunctionParamsEmitter::FunctionParamsEmitter(BytecodeEmitter* bce,
                                             FunctionBox* funbox)
    : bce_(bce),
      funbox_(funbox),
      functionEmitterScope_(bce_->innermostEmitterScope()) {}

bool FunctionParamsEmitter::emitSimple(TaggedParserAtomIndex paramName) {
  MOZ_ASSERT(state_ == State::Start);

  //                [stack]

  if (funbox_->hasParameterExprs) {
    if (!bce_->emitArgOp(JSOp::GetArg, argSlot_)) {
      //            [stack] ARG
      return false;
    }

    if (!emitAssignment(paramName)) {
      //            [stack]
      return false;
    }
  }

  argSlot_++;
  return true;
}

bool FunctionParamsEmitter::prepareForDefault() {
  MOZ_ASSERT(state_ == State::Start);

  //                [stack]

  if (!prepareForInitializer()) {
    //              [stack]
    return false;
  }

#ifdef DEBUG
  state_ = State::Default;
#endif
  return true;
}

bool FunctionParamsEmitter::emitDefaultEnd(TaggedParserAtomIndex paramName) {
  MOZ_ASSERT(state_ == State::Default);

  //                [stack] DEFAULT

  if (!emitInitializerEnd()) {
    //              [stack] ARG/DEFAULT
    return false;
  }
  if (!emitAssignment(paramName)) {
    //              [stack]
    return false;
  }

  argSlot_++;

#ifdef DEBUG
  state_ = State::Start;
#endif
  return true;
}

bool FunctionParamsEmitter::prepareForDestructuring() {
  MOZ_ASSERT(state_ == State::Start);

  //                [stack]

  if (!bce_->emitArgOp(JSOp::GetArg, argSlot_)) {
    //              [stack] ARG
    return false;
  }

#ifdef DEBUG
  state_ = State::Destructuring;
#endif
  return true;
}

bool FunctionParamsEmitter::emitDestructuringEnd() {
  MOZ_ASSERT(state_ == State::Destructuring);

  //                [stack] ARG

  if (!bce_->emit1(JSOp::Pop)) {
    //              [stack]
    return false;
  }

  argSlot_++;

#ifdef DEBUG
  state_ = State::Start;
#endif
  return true;
}

bool FunctionParamsEmitter::prepareForDestructuringDefaultInitializer() {
  MOZ_ASSERT(state_ == State::Start);

  //                [stack]

  if (!prepareForInitializer()) {
    //              [stack]
    return false;
  }

#ifdef DEBUG
  state_ = State::DestructuringDefaultInitializer;
#endif
  return true;
}

bool FunctionParamsEmitter::prepareForDestructuringDefault() {
  MOZ_ASSERT(state_ == State::DestructuringDefaultInitializer);

  //                [stack] DEFAULT

  if (!emitInitializerEnd()) {
    //              [stack] ARG/DEFAULT
    return false;
  }

#ifdef DEBUG
  state_ = State::DestructuringDefault;
#endif
  return true;
}

bool FunctionParamsEmitter::emitDestructuringDefaultEnd() {
  MOZ_ASSERT(state_ == State::DestructuringDefault);

  //                [stack] ARG/DEFAULT

  if (!bce_->emit1(JSOp::Pop)) {
    //              [stack]
    return false;
  }

  argSlot_++;

#ifdef DEBUG
  state_ = State::Start;
#endif
  return true;
}

bool FunctionParamsEmitter::emitRest(TaggedParserAtomIndex paramName) {
  MOZ_ASSERT(state_ == State::Start);

  //                [stack]

  if (!emitRestArray()) {
    //              [stack] REST
    return false;
  }
  if (!emitAssignment(paramName)) {
    //              [stack]
    return false;
  }

#ifdef DEBUG
  state_ = State::End;
#endif
  return true;
}

bool FunctionParamsEmitter::prepareForDestructuringRest() {
  MOZ_ASSERT(state_ == State::Start);

  //                [stack]

  if (!emitRestArray()) {
    //              [stack] REST
    return false;
  }

#ifdef DEBUG
  state_ = State::DestructuringRest;
#endif
  return true;
}

bool FunctionParamsEmitter::emitDestructuringRestEnd() {
  MOZ_ASSERT(state_ == State::DestructuringRest);

  //                [stack] REST

  if (!bce_->emit1(JSOp::Pop)) {
    //              [stack]
    return false;
  }

#ifdef DEBUG
  state_ = State::End;
#endif
  return true;
}

bool FunctionParamsEmitter::prepareForInitializer() {
  //                [stack]

  // If we have an initializer, emit the initializer and assign it
  // to the argument slot. TDZ is taken care of afterwards.
  MOZ_ASSERT(funbox_->hasParameterExprs);
  if (!bce_->emitArgOp(JSOp::GetArg, argSlot_)) {
    //              [stack] ARG
    return false;
  }
  default_.emplace(bce_);
  if (!default_->prepareForDefault()) {
    //              [stack]
    return false;
  }
  return true;
}

bool FunctionParamsEmitter::emitInitializerEnd() {
  //                [stack] DEFAULT

  if (!default_->emitEnd()) {
    //              [stack] ARG/DEFAULT
    return false;
  }
  default_.reset();
  return true;
}

bool FunctionParamsEmitter::emitRestArray() {
  //                [stack]

  if (!bce_->emit1(JSOp::Rest)) {
    //              [stack] REST
    return false;
  }
  return true;
}

bool FunctionParamsEmitter::emitAssignment(TaggedParserAtomIndex paramName) {
  //                [stack] ARG

  NameLocation paramLoc =
      *bce_->locationOfNameBoundInScope(paramName, functionEmitterScope_);

  // RHS is already pushed in the caller side.
  // Make sure prepareForRhs doesn't touch stack.
  MOZ_ASSERT(paramLoc.kind() == NameLocation::Kind::ArgumentSlot ||
             paramLoc.kind() == NameLocation::Kind::FrameSlot ||
             paramLoc.kind() == NameLocation::Kind::EnvironmentCoordinate);

  NameOpEmitter noe(bce_, paramName, paramLoc, NameOpEmitter::Kind::Initialize);
  if (!noe.prepareForRhs()) {
    //              [stack] ARG
    return false;
  }

  if (!noe.emitAssignment()) {
    //              [stack] ARG
    return false;
  }

  if (!bce_->emit1(JSOp::Pop)) {
    //              [stack]
    return false;
  }

  return true;
}
