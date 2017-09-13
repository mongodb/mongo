/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_mips_shared_LIR_mips_shared_h
#define jit_mips_shared_LIR_mips_shared_h

namespace js {
namespace jit {

// Convert a 32-bit unsigned integer to a double.
class LAsmJSUInt32ToDouble : public LInstructionHelper<1, 1, 0>
{
  public:
    LIR_HEADER(AsmJSUInt32ToDouble)

    LAsmJSUInt32ToDouble(const LAllocation& input) {
        setOperand(0, input);
    }
};

// Convert a 32-bit unsigned integer to a float32.
class LAsmJSUInt32ToFloat32 : public LInstructionHelper<1, 1, 0>
{
  public:
    LIR_HEADER(AsmJSUInt32ToFloat32)

    LAsmJSUInt32ToFloat32(const LAllocation& input) {
        setOperand(0, input);
    }
};


class LDivI : public LBinaryMath<1>
{
  public:
    LIR_HEADER(DivI);

    LDivI(const LAllocation& lhs, const LAllocation& rhs,
          const LDefinition& temp) {
        setOperand(0, lhs);
        setOperand(1, rhs);
        setTemp(0, temp);
    }

    MDiv* mir() const {
        return mir_->toDiv();
    }
};

class LDivPowTwoI : public LInstructionHelper<1, 1, 1>
{
    const int32_t shift_;

  public:
    LIR_HEADER(DivPowTwoI)

    LDivPowTwoI(const LAllocation& lhs, int32_t shift, const LDefinition& temp)
      : shift_(shift)
    {
        setOperand(0, lhs);
        setTemp(0, temp);
    }

    const LAllocation* numerator() {
        return getOperand(0);
    }

    int32_t shift() {
        return shift_;
    }

    MDiv* mir() const {
        return mir_->toDiv();
    }
};

class LModI : public LBinaryMath<1>
{
  public:
    LIR_HEADER(ModI);

    LModI(const LAllocation& lhs, const LAllocation& rhs,
          const LDefinition& callTemp)
    {
        setOperand(0, lhs);
        setOperand(1, rhs);
        setTemp(0, callTemp);
    }

    const LDefinition* callTemp() {
        return getTemp(0);
    }

    MMod* mir() const {
        return mir_->toMod();
    }
};

class LModPowTwoI : public LInstructionHelper<1, 1, 0>
{
    const int32_t shift_;

  public:
    LIR_HEADER(ModPowTwoI);
    int32_t shift()
    {
        return shift_;
    }

    LModPowTwoI(const LAllocation& lhs, int32_t shift)
      : shift_(shift)
    {
        setOperand(0, lhs);
    }

    MMod* mir() const {
        return mir_->toMod();
    }
};

class LModMaskI : public LInstructionHelper<1, 1, 2>
{
    const int32_t shift_;

  public:
    LIR_HEADER(ModMaskI);

    LModMaskI(const LAllocation& lhs, const LDefinition& temp0, const LDefinition& temp1,
              int32_t shift)
      : shift_(shift)
    {
        setOperand(0, lhs);
        setTemp(0, temp0);
        setTemp(1, temp1);
    }

    int32_t shift() const {
        return shift_;
    }

    MMod* mir() const {
        return mir_->toMod();
    }
};

// Takes a tableswitch with an integer to decide
class LTableSwitch : public LInstructionHelper<0, 1, 2>
{
  public:
    LIR_HEADER(TableSwitch);

    LTableSwitch(const LAllocation& in, const LDefinition& inputCopy,
                 const LDefinition& jumpTablePointer, MTableSwitch* ins) {
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
    // This is added to share the same CodeGenerator prefixes.
    const LDefinition* tempPointer() {
        return getTemp(1);
    }
};

// Takes a tableswitch with an integer to decide
class LTableSwitchV : public LInstructionHelper<0, BOX_PIECES, 3>
{
  public:
    LIR_HEADER(TableSwitchV);

    LTableSwitchV(const LDefinition& inputCopy, const LDefinition& floatCopy,
                  const LDefinition& jumpTablePointer, MTableSwitch* ins)
    {
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

class LGuardShape : public LInstructionHelper<0, 1, 1>
{
  public:
    LIR_HEADER(GuardShape);

    LGuardShape(const LAllocation& in, const LDefinition& temp) {
        setOperand(0, in);
        setTemp(0, temp);
    }
    const MGuardShape* mir() const {
        return mir_->toGuardShape();
    }
    const LDefinition* tempInt() {
        return getTemp(0);
    }
};

class LGuardObjectGroup : public LInstructionHelper<0, 1, 1>
{
  public:
    LIR_HEADER(GuardObjectGroup);

    LGuardObjectGroup(const LAllocation& in, const LDefinition& temp) {
        setOperand(0, in);
        setTemp(0, temp);
    }
    const MGuardObjectGroup* mir() const {
        return mir_->toGuardObjectGroup();
    }
    const LDefinition* tempInt() {
        return getTemp(0);
    }
};

class LMulI : public LBinaryMath<0>
{
  public:
    LIR_HEADER(MulI);

    MMul* mir() {
        return mir_->toMul();
    }
};

class LUDivOrMod : public LBinaryMath<0>
{
  public:
    LIR_HEADER(UDivOrMod);

    MBinaryArithInstruction* mir() const {
        MOZ_ASSERT(mir_->isDiv() || mir_->isMod());
        return static_cast<MBinaryArithInstruction*>(mir_);
    }

    bool canBeDivideByZero() const {
        if (mir_->isMod())
            return mir_->toMod()->canBeDivideByZero();
        return mir_->toDiv()->canBeDivideByZero();
    }
};

class LAsmJSLoadFuncPtr : public LInstructionHelper<1, 1, 0>
{
  public:
    LIR_HEADER(AsmJSLoadFuncPtr);
    LAsmJSLoadFuncPtr(const LAllocation& index) {
        setOperand(0, index);
    }
    const MAsmJSLoadFuncPtr* mir() const {
        return mir_->toAsmJSLoadFuncPtr();
    }
    const LAllocation* index() {
        return getOperand(0);
    }
};

} // namespace jit
} // namespace js

#endif /* jit_mips_shared_LIR_mips_shared_h */
