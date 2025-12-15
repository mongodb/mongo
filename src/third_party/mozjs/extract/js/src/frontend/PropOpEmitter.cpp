/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/PropOpEmitter.h"

#include "frontend/BytecodeEmitter.h"
#include "frontend/ParserAtom.h"  // ParserAtom
#include "frontend/SharedContext.h"
#include "vm/Opcodes.h"
#include "vm/ThrowMsgKind.h"  // ThrowMsgKind

using namespace js;
using namespace js::frontend;

PropOpEmitter::PropOpEmitter(BytecodeEmitter* bce, Kind kind, ObjKind objKind)
    : bce_(bce), kind_(kind), objKind_(objKind) {}

bool PropOpEmitter::prepareAtomIndex(TaggedParserAtomIndex prop) {
  return bce_->makeAtomIndex(prop, ParserAtom::Atomize::Yes, &propAtomIndex_);
}

bool PropOpEmitter::prepareForObj() {
  MOZ_ASSERT(state_ == State::Start);

#ifdef DEBUG
  state_ = State::Obj;
#endif
  return true;
}

bool PropOpEmitter::emitGet(TaggedParserAtomIndex prop) {
  MOZ_ASSERT(state_ == State::Obj);

  if (!prepareAtomIndex(prop)) {
    return false;
  }
  if (isCall()) {
    if (!bce_->emit1(JSOp::Dup)) {
      //            [stack] # if Super
      //            [stack] THIS THIS
      //            [stack] # otherwise
      //            [stack] OBJ OBJ
      return false;
    }
  }
  if (isSuper()) {
    if (!bce_->emitSuperBase()) {
      //            [stack] THIS? THIS SUPERBASE
      return false;
    }
  }
  if (isIncDec() || isCompoundAssignment()) {
    if (isSuper()) {
      if (!bce_->emit1(JSOp::Dup2)) {
        //          [stack] THIS SUPERBASE THIS SUPERBASE
        return false;
      }
    } else {
      if (!bce_->emit1(JSOp::Dup)) {
        //          [stack] OBJ OBJ
        return false;
      }
    }
  }

  JSOp op = isSuper() ? JSOp::GetPropSuper : JSOp::GetProp;
  if (!bce_->emitAtomOp(op, propAtomIndex_)) {
    //              [stack] # if Get
    //              [stack] PROP
    //              [stack] # if Call
    //              [stack] THIS PROP
    //              [stack] # if Inc/Dec/Compound, Super]
    //              [stack] THIS SUPERBASE PROP
    //              [stack] # if Inc/Dec/Compound, other
    //              [stack] OBJ PROP
    return false;
  }
  if (isCall()) {
    if (!bce_->emit1(JSOp::Swap)) {
      //            [stack] PROP THIS
      return false;
    }
  }

#ifdef DEBUG
  state_ = State::Get;
#endif
  return true;
}

bool PropOpEmitter::prepareForRhs() {
  MOZ_ASSERT(isSimpleAssignment() || isPropInit() || isCompoundAssignment());
  MOZ_ASSERT_IF(isSimpleAssignment() || isPropInit(), state_ == State::Obj);
  MOZ_ASSERT_IF(isCompoundAssignment(), state_ == State::Get);

  if (isSimpleAssignment() || isPropInit()) {
    // For CompoundAssignment, SuperBase is already emitted by emitGet.
    if (isSuper()) {
      if (!bce_->emitSuperBase()) {
        //          [stack] THIS SUPERBASE
        return false;
      }
    }
  }

#ifdef DEBUG
  state_ = State::Rhs;
#endif
  return true;
}

bool PropOpEmitter::emitDelete(TaggedParserAtomIndex prop) {
  MOZ_ASSERT(state_ == State::Obj);
  MOZ_ASSERT(isDelete());

  if (!prepareAtomIndex(prop)) {
    return false;
  }
  if (isSuper()) {
    if (!bce_->emitSuperBase()) {
      //            [stack] THIS SUPERBASE
      return false;
    }

    // Unconditionally throw when attempting to delete a super-reference.
    if (!bce_->emit2(JSOp::ThrowMsg, uint8_t(ThrowMsgKind::CantDeleteSuper))) {
      //            [stack] THIS SUPERBASE
      return false;
    }

    // Another wrinkle: Balance the stack from the emitter's point of view.
    // Execution will not reach here, as the last bytecode threw.
    if (!bce_->emit1(JSOp::Pop)) {
      //            [stack] THIS
      return false;
    }
  } else {
    JSOp op = bce_->sc->strict() ? JSOp::StrictDelProp : JSOp::DelProp;
    if (!bce_->emitAtomOp(op, propAtomIndex_)) {
      //            [stack] SUCCEEDED
      return false;
    }
  }

#ifdef DEBUG
  state_ = State::Delete;
#endif
  return true;
}

bool PropOpEmitter::emitAssignment(TaggedParserAtomIndex prop) {
  MOZ_ASSERT(isSimpleAssignment() || isPropInit() || isCompoundAssignment());
  MOZ_ASSERT(state_ == State::Rhs);

  if (isSimpleAssignment() || isPropInit()) {
    if (!prepareAtomIndex(prop)) {
      return false;
    }
  }

  MOZ_ASSERT_IF(isPropInit(), !isSuper());
  JSOp setOp = isPropInit() ? JSOp::InitProp
               : isSuper()  ? bce_->sc->strict() ? JSOp::StrictSetPropSuper
                                                 : JSOp::SetPropSuper
               : bce_->sc->strict() ? JSOp::StrictSetProp
                                    : JSOp::SetProp;
  if (!bce_->emitAtomOp(setOp, propAtomIndex_)) {
    //              [stack] VAL
    return false;
  }

#ifdef DEBUG
  state_ = State::Assignment;
#endif
  return true;
}

bool PropOpEmitter::emitIncDec(TaggedParserAtomIndex prop,
                               ValueUsage valueUsage) {
  MOZ_ASSERT(state_ == State::Obj);
  MOZ_ASSERT(isIncDec());

  if (!emitGet(prop)) {
    return false;
  }

  MOZ_ASSERT(state_ == State::Get);

  JSOp incOp = isInc() ? JSOp::Inc : JSOp::Dec;

  if (!bce_->emit1(JSOp::ToNumeric)) {
    //              [stack] ... N
    return false;
  }
  if (isPostIncDec() && valueUsage == ValueUsage::WantValue) {
    //              [stack] OBJ SUPERBASE? N
    if (!bce_->emit1(JSOp::Dup)) {
      //            [stack] .. N N
      return false;
    }
    if (!bce_->emit2(JSOp::Unpick, 2 + isSuper())) {
      //            [stack] N OBJ SUPERBASE? N
      return false;
    }
  }
  if (!bce_->emit1(incOp)) {
    //              [stack] ... N+1
    return false;
  }

  JSOp setOp = isSuper() ? bce_->sc->strict() ? JSOp::StrictSetPropSuper
                                              : JSOp::SetPropSuper
               : bce_->sc->strict() ? JSOp::StrictSetProp
                                    : JSOp::SetProp;
  if (!bce_->emitAtomOp(setOp, propAtomIndex_)) {
    //              [stack] N? N+1
    return false;
  }
  if (isPostIncDec() && valueUsage == ValueUsage::WantValue) {
    if (!bce_->emit1(JSOp::Pop)) {
      //            [stack] N
      return false;
    }
  }

#ifdef DEBUG
  state_ = State::IncDec;
#endif
  return true;
}
