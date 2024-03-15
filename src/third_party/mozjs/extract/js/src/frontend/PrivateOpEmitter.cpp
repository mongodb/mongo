/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/PrivateOpEmitter.h"

#include "frontend/BytecodeEmitter.h"
#include "frontend/NameOpEmitter.h"
#include "vm/Opcodes.h"
#include "vm/ThrowMsgKind.h"  // ThrowMsgKind

using namespace js;
using namespace js::frontend;

PrivateOpEmitter::PrivateOpEmitter(BytecodeEmitter* bce, Kind kind,
                                   TaggedParserAtomIndex name)
    : bce_(bce), kind_(kind), name_(name) {
  MOZ_ASSERT(kind_ != Kind::Delete);
}

bool PrivateOpEmitter::init() {
  // Static analysis needs us to initialise this to something, so use Dynamic()
  NameLocation loc = NameLocation::Dynamic();
  bce_->lookupPrivate(name_, loc, brandLoc_);
  loc_ = mozilla::Some(loc);
  return true;
}

bool PrivateOpEmitter::emitLoad(TaggedParserAtomIndex name,
                                const NameLocation& loc) {
  NameOpEmitter noe(bce_, name, loc, NameOpEmitter::Kind::Get);
  return noe.emitGet();
}

bool PrivateOpEmitter::emitLoadPrivateBrand() {
  return emitLoad(TaggedParserAtomIndex::WellKnown::dotPrivateBrand(),
                  *brandLoc_);
}

bool PrivateOpEmitter::emitBrandCheck() {
  MOZ_ASSERT(state_ == State::Reference);

  if (isBrandCheck()) {
    // Emit a CheckPrivateField CheckRhs; note: The message is irrelvant here,
    // it will never be thrown, so DoubleInit was chosen arbitrarily.
    if (!bce_->emitCheckPrivateField(ThrowCondition::OnlyCheckRhs,
                                     ThrowMsgKind::PrivateDoubleInit)) {
      //            [stack] OBJ KEY BBOOL
      return false;
    }

    return true;
  }

  //                [stack] OBJ KEY
  if (isFieldInit()) {
    if (!bce_->emitCheckPrivateField(ThrowCondition::ThrowHas,
                                     ThrowMsgKind::PrivateDoubleInit)) {
      //            [stack] OBJ KEY false
      return false;
    }
  } else {
    bool assigning =
        isSimpleAssignment() || isCompoundAssignment() || isIncDec();
    if (!bce_->emitCheckPrivateField(ThrowCondition::ThrowHasNot,
                                     assigning
                                         ? ThrowMsgKind::MissingPrivateOnSet
                                         : ThrowMsgKind::MissingPrivateOnGet)) {
      //            [stack] OBJ KEY true
      return false;
    }
  }

  return true;
}

bool PrivateOpEmitter::emitReference() {
  MOZ_ASSERT(state_ == State::Start);

  if (!init()) {
    return false;
  }

  if (brandLoc_) {
    if (!emitLoadPrivateBrand()) {
      //            [stack] OBJ BRAND
      return false;
    }
  } else {
    if (!emitLoad(name_, loc_.ref())) {
      //            [stack] OBJ NAME
      return false;
    }
  }
#ifdef DEBUG
  state_ = State::Reference;
#endif
  return true;
}

bool PrivateOpEmitter::skipReference() {
  MOZ_ASSERT(state_ == State::Start);

  if (!init()) {
    return false;
  }

#ifdef DEBUG
  state_ = State::Reference;
#endif
  return true;
}

bool PrivateOpEmitter::emitGet() {
  MOZ_ASSERT(state_ == State::Reference);

  //                [stack] OBJ NAME

  if (brandLoc_) {
    // Note that the decision of what we leave on the stack depends on kind_,
    // not loc_->bindingKind().  We can't emit code for a call just because this
    // private member is a method. `obj.#method` is allowed without a call,
    // just fetching the function object (it's useful in code like
    // `obj.#method.bind(...)`). Even if the user says `obj.#method += 7`, we
    // emit honest bytecode for the brand check, method load, and addition, and
    // throw the error later. This preserves stack nuses/ndefs balance.
    if (!emitBrandCheck()) {
      //            [stack] OBJ BRAND true
      return false;
    }

    if (isCompoundAssignment()) {
      if (!bce_->emit1(JSOp::Pop)) {
        //          [stack] OBJ BRAND
        return false;
      }
    } else if (isCall()) {
      if (!bce_->emitPopN(2)) {
        //          [stack] OBJ
        return false;
      }
    } else {
      if (!bce_->emitPopN(3)) {
        //          [stack]
        return false;
      }
    }

    if (!emitLoad(name_, loc_.ref())) {
      //            [stack] OBJ BRAND METHOD  # if isCompoundAssignment
      //            [stack] OBJ METHOD        # if call
      //            [stack] METHOD            # otherwise
      return false;
    }
  } else {
    if (isCall()) {
      if (!bce_->emitDupAt(1)) {
        //          [stack] OBJ NAME OBJ
        return false;
      }
      if (!bce_->emit1(JSOp::Swap)) {
        //          [stack] OBJ OBJ NAME
        return false;
      }
    }
    //              [stack] OBJ? OBJ NAME
    if (!emitBrandCheck()) {
      //            [stack] OBJ? OBJ NAME true
      return false;
    }
    if (!bce_->emit1(JSOp::Pop)) {
      //            [stack] OBJ? OBJ NAME
      return false;
    }

    if (isCompoundAssignment()) {
      if (!bce_->emit1(JSOp::Dup2)) {
        //          [stack] OBJ NAME OBJ NAME
        return false;
      }
    }

    if (!bce_->emitElemOpBase(JSOp::GetElem)) {
      //            [stack] OBJ NAME VALUE  # if isCompoundAssignment
      //            [stack] OBJ METHOD      # if Call
      //            [stack] VALUE           # otherwise
      return false;
    }
  }

  if (isCall()) {
    if (!bce_->emit1(JSOp::Swap)) {
      //            [stack] METHOD OBJ
      return false;
    }
  }

  //                [stack] OBJ NAME VALUE  # if isCompoundAssignment
  //                [stack] METHOD OBJ      # if call
  //                [stack] VALUE           # otherwise

#ifdef DEBUG
  state_ = State::Get;
#endif
  return true;
}

bool PrivateOpEmitter::emitGetForCallOrNew() { return emitGet(); }

bool PrivateOpEmitter::emitAssignment() {
  MOZ_ASSERT(isSimpleAssignment() || isFieldInit() || isCompoundAssignment());
  MOZ_ASSERT_IF(!isCompoundAssignment(), state_ == State::Reference);
  MOZ_ASSERT_IF(isCompoundAssignment(), state_ == State::Get);

  //                [stack] OBJ KEY RHS

  if (brandLoc_) {
    if (!bce_->emit2(JSOp::ThrowMsg,
                     uint8_t(ThrowMsgKind::AssignToPrivateMethod))) {
      return false;
    }

    // Balance the expression stack.
    if (!bce_->emitPopN(2)) {
      //            [stack] OBJ
      return false;
    }
  } else {
    // Emit a brand check. If this is compound assignment, emitGet() already
    // emitted a check for this object and key. There's no point checking
    // again--a private field can't be removed from an object.
    if (!isCompoundAssignment()) {
      if (!bce_->emitUnpickN(2)) {
        //          [stack] RHS OBJ KEY
        return false;
      }
      if (!emitBrandCheck()) {
        //          [stack] RHS OBJ KEY BOOL
        return false;
      }
      if (!bce_->emit1(JSOp::Pop)) {
        //          [stack] RHS OBJ KEY
        return false;
      }
      if (!bce_->emitPickN(2)) {
        //          [stack] OBJ KEY RHS
        return false;
      }
    }

    JSOp setOp = isFieldInit() ? JSOp::InitElem : JSOp::StrictSetElem;
    if (!bce_->emitElemOpBase(setOp)) {
      //            [stack] RHS
      return false;
    }
  }

#ifdef DEBUG
  state_ = State::Assignment;
#endif
  return true;
}

bool PrivateOpEmitter::emitIncDec(ValueUsage valueUsage) {
  MOZ_ASSERT(state_ == State::Reference);
  MOZ_ASSERT(isIncDec());
  //                [stack] OBJ NAME

  if (!bce_->emitDupAt(1, 2)) {
    //              [stack] OBJ NAME OBJ NAME
    return false;
  }

  if (!emitGet()) {
    //              [stack] OBJ NAME VALUE
    return false;
  }

  MOZ_ASSERT(state_ == State::Get);

  JSOp incOp = isInc() ? JSOp::Inc : JSOp::Dec;

  if (!bce_->emit1(JSOp::ToNumeric)) {
    //              [stack] OBJ NAME N
    return false;
  }
  if (isPostIncDec() && valueUsage == ValueUsage::WantValue) {
    //              [stack] OBJ NAME N
    if (!bce_->emit1(JSOp::Dup)) {
      //            [stack] OBJ NAME N N
      return false;
    }
    if (!bce_->emit2(JSOp::Unpick, 3)) {
      //            [stack] N OBJ NAME N
      return false;
    }
  }
  if (!bce_->emit1(incOp)) {
    //              [stack] N? OBJ NAME N+1
    return false;
  }

  if (brandLoc_) {
    if (!bce_->emit2(JSOp::ThrowMsg,
                     uint8_t(ThrowMsgKind::AssignToPrivateMethod))) {
      return false;
    }

    // Balance the expression stack.
    if (!bce_->emitPopN(2)) {
      //            [stack] N? N+1
      return false;
    }
  } else {
    if (!bce_->emitElemOpBase(JSOp::StrictSetElem)) {
      //            [stack] N? N+1
      return false;
    }
  }

  if (isPostIncDec() && valueUsage == ValueUsage::WantValue) {
    if (!bce_->emit1(JSOp::Pop)) {
      //            [stack] N
      return false;
    }
  }

  return true;
}
