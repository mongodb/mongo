/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/AsyncEmitter.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include "frontend/BytecodeEmitter.h"  // BytecodeEmitter
#include "frontend/NameOpEmitter.h"    // NameOpEmitter
#include "frontend/ParserAtom.h"       // TaggedParserAtomIndex
#include "vm/Opcodes.h"                // JSOp

using namespace js;
using namespace js::frontend;

bool AsyncEmitter::prepareForParamsWithExpressionOrDestructuring() {
  MOZ_ASSERT(state_ == State::Start);
#ifdef DEBUG
  state_ = State::Parameters;
#endif

  rejectTryCatch_.emplace(bce_, TryEmitter::Kind::TryCatch,
                          TryEmitter::ControlKind::NonSyntactic);
  return rejectTryCatch_->emitTry();
}

bool AsyncEmitter::prepareForParamsWithoutExpressionOrDestructuring() {
  MOZ_ASSERT(state_ == State::Start);
#ifdef DEBUG
  state_ = State::Parameters;
#endif
  return true;
}

bool AsyncEmitter::emitParamsEpilogue() {
  MOZ_ASSERT(state_ == State::Parameters);

  if (rejectTryCatch_) {
    // If we get here, we need to reset the TryEmitter. Parameters can't reuse
    // the reject try-catch block from the function body, because the body
    // may have pushed an additional var-environment. This messes up scope
    // resolution for the |.generator| variable, because we'd need different
    // hops to reach |.generator| depending on whether the error was thrown
    // from the parameters or the function body.
    if (!emitRejectCatch()) {
      return false;
    }
  }

#ifdef DEBUG
  state_ = State::PostParams;
#endif
  return true;
}

bool AsyncEmitter::prepareForModule() {
  // Unlike functions, modules do not have params that we need to worry about.
  // Instead, this code is for setting up the required generator that will be
  // used for top level await. Before we can start using top-level await in
  // modules, we need to emit a
  // |.generator| which we can use to pause and resume execution.
  MOZ_ASSERT(state_ == State::Start);
  MOZ_ASSERT(
      bce_->lookupName(TaggedParserAtomIndex::WellKnown::dot_generator_())
          .hasKnownSlot());

  NameOpEmitter noe(bce_, TaggedParserAtomIndex::WellKnown::dot_generator_(),
                    NameOpEmitter::Kind::Initialize);
  if (!noe.prepareForRhs()) {
    //              [stack]
    return false;
  }
  if (!bce_->emit1(JSOp::Generator)) {
    //              [stack] GEN
    return false;
  }
  if (!noe.emitAssignment()) {
    //              [stack] GEN
    return false;
  }
  if (!bce_->emit1(JSOp::Pop)) {
    //              [stack]
    return false;
  }

#ifdef DEBUG
  state_ = State::ModulePrologue;
#endif

  return true;
}

bool AsyncEmitter::prepareForBody() {
  MOZ_ASSERT(state_ == State::PostParams || state_ == State::ModulePrologue);

  rejectTryCatch_.emplace(bce_, TryEmitter::Kind::TryCatch,
                          TryEmitter::ControlKind::NonSyntactic);
#ifdef DEBUG
  state_ = State::Body;
#endif
  return rejectTryCatch_->emitTry();
}

bool AsyncEmitter::emitEndFunction() {
#ifdef DEBUG
  MOZ_ASSERT(state_ == State::Body);
#endif

  // The final yield has already been emitted
  // by FunctionScriptEmitter::emitEndBody().

  if (!emitRejectCatch()) {
    return false;
  }

#ifdef DEBUG
  state_ = State::End;
#endif
  return true;
}

bool AsyncEmitter::emitEndModule() {
#ifdef DEBUG
  MOZ_ASSERT(state_ == State::Body);
#endif

  if (!emitFinalYield()) {
    return false;
  }

  if (!emitRejectCatch()) {
    return false;
  }

#ifdef DEBUG
  state_ = State::End;
#endif
  return true;
}

bool AsyncEmitter::emitFinalYield() {
  if (!bce_->emit1(JSOp::Undefined)) {
    //              [stack] UNDEF
    return false;
  }

  if (!bce_->emitGetDotGeneratorInInnermostScope()) {
    //              [stack] UNDEF GEN
    return false;
  }

  if (!bce_->emit1(JSOp::AsyncResolve)) {
    //              [stack] PROMISE
    return false;
  }

  if (!bce_->emit1(JSOp::SetRval)) {
    //              [stack]
    return false;
  }

  if (!bce_->emitGetDotGeneratorInInnermostScope()) {
    //              [stack] GEN
    return false;
  }

  if (!bce_->emitYieldOp(JSOp::FinalYieldRval)) {
    //              [stack]
    return false;
  }

  return true;
}

bool AsyncEmitter::emitRejectCatch() {
  if (!rejectTryCatch_->emitCatch(TryEmitter::ExceptionStack::Yes)) {
    //              [stack] EXC STACK
    return false;
  }

  if (!bce_->emitGetDotGeneratorInInnermostScope()) {
    //              [stack] EXC STACK GEN
    return false;
  }

  if (!bce_->emit1(JSOp::AsyncReject)) {
    //              [stack] PROMISE
    return false;
  }

  if (!bce_->emit1(JSOp::SetRval)) {
    //              [stack]
    return false;
  }

  if (!bce_->emitGetDotGeneratorInInnermostScope()) {
    //              [stack] GEN
    return false;
  }

  if (!bce_->emitYieldOp(JSOp::FinalYieldRval)) {
    //              [stack]
    return false;
  }

  if (!rejectTryCatch_->emitEnd()) {
    return false;
  }

  rejectTryCatch_.reset();
  return true;
}
