/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/DoWhileEmitter.h"

#include "frontend/BytecodeEmitter.h"
#include "vm/Opcodes.h"
#include "vm/StencilEnums.h"  // TryNoteKind

using namespace js;
using namespace js::frontend;

DoWhileEmitter::DoWhileEmitter(BytecodeEmitter* bce) : bce_(bce) {}

bool DoWhileEmitter::emitBody(uint32_t doPos, uint32_t bodyPos) {
  MOZ_ASSERT(state_ == State::Start);

  // Ensure that the column of the 'do' is set properly.
  if (!bce_->updateSourceCoordNotes(doPos)) {
    return false;
  }

  // We need a nop here to make it possible to set a breakpoint on `do`.
  if (!bce_->emit1(JSOp::Nop)) {
    return false;
  }

  loopInfo_.emplace(bce_, StatementKind::DoLoop);

  if (!loopInfo_->emitLoopHead(bce_, mozilla::Some(bodyPos))) {
    return false;
  }

#ifdef DEBUG
  state_ = State::Body;
#endif
  return true;
}

bool DoWhileEmitter::emitCond() {
  MOZ_ASSERT(state_ == State::Body);

  if (!loopInfo_->emitContinueTarget(bce_)) {
    return false;
  }

#ifdef DEBUG
  state_ = State::Cond;
#endif
  return true;
}

bool DoWhileEmitter::emitEnd() {
  MOZ_ASSERT(state_ == State::Cond);

  if (!loopInfo_->emitLoopEnd(bce_, JSOp::JumpIfTrue, TryNoteKind::Loop)) {
    return false;
  }

  loopInfo_.reset();

#ifdef DEBUG
  state_ = State::End;
#endif
  return true;
}
