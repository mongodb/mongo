/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/ElemOpEmitter.h"

#include "frontend/BytecodeEmitter.h"
#include "frontend/SharedContext.h"
#include "vm/Opcodes.h"
#include "vm/ThrowMsgKind.h"  // ThrowMsgKind

using namespace js;
using namespace js::frontend;

ElemOpEmitter::ElemOpEmitter(BytecodeEmitter* bce, Kind kind, ObjKind objKind)
    : bce_(bce), kind_(kind), objKind_(objKind) {}

bool ElemOpEmitter::prepareForObj() {
  MOZ_ASSERT(state_ == State::Start);

#ifdef DEBUG
  state_ = State::Obj;
#endif
  return true;
}

bool ElemOpEmitter::prepareForKey() {
  MOZ_ASSERT(state_ == State::Obj);

  if (isCall()) {
    if (!bce_->emit1(JSOp::Dup)) {
      //            [stack] # if Super
      //            [stack] THIS THIS
      //            [stack] # otherwise
      //            [stack] OBJ OBJ
      return false;
    }
  }

#ifdef DEBUG
  state_ = State::Key;
#endif
  return true;
}

bool ElemOpEmitter::emitGet() {
  MOZ_ASSERT(state_ == State::Key);

  // Inc/dec and compound assignment use the KEY twice, but if it's an object,
  // it must be converted ToPropertyKey only once, per spec.
  if (isIncDec() || isCompoundAssignment()) {
    if (!bce_->emit1(JSOp::ToPropertyKey)) {
      //            [stack] # if Super
      //            [stack] THIS KEY
      //            [stack] # otherwise
      //            [stack] OBJ KEY
      return false;
    }
  }

  if (isSuper()) {
    if (!bce_->emitSuperBase()) {
      //            [stack] THIS? THIS KEY SUPERBASE
      return false;
    }
  }
  if (isIncDec() || isCompoundAssignment()) {
    if (isSuper()) {
      if (!bce_->emitDupAt(2, 3)) {
        //          [stack] THIS KEY SUPERBASE THIS KEY SUPERBASE
        return false;
      }
    } else {
      if (!bce_->emit1(JSOp::Dup2)) {
        //          [stack] OBJ KEY OBJ KEY
        return false;
      }
    }
  }

  JSOp op;
  if (isSuper()) {
    op = JSOp::GetElemSuper;
  } else {
    op = JSOp::GetElem;
  }
  if (!bce_->emitElemOpBase(op)) {
    //              [stack] # if Get
    //              [stack] ELEM
    //              [stack] # if Call
    //              [stack] THIS ELEM
    //              [stack] # if Inc/Dec/Assignment, with Super
    //              [stack] THIS KEY SUPERBASE ELEM
    //              [stack] # if Inc/Dec/Assignment, other
    //              [stack] OBJ KEY ELEM
    return false;
  }
  if (isCall()) {
    if (!bce_->emit1(JSOp::Swap)) {
      //            [stack] ELEM THIS
      return false;
    }
  }

#ifdef DEBUG
  state_ = State::Get;
#endif
  return true;
}

bool ElemOpEmitter::prepareForRhs() {
  MOZ_ASSERT(isSimpleAssignment() || isPropInit() || isCompoundAssignment());
  MOZ_ASSERT_IF(isSimpleAssignment() || isPropInit(), state_ == State::Key);
  MOZ_ASSERT_IF(isCompoundAssignment(), state_ == State::Get);

  if (isSimpleAssignment() || isPropInit()) {
    // For CompoundAssignment, SuperBase is already emitted by emitGet.
    if (isSuper()) {
      if (!bce_->emitSuperBase()) {
        //          [stack] THIS KEY SUPERBASE
        return false;
      }
    }
  }

#ifdef DEBUG
  state_ = State::Rhs;
#endif
  return true;
}

bool ElemOpEmitter::skipObjAndKeyAndRhs() {
  MOZ_ASSERT(state_ == State::Start);
  MOZ_ASSERT(isSimpleAssignment() || isPropInit());

#ifdef DEBUG
  state_ = State::Rhs;
#endif
  return true;
}

bool ElemOpEmitter::emitDelete() {
  MOZ_ASSERT(state_ == State::Key);
  MOZ_ASSERT(isDelete());

  if (isSuper()) {
    if (!bce_->emit1(JSOp::ToPropertyKey)) {
      //            [stack] THIS KEY
      return false;
    }
    if (!bce_->emitSuperBase()) {
      //            [stack] THIS KEY SUPERBASE
      return false;
    }

    // Unconditionally throw when attempting to delete a super-reference.
    if (!bce_->emit2(JSOp::ThrowMsg, uint8_t(ThrowMsgKind::CantDeleteSuper))) {
      //            [stack] THIS KEY SUPERBASE
      return false;
    }

    // Another wrinkle: Balance the stack from the emitter's point of view.
    // Execution will not reach here, as the last bytecode threw.
    if (!bce_->emitPopN(2)) {
      //            [stack] THIS
      return false;
    }
  } else {
    JSOp op = bce_->sc->strict() ? JSOp::StrictDelElem : JSOp::DelElem;
    if (!bce_->emitElemOpBase(op)) {
      // SUCCEEDED
      return false;
    }
  }

#ifdef DEBUG
  state_ = State::Delete;
#endif
  return true;
}

bool ElemOpEmitter::emitAssignment() {
  MOZ_ASSERT(isSimpleAssignment() || isPropInit() || isCompoundAssignment());
  MOZ_ASSERT(state_ == State::Rhs);

  MOZ_ASSERT_IF(isPropInit(), !isSuper());

  JSOp setOp = isPropInit() ? JSOp::InitElem
               : isSuper()  ? bce_->sc->strict() ? JSOp::StrictSetElemSuper
                                                 : JSOp::SetElemSuper
               : bce_->sc->strict() ? JSOp::StrictSetElem
                                    : JSOp::SetElem;
  if (!bce_->emitElemOpBase(setOp)) {
    //              [stack] ELEM
    return false;
  }

#ifdef DEBUG
  state_ = State::Assignment;
#endif
  return true;
}

bool ElemOpEmitter::emitIncDec(ValueUsage valueUsage) {
  MOZ_ASSERT(state_ == State::Key);
  MOZ_ASSERT(isIncDec());

  if (!emitGet()) {
    //              [stack] ... ELEM
    return false;
  }

  MOZ_ASSERT(state_ == State::Get);

  JSOp incOp = isInc() ? JSOp::Inc : JSOp::Dec;
  if (!bce_->emit1(JSOp::ToNumeric)) {
    //              [stack] ... N
    return false;
  }
  if (isPostIncDec() && valueUsage == ValueUsage::WantValue) {
    //              [stack] OBJ KEY SUPERBASE? N
    if (!bce_->emit1(JSOp::Dup)) {
      //            [stack] ... N N
      return false;
    }
    if (!bce_->emit2(JSOp::Unpick, 3 + isSuper())) {
      //            [stack] N OBJ KEY SUPERBASE? N
      return false;
    }
  }
  if (!bce_->emit1(incOp)) {
    //              [stack] ... N+1
    return false;
  }

  JSOp setOp =
      isSuper()
          ? (bce_->sc->strict() ? JSOp::StrictSetElemSuper : JSOp::SetElemSuper)
          : (bce_->sc->strict() ? JSOp::StrictSetElem : JSOp::SetElem);
  if (!bce_->emitElemOpBase(setOp)) {
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
