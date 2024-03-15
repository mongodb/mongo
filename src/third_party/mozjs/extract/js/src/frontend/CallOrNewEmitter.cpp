/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/CallOrNewEmitter.h"

#include "frontend/BytecodeEmitter.h"
#include "frontend/NameOpEmitter.h"
#include "vm/Opcodes.h"

using namespace js;
using namespace js::frontend;

CallOrNewEmitter::CallOrNewEmitter(BytecodeEmitter* bce, JSOp op,
                                   ArgumentsKind argumentsKind,
                                   ValueUsage valueUsage)
    : bce_(bce), op_(op), argumentsKind_(argumentsKind) {
  if (op_ == JSOp::Call && valueUsage == ValueUsage::IgnoreValue) {
    op_ = JSOp::CallIgnoresRv;
  }

  MOZ_ASSERT(isCall() || isNew() || isSuperCall());
}

bool CallOrNewEmitter::emitNameCallee(TaggedParserAtomIndex name) {
  MOZ_ASSERT(state_ == State::Start);

  //                [stack]

  NameOpEmitter noe(
      bce_, name,
      isCall() ? NameOpEmitter::Kind::Call : NameOpEmitter::Kind::Get);
  if (!noe.emitGet()) {
    //              [stack] # if isCall()
    //              [stack] CALLEE THIS
    //              [stack] # if isNew() or isSuperCall()
    //              [stack] CALLEE
    return false;
  }

  state_ = State::NameCallee;
  return true;
}

[[nodiscard]] PropOpEmitter& CallOrNewEmitter::prepareForPropCallee(
    bool isSuperProp) {
  MOZ_ASSERT(state_ == State::Start);
  MOZ_ASSERT(bce_->emitterMode != BytecodeEmitter::SelfHosting);

  //                [stack]

  poe_.emplace(bce_,
               isCall() ? PropOpEmitter::Kind::Call : PropOpEmitter::Kind::Get,
               isSuperProp ? PropOpEmitter::ObjKind::Super
                           : PropOpEmitter::ObjKind::Other);

  state_ = State::PropCallee;
  return *poe_;
}

[[nodiscard]] ElemOpEmitter& CallOrNewEmitter::prepareForElemCallee(
    bool isSuperElem) {
  MOZ_ASSERT(state_ == State::Start);
  MOZ_ASSERT(bce_->emitterMode != BytecodeEmitter::SelfHosting);

  //                [stack]

  eoe_.emplace(bce_,
               isCall() ? ElemOpEmitter::Kind::Call : ElemOpEmitter::Kind::Get,
               isSuperElem ? ElemOpEmitter::ObjKind::Super
                           : ElemOpEmitter::ObjKind::Other);

  state_ = State::ElemCallee;
  return *eoe_;
}

PrivateOpEmitter& CallOrNewEmitter::prepareForPrivateCallee(
    TaggedParserAtomIndex privateName) {
  MOZ_ASSERT(state_ == State::Start);
  MOZ_ASSERT(bce_->emitterMode != BytecodeEmitter::SelfHosting);

  //                [stack]

  xoe_.emplace(
      bce_,
      isCall() ? PrivateOpEmitter::Kind::Call : PrivateOpEmitter::Kind::Get,
      privateName);
  state_ = State::PrivateCallee;
  return *xoe_;
}

bool CallOrNewEmitter::prepareForFunctionCallee() {
  MOZ_ASSERT(state_ == State::Start);
  MOZ_ASSERT(bce_->emitterMode != BytecodeEmitter::SelfHosting);

  //                [stack]

  state_ = State::FunctionCallee;
  return true;
}

bool CallOrNewEmitter::emitSuperCallee() {
  MOZ_ASSERT(state_ == State::Start);
  MOZ_ASSERT(bce_->emitterMode != BytecodeEmitter::SelfHosting);

  //                [stack]

  if (!bce_->emitThisEnvironmentCallee()) {
    //              [stack] CALLEE
    return false;
  }
  if (!bce_->emit1(JSOp::SuperFun)) {
    //              [stack] SUPER_FUN
    return false;
  }
  if (!bce_->emit1(JSOp::IsConstructing)) {
    //              [stack] SUPER_FUN IS_CONSTRUCTING
    return false;
  }

  state_ = State::SuperCallee;
  return true;
}

bool CallOrNewEmitter::prepareForOtherCallee() {
  MOZ_ASSERT(state_ == State::Start);
  MOZ_ASSERT(bce_->emitterMode != BytecodeEmitter::SelfHosting);

  //                [stack]

  state_ = State::OtherCallee;
  return true;
}

bool CallOrNewEmitter::emitThis() {
  MOZ_ASSERT(state_ == State::NameCallee || state_ == State::PropCallee ||
             state_ == State::ElemCallee || state_ == State::PrivateCallee ||
             state_ == State::FunctionCallee || state_ == State::SuperCallee ||
             state_ == State::OtherCallee);

  //                [stack] # if isCall()
  //                [stack] CALLEE THIS?
  //                [stack] # if isNew() or isSuperCall()
  //                [stack] CALLEE

  bool needsThis = false;
  switch (state_) {
    case State::NameCallee:
      if (!isCall()) {
        needsThis = true;
      }
      break;
    case State::PropCallee:
      poe_.reset();
      if (!isCall()) {
        needsThis = true;
      }
      break;
    case State::ElemCallee:
      eoe_.reset();
      if (!isCall()) {
        needsThis = true;
      }
      break;
    case State::PrivateCallee:
      xoe_.reset();
      if (!isCall()) {
        needsThis = true;
      }
      break;
    case State::FunctionCallee:
      needsThis = true;
      break;
    case State::SuperCallee:
      break;
    case State::OtherCallee:
      needsThis = true;
      break;
    default:;
  }
  if (needsThis) {
    if (isNew() || isSuperCall()) {
      if (!bce_->emit1(JSOp::IsConstructing)) {
        //          [stack] CALLEE IS_CONSTRUCTING
        return false;
      }
    } else {
      if (!bce_->emit1(JSOp::Undefined)) {
        //          [stack] CALLEE THIS
        return false;
      }
    }
  }

  //                [stack] CALLEE THIS

  state_ = State::This;
  return true;
}

bool CallOrNewEmitter::prepareForNonSpreadArguments() {
  MOZ_ASSERT(state_ == State::This);
  MOZ_ASSERT(!isSpread());

  //                [stack] CALLEE THIS

  state_ = State::Arguments;
  return true;
}

// See the usage in the comment at the top of the class.
bool CallOrNewEmitter::wantSpreadOperand() {
  MOZ_ASSERT(state_ == State::This);
  MOZ_ASSERT(isSpread());

  //                [stack] CALLEE THIS

  state_ = State::WantSpreadOperand;
  return isSingleSpread() || isPassthroughRest();
}

bool CallOrNewEmitter::prepareForSpreadArguments() {
  MOZ_ASSERT(state_ == State::WantSpreadOperand);
  MOZ_ASSERT(isSpread());
  MOZ_ASSERT(!isSingleSpread() && !isPassthroughRest());

  //                [stack] CALLEE THIS

  state_ = State::Arguments;
  return true;
}

bool CallOrNewEmitter::emitSpreadArgumentsTest() {
  // Caller should check wantSpreadOperand before this.
  MOZ_ASSERT(state_ == State::WantSpreadOperand);
  MOZ_ASSERT(isSpread());
  MOZ_ASSERT(isSingleSpread() || isPassthroughRest());

  //                [stack] CALLEE THIS ARG0

  if (isSingleSpread()) {
    // Emit a preparation code to optimize the spread call:
    //
    //   g(...args);
    //
    // If the spread operand is a packed array, skip the spread
    // operation and pass it directly to spread call operation.
    // See the comment in OptimizeSpreadCall in Interpreter.cpp
    // for the optimizable conditions.
    //              [stack] CALLEE THIS ARG0

    ifNotOptimizable_.emplace(bce_);
    if (!bce_->emit1(JSOp::Dup)) {
      //            [stack] CALLEE THIS ARG0 ARG0
      return false;
    }
    if (!bce_->emit1(JSOp::OptimizeSpreadCall)) {
      //            [stack] CALLEE THIS ARG0 ARRAY_OR_UNDEF
      return false;
    }

    if (!bce_->emit1(JSOp::Dup)) {
      //            [stack] CALLEE THIS ARG0 ARRAY_OR_UNDEF ARRAY_OR_UNDEF
      return false;
    }
    if (!bce_->emit1(JSOp::Undefined)) {
      //            [stack] CALLEE THIS ARG0 ARRAY_OR_UNDEF ARRAY_OR_UNDEF UNDEF
      return false;
    }
    if (!bce_->emit1(JSOp::StrictEq)) {
      //            [stack] CALLEE THIS ARG0 ARRAY_OR_UNDEF EQ
      return false;
    }

    if (!ifNotOptimizable_->emitThenElse()) {
      //            [stack] CALLEE THIS ARG0 ARRAY_OR_UNDEF
      return false;
    }
    if (!bce_->emit1(JSOp::Pop)) {
      //            [stack] CALLEE THIS ARG0
      return false;
    }
  }

  state_ = State::SpreadArgumentsTest;
  return true;
}

bool CallOrNewEmitter::wantSpreadIteration() {
  MOZ_ASSERT(state_ == State::SpreadArgumentsTest);
  MOZ_ASSERT(isSpread());

  state_ = State::SpreadIteration;
  return !isPassthroughRest();
}

bool CallOrNewEmitter::emitSpreadArgumentsTestEnd() {
  MOZ_ASSERT(state_ == State::SpreadIteration);
  MOZ_ASSERT(isSpread());

  if (isSingleSpread()) {
    if (!ifNotOptimizable_->emitElse()) {
      //            [stack] CALLEE THIS ARG0 ARRAY_OR_UNDEF
      return false;
    }
    if (!bce_->emit1(JSOp::Swap)) {
      //            [stack] CALLEE THIS ARRAY_OR_UNDEF ARG0
      return false;
    }
    if (!bce_->emit1(JSOp::Pop)) {
      //            [stack] CALLEE THIS ARRAY_OR_UNDEF
      return false;
    }

    if (!ifNotOptimizable_->emitEnd()) {
      //            [stack] CALLEE THIS ARR
      return false;
    }

    ifNotOptimizable_.reset();
  }

  state_ = State::Arguments;
  return true;
}

bool CallOrNewEmitter::emitEnd(uint32_t argc, uint32_t beginPos) {
  MOZ_ASSERT(state_ == State::Arguments);

  //                [stack] # if isCall()
  //                [stack] CALLEE THIS ARG0 ... ARGN
  //                [stack] # if isNew() or isSuperCall()
  //                [stack] CALLEE IS_CONSTRUCTING ARG0 ... ARGN NEW.TARGET?

  if (!bce_->updateSourceCoordNotes(beginPos)) {
    return false;
  }
  if (!bce_->markSimpleBreakpoint()) {
    return false;
  }
  if (!isSpread()) {
    if (!bce_->emitCall(op_, argc)) {
      //            [stack] RVAL
      return false;
    }
  } else {
    if (!bce_->emit1(op_)) {
      //            [stack] RVAL
      return false;
    }
  }

  if (isEval()) {
    uint32_t lineNum = bce_->errorReporter().lineAt(beginPos);
    if (!bce_->emitUint32Operand(JSOp::Lineno, lineNum)) {
      //            [stack] RVAL
      return false;
    }
  }

  state_ = State::End;
  return true;
}
