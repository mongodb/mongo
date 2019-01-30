/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_arm_LIR_arm_h
#define jit_arm_LIR_arm_h

namespace js {
namespace jit {

class LBoxFloatingPoint : public LInstructionHelper<2, 1, 1>
{
    MIRType type_;

  public:
    LIR_HEADER(BoxFloatingPoint);

    LBoxFloatingPoint(const LAllocation& in, const LDefinition& temp, MIRType type)
      : LInstructionHelper(classOpcode),
        type_(type)
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

    LUnbox()
      : LInstructionHelper(classOpcode)
    {}

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
      : LInstructionHelper(classOpcode),
        type_(type)
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

// Convert a 32-bit unsigned integer to a double.
class LWasmUint32ToDouble : public LInstructionHelper<1, 1, 0>
{
  public:
    LIR_HEADER(WasmUint32ToDouble)

    explicit LWasmUint32ToDouble(const LAllocation& input)
      : LInstructionHelper(classOpcode)
    {
        setOperand(0, input);
    }
};

// Convert a 32-bit unsigned integer to a float32.
class LWasmUint32ToFloat32 : public LInstructionHelper<1, 1, 0>
{
  public:
    LIR_HEADER(WasmUint32ToFloat32)

    explicit LWasmUint32ToFloat32(const LAllocation& input)
      : LInstructionHelper(classOpcode)
    {
        setOperand(0, input);
    }
};

class LDivI : public LBinaryMath<1>
{
  public:
    LIR_HEADER(DivI);

    LDivI(const LAllocation& lhs, const LAllocation& rhs,
          const LDefinition& temp)
      : LBinaryMath(classOpcode)
    {
        setOperand(0, lhs);
        setOperand(1, rhs);
        setTemp(0, temp);
    }

    MDiv* mir() const {
        return mir_->toDiv();
    }
};

class LDivOrModI64 : public LCallInstructionHelper<INT64_PIECES, INT64_PIECES*2, 0>
{
  public:
    LIR_HEADER(DivOrModI64)

    static const size_t Lhs = 0;
    static const size_t Rhs = INT64_PIECES;

    LDivOrModI64(const LInt64Allocation& lhs, const LInt64Allocation& rhs)
      : LCallInstructionHelper(classOpcode)
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
      : LCallInstructionHelper(classOpcode)
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

// LSoftDivI is a software divide for ARM cores that don't support a hardware
// divide instruction, implemented as a C++ native call.
class LSoftDivI : public LBinaryCallInstructionHelper<1, 0>
{
  public:
    LIR_HEADER(SoftDivI);

    LSoftDivI(const LAllocation& lhs, const LAllocation& rhs)
      : LBinaryCallInstructionHelper(classOpcode)
    {
        setOperand(0, lhs);
        setOperand(1, rhs);
    }

    MDiv* mir() const {
        return mir_->toDiv();
    }
};

class LDivPowTwoI : public LInstructionHelper<1, 1, 0>
{
    const int32_t shift_;

  public:
    LIR_HEADER(DivPowTwoI)

    LDivPowTwoI(const LAllocation& lhs, int32_t shift)
      : LInstructionHelper(classOpcode),
        shift_(shift)
    {
        setOperand(0, lhs);
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
      : LBinaryMath(classOpcode)
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

class LSoftModI : public LBinaryCallInstructionHelper<1, 1>
{
  public:
    LIR_HEADER(SoftModI);

    LSoftModI(const LAllocation& lhs, const LAllocation& rhs,
              const LDefinition& temp)
      : LBinaryCallInstructionHelper(classOpcode)
    {
        setOperand(0, lhs);
        setOperand(1, rhs);
        setTemp(0, temp);
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
      : LInstructionHelper(classOpcode),
        shift_(shift)
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

    LModMaskI(const LAllocation& lhs, const LDefinition& temp1, const LDefinition& temp2,
              int32_t shift)
      : LInstructionHelper(classOpcode),
        shift_(shift)
    {
        setOperand(0, lhs);
        setTemp(0, temp1);
        setTemp(1, temp2);
    }

    int32_t shift() const {
        return shift_;
    }

    MMod* mir() const {
        return mir_->toMod();
    }
};

// Takes a tableswitch with an integer to decide.
class LTableSwitch : public LInstructionHelper<0, 1, 1>
{
  public:
    LIR_HEADER(TableSwitch);

    LTableSwitch(const LAllocation& in, const LDefinition& inputCopy, MTableSwitch* ins)
      : LInstructionHelper(classOpcode)
    {
        setOperand(0, in);
        setTemp(0, inputCopy);
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
        return nullptr;
    }
};

// Takes a tableswitch with an integer to decide.
class LTableSwitchV : public LInstructionHelper<0, BOX_PIECES, 2>
{
  public:
    LIR_HEADER(TableSwitchV);

    LTableSwitchV(const LBoxAllocation& input, const LDefinition& inputCopy,
                  const LDefinition& floatCopy, MTableSwitch* ins)
      : LInstructionHelper(classOpcode)
    {
        setBoxOperand(InputValue, input);
        setTemp(0, inputCopy);
        setTemp(1, floatCopy);
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
        return nullptr;
    }
};

class LMulI : public LBinaryMath<0>
{
  public:
    LIR_HEADER(MulI);

    LMulI()
      : LBinaryMath(classOpcode)
    {}

    MMul* mir() {
        return mir_->toMul();
    }
};

class LUDiv : public LBinaryMath<0>
{
  public:
    LIR_HEADER(UDiv);

    LUDiv()
      : LBinaryMath(classOpcode)
    {}

    MDiv* mir() {
        return mir_->toDiv();
    }
};

class LUMod : public LBinaryMath<0>
{
  public:
    LIR_HEADER(UMod);

    LUMod()
      : LBinaryMath(classOpcode)
    {}

    MMod* mir() {
        return mir_->toMod();
    }
};

class LSoftUDivOrMod : public LBinaryCallInstructionHelper<1, 0>
{
  public:
    LIR_HEADER(SoftUDivOrMod);

    LSoftUDivOrMod(const LAllocation& lhs, const LAllocation& rhs)
      : LBinaryCallInstructionHelper(classOpcode)
    {
        setOperand(0, lhs);
        setOperand(1, rhs);
    }

    MInstruction* mir() {
        return mir_->toInstruction();
    }
};

class LWasmTruncateToInt64 : public LCallInstructionHelper<INT64_PIECES, 1, 0>
{
  public:
    LIR_HEADER(WasmTruncateToInt64);

    explicit LWasmTruncateToInt64(const LAllocation& in)
      : LCallInstructionHelper(classOpcode)
    {
        setOperand(0, in);
    }

    MWasmTruncateToInt64* mir() const {
        return mir_->toWasmTruncateToInt64();
    }
};

class LInt64ToFloatingPointCall: public LCallInstructionHelper<1, INT64_PIECES, 0>
{
  public:
    LIR_HEADER(Int64ToFloatingPointCall);

    LInt64ToFloatingPointCall()
      : LCallInstructionHelper(classOpcode)
    {}

    MInt64ToFloatingPoint* mir() const {
        return mir_->toInt64ToFloatingPoint();
    }
};

namespace details {

// Base class for the int64 and non-int64 variants.
template<size_t NumDefs>
class LWasmUnalignedLoadBase : public details::LWasmLoadBase<NumDefs, 4>
{
  public:
    typedef LWasmLoadBase<NumDefs, 4> Base;
    explicit LWasmUnalignedLoadBase(LNode::Opcode opcode,
                                    const LAllocation& ptr, const LDefinition& ptrCopy,
                                    const LDefinition& temp1, const LDefinition& temp2,
                                    const LDefinition& temp3)
      : Base(opcode, ptr, LAllocation())
    {
        Base::setTemp(0, ptrCopy);
        Base::setTemp(1, temp1);
        Base::setTemp(2, temp2);
        Base::setTemp(3, temp3);
    }

    const LDefinition* ptrCopy() {
        return Base::getTemp(0);
    }
};

} // namespace details

class LWasmUnalignedLoad : public details::LWasmUnalignedLoadBase<1>
{
  public:
    explicit LWasmUnalignedLoad(const LAllocation& ptr, const LDefinition& ptrCopy,
                                const LDefinition& temp1, const LDefinition& temp2,
                                const LDefinition& temp3)
      : LWasmUnalignedLoadBase(classOpcode, ptr, ptrCopy, temp1, temp2, temp3)
    {}
    LIR_HEADER(WasmUnalignedLoad);
};

class LWasmUnalignedLoadI64 : public details::LWasmUnalignedLoadBase<INT64_PIECES>
{
  public:
    explicit LWasmUnalignedLoadI64(const LAllocation& ptr, const LDefinition& ptrCopy,
                                   const LDefinition& temp1, const LDefinition& temp2,
                                   const LDefinition& temp3)
      : LWasmUnalignedLoadBase(classOpcode, ptr, ptrCopy, temp1, temp2, temp3)
    {}
    LIR_HEADER(WasmUnalignedLoadI64);
};

namespace details {

// Base class for the int64 and non-int64 variants.
template<size_t NumOps>
class LWasmUnalignedStoreBase : public LInstructionHelper<0, NumOps, 2>
{
  public:
    typedef LInstructionHelper<0, NumOps, 2> Base;

    static const uint32_t ValueIndex = 1;

    LWasmUnalignedStoreBase(LNode::Opcode opcode, const LAllocation& ptr,
                            const LDefinition& ptrCopy, const LDefinition& valueHelper)
      : Base(opcode)
    {
        Base::setOperand(0, ptr);
        Base::setTemp(0, ptrCopy);
        Base::setTemp(1, valueHelper);
    }
    MWasmStore* mir() const {
        return Base::mir_->toWasmStore();
    }
    const LDefinition* ptrCopy() {
        return Base::getTemp(0);
    }
    const LDefinition* valueHelper() {
        return Base::getTemp(1);
    }
};

} // namespace details

class LWasmUnalignedStore : public details::LWasmUnalignedStoreBase<2>
{
  public:
    LIR_HEADER(WasmUnalignedStore);
    LWasmUnalignedStore(const LAllocation& ptr, const LAllocation& value,
                        const LDefinition& ptrCopy, const LDefinition& valueHelper)
      : LWasmUnalignedStoreBase(classOpcode, ptr, ptrCopy, valueHelper)
    {
        setOperand(1, value);
    }
};

class LWasmUnalignedStoreI64 : public details::LWasmUnalignedStoreBase<1 + INT64_PIECES>
{
  public:
    LIR_HEADER(WasmUnalignedStoreI64);
    LWasmUnalignedStoreI64(const LAllocation& ptr, const LInt64Allocation& value,
                           const LDefinition& ptrCopy, const LDefinition& valueHelper)
      : LWasmUnalignedStoreBase(classOpcode, ptr, ptrCopy, valueHelper)
    {
        setInt64Operand(1, value);
    }
};

class LWasmAtomicLoadI64 : public LInstructionHelper<INT64_PIECES, 1, 0>
{
  public:
    LIR_HEADER(WasmAtomicLoadI64);

    explicit LWasmAtomicLoadI64(const LAllocation& ptr)
      : LInstructionHelper(classOpcode)
    {
        setOperand(0, ptr);
    }

    MWasmLoad* mir() const {
        return mir_->toWasmLoad();
    }
    const LAllocation* ptr() {
        return getOperand(0);
    }
};

class LWasmAtomicStoreI64 : public LInstructionHelper<0, 1 + INT64_PIECES, 2>
{
  public:
    LIR_HEADER(WasmAtomicStoreI64);

    LWasmAtomicStoreI64(const LAllocation& ptr, const LInt64Allocation& value,
                        const LDefinition& tmpLow, const LDefinition& tmpHigh)
      : LInstructionHelper(classOpcode)
    {
        setOperand(0, ptr);
        setInt64Operand(1, value);
        setTemp(0, tmpLow);
        setTemp(1, tmpHigh);
    }

    MWasmStore* mir() const {
        return mir_->toWasmStore();
    }
    const LAllocation* ptr() {
        return getOperand(0);
    }
    const LInt64Allocation value() {
        return getInt64Operand(1);
    }
    const LDefinition* tmpLow() {
        return getTemp(0);
    }
    const LDefinition* tmpHigh() {
        return getTemp(1);
    }
};

class LWasmCompareExchangeI64 : public LInstructionHelper<INT64_PIECES, 1 + 2*INT64_PIECES, 0>
{
  public:
    LIR_HEADER(WasmCompareExchangeI64);

    LWasmCompareExchangeI64(const LAllocation& ptr, const LInt64Allocation& expected,
                            const LInt64Allocation& replacement)
      : LInstructionHelper(classOpcode)
    {
        setOperand(0, ptr);
        setInt64Operand(1, expected);
        setInt64Operand(1 + INT64_PIECES, replacement);
    }

    MWasmCompareExchangeHeap* mir() const {
        return mir_->toWasmCompareExchangeHeap();
    }
    const LAllocation* ptr() {
        return getOperand(0);
    }
    const LInt64Allocation expected() {
        return getInt64Operand(1);
    }
    const LInt64Allocation replacement() {
        return getInt64Operand(1 + INT64_PIECES);
    }
};

class LWasmAtomicBinopI64 : public LInstructionHelper<INT64_PIECES, 1 + INT64_PIECES, 2>
{
    const wasm::MemoryAccessDesc& access_;
    AtomicOp op_;

  public:
    LIR_HEADER(WasmAtomicBinopI64);

    LWasmAtomicBinopI64(const LAllocation& ptr, const LInt64Allocation& value,
                        const LDefinition& tmpLow, const LDefinition& tmpHigh,
                        const wasm::MemoryAccessDesc& access, AtomicOp op)
      : LInstructionHelper(classOpcode),
        access_(access),
        op_(op)
    {
        setOperand(0, ptr);
        setInt64Operand(1, value);
        setTemp(0, tmpLow);
        setTemp(1, tmpHigh);
    }

    const LAllocation* ptr() {
        return getOperand(0);
    }
    const LInt64Allocation value() {
        return getInt64Operand(1);
    }
    const wasm::MemoryAccessDesc& access() {
        return access_;
    }
    AtomicOp operation() const {
        return op_;
    }
    const LDefinition* tmpLow() {
        return getTemp(0);
    }
    const LDefinition* tmpHigh() {
        return getTemp(1);
    }
};

class LWasmAtomicExchangeI64 : public LInstructionHelper<INT64_PIECES, 1 + INT64_PIECES, 0>
{
    const wasm::MemoryAccessDesc& access_;

  public:
    LIR_HEADER(WasmAtomicExchangeI64);

    LWasmAtomicExchangeI64(const LAllocation& ptr, const LInt64Allocation& value,
                           const wasm::MemoryAccessDesc& access)
      : LInstructionHelper(classOpcode),
        access_(access)
    {
        setOperand(0, ptr);
        setInt64Operand(1, value);
    }

    const LAllocation* ptr() {
        return getOperand(0);
    }
    const LInt64Allocation value() {
        return getInt64Operand(1);
    }
    const wasm::MemoryAccessDesc& access() {
        return access_;
    }
};

} // namespace jit
} // namespace js

#endif /* jit_arm_LIR_arm_h */
