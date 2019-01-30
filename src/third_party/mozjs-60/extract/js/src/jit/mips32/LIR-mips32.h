/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_mips32_LIR_mips32_h
#define jit_mips32_LIR_mips32_h

namespace js {
namespace jit {

class LBoxFloatingPoint : public LInstructionHelper<2, 1, 1>
{
    MIRType type_;

  public:
    LIR_HEADER(BoxFloatingPoint);

    LBoxFloatingPoint(const LAllocation& in, const LDefinition& temp, MIRType type)
      : type_(type)
    {
        setOperand(0, in);
        setTemp(0, temp);
    }

    MIRType type() const {
        return type_;
    }
    const char* extraName() const {
        return StringFromMIRType(type_);
    }
};

class LUnbox : public LInstructionHelper<1, 2, 0>
{
  public:
    LIR_HEADER(Unbox);

    MUnbox* mir() const {
        return mir_->toUnbox();
    }
    const LAllocation* payload() {
        return getOperand(0);
    }
    const LAllocation* type() {
        return getOperand(1);
    }
    const char* extraName() const {
        return StringFromMIRType(mir()->type());
    }
};

class LUnboxFloatingPoint : public LInstructionHelper<1, 2, 0>
{
    MIRType type_;

  public:
    LIR_HEADER(UnboxFloatingPoint);

    static const size_t Input = 0;

    LUnboxFloatingPoint(const LBoxAllocation& input, MIRType type)
      : type_(type)
    {
        setBoxOperand(Input, input);
    }

    MUnbox* mir() const {
        return mir_->toUnbox();
    }

    MIRType type() const {
        return type_;
    }
    const char* extraName() const {
        return StringFromMIRType(type_);
    }
};

class LDivOrModI64 : public LCallInstructionHelper<INT64_PIECES, INT64_PIECES*2, 0>
{
  public:
    LIR_HEADER(DivOrModI64)

    static const size_t Lhs = 0;
    static const size_t Rhs = INT64_PIECES;

    LDivOrModI64(const LInt64Allocation& lhs, const LInt64Allocation& rhs)
    {
        setInt64Operand(Lhs, lhs);
        setInt64Operand(Rhs, rhs);
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

class LUDivOrModI64 : public LCallInstructionHelper<INT64_PIECES, INT64_PIECES*2, 0>
{
  public:
    LIR_HEADER(UDivOrModI64)

    static const size_t Lhs = 0;
    static const size_t Rhs = INT64_PIECES;

    LUDivOrModI64(const LInt64Allocation& lhs, const LInt64Allocation& rhs)
    {
        setInt64Operand(Lhs, lhs);
        setInt64Operand(Rhs, rhs);
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

class LWasmTruncateToInt64 : public LCallInstructionHelper<INT64_PIECES, 1, 0>
{
  public:
    LIR_HEADER(WasmTruncateToInt64);

    explicit LWasmTruncateToInt64(const LAllocation& in)
    {
        setOperand(0, in);
    }

    MWasmTruncateToInt64* mir() const {
        return mir_->toWasmTruncateToInt64();
    }
};

class LInt64ToFloatingPoint : public LCallInstructionHelper<1, INT64_PIECES, 0>
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

class LWasmAtomicLoadI64 : public LInstructionHelper<INT64_PIECES, 1, 0>
{
  public:
    LIR_HEADER(WasmAtomicLoadI64);

    LWasmAtomicLoadI64(const LAllocation& ptr)
    {
        setOperand(0, ptr);
    }

    const LAllocation* ptr() {
        return getOperand(0);
    }
    const MWasmLoad* mir() const {
        return mir_->toWasmLoad();
    }
};

class LWasmAtomicStoreI64 : public LInstructionHelper<0, 1 + INT64_PIECES, 1>
{
  public:
    LIR_HEADER(WasmAtomicStoreI64);

    LWasmAtomicStoreI64(const LAllocation& ptr, const LInt64Allocation& value, const LDefinition& tmp)
    {
        setOperand(0, ptr);
        setInt64Operand(1, value);
        setTemp(0, tmp);
    }

    const LAllocation* ptr() {
        return getOperand(0);
    }
    const LInt64Allocation value() {
        return getInt64Operand(1);
    }
    const LDefinition* tmp() {
        return getTemp(0);
    }
    const MWasmStore* mir() const {
        return mir_->toWasmStore();
    }
};

} // namespace jit
} // namespace js

#endif /* jit_mips32_LIR_mips32_h */
