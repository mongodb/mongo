/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_mips64_LIR_mips64_h
#define jit_mips64_LIR_mips64_h

namespace js {
namespace jit {

class LUnbox : public LInstructionHelper<1, 1, 0>
{
  public:
    LIR_HEADER(Unbox);

    explicit LUnbox(const LAllocation& input) {
        setOperand(0, input);
    }

    static const size_t Input = 0;

    MUnbox* mir() const {
        return mir_->toUnbox();
    }
    const char* extraName() const {
        return StringFromMIRType(mir()->type());
    }
};

class LUnboxFloatingPoint : public LUnbox
{
    MIRType type_;

  public:
    LIR_HEADER(UnboxFloatingPoint);

    LUnboxFloatingPoint(const LAllocation& input, MIRType type)
      : LUnbox(input),
        type_(type)
    { }

    MIRType type() const {
        return type_;
    }
};

class LDivOrModI64 : public LBinaryMath<1>
{
  public:
    LIR_HEADER(DivOrModI64)

    LDivOrModI64(const LAllocation& lhs, const LAllocation& rhs, const LDefinition& temp) {
        setOperand(0, lhs);
        setOperand(1, rhs);
        setTemp(0, temp);
    }

    const LDefinition* remainder() {
        return getTemp(0);
    }

    MBinaryArithInstruction* mir() const {
        MOZ_ASSERT(mir_->isDiv() || mir_->isMod());
        return static_cast<MBinaryArithInstruction*>(mir_);
    }
    bool canBeDivideByZero() const {
        if (mir_->isMod())
            return mir_->toMod()->canBeDivideByZero();
        return mir_->toDiv()->canBeDivideByZero();
    }
    bool canBeNegativeOverflow() const {
        if (mir_->isMod())
            return mir_->toMod()->canBeNegativeDividend();
        return mir_->toDiv()->canBeNegativeOverflow();
    }
    wasm::BytecodeOffset bytecodeOffset() const {
        MOZ_ASSERT(mir_->isDiv() || mir_->isMod());
        if (mir_->isMod())
            return mir_->toMod()->bytecodeOffset();
        return mir_->toDiv()->bytecodeOffset();
    }
};

class LUDivOrModI64 : public LBinaryMath<1>
{
  public:
    LIR_HEADER(UDivOrModI64);

    LUDivOrModI64(const LAllocation& lhs, const LAllocation& rhs, const LDefinition& temp) {
        setOperand(0, lhs);
        setOperand(1, rhs);
        setTemp(0, temp);
    }

    const LDefinition* remainder() {
        return getTemp(0);
    }

    const char* extraName() const {
        return mir()->isTruncated() ? "Truncated" : nullptr;
    }

    MBinaryArithInstruction* mir() const {
        MOZ_ASSERT(mir_->isDiv() || mir_->isMod());
        return static_cast<MBinaryArithInstruction*>(mir_);
    }

    bool canBeDivideByZero() const {
        if (mir_->isMod())
            return mir_->toMod()->canBeDivideByZero();
        return mir_->toDiv()->canBeDivideByZero();
    }
    wasm::BytecodeOffset bytecodeOffset() const {
        MOZ_ASSERT(mir_->isDiv() || mir_->isMod());
        if (mir_->isMod())
            return mir_->toMod()->bytecodeOffset();
        return mir_->toDiv()->bytecodeOffset();
    }
};

class LWasmTruncateToInt64 : public LInstructionHelper<1, 1, 0>
{
  public:
    LIR_HEADER(WasmTruncateToInt64);

    explicit LWasmTruncateToInt64(const LAllocation& in) {
        setOperand(0, in);
    }

    MWasmTruncateToInt64* mir() const {
        return mir_->toWasmTruncateToInt64();
    }
};

class LInt64ToFloatingPoint : public LInstructionHelper<1, 1, 0>
{
  public:
    LIR_HEADER(Int64ToFloatingPoint);

    explicit LInt64ToFloatingPoint(const LInt64Allocation& in) {
        setInt64Operand(0, in);
    }

    MInt64ToFloatingPoint* mir() const {
        return mir_->toInt64ToFloatingPoint();
    }
};

} // namespace jit
} // namespace js

#endif /* jit_mips64_LIR_mips64_h */
