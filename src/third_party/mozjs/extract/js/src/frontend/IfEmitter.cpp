/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/IfEmitter.h"

#include "frontend/BytecodeEmitter.h"
#include "vm/Opcodes.h"

using namespace js;
using namespace js::frontend;

using mozilla::Maybe;

BranchEmitterBase::BranchEmitterBase(BytecodeEmitter* bce,
                                     LexicalKind lexicalKind)
    : bce_(bce), lexicalKind_(lexicalKind) {}

IfEmitter::IfEmitter(BytecodeEmitter* bce, LexicalKind lexicalKind)
    : BranchEmitterBase(bce, lexicalKind) {}

IfEmitter::IfEmitter(BytecodeEmitter* bce)
    : IfEmitter(bce, LexicalKind::MayContainLexicalAccessInBranch) {}

bool BranchEmitterBase::emitThenInternal(ConditionKind conditionKind) {
  // The end of TDZCheckCache for cond for else-if.
  if (lexicalKind_ == LexicalKind::MayContainLexicalAccessInBranch) {
    tdzCache_.reset();
  }

  // Emit a jump around the then part.
  JSOp op = conditionKind == ConditionKind::Positive ? JSOp::JumpIfFalse
                                                     : JSOp::JumpIfTrue;
  if (!bce_->emitJump(op, &jumpAroundThen_)) {
    return false;
  }

  // To restore stack depth in else part (if present), save depth of the then
  // part.
  thenDepth_ = bce_->bytecodeSection().stackDepth();

  // Enclose then-branch with TDZCheckCache.
  if (lexicalKind_ == LexicalKind::MayContainLexicalAccessInBranch) {
    tdzCache_.emplace(bce_);
  }

  return true;
}

void BranchEmitterBase::calculateOrCheckPushed() {
#ifdef DEBUG
  if (!calculatedPushed_) {
    pushed_ = bce_->bytecodeSection().stackDepth() - thenDepth_;
    calculatedPushed_ = true;
  } else {
    MOZ_ASSERT(pushed_ == bce_->bytecodeSection().stackDepth() - thenDepth_);
  }
#endif
}

bool BranchEmitterBase::emitElseInternal() {
  calculateOrCheckPushed();

  // The end of TDZCheckCache for then-clause.
  if (lexicalKind_ == LexicalKind::MayContainLexicalAccessInBranch) {
    MOZ_ASSERT(tdzCache_.isSome());
    tdzCache_.reset();
  }

  // Emit a jump from the end of our then part around the else part. The
  // patchJumpsToTarget call at the bottom of this function will fix up
  // the offset with jumpsAroundElse value.
  if (!bce_->emitJump(JSOp::Goto, &jumpsAroundElse_)) {
    return false;
  }

  // Ensure the branch-if-false comes here, then emit the else.
  if (!bce_->emitJumpTargetAndPatch(jumpAroundThen_)) {
    return false;
  }

  // Clear jumpAroundThen_ offset, to tell emitEnd there was an else part.
  jumpAroundThen_ = JumpList();

  // Restore stack depth of the then part.
  bce_->bytecodeSection().setStackDepth(thenDepth_);

  // Enclose else-branch with TDZCheckCache.
  if (lexicalKind_ == LexicalKind::MayContainLexicalAccessInBranch) {
    tdzCache_.emplace(bce_);
  }

  return true;
}

bool BranchEmitterBase::emitEndInternal() {
  // The end of TDZCheckCache for then or else-clause.
  if (lexicalKind_ == LexicalKind::MayContainLexicalAccessInBranch) {
    MOZ_ASSERT(tdzCache_.isSome());
    tdzCache_.reset();
  }

  calculateOrCheckPushed();

  if (jumpAroundThen_.offset.valid()) {
    // No else part for the last branch, fixup the branch-if-false to
    // come here.
    if (!bce_->emitJumpTargetAndPatch(jumpAroundThen_)) {
      return false;
    }
  }

  // Patch all the jumps around else parts.
  if (!bce_->emitJumpTargetAndPatch(jumpsAroundElse_)) {
    return false;
  }

  return true;
}

bool IfEmitter::emitIf(const Maybe<uint32_t>& ifPos) {
  MOZ_ASSERT(state_ == State::Start);

  if (ifPos) {
    // Make sure this code is attributed to the "if" so that it gets a
    // useful column number, instead of the default 0 value.
    if (!bce_->updateSourceCoordNotes(*ifPos)) {
      return false;
    }
  }

#ifdef DEBUG
  state_ = State::If;
#endif
  return true;
}

bool IfEmitter::emitThen(
    ConditionKind conditionKind /* = ConditionKind::Positive */) {
  MOZ_ASSERT(state_ == State::If || state_ == State::ElseIf);

  if (lexicalKind_ == LexicalKind::MayContainLexicalAccessInBranch) {
    MOZ_ASSERT_IF(state_ == State::ElseIf, tdzCache_.isSome());
    MOZ_ASSERT_IF(state_ != State::ElseIf, tdzCache_.isNothing());
  }

  if (!emitThenInternal(conditionKind)) {
    return false;
  }

#ifdef DEBUG
  state_ = State::Then;
#endif
  return true;
}

bool IfEmitter::emitThenElse(
    ConditionKind conditionKind /* = ConditionKind::Positive */) {
  MOZ_ASSERT(state_ == State::If || state_ == State::ElseIf);

  if (lexicalKind_ == LexicalKind::MayContainLexicalAccessInBranch) {
    MOZ_ASSERT_IF(state_ == State::ElseIf, tdzCache_.isSome());
    MOZ_ASSERT_IF(state_ != State::ElseIf, tdzCache_.isNothing());
  }

  if (!emitThenInternal(conditionKind)) {
    return false;
  }

#ifdef DEBUG
  state_ = State::ThenElse;
#endif
  return true;
}

bool IfEmitter::emitElseIf(const Maybe<uint32_t>& ifPos) {
  MOZ_ASSERT(state_ == State::ThenElse);

  if (!emitElseInternal()) {
    return false;
  }

  if (ifPos) {
    // Make sure this code is attributed to the "if" so that it gets a
    // useful column number, instead of the default 0 value.
    if (!bce_->updateSourceCoordNotes(*ifPos)) {
      return false;
    }
  }

#ifdef DEBUG
  state_ = State::ElseIf;
#endif
  return true;
}

bool IfEmitter::emitElse() {
  MOZ_ASSERT(state_ == State::ThenElse);

  if (!emitElseInternal()) {
    return false;
  }

#ifdef DEBUG
  state_ = State::Else;
#endif
  return true;
}

bool IfEmitter::emitEnd() {
  MOZ_ASSERT(state_ == State::Then || state_ == State::Else);
  // If there was an else part for the last branch, jumpAroundThen_ is
  // already fixed up when emitting the else part.
  MOZ_ASSERT_IF(state_ == State::Then, jumpAroundThen_.offset.valid());
  MOZ_ASSERT_IF(state_ == State::Else, !jumpAroundThen_.offset.valid());

  if (!emitEndInternal()) {
    return false;
  }

#ifdef DEBUG
  state_ = State::End;
#endif
  return true;
}

InternalIfEmitter::InternalIfEmitter(BytecodeEmitter* bce,
                                     LexicalKind lexicalKind)
    : IfEmitter(bce, lexicalKind) {
#ifdef DEBUG
  // Skip emitIf (see the comment above InternalIfEmitter declaration).
  state_ = State::If;
#endif
}

CondEmitter::CondEmitter(BytecodeEmitter* bce)
    : BranchEmitterBase(bce, LexicalKind::MayContainLexicalAccessInBranch) {}

bool CondEmitter::emitCond() {
  MOZ_ASSERT(state_ == State::Start);
#ifdef DEBUG
  state_ = State::Cond;
#endif
  return true;
}

bool CondEmitter::emitThenElse(
    ConditionKind conditionKind /* = ConditionKind::Positive */) {
  MOZ_ASSERT(state_ == State::Cond);
  if (!emitThenInternal(conditionKind)) {
    return false;
  }

#ifdef DEBUG
  state_ = State::ThenElse;
#endif
  return true;
}

bool CondEmitter::emitElse() {
  MOZ_ASSERT(state_ == State::ThenElse);

  if (!emitElseInternal()) {
    return false;
  }

#ifdef DEBUG
  state_ = State::Else;
#endif
  return true;
}

bool CondEmitter::emitEnd() {
  MOZ_ASSERT(state_ == State::Else);
  MOZ_ASSERT(!jumpAroundThen_.offset.valid());

  if (!emitEndInternal()) {
    return false;
  }

#ifdef DEBUG
  state_ = State::End;
#endif
  return true;
}
