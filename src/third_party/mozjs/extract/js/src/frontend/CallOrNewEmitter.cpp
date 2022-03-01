/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/CallOrNewEmitter.h"

#include "frontend/BytecodeEmitter.h"
#include "frontend/NameOpEmitter.h"
#include "frontend/SharedContext.h"
#include "vm/Opcodes.h"
#include "vm/StringType.h"

using namespace js;
using namespace js::frontend;

using mozilla::Maybe;

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

  NameOpEmitter noe(
      bce_, name,
      isCall() ? NameOpEmitter::Kind::Call : NameOpEmitter::Kind::Get);
  if (!noe.emitGet()) {
    //              [stack] CALLEE THIS?
    return false;
  }

  state_ = State::NameCallee;
  return true;
}

[[nodiscard]] PropOpEmitter& CallOrNewEmitter::prepareForPropCallee(
    bool isSuperProp) {
  MOZ_ASSERT(state_ == State::Start);

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
  xoe_.emplace(
      bce_,
      isCall() ? PrivateOpEmitter::Kind::Call : PrivateOpEmitter::Kind::Get,
      privateName);
  state_ = State::PrivateCallee;
  return *xoe_;
}

bool CallOrNewEmitter::prepareForFunctionCallee() {
  MOZ_ASSERT(state_ == State::Start);

  state_ = State::FunctionCallee;
  return true;
}

bool CallOrNewEmitter::emitSuperCallee() {
  MOZ_ASSERT(state_ == State::Start);

  if (!bce_->emitThisEnvironmentCallee()) {
    //              [stack] CALLEE
    return false;
  }
  if (!bce_->emit1(JSOp::SuperFun)) {
    //              [stack] CALLEE
    return false;
  }
  if (!bce_->emit1(JSOp::IsConstructing)) {
    //              [stack] CALLEE THIS
    return false;
  }

  state_ = State::SuperCallee;
  return true;
}

bool CallOrNewEmitter::prepareForOtherCallee() {
  MOZ_ASSERT(state_ == State::Start);

  state_ = State::OtherCallee;
  return true;
}

bool CallOrNewEmitter::emitThis() {
  MOZ_ASSERT(state_ == State::NameCallee || state_ == State::PropCallee ||
             state_ == State::ElemCallee || state_ == State::PrivateCallee ||
             state_ == State::FunctionCallee || state_ == State::SuperCallee ||
             state_ == State::OtherCallee);

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
        //          [stack] CALLEE THIS
        return false;
      }
    } else {
      if (!bce_->emit1(JSOp::Undefined)) {
        //          [stack] CALLEE THIS
        return false;
      }
    }
  }

  state_ = State::This;
  return true;
}

// Used by BytecodeEmitter::emitPipeline to reuse CallOrNewEmitter instance
// across multiple chained calls.
void CallOrNewEmitter::reset() {
  MOZ_ASSERT(state_ == State::End);
  state_ = State::Start;
}

bool CallOrNewEmitter::prepareForNonSpreadArguments() {
  MOZ_ASSERT(state_ == State::This);
  MOZ_ASSERT(!isSpread());

  state_ = State::Arguments;
  return true;
}

// See the usage in the comment at the top of the class.
bool CallOrNewEmitter::wantSpreadOperand() {
  MOZ_ASSERT(state_ == State::This);
  MOZ_ASSERT(isSpread());

  state_ = State::WantSpreadOperand;
  return isSingleSpread() || isPassthroughRest();
}

bool CallOrNewEmitter::emitSpreadArgumentsTest() {
  // Caller should check wantSpreadOperand before this.
  MOZ_ASSERT(state_ == State::WantSpreadOperand);
  MOZ_ASSERT(isSpread());

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
    if (!bce_->emit1(JSOp::OptimizeSpreadCall)) {
      //            [stack] CALLEE THIS ARG0 OPTIMIZED
      return false;
    }
    if (!ifNotOptimizable_->emitThen(IfEmitter::ConditionKind::Negative)) {
      //            [stack] CALLEE THIS ARG0
      return false;
    }
    if (!bce_->emit1(JSOp::Pop)) {
      //            [stack] CALLEE THIS
      return false;
    }
  }

  state_ = State::SpreadIteration;
  return true;
}

bool CallOrNewEmitter::wantSpreadIteration() {
  MOZ_ASSERT(state_ == State::SpreadIteration);
  MOZ_ASSERT(isSpread());

  state_ = State::Arguments;
  return !isPassthroughRest();
}

bool CallOrNewEmitter::emitEnd(uint32_t argc, const Maybe<uint32_t>& beginPos) {
  MOZ_ASSERT(state_ == State::Arguments);

  if (isSingleSpread()) {
    if (!ifNotOptimizable_->emitEnd()) {
      //            [stack] CALLEE THIS ARR
      return false;
    }

    ifNotOptimizable_.reset();
  }
  if (isNew() || isSuperCall()) {
    if (isSuperCall()) {
      if (!bce_->emit1(JSOp::NewTarget)) {
        //          [stack] CALLEE THIS ARG.. NEW.TARGET
        return false;
      }
    } else {
      // Repush the callee as new.target
      uint32_t effectiveArgc = isSpread() ? 1 : argc;
      if (!bce_->emitDupAt(effectiveArgc + 1)) {
        //          [stack] CALLEE THIS ARR CALLEE
        return false;
      }
    }
  }
  if (beginPos) {
    if (!bce_->updateSourceCoordNotes(*beginPos)) {
      return false;
    }
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

  if (isEval() && beginPos) {
    uint32_t lineNum = bce_->parser->errorReporter().lineAt(*beginPos);
    if (!bce_->emitUint32Operand(JSOp::Lineno, lineNum)) {
      return false;
    }
  }

  state_ = State::End;
  return true;
}
