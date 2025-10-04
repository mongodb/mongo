/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x86_LIR_x86_h
#define jit_x86_LIR_x86_h

namespace js {
namespace jit {

class LBoxFloatingPoint : public LInstructionHelper<2, 1, 2> {
  MIRType type_;

 public:
  LIR_HEADER(BoxFloatingPoint);

  LBoxFloatingPoint(const LAllocation& in, const LDefinition& temp,
                    const LDefinition& spectreTemp, MIRType type)
      : LInstructionHelper(classOpcode), type_(type) {
    MOZ_ASSERT(IsFloatingPointType(type));
    setOperand(0, in);
    setTemp(0, temp);
    setTemp(1, spectreTemp);
  }

  const LDefinition* spectreTemp() { return getTemp(1); }

  MIRType type() const { return type_; }
  const char* extraName() const { return StringFromMIRType(type_); }
};

class LUnbox : public LInstructionHelper<1, 2, 0> {
 public:
  LIR_HEADER(Unbox);

  LUnbox() : LInstructionHelper(classOpcode) {}

  MUnbox* mir() const { return mir_->toUnbox(); }
  const LAllocation* payload() { return getOperand(0); }
  const LAllocation* type() { return getOperand(1); }
  const char* extraName() const { return StringFromMIRType(mir()->type()); }
};

class LUnboxFloatingPoint : public LInstructionHelper<1, 2, 0> {
  MIRType type_;

 public:
  LIR_HEADER(UnboxFloatingPoint);

  static const size_t Input = 0;

  LUnboxFloatingPoint(const LBoxAllocation& input, MIRType type)
      : LInstructionHelper(classOpcode), type_(type) {
    setBoxOperand(Input, input);
  }

  MUnbox* mir() const { return mir_->toUnbox(); }

  MIRType type() const { return type_; }
  const char* extraName() const { return StringFromMIRType(type_); }
};

// Convert a 32-bit unsigned integer to a double.
class LWasmUint32ToDouble : public LInstructionHelper<1, 1, 1> {
 public:
  LIR_HEADER(WasmUint32ToDouble)

  LWasmUint32ToDouble(const LAllocation& input, const LDefinition& temp)
      : LInstructionHelper(classOpcode) {
    setOperand(0, input);
    setTemp(0, temp);
  }
  const LDefinition* temp() { return getTemp(0); }
};

// Convert a 32-bit unsigned integer to a float32.
class LWasmUint32ToFloat32 : public LInstructionHelper<1, 1, 1> {
 public:
  LIR_HEADER(WasmUint32ToFloat32)

  LWasmUint32ToFloat32(const LAllocation& input, const LDefinition& temp)
      : LInstructionHelper(classOpcode) {
    setOperand(0, input);
    setTemp(0, temp);
  }
  const LDefinition* temp() { return getTemp(0); }
};

class LDivOrModI64
    : public LCallInstructionHelper<INT64_PIECES, INT64_PIECES * 2 + 1, 0> {
 public:
  LIR_HEADER(DivOrModI64)

  static const size_t Lhs = 0;
  static const size_t Rhs = INT64_PIECES;
  static const size_t Instance = 2 * INT64_PIECES;

  LDivOrModI64(const LInt64Allocation& lhs, const LInt64Allocation& rhs,
               const LAllocation& instance)
      : LCallInstructionHelper(classOpcode) {
    setInt64Operand(Lhs, lhs);
    setInt64Operand(Rhs, rhs);
    setOperand(Instance, instance);
  }

  MDefinition* mir() const {
    MOZ_ASSERT(mir_->isWasmBuiltinDivI64() || mir_->isWasmBuiltinModI64());
    return mir_;
  }
  bool canBeDivideByZero() const {
    if (mir_->isWasmBuiltinModI64()) {
      return mir_->toWasmBuiltinModI64()->canBeDivideByZero();
    }
    return mir_->toWasmBuiltinDivI64()->canBeDivideByZero();
  }
  bool canBeNegativeOverflow() const {
    if (mir_->isWasmBuiltinModI64()) {
      return mir_->toWasmBuiltinModI64()->canBeNegativeDividend();
    }
    return mir_->toWasmBuiltinDivI64()->canBeNegativeOverflow();
  }
  wasm::BytecodeOffset bytecodeOffset() const {
    MOZ_ASSERT(mir_->isWasmBuiltinDivI64() || mir_->isWasmBuiltinModI64());
    if (mir_->isWasmBuiltinModI64()) {
      return mir_->toWasmBuiltinModI64()->bytecodeOffset();
    }
    return mir_->toWasmBuiltinDivI64()->bytecodeOffset();
  }
};

class LUDivOrModI64
    : public LCallInstructionHelper<INT64_PIECES, INT64_PIECES * 2 + 1, 0> {
 public:
  LIR_HEADER(UDivOrModI64)

  static const size_t Lhs = 0;
  static const size_t Rhs = INT64_PIECES;
  static const size_t Instance = 2 * INT64_PIECES;

  LUDivOrModI64(const LInt64Allocation& lhs, const LInt64Allocation& rhs,
                const LAllocation& instance)
      : LCallInstructionHelper(classOpcode) {
    setInt64Operand(Lhs, lhs);
    setInt64Operand(Rhs, rhs);
    setOperand(Instance, instance);
  }

  MDefinition* mir() const {
    MOZ_ASSERT(mir_->isWasmBuiltinDivI64() || mir_->isWasmBuiltinModI64());
    return mir_;
  }
  bool canBeDivideByZero() const {
    if (mir_->isWasmBuiltinModI64()) {
      return mir_->toWasmBuiltinModI64()->canBeDivideByZero();
    }
    return mir_->toWasmBuiltinDivI64()->canBeDivideByZero();
  }
  bool canBeNegativeOverflow() const {
    if (mir_->isWasmBuiltinModI64()) {
      return mir_->toWasmBuiltinModI64()->canBeNegativeDividend();
    }
    return mir_->toWasmBuiltinDivI64()->canBeNegativeOverflow();
  }
  wasm::BytecodeOffset bytecodeOffset() const {
    MOZ_ASSERT(mir_->isWasmBuiltinDivI64() || mir_->isWasmBuiltinModI64());
    if (mir_->isWasmBuiltinModI64()) {
      return mir_->toWasmBuiltinModI64()->bytecodeOffset();
    }
    return mir_->toWasmBuiltinDivI64()->bytecodeOffset();
  }
};

class LWasmTruncateToInt64 : public LInstructionHelper<INT64_PIECES, 1, 1> {
 public:
  LIR_HEADER(WasmTruncateToInt64);

  LWasmTruncateToInt64(const LAllocation& in, const LDefinition& temp)
      : LInstructionHelper(classOpcode) {
    setOperand(0, in);
    setTemp(0, temp);
  }

  MWasmTruncateToInt64* mir() const { return mir_->toWasmTruncateToInt64(); }

  const LDefinition* temp() { return getTemp(0); }
};

class LWasmAtomicLoadI64 : public LInstructionHelper<INT64_PIECES, 2, 2> {
 public:
  LIR_HEADER(WasmAtomicLoadI64);

  LWasmAtomicLoadI64(const LAllocation& memoryBase, const LAllocation& ptr,
                     const LDefinition& t1, const LDefinition& t2)
      : LInstructionHelper(classOpcode) {
    setOperand(0, memoryBase);
    setOperand(1, ptr);
    setTemp(0, t1);
    setTemp(1, t2);
  }

  MWasmLoad* mir() const { return mir_->toWasmLoad(); }
  const LAllocation* memoryBase() { return getOperand(0); }
  const LAllocation* ptr() { return getOperand(1); }
  const LDefinition* t1() { return getTemp(0); }
  const LDefinition* t2() { return getTemp(1); }
};

class LWasmAtomicStoreI64 : public LInstructionHelper<0, 2 + INT64_PIECES, 2> {
 public:
  LIR_HEADER(WasmAtomicStoreI64);

  LWasmAtomicStoreI64(const LAllocation& memoryBase, const LAllocation& ptr,
                      const LInt64Allocation& value, const LDefinition& t1,
                      const LDefinition& t2)
      : LInstructionHelper(classOpcode) {
    setOperand(0, memoryBase);
    setOperand(1, ptr);
    setInt64Operand(2, value);
    setTemp(0, t1);
    setTemp(1, t2);
  }

  MWasmStore* mir() const { return mir_->toWasmStore(); }
  const LAllocation* memoryBase() { return getOperand(0); }
  const LAllocation* ptr() { return getOperand(1); }
  const LInt64Allocation value() { return getInt64Operand(2); }
  const LDefinition* t1() { return getTemp(0); }
  const LDefinition* t2() { return getTemp(1); }
};

class LWasmCompareExchangeI64
    : public LInstructionHelper<INT64_PIECES, 2 + 2 * INT64_PIECES, 0> {
 public:
  LIR_HEADER(WasmCompareExchangeI64);

  LWasmCompareExchangeI64(const LAllocation& memoryBase, const LAllocation& ptr,
                          const LInt64Allocation& expected,
                          const LInt64Allocation& replacement)
      : LInstructionHelper(classOpcode) {
    setOperand(0, memoryBase);
    setOperand(1, ptr);
    setInt64Operand(2, expected);
    setInt64Operand(2 + INT64_PIECES, replacement);
  }

  MWasmCompareExchangeHeap* mir() const {
    return mir_->toWasmCompareExchangeHeap();
  }
  const LAllocation* memoryBase() { return getOperand(0); }
  const LAllocation* ptr() { return getOperand(1); }
  const LInt64Allocation expected() { return getInt64Operand(2); }
  const LInt64Allocation replacement() {
    return getInt64Operand(2 + INT64_PIECES);
  }
};

class LWasmAtomicExchangeI64
    : public LInstructionHelper<INT64_PIECES, 2 + INT64_PIECES, 0> {
  const wasm::MemoryAccessDesc& access_;

 public:
  LIR_HEADER(WasmAtomicExchangeI64);

  LWasmAtomicExchangeI64(const LAllocation& memoryBase, const LAllocation& ptr,
                         const LInt64Allocation& value,
                         const wasm::MemoryAccessDesc& access)
      : LInstructionHelper(classOpcode), access_(access) {
    setOperand(0, memoryBase);
    setOperand(1, ptr);
    setInt64Operand(2, value);
  }

  const LAllocation* memoryBase() { return getOperand(0); }
  const LAllocation* ptr() { return getOperand(1); }
  const LInt64Allocation value() { return getInt64Operand(2); }
  const wasm::MemoryAccessDesc& access() { return access_; }
};

class LWasmAtomicBinopI64
    : public LInstructionHelper<INT64_PIECES, 2 + INT64_PIECES, 0> {
  const wasm::MemoryAccessDesc& access_;
  AtomicOp op_;

 public:
  LIR_HEADER(WasmAtomicBinopI64);

  LWasmAtomicBinopI64(const LAllocation& memoryBase, const LAllocation& ptr,
                      const LInt64Allocation& value,
                      const wasm::MemoryAccessDesc& access, AtomicOp op)
      : LInstructionHelper(classOpcode), access_(access), op_(op) {
    setOperand(0, memoryBase);
    setOperand(1, ptr);
    setInt64Operand(2, value);
  }

  const LAllocation* memoryBase() { return getOperand(0); }
  const LAllocation* ptr() { return getOperand(1); }
  const LInt64Allocation value() { return getInt64Operand(2); }
  const wasm::MemoryAccessDesc& access() { return access_; }
  AtomicOp operation() const { return op_; }
};

}  // namespace jit
}  // namespace js

#endif /* jit_x86_LIR_x86_h */
