/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/OptionalEmitter.h"

#include "frontend/BytecodeEmitter.h"
#include "frontend/IfEmitter.h"  // IfEmitter, InternalIfEmitter, CondEmitter
#include "frontend/SharedContext.h"
#include "vm/Opcodes.h"
#include "vm/StringType.h"

using namespace js;
using namespace js::frontend;

OptionalEmitter::OptionalEmitter(BytecodeEmitter* bce, int32_t initialDepth)
    : bce_(bce), tdzCache_(bce), initialDepth_(initialDepth) {}

bool OptionalEmitter::emitJumpShortCircuit() {
  MOZ_ASSERT(state_ == State::Start || state_ == State::ShortCircuit ||
             state_ == State::ShortCircuitForCall);
  MOZ_ASSERT(initialDepth_ + 1 == bce_->bytecodeSection().stackDepth());
  InternalIfEmitter ifEmitter(bce_);
  if (!bce_->emitPushNotUndefinedOrNull()) {
    //              [stack] OBJ NOT-UNDEFINED-OR-NULL
    return false;
  }

  if (!bce_->emit1(JSOp::Not)) {
    //              [stack] OBJ UNDEFINED-OR-NULL
    return false;
  }

  if (!ifEmitter.emitThen()) {
    return false;
  }

  if (!bce_->emitJump(JSOp::Goto, &jumpShortCircuit_)) {
    //              [stack] UNDEFINED-OR-NULL
    return false;
  }

  if (!ifEmitter.emitEnd()) {
    return false;
  }
#ifdef DEBUG
  state_ = State::ShortCircuit;
#endif
  return true;
}

bool OptionalEmitter::emitJumpShortCircuitForCall() {
  MOZ_ASSERT(state_ == State::Start || state_ == State::ShortCircuit ||
             state_ == State::ShortCircuitForCall);
  int32_t depth = bce_->bytecodeSection().stackDepth();
  MOZ_ASSERT(initialDepth_ + 2 == depth);
  if (!bce_->emit1(JSOp::Swap)) {
    //              [stack] THIS CALLEE
    return false;
  }

  InternalIfEmitter ifEmitter(bce_);
  if (!bce_->emitPushNotUndefinedOrNull()) {
    //              [stack] THIS CALLEE NOT-UNDEFINED-OR-NULL
    return false;
  }

  if (!bce_->emit1(JSOp::Not)) {
    //              [stack] THIS CALLEE UNDEFINED-OR-NULL
    return false;
  }

  if (!ifEmitter.emitThen()) {
    return false;
  }

  if (!bce_->emit1(JSOp::Pop)) {
    //              [stack] THIS
    return false;
  }

  if (!bce_->emitJump(JSOp::Goto, &jumpShortCircuit_)) {
    //              [stack] UNDEFINED-OR-NULL
    return false;
  }

  if (!ifEmitter.emitEnd()) {
    return false;
  }

  bce_->bytecodeSection().setStackDepth(depth);

  if (!bce_->emit1(JSOp::Swap)) {
    //              [stack] THIS CALLEE
    return false;
  }
#ifdef DEBUG
  state_ = State::ShortCircuitForCall;
#endif
  return true;
}

bool OptionalEmitter::emitOptionalJumpTarget(JSOp op,
                                             Kind kind /* = Kind::Other */) {
#ifdef DEBUG
  int32_t depth = bce_->bytecodeSection().stackDepth();
#endif
  MOZ_ASSERT(state_ == State::ShortCircuit ||
             state_ == State::ShortCircuitForCall);

  // if we get to this point, it means that the optional chain did not short
  // circuit, so we should skip the short circuiting bytecode.
  if (!bce_->emitJump(JSOp::Goto, &jumpFinish_)) {
    //              [stack] # if call
    //              [stack] CALLEE THIS
    //              [stack] # otherwise, if defined
    //              [stack] VAL
    //              [stack] # otherwise
    //              [stack] UNDEFINED-OR-NULL
    return false;
  }

  if (!bce_->emitJumpTargetAndPatch(jumpShortCircuit_)) {
    //              [stack] UNDEFINED-OR-NULL
    return false;
  }

  // reset stack depth to the depth when we jumped
  bce_->bytecodeSection().setStackDepth(initialDepth_ + 1);

  if (!bce_->emit1(JSOp::Pop)) {
    //              [stack]
    return false;
  }

  if (!bce_->emit1(op)) {
    //              [stack] JSOP
    return false;
  }

  if (kind == Kind::Reference) {
    if (!bce_->emit1(op)) {
      //            [stack] JSOP JSOP
      return false;
    }
  }

  MOZ_ASSERT(depth == bce_->bytecodeSection().stackDepth());

  if (!bce_->emitJumpTargetAndPatch(jumpFinish_)) {
    //              [stack] # if call
    //              [stack] CALLEE THIS
    //              [stack] # otherwise
    //              [stack] VAL
    return false;
  }
#ifdef DEBUG
  state_ = State::JumpEnd;
#endif
  return true;
}
