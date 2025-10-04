/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_arm64_LIR_arm64_h
#define jit_arm64_LIR_arm64_h

namespace js {
namespace jit {

class LUnboxBase : public LInstructionHelper<1, 1, 0> {
 public:
  LUnboxBase(LNode::Opcode opcode, const LAllocation& input)
      : LInstructionHelper(opcode) {
    setOperand(0, input);
  }

  static const size_t Input = 0;

  MUnbox* mir() const { return mir_->toUnbox(); }
};

class LUnbox : public LUnboxBase {
 public:
  LIR_HEADER(Unbox);

  explicit LUnbox(const LAllocation& input) : LUnboxBase(classOpcode, input) {}

  const char* extraName() const { return StringFromMIRType(mir()->type()); }
};

class LUnboxFloatingPoint : public LUnboxBase {
  MIRType type_;

 public:
  LIR_HEADER(UnboxFloatingPoint);

  LUnboxFloatingPoint(const LAllocation& input, MIRType type)
      : LUnboxBase(classOpcode, input), type_(type) {}

  MIRType type() const { return type_; }
  const char* extraName() const { return StringFromMIRType(type_); }
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

class LDivPowTwoI : public LInstructionHelper<1, 1, 0> {
  const int32_t shift_;
  const bool negativeDivisor_;

 public:
  LIR_HEADER(DivPowTwoI)

  LDivPowTwoI(const LAllocation& lhs, int32_t shift, bool negativeDivisor)
      : LInstructionHelper(classOpcode),
        shift_(shift),
        negativeDivisor_(negativeDivisor) {
    setOperand(0, lhs);
  }

  const LAllocation* numerator() { return getOperand(0); }

  int32_t shift() { return shift_; }
  bool negativeDivisor() { return negativeDivisor_; }

  MDiv* mir() const { return mir_->toDiv(); }
};

class LDivConstantI : public LInstructionHelper<1, 1, 1> {
  const int32_t denominator_;

 public:
  LIR_HEADER(DivConstantI)

  LDivConstantI(const LAllocation& lhs, int32_t denominator,
                const LDefinition& temp)
      : LInstructionHelper(classOpcode), denominator_(denominator) {
    setOperand(0, lhs);
    setTemp(0, temp);
  }

  const LAllocation* numerator() { return getOperand(0); }
  const LDefinition* temp() { return getTemp(0); }
  int32_t denominator() const { return denominator_; }
  MDiv* mir() const { return mir_->toDiv(); }
  bool canBeNegativeDividend() const { return mir()->canBeNegativeDividend(); }
};

class LUDivConstantI : public LInstructionHelper<1, 1, 1> {
  const int32_t denominator_;

 public:
  LIR_HEADER(UDivConstantI)

  LUDivConstantI(const LAllocation& lhs, int32_t denominator,
                 const LDefinition& temp)
      : LInstructionHelper(classOpcode), denominator_(denominator) {
    setOperand(0, lhs);
    setTemp(0, temp);
  }

  const LAllocation* numerator() { return getOperand(0); }
  const LDefinition* temp() { return getTemp(0); }
  int32_t denominator() const { return denominator_; }
  MDiv* mir() const { return mir_->toDiv(); }
};

class LModI : public LBinaryMath<0> {
 public:
  LIR_HEADER(ModI);

  LModI(const LAllocation& lhs, const LAllocation& rhs)
      : LBinaryMath(classOpcode) {
    setOperand(0, lhs);
    setOperand(1, rhs);
  }

  MMod* mir() const { return mir_->toMod(); }
};

class LModPowTwoI : public LInstructionHelper<1, 1, 0> {
  const int32_t shift_;

 public:
  LIR_HEADER(ModPowTwoI);
  int32_t shift() { return shift_; }

  LModPowTwoI(const LAllocation& lhs, int32_t shift)
      : LInstructionHelper(classOpcode), shift_(shift) {
    setOperand(0, lhs);
  }

  MMod* mir() const { return mir_->toMod(); }
};

class LModMaskI : public LInstructionHelper<1, 1, 2> {
  const int32_t shift_;

 public:
  LIR_HEADER(ModMaskI);

  LModMaskI(const LAllocation& lhs, const LDefinition& temp1,
            const LDefinition& temp2, int32_t shift)
      : LInstructionHelper(classOpcode), shift_(shift) {
    setOperand(0, lhs);
    setTemp(0, temp1);
    setTemp(1, temp2);
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

class LUDiv : public LBinaryMath<1> {
 public:
  LIR_HEADER(UDiv);

  LUDiv(const LAllocation& lhs, const LAllocation& rhs,
        const LDefinition& remainder)
      : LBinaryMath(classOpcode) {
    setOperand(0, lhs);
    setOperand(1, rhs);
    setTemp(0, remainder);
  }

  const LDefinition* remainder() { return getTemp(0); }

  MDiv* mir() { return mir_->toDiv(); }
};

class LUMod : public LBinaryMath<0> {
 public:
  LIR_HEADER(UMod);

  LUMod(const LAllocation& lhs, const LAllocation& rhs)
      : LBinaryMath(classOpcode) {
    setOperand(0, lhs);
    setOperand(1, rhs);
  }

  MMod* mir() { return mir_->toMod(); }
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

class LWasmTruncateToInt64 : public LInstructionHelper<1, 1, 0> {
 public:
  LIR_HEADER(WasmTruncateToInt64);

  explicit LWasmTruncateToInt64(const LAllocation& in)
      : LInstructionHelper(classOpcode) {
    setOperand(0, in);
  }

  MWasmTruncateToInt64* mir() const { return mir_->toWasmTruncateToInt64(); }
};

class LDivOrModI64 : public LBinaryMath<0> {
 public:
  LIR_HEADER(DivOrModI64)

  LDivOrModI64(const LAllocation& lhs, const LAllocation& rhs)
      : LBinaryMath(classOpcode) {
    setOperand(0, lhs);
    setOperand(1, rhs);
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

class LUDivOrModI64 : public LBinaryMath<0> {
 public:
  LIR_HEADER(UDivOrModI64);

  LUDivOrModI64(const LAllocation& lhs, const LAllocation& rhs)
      : LBinaryMath(classOpcode) {
    setOperand(0, lhs);
    setOperand(1, rhs);
  }

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

}  // namespace jit
}  // namespace js

#endif /* jit_arm64_LIR_arm64_h */
