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
class LWasmUint32ToDouble : public LInstructionHelper<1, 1, 0>
{
  public:
    LIR_HEADER(WasmUint32ToDouble)

    LWasmUint32ToDouble(const LAllocation& input) {
        setOperand(0, input);
    }
};

// Convert a 32-bit unsigned integer to a float32.
class LWasmUint32ToFloat32 : public LInstructionHelper<1, 1, 0>
{
  public:
    LIR_HEADER(WasmUint32ToFloat32)

    LWasmUint32ToFloat32(const LAllocation& input) {
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

    LTableSwitchV(const LBoxAllocation& input, const LDefinition& inputCopy,
                  const LDefinition& floatCopy, const LDefinition& jumpTablePointer,
                  MTableSwitch* ins)
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

    bool trapOnError() const {
        if (mir_->isMod())
            return mir_->toMod()->trapOnError();
        return mir_->toDiv()->trapOnError();
    }

    wasm::BytecodeOffset bytecodeOffset() const {
        MOZ_ASSERT(mir_->isDiv() || mir_->isMod());
        if (mir_->isMod())
            return mir_->toMod()->bytecodeOffset();
        return mir_->toDiv()->bytecodeOffset();
    }
};

namespace details {

// Base class for the int64 and non-int64 variants.
template<size_t NumDefs>
class LWasmUnalignedLoadBase : public details::LWasmLoadBase<NumDefs, 2>
{
  public:
    typedef LWasmLoadBase<NumDefs, 2> Base;

    explicit LWasmUnalignedLoadBase(const LAllocation& ptr, const LDefinition& valueHelper)
      : Base(ptr, LAllocation())
    {
        Base::setTemp(0, LDefinition::BogusTemp());
        Base::setTemp(1, valueHelper);
    }
    const LAllocation* ptr() {
        return Base::getOperand(0);
    }
    const LDefinition* ptrCopy() {
        return Base::getTemp(0);
    }
};

} // namespace details

class LWasmUnalignedLoad : public details::LWasmUnalignedLoadBase<1>
{
  public:
    explicit LWasmUnalignedLoad(const LAllocation& ptr, const LDefinition& valueHelper)
      : LWasmUnalignedLoadBase(ptr, valueHelper)
    {}
    LIR_HEADER(WasmUnalignedLoad);
};

class LWasmUnalignedLoadI64 : public details::LWasmUnalignedLoadBase<INT64_PIECES>
{
  public:
    explicit LWasmUnalignedLoadI64(const LAllocation& ptr, const LDefinition& valueHelper)
      : LWasmUnalignedLoadBase(ptr, valueHelper)
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

    static const size_t PtrIndex = 0;
    static const size_t ValueIndex = 1;

    LWasmUnalignedStoreBase(const LAllocation& ptr, const LDefinition& valueHelper)
    {
        Base::setOperand(0, ptr);
        Base::setTemp(0, LDefinition::BogusTemp());
        Base::setTemp(1, valueHelper);
    }
    MWasmStore* mir() const {
        return Base::mir_->toWasmStore();
    }
    const LAllocation* ptr() {
        return Base::getOperand(PtrIndex);
    }
    const LDefinition* ptrCopy() {
        return Base::getTemp(0);
    }
};

} // namespace details

class LWasmUnalignedStore : public details::LWasmUnalignedStoreBase<2>
{
  public:
    LIR_HEADER(WasmUnalignedStore);
    LWasmUnalignedStore(const LAllocation& ptr, const LAllocation& value,
                        const LDefinition& valueHelper)
      : LWasmUnalignedStoreBase(ptr, valueHelper)
    {
        setOperand(1, value);
    }
    const LAllocation* value() {
        return Base::getOperand(ValueIndex);
    }
};

class LWasmUnalignedStoreI64 : public details::LWasmUnalignedStoreBase<1 + INT64_PIECES>
{
  public:
    LIR_HEADER(WasmUnalignedStoreI64);
    LWasmUnalignedStoreI64(const LAllocation& ptr, const LInt64Allocation& value,
                           const LDefinition& valueHelper)
      : LWasmUnalignedStoreBase(ptr, valueHelper)
    {
        setInt64Operand(1, value);
    }
    const LInt64Allocation value() {
        return getInt64Operand(ValueIndex);
    }
};

class LWasmCompareExchangeI64 : public LInstructionHelper<INT64_PIECES, 1 + INT64_PIECES + INT64_PIECES, 0>
{
  public:
    LIR_HEADER(WasmCompareExchangeI64);

    LWasmCompareExchangeI64(const LAllocation& ptr, const LInt64Allocation& oldValue, const LInt64Allocation& newValue)
    {
        setOperand(0, ptr);
        setInt64Operand(1, oldValue);
        setInt64Operand(1 + INT64_PIECES, newValue);
    }

    const LAllocation* ptr() {
        return getOperand(0);
    }
    const LInt64Allocation oldValue() {
        return getInt64Operand(1);
    }
    const LInt64Allocation newValue() {
        return getInt64Operand(1 + INT64_PIECES);
    }
    const MWasmCompareExchangeHeap* mir() const {
        return mir_->toWasmCompareExchangeHeap();
    }
};

class LWasmAtomicExchangeI64 : public LInstructionHelper<INT64_PIECES, 1 + INT64_PIECES, 0>
{
  public:
    LIR_HEADER(WasmAtomicExchangeI64);

    LWasmAtomicExchangeI64(const LAllocation& ptr, const LInt64Allocation& value)
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
    const MWasmAtomicExchangeHeap* mir() const {
        return mir_->toWasmAtomicExchangeHeap();
    }
};

class LWasmAtomicBinopI64 : public LInstructionHelper<INT64_PIECES, 1 + INT64_PIECES, 2>
{
  public:
    LIR_HEADER(WasmAtomicBinopI64);

    LWasmAtomicBinopI64(const LAllocation& ptr, const LInt64Allocation& value)
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

    const MWasmAtomicBinopHeap* mir() const {
        return mir_->toWasmAtomicBinopHeap();
    }
};


} // namespace jit
} // namespace js

#endif /* jit_mips_shared_LIR_mips_shared_h */
