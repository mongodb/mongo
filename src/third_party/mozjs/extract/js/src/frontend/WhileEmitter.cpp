/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/WhileEmitter.h"

#include "frontend/BytecodeEmitter.h"
#include "vm/Opcodes.h"
#include "vm/StencilEnums.h"  // TryNoteKind

using namespace js;
using namespace js::frontend;

WhileEmitter::WhileEmitter(BytecodeEmitter* bce) : bce_(bce) {}

bool WhileEmitter::emitCond(uint32_t whilePos, uint32_t condPos,
                            uint32_t endPos) {
  MOZ_ASSERT(state_ == State::Start);

  // If we have a single-line while, like "while (x) ;", we want to emit the
  // line note before the loop, so that the debugger sees a single entry point.
  // This way, if there is a breakpoint on the line, it will only fire once; and
  // "next"ing will skip the whole loop. However, for the multi-line case we
  // want to emit the line note for the JSOp::LoopHead, so that "cont" stops on
  // each iteration -- but without a stop before the first iteration.
  if (bce_->errorReporter().lineAt(whilePos) ==
      bce_->errorReporter().lineAt(endPos)) {
    if (!bce_->updateSourceCoordNotes(whilePos)) {
      return false;
    }
    // Emit a Nop to ensure the source position is not part of the loop.
    if (!bce_->emit1(JSOp::Nop)) {
      return false;
    }
  }

  loopInfo_.emplace(bce_, StatementKind::WhileLoop);

  if (!loopInfo_->emitLoopHead(bce_, mozilla::Some(condPos))) {
    return false;
  }

#ifdef DEBUG
  state_ = State::Cond;
#endif
  return true;
}

bool WhileEmitter::emitBody() {
  MOZ_ASSERT(state_ == State::Cond);

  if (!bce_->emitJump(JSOp::JumpIfFalse, &loopInfo_->breaks)) {
    return false;
  }

  tdzCacheForBody_.emplace(bce_);

#ifdef DEBUG
  state_ = State::Body;
#endif
  return true;
}

bool WhileEmitter::emitEnd() {
  MOZ_ASSERT(state_ == State::Body);

  tdzCacheForBody_.reset();

  if (!loopInfo_->emitContinueTarget(bce_)) {
    return false;
  }

  if (!loopInfo_->emitLoopEnd(bce_, JSOp::Goto, TryNoteKind::Loop)) {
    return false;
  }

  loopInfo_.reset();

#ifdef DEBUG
  state_ = State::End;
#endif
  return true;
}
