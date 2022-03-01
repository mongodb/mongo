/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/TryEmitter.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include "frontend/BytecodeEmitter.h"  // BytecodeEmitter
#include "frontend/SharedContext.h"    // StatementKind
#include "vm/JSScript.h"               // JSTRY_CATCH, JSTRY_FINALLY
#include "vm/Opcodes.h"                // JSOp

using namespace js;
using namespace js::frontend;

using mozilla::Maybe;

TryEmitter::TryEmitter(BytecodeEmitter* bce, Kind kind, ControlKind controlKind)
    : bce_(bce),
      kind_(kind),
      controlKind_(controlKind),
      depth_(0),
      tryOpOffset_(0)
#ifdef DEBUG
      ,
      state_(State::Start)
#endif
{
  if (controlKind_ == ControlKind::Syntactic) {
    controlInfo_.emplace(
        bce_, hasFinally() ? StatementKind::Finally : StatementKind::Try);
  }
}

bool TryEmitter::emitTry() {
  MOZ_ASSERT(state_ == State::Start);

  // Since an exception can be thrown at any place inside the try block,
  // we need to restore the stack and the scope chain before we transfer
  // the control to the exception handler.
  //
  // For that we store in a try note associated with the catch or
  // finally block the stack depth upon the try entry. The interpreter
  // uses this depth to properly unwind the stack and the scope chain.
  depth_ = bce_->bytecodeSection().stackDepth();

  tryOpOffset_ = bce_->bytecodeSection().offset();
  if (!bce_->emit1(JSOp::Try)) {
    return false;
  }

#ifdef DEBUG
  state_ = State::Try;
#endif
  return true;
}

bool TryEmitter::emitTryEnd() {
  MOZ_ASSERT(state_ == State::Try);
  MOZ_ASSERT(depth_ == bce_->bytecodeSection().stackDepth());

  // Gosub to finally, if present.
  if (hasFinally() && controlInfo_) {
    if (!bce_->emitGoSub(&controlInfo_->gosubs)) {
      return false;
    }
  }

  // Emit jump over catch and/or finally.
  if (!bce_->emitJump(JSOp::Goto, &catchAndFinallyJump_)) {
    return false;
  }

  if (!bce_->emitJumpTarget(&tryEnd_)) {
    return false;
  }

  return true;
}

bool TryEmitter::emitCatch() {
  MOZ_ASSERT(state_ == State::Try);
  if (!emitTryEnd()) {
    return false;
  }

  MOZ_ASSERT(bce_->bytecodeSection().stackDepth() == depth_);

  if (controlKind_ == ControlKind::Syntactic) {
    // Clear the frame's return value that might have been set by the
    // try block:
    //
    //   eval("try { 1; throw 2 } catch(e) {}"); // undefined, not 1
    if (!bce_->emit1(JSOp::Undefined)) {
      return false;
    }
    if (!bce_->emit1(JSOp::SetRval)) {
      return false;
    }
  }

  if (!bce_->emit1(JSOp::Exception)) {
    return false;
  }

#ifdef DEBUG
  state_ = State::Catch;
#endif
  return true;
}

bool TryEmitter::emitCatchEnd() {
  MOZ_ASSERT(state_ == State::Catch);

  if (!controlInfo_) {
    return true;
  }

  // gosub <finally>, if required.
  if (hasFinally()) {
    if (!bce_->emitGoSub(&controlInfo_->gosubs)) {
      return false;
    }
    MOZ_ASSERT(bce_->bytecodeSection().stackDepth() == depth_);

    // Jump over the finally block.
    if (!bce_->emitJump(JSOp::Goto, &catchAndFinallyJump_)) {
      return false;
    }
  }

  return true;
}

bool TryEmitter::emitFinally(
    const Maybe<uint32_t>& finallyPos /* = Nothing() */) {
  // If we are using controlInfo_ (i.e., emitting a syntactic try
  // blocks), we must have specified up front if there will be a finally
  // close. For internal non-syntactic try blocks, like those emitted for
  // yield* and IteratorClose inside for-of loops, we can emitFinally even
  // without specifying up front, since the internal non-syntactic try
  // blocks emit no GOSUBs.
  if (!controlInfo_) {
    if (kind_ == Kind::TryCatch) {
      kind_ = Kind::TryCatchFinally;
    }
  } else {
    MOZ_ASSERT(hasFinally());
  }

  if (!hasCatch()) {
    MOZ_ASSERT(state_ == State::Try);
    if (!emitTryEnd()) {
      return false;
    }
  } else {
    MOZ_ASSERT(state_ == State::Catch);
    if (!emitCatchEnd()) {
      return false;
    }
  }

  MOZ_ASSERT(bce_->bytecodeSection().stackDepth() == depth_);

  if (!bce_->emitJumpTarget(&finallyStart_)) {
    return false;
  }

  if (controlInfo_) {
    // Fix up the gosubs that might have been emitted before non-local
    // jumps to the finally code.
    bce_->patchJumpsToTarget(controlInfo_->gosubs, finallyStart_);

    // Indicate that we're emitting a subroutine body.
    controlInfo_->setEmittingSubroutine();
  }
  if (finallyPos) {
    if (!bce_->updateSourceCoordNotes(finallyPos.value())) {
      return false;
    }
  }
  if (!bce_->emit1(JSOp::Finally)) {
    return false;
  }

  if (controlKind_ == ControlKind::Syntactic) {
    if (!bce_->emit1(JSOp::GetRval)) {
      return false;
    }

    // Clear the frame's return value to make break/continue return
    // correct value even if there's no other statement before them:
    //
    //   eval("x: try { 1 } finally { break x; }"); // undefined, not 1
    if (!bce_->emit1(JSOp::Undefined)) {
      return false;
    }
    if (!bce_->emit1(JSOp::SetRval)) {
      return false;
    }
  }

#ifdef DEBUG
  state_ = State::Finally;
#endif
  return true;
}

bool TryEmitter::emitFinallyEnd() {
  MOZ_ASSERT(state_ == State::Finally);

  if (controlKind_ == ControlKind::Syntactic) {
    if (!bce_->emit1(JSOp::SetRval)) {
      return false;
    }
  }

  if (!bce_->emit1(JSOp::Retsub)) {
    return false;
  }

  bce_->hasTryFinally = true;
  return true;
}

bool TryEmitter::emitEnd() {
  if (!hasFinally()) {
    MOZ_ASSERT(state_ == State::Catch);
    if (!emitCatchEnd()) {
      return false;
    }
  } else {
    MOZ_ASSERT(state_ == State::Finally);
    if (!emitFinallyEnd()) {
      return false;
    }
  }

  MOZ_ASSERT(bce_->bytecodeSection().stackDepth() == depth_);

  // Fix up the end-of-try/catch jumps to come here.
  if (!bce_->emitJumpTargetAndPatch(catchAndFinallyJump_)) {
    return false;
  }

  // Add the try note last, to let post-order give us the right ordering
  // (first to last for a given nesting level, inner to outer by level).
  if (hasCatch()) {
    if (!bce_->addTryNote(TryNoteKind::Catch, depth_, offsetAfterTryOp(),
                          tryEnd_.offset)) {
      return false;
    }
  }

  // If we've got a finally, mark try+catch region with additional
  // trynote to catch exceptions (re)thrown from a catch block or
  // for the try{}finally{} case.
  if (hasFinally()) {
    if (!bce_->addTryNote(TryNoteKind::Finally, depth_, offsetAfterTryOp(),
                          finallyStart_.offset)) {
      return false;
    }
  }

#ifdef DEBUG
  state_ = State::End;
#endif
  return true;
}
