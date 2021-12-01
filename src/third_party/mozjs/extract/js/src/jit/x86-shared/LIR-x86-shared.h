/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x86_shared_LIR_x86_shared_h
#define jit_x86_shared_LIR_x86_shared_h

namespace js {
namespace jit {

class LDivI : public LBinaryMath<1>
{
  public:
    LIR_HEADER(DivI)

    LDivI(const LAllocation& lhs, const LAllocation& rhs, const LDefinition& temp)
      : LBinaryMath(classOpcode)
    {
        setOperand(0, lhs);
        setOperand(1, rhs);
        setTemp(0, temp);
    }

    const char* extraName() const {
        if (mir()->isTruncated()) {
            if (mir()->canBeNegativeZero()) {
                return mir()->canBeNegativeOverflow()
                       ? "Truncate_NegativeZero_NegativeOverflow"
                       : "Truncate_NegativeZero";
            }
            return mir()->canBeNegativeOverflow() ? "Truncate_NegativeOverflow" : "Truncate";
        }
        if (mir()->canBeNegativeZero())
            return mir()->canBeNegativeOverflow() ? "NegativeZero_NegativeOverflow" : "NegativeZero";
        return mir()->canBeNegativeOverflow() ? "NegativeOverflow" : nullptr;
    }

    const LDefinition* remainder() {
        return getTemp(0);
    }
    MDiv* mir() const {
        return mir_->toDiv();
    }
};

// Signed division by a power-of-two constant.
class LDivPowTwoI : public LBinaryMath<0>
{
    const int32_t shift_;
    const bool negativeDivisor_;

  public:
    LIR_HEADER(DivPowTwoI)

    LDivPowTwoI(const LAllocation& lhs, const LAllocation& lhsCopy, int32_t shift, bool negativeDivisor)
      : LBinaryMath(classOpcode),
        shift_(shift),
        negativeDivisor_(negativeDivisor)
    {
        setOperand(0, lhs);
        setOperand(1, lhsCopy);
    }

    const LAllocation* numerator() {
        return getOperand(0);
    }
    const LAllocation* numeratorCopy() {
        return getOperand(1);
    }
    int32_t shift() const {
        return shift_;
    }
    bool negativeDivisor() const {
        return negativeDivisor_;
    }
    MDiv* mir() const {
        return mir_->toDiv();
    }
};

class LDivOrModConstantI : public LInstructionHelper<1, 1, 1>
{
    const int32_t denominator_;

  public:
    LIR_HEADER(DivOrModConstantI)

    LDivOrModConstantI(const LAllocation& lhs, int32_t denominator, const LDefinition& temp)
      : LInstructionHelper(classOpcode),
        denominator_(denominator)
    {
        setOperand(0, lhs);
        setTemp(0, temp);
    }

    const LAllocation* numerator() {
        return getOperand(0);
    }
    int32_t denominator() const {
        return denominator_;
    }
    MBinaryArithInstruction* mir() const {
        MOZ_ASSERT(mir_->isDiv() || mir_->isMod());
        return static_cast<MBinaryArithInstruction*>(mir_);
    }
    bool canBeNegativeDividend() const {
        if (mir_->isMod())
            return mir_->toMod()->canBeNegativeDividend();
        return mir_->toDiv()->canBeNegativeDividend();
    }
};

class LModI : public LBinaryMath<1>
{
  public:
    LIR_HEADER(ModI)

    LModI(const LAllocation& lhs, const LAllocation& rhs, const LDefinition& temp)
      : LBinaryMath(classOpcode)
    {
        setOperand(0, lhs);
        setOperand(1, rhs);
        setTemp(0, temp);
    }

    const char* extraName() const {
        return mir()->isTruncated() ? "Truncated" : nullptr;
    }

    const LDefinition* remainder() {
        return getDef(0);
    }
    MMod* mir() const {
        return mir_->toMod();
    }
};

// This class performs a simple x86 'div', yielding either a quotient or remainder depending on
// whether this instruction is defined to output eax (quotient) or edx (remainder).
class LUDivOrMod : public LBinaryMath<1>
{
  public:
    LIR_HEADER(UDivOrMod);

    LUDivOrMod(const LAllocation& lhs, const LAllocation& rhs, const LDefinition& temp)
      : LBinaryMath(classOpcode)
    {
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

    bool trapOnError() const {
        if (mir_->isMod())
            return mir_->toMod()->trapOnError();
        return mir_->toDiv()->trapOnError();
    }

    wasm::BytecodeOffset bytecodeOffset() const {
        if (mir_->isMod())
            return mir_->toMod()->bytecodeOffset();
        return mir_->toDiv()->bytecodeOffset();
    }
};

class LUDivOrModConstant : public LInstructionHelper<1, 1, 1>
{
    const uint32_t denominator_;

  public:
    LIR_HEADER(UDivOrModConstant)

    LUDivOrModConstant(const LAllocation &lhs, uint32_t denominator, const LDefinition& temp)
      : LInstructionHelper(classOpcode),
        denominator_(denominator)
    {
        setOperand(0, lhs);
        setTemp(0, temp);
    }

    const LAllocation* numerator() {
        return getOperand(0);
    }
    uint32_t denominator() const {
        return denominator_;
    }
    MBinaryArithInstruction *mir() const {
        MOZ_ASSERT(mir_->isDiv() || mir_->isMod());
        return static_cast<MBinaryArithInstruction *>(mir_);
    }
    bool canBeNegativeDividend() const {
        if (mir_->isMod())
            return mir_->toMod()->canBeNegativeDividend();
        return mir_->toDiv()->canBeNegativeDividend();
    }
    bool trapOnError() const {
        if (mir_->isMod())
            return mir_->toMod()->trapOnError();
        return mir_->toDiv()->trapOnError();
    }
    wasm::BytecodeOffset bytecodeOffset() const {
        if (mir_->isMod())
            return mir_->toMod()->bytecodeOffset();
        return mir_->toDiv()->bytecodeOffset();
    }
};

class LModPowTwoI : public LInstructionHelper<1,1,0>
{
    const int32_t shift_;

  public:
    LIR_HEADER(ModPowTwoI)

    LModPowTwoI(const LAllocation& lhs, int32_t shift)
      : LInstructionHelper(classOpcode),
        shift_(shift)
    {
        setOperand(0, lhs);
    }

    int32_t shift() const {
        return shift_;
    }
    const LDefinition* remainder() {
        return getDef(0);
    }
    MMod* mir() const {
        return mir_->toMod();
    }
};

// Takes a tableswitch with an integer to decide
class LTableSwitch : public LInstructionHelper<0, 1, 2>
{
  public:
    LIR_HEADER(TableSwitch)

    LTableSwitch(const LAllocation& in, const LDefinition& inputCopy,
                 const LDefinition& jumpTablePointer, MTableSwitch* ins)
      : LInstructionHelper(classOpcode)
    {
        setOperand(0, in);
        setTemp(0, inputCopy);
        setTemp(1, jumpTablePointer);
        setMir(ins);
    }

    MTableSwitch* mir() const {
        return mir_->toTableSwitch();
    }

    const LAllocation* index() {
        return getOperand(0);
    }
    const LDefinition* tempInt() {
        return getTemp(0);
    }
    const LDefinition* tempPointer() {
        return getTemp(1);
    }
};

// Takes a tableswitch with a value to decide
class LTableSwitchV : public LInstructionHelper<0, BOX_PIECES, 3>
{
  public:
    LIR_HEADER(TableSwitchV)

    LTableSwitchV(const LBoxAllocation& input, const LDefinition& inputCopy,
                  const LDefinition& floatCopy, const LDefinition& jumpTablePointer,
                  MTableSwitch* ins)
      : LInstructionHelper(classOpcode)
    {
        setBoxOperand(InputValue, input);
        setTemp(0, inputCopy);
        setTemp(1, floatCopy);
        setTemp(2, jumpTablePointer);
        setMir(ins);
    }

    MTableSwitch* mir() const {
        return mir_->toTableSwitch();
    }

    static const size_t InputValue = 0;

    const LDefinition* tempInt() {
        return getTemp(0);
    }
    const LDefinition* tempFloat() {
        return getTemp(1);
    }
    const LDefinition* tempPointer() {
        return getTemp(2);
    }
};

class LMulI : public LBinaryMath<0, 1>
{
  public:
    LIR_HEADER(MulI)

    LMulI(const LAllocation& lhs, const LAllocation& rhs, const LAllocation& lhsCopy)
      : LBinaryMath(classOpcode)
    {
        setOperand(0, lhs);
        setOperand(1, rhs);
        setOperand(2, lhsCopy);
    }

    const char* extraName() const {
        return (mir()->mode() == MMul::Integer)
               ? "Integer"
               : (mir()->canBeNegativeZero() ? "CanBeNegativeZero" : nullptr);
    }

    MMul* mir() const {
        return mir_->toMul();
    }
    const LAllocation* lhsCopy() {
        return this->getOperand(2);
    }
};

// Constructs an int32x4 SIMD value.
class LSimdValueInt32x4 : public LInstructionHelper<1, 4, 0>
{
  public:
    LIR_HEADER(SimdValueInt32x4)
    LSimdValueInt32x4(const LAllocation& x, const LAllocation& y,
                      const LAllocation& z, const LAllocation& w)
      : LInstructionHelper(classOpcode)
    {
        setOperand(0, x);
        setOperand(1, y);
        setOperand(2, z);
        setOperand(3, w);
    }

    MSimdValueX4* mir() const {
        return mir_->toSimdValueX4();
    }
};

// Constructs a float32x4 SIMD value, optimized for x86 family
class LSimdValueFloat32x4 : public LInstructionHelper<1, 4, 1>
{
  public:
    LIR_HEADER(SimdValueFloat32x4)
    LSimdValueFloat32x4(const LAllocation& x, const LAllocation& y,
                        const LAllocation& z, const LAllocation& w,
                        const LDefinition& copyY)
      : LInstructionHelper(classOpcode)
    {
        setOperand(0, x);
        setOperand(1, y);
        setOperand(2, z);
        setOperand(3, w);

        setTemp(0, copyY);
    }

    MSimdValueX4* mir() const {
        return mir_->toSimdValueX4();
    }
};

class LInt64ToFloatingPoint : public LInstructionHelper<1, INT64_PIECES, 1>
{
  public:
    LIR_HEADER(Int64ToFloatingPoint);

    LInt64ToFloatingPoint(const LInt64Allocation& in, const LDefinition& temp)
      : LInstructionHelper(classOpcode)
    {
        setInt64Operand(0, in);
        setTemp(0, temp);
    }

    MInt64ToFloatingPoint* mir() const {
        return mir_->toInt64ToFloatingPoint();
    }

    const LDefinition* temp() {
        return getTemp(0);
    }
};

} // namespace jit
} // namespace js

#endif /* jit_x86_shared_LIR_x86_shared_h */
