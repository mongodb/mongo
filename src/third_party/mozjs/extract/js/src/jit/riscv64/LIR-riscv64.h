/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_riscv64_LIR_riscv64_h
#define jit_riscv64_LIR_riscv64_h

namespace js {
namespace jit {

class LUnbox : public LInstructionHelper<1, 1, 0> {
 protected:
  LUnbox(LNode::Opcode opcode, const LAllocation& input)
      : LInstructionHelper(opcode) {
    setOperand(0, input);
  }

 public:
  LIR_HEADER(Unbox);

  explicit LUnbox(const LAllocation& input) : LInstructionHelper(classOpcode) {
    setOperand(0, input);
  }

  static const size_t Input = 0;

  MUnbox* mir() const { return mir_->toUnbox(); }
  const char* extraName() const { return StringFromMIRType(mir()->type()); }
};

class LUnboxFloatingPoint : public LUnbox {
  MIRType type_;

 public:
  LIR_HEADER(UnboxFloatingPoint);

  LUnboxFloatingPoint(const LAllocation& input, MIRType type)
      : LUnbox(classOpcode, input), type_(type) {}

  MIRType type() const { return type_; }
};

// Convert a 32-bit unsigned integer to a double.
class LWasmUint32ToDouble : public LInstructionHelper<1, 1, 0> {
 public:
  LIR_HEADER(WasmUint32ToDouble)

  explicit LWasmUint32ToDouble(const LAllocation& input)
      : LInstructionHelper(classOpcode) {
    setOperand(0, input);
  }
};

// Convert a 32-bit unsigned integer to a float32.
class LWasmUint32ToFloat32 : public LInstructionHelper<1, 1, 0> {
 public:
  LIR_HEADER(WasmUint32ToFloat32)

  explicit LWasmUint32ToFloat32(const LAllocation& input)
      : LInstructionHelper(classOpcode) {
    setOperand(0, input);
  }
};

class LDivI : public LBinaryMath<1> {
 public:
  LIR_HEADER(DivI);

  LDivI(const LAllocation& lhs, const LAllocation& rhs, const LDefinition& temp)
      : LBinaryMath(classOpcode) {
    setOperand(0, lhs);
    setOperand(1, rhs);
    setTemp(0, temp);
  }

  MDiv* mir() const { return mir_->toDiv(); }
};

class LDivPowTwoI : public LInstructionHelper<1, 1, 1> {
  const int32_t shift_;

 public:
  LIR_HEADER(DivPowTwoI)

  LDivPowTwoI(const LAllocation& lhs, int32_t shift, const LDefinition& temp)
      : LInstructionHelper(classOpcode), shift_(shift) {
    setOperand(0, lhs);
    setTemp(0, temp);
  }

  const LAllocation* numerator() { return getOperand(0); }
  int32_t shift() const { return shift_; }
  MDiv* mir() const { return mir_->toDiv(); }
};

class LModI : public LBinaryMath<1> {
 public:
  LIR_HEADER(ModI);

  LModI(const LAllocation& lhs, const LAllocation& rhs,
        const LDefinition& callTemp)
      : LBinaryMath(classOpcode) {
    setOperand(0, lhs);
    setOperand(1, rhs);
    setTemp(0, callTemp);
  }

  const LDefinition* callTemp() { return getTemp(0); }
  MMod* mir() const { return mir_->toMod(); }
};

class LModPowTwoI : public LInstructionHelper<1, 1, 0> {
  const int32_t shift_;

 public:
  LIR_HEADER(ModPowTwoI);

  LModPowTwoI(const LAllocation& lhs, int32_t shift)
      : LInstructionHelper(classOpcode), shift_(shift) {
    setOperand(0, lhs);
  }

  int32_t shift() const { return shift_; }
  MMod* mir() const { return mir_->toMod(); }
};

class LModMaskI : public LInstructionHelper<1, 1, 2> {
  const int32_t shift_;

 public:
  LIR_HEADER(ModMaskI);

  LModMaskI(const LAllocation& lhs, const LDefinition& temp0,
            const LDefinition& temp1, int32_t shift)
      : LInstructionHelper(classOpcode), shift_(shift) {
    setOperand(0, lhs);
    setTemp(0, temp0);
    setTemp(1, temp1);
  }

  int32_t shift() const { return shift_; }
  MMod* mir() const { return mir_->toMod(); }
};

// Takes a tableswitch with an integer to decide
class LTableSwitch : public LInstructionHelper<0, 1, 2> {
 public:
  LIR_HEADER(TableSwitch);

  LTableSwitch(const LAllocation& in, const LDefinition& inputCopy,
               const LDefinition& jumpTablePointer, MTableSwitch* ins)
      : LInstructionHelper(classOpcode) {
    setOperand(0, in);
    setTemp(0, inputCopy);
    setTemp(1, jumpTablePointer);
    setMir(ins);
  }

  MTableSwitch* mir() const { return mir_->toTableSwitch(); }
  const LAllocation* index() { return getOperand(0); }
  const LDefinition* tempInt() { return getTemp(0); }
  // This is added to share the same CodeGenerator prefixes.
  const LDefinition* tempPointer() { return getTemp(1); }
};

// Takes a tableswitch with an integer to decide
class LTableSwitchV : public LInstructionHelper<0, BOX_PIECES, 3> {
 public:
  LIR_HEADER(TableSwitchV);

  LTableSwitchV(const LBoxAllocation& input, const LDefinition& inputCopy,
                const LDefinition& floatCopy,
                const LDefinition& jumpTablePointer, MTableSwitch* ins)
      : LInstructionHelper(classOpcode) {
    setBoxOperand(InputValue, input);
    setTemp(0, inputCopy);
    setTemp(1, floatCopy);
    setTemp(2, jumpTablePointer);
    setMir(ins);
  }

  MTableSwitch* mir() const { return mir_->toTableSwitch(); }

  static const size_t InputValue = 0;

  const LDefinition* tempInt() { return getTemp(0); }
  const LDefinition* tempFloat() { return getTemp(1); }
  const LDefinition* tempPointer() { return getTemp(2); }
};

class LMulI : public LBinaryMath<0> {
 public:
  LIR_HEADER(MulI);

  LMulI() : LBinaryMath(classOpcode) {}

  MMul* mir() { return mir_->toMul(); }
};

class LUDivOrMod : public LBinaryMath<0> {
 public:
  LIR_HEADER(UDivOrMod);

  LUDivOrMod() : LBinaryMath(classOpcode) {}

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

  wasm::BytecodeOffset bytecodeOffset() const {
    MOZ_ASSERT(mir_->isDiv() || mir_->isMod());
    if (mir_->isMod()) {
      return mir_->toMod()->bytecodeOffset();
    }
    return mir_->toDiv()->bytecodeOffset();
  }
};

class LWasmCompareExchangeI64
    : public LInstructionHelper<INT64_PIECES, 2 + INT64_PIECES + INT64_PIECES,
                                0> {
 public:
  LIR_HEADER(WasmCompareExchangeI64);

  LWasmCompareExchangeI64(const LAllocation& ptr,
                          const LInt64Allocation& oldValue,
                          const LInt64Allocation& newValue,
                          const LAllocation& memoryBase)
      : LInstructionHelper(classOpcode) {
    setOperand(0, ptr);
    setInt64Operand(1, oldValue);
    setInt64Operand(1 + INT64_PIECES, newValue);
    setOperand(1 + 2 * INT64_PIECES, memoryBase);
  }

  const LAllocation* ptr() { return getOperand(0); }
  const LInt64Allocation oldValue() { return getInt64Operand(1); }
  const LInt64Allocation newValue() {
    return getInt64Operand(1 + INT64_PIECES);
  }
  const LAllocation* memoryBase() { return getOperand(1 + 2 * INT64_PIECES); }
  const MWasmCompareExchangeHeap* mir() const {
    return mir_->toWasmCompareExchangeHeap();
  }
};

class LWasmAtomicExchangeI64
    : public LInstructionHelper<INT64_PIECES, 2 + INT64_PIECES, 0> {
 public:
  LIR_HEADER(WasmAtomicExchangeI64);

  LWasmAtomicExchangeI64(const LAllocation& ptr, const LInt64Allocation& value,
                         const LAllocation& memoryBase)
      : LInstructionHelper(classOpcode) {
    setOperand(0, ptr);
    setInt64Operand(1, value);
    setOperand(1 + INT64_PIECES, memoryBase);
  }

  const LAllocation* ptr() { return getOperand(0); }
  const LInt64Allocation value() { return getInt64Operand(1); }
  const LAllocation* memoryBase() { return getOperand(1 + INT64_PIECES); }
  const MWasmAtomicExchangeHeap* mir() const {
    return mir_->toWasmAtomicExchangeHeap();
  }
};

class LWasmAtomicBinopI64
    : public LInstructionHelper<INT64_PIECES, 2 + INT64_PIECES, 2> {
 public:
  LIR_HEADER(WasmAtomicBinopI64);

  LWasmAtomicBinopI64(const LAllocation& ptr, const LInt64Allocation& value,
                      const LAllocation& memoryBase)
      : LInstructionHelper(classOpcode) {
    setOperand(0, ptr);
    setInt64Operand(1, value);
    setOperand(1 + INT64_PIECES, memoryBase);
  }

  const LAllocation* ptr() { return getOperand(0); }
  const LInt64Allocation value() { return getInt64Operand(1); }
  const LAllocation* memoryBase() { return getOperand(1 + INT64_PIECES); }
  const MWasmAtomicBinopHeap* mir() const {
    return mir_->toWasmAtomicBinopHeap();
  }
};

class LDivOrModI64 : public LBinaryMath<1> {
 public:
  LIR_HEADER(DivOrModI64)

  LDivOrModI64(const LAllocation& lhs, const LAllocation& rhs,
               const LDefinition& temp)
      : LBinaryMath(classOpcode) {
    setOperand(0, lhs);
    setOperand(1, rhs);
    setTemp(0, temp);
  }

  const LDefinition* remainder() { return getTemp(0); }
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
  bool canBeNegativeOverflow() const {
    if (mir_->isMod()) {
      return mir_->toMod()->canBeNegativeDividend();
    }
    return mir_->toDiv()->canBeNegativeOverflow();
  }
  wasm::BytecodeOffset bytecodeOffset() const {
    MOZ_ASSERT(mir_->isDiv() || mir_->isMod());
    if (mir_->isMod()) {
      return mir_->toMod()->bytecodeOffset();
    }
    return mir_->toDiv()->bytecodeOffset();
  }
};

class LUDivOrModI64 : public LBinaryMath<1> {
 public:
  LIR_HEADER(UDivOrModI64);

  LUDivOrModI64(const LAllocation& lhs, const LAllocation& rhs,
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
  wasm::BytecodeOffset bytecodeOffset() const {
    MOZ_ASSERT(mir_->isDiv() || mir_->isMod());
    if (mir_->isMod()) {
      return mir_->toMod()->bytecodeOffset();
    }
    return mir_->toDiv()->bytecodeOffset();
  }
};

class LWasmTruncateToInt64 : public LInstructionHelper<1, 1, 0> {
 public:
  LIR_HEADER(WasmTruncateToInt64);

  explicit LWasmTruncateToInt64(const LAllocation& in)
      : LInstructionHelper(classOpcode) {
    setOperand(0, in);
  }

  MWasmTruncateToInt64* mir() const { return mir_->toWasmTruncateToInt64(); }
};

class LInt64ToFloatingPoint : public LInstructionHelper<1, 1, 0> {
 public:
  LIR_HEADER(Int64ToFloatingPoint);

  explicit LInt64ToFloatingPoint(const LInt64Allocation& in)
      : LInstructionHelper(classOpcode) {
    setInt64Operand(0, in);
  }

  MInt64ToFloatingPoint* mir() const { return mir_->toInt64ToFloatingPoint(); }
};

}  // namespace jit
}  // namespace js

#endif /* jit_riscv64_LIR_riscv64_h */
