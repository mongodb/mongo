/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/ExpressionStatementEmitter.h"

#include "frontend/BytecodeEmitter.h"
#include "vm/Opcodes.h"

using namespace js;
using namespace js::frontend;

using mozilla::Maybe;

ExpressionStatementEmitter::ExpressionStatementEmitter(BytecodeEmitter* bce,
                                                       ValueUsage valueUsage)
    : bce_(bce), valueUsage_(valueUsage) {}

bool ExpressionStatementEmitter::prepareForExpr(uint32_t beginPos) {
  MOZ_ASSERT(state_ == State::Start);

  if (!bce_->updateSourceCoordNotes(beginPos)) {
    return false;
  }

#ifdef DEBUG
  depth_ = bce_->bytecodeSection().stackDepth();
  state_ = State::Expr;
#endif
  return true;
}

bool ExpressionStatementEmitter::emitEnd() {
  MOZ_ASSERT(state_ == State::Expr);
  MOZ_ASSERT(bce_->bytecodeSection().stackDepth() == depth_ + 1);

  //                [stack] VAL

  JSOp op = valueUsage_ == ValueUsage::WantValue ? JSOp::SetRval : JSOp::Pop;
  if (!bce_->emit1(op)) {
    //              [stack] # if WantValue
    //              [stack] VAL
    //              [stack] # otherwise
    //              [stack]
    return false;
  }

#ifdef DEBUG
  state_ = State::End;
#endif
  return true;
}
