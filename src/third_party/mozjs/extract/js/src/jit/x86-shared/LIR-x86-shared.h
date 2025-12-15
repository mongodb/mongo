/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x86_shared_LIR_x86_shared_h
#define jit_x86_shared_LIR_x86_shared_h

namespace js {
namespace jit {

class LDivOrModConstantI : public LInstructionHelper<1, 1, 1> {
  const int32_t denominator_;

 public:
  LIR_HEADER(DivOrModConstantI)

  LDivOrModConstantI(const LAllocation& lhs, int32_t denominator,
                     const LDefinition& temp)
      : LInstructionHelper(classOpcode), denominator_(denominator) {
    setOperand(0, lhs);
    setTemp(0, temp);
  }

  const LAllocation* numerator() { return getOperand(0); }
  int32_t denominator() const { return denominator_; }
  MBinaryArithInstruction* mir() const {
    MOZ_ASSERT(mir_->isDiv() || mir_->isMod());
    return static_cast<MBinaryArithInstruction*>(mir_);
  }
  bool canBeNegativeDividend() const {
    if (mir_->isMod()) {
      return mir_->toMod()->canBeNegativeDividend();
    }
    return mir_->toDiv()->canBeNegativeDividend();
  }
};

// This class performs a simple x86 'div', yielding either a quotient or
// remainder depending on whether this instruction is defined to output eax
// (quotient) or edx (remainder).
class LUDivOrMod : public LBinaryMath<1> {
 public:
  LIR_HEADER(UDivOrMod);

  LUDivOrMod(const LAllocation& lhs, const LAllocation& rhs,
             const LDefinition& temp)
      : LBinaryMath(classOpcode) {
    setOperand(0, lhs);
    setOperand(1, rhs);
    setTemp(0, temp);
  }

  const LDefinition* remainder() { return getTemp(0); }

  const char* extraName() const {
    return mir()->isTruncated() ? "Truncated" : nullptr;
  }

  MBinaryArithInstruction* mir() const {
    MOZ_ASSERT(mir_->isDiv() || mir_->isMod());
    return static_cast<MBinaryArithInstruction*>(mir_);
  }

  bool canBeDivideByZero() const {
    if (mir_->isMod()) {
      return mir_->toMod()->canBeDivideByZero();
    }
    return mir_->toDiv()->canBeDivideByZero();
  }

  bool trapOnError() const {
    if (mir_->isMod()) {
      return mir_->toMod()->trapOnError();
    }
    return mir_->toDiv()->trapOnError();
  }

  wasm::TrapSiteDesc trapSiteDesc() const {
    if (mir_->isMod()) {
      return mir_->toMod()->trapSiteDesc();
    }
    return mir_->toDiv()->trapSiteDesc();
  }
};

class LUDivOrModConstant : public LInstructionHelper<1, 1, 1> {
  const uint32_t denominator_;

 public:
  LIR_HEADER(UDivOrModConstant)

  LUDivOrModConstant(const LAllocation& lhs, uint32_t denominator,
                     const LDefinition& temp)
      : LInstructionHelper(classOpcode), denominator_(denominator) {
    setOperand(0, lhs);
    setTemp(0, temp);
  }

  const LAllocation* numerator() { return getOperand(0); }
  uint32_t denominator() const { return denominator_; }
  MBinaryArithInstruction* mir() const {
    MOZ_ASSERT(mir_->isDiv() || mir_->isMod());
    return static_cast<MBinaryArithInstruction*>(mir_);
  }
  bool canBeNegativeDividend() const {
    if (mir_->isMod()) {
      return mir_->toMod()->canBeNegativeDividend();
    }
    return mir_->toDiv()->canBeNegativeDividend();
  }
  bool trapOnError() const {
    if (mir_->isMod()) {
      return mir_->toMod()->trapOnError();
    }
    return mir_->toDiv()->trapOnError();
  }
  wasm::TrapSiteDesc trapSiteDesc() const {
    if (mir_->isMod()) {
      return mir_->toMod()->trapSiteDesc();
    }
    return mir_->toDiv()->trapSiteDesc();
  }
};

}  // namespace jit
}  // namespace js

#endif /* jit_x86_shared_LIR_x86_shared_h */
