/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_none_Assembler_none_h
#define jit_none_Assembler_none_h

#include "mozilla/Assertions.h"

#include <stdint.h>

#include "jit/none/Architecture-none.h"
#include "jit/Registers.h"
#include "jit/RegisterSets.h"
#include "jit/shared/Assembler-shared.h"

namespace js {
namespace jit {

class MacroAssembler;

static constexpr Register StackPointer{Registers::invalid_reg};
static constexpr Register FramePointer{Registers::invalid_reg};
static constexpr Register ReturnReg{Registers::invalid_reg2};
static constexpr FloatRegister ReturnFloat32Reg = {FloatRegisters::invalid_reg};
static constexpr FloatRegister ReturnDoubleReg = {FloatRegisters::invalid_reg};
static constexpr FloatRegister ReturnSimd128Reg = {FloatRegisters::invalid_reg};
static constexpr FloatRegister ScratchSimd128Reg = {
    FloatRegisters::invalid_reg};
static constexpr FloatRegister InvalidFloatReg = {FloatRegisters::invalid_reg};

struct ScratchFloat32Scope : FloatRegister {
  explicit ScratchFloat32Scope(MacroAssembler& masm) {}
};

struct ScratchDoubleScope : FloatRegister {
  explicit ScratchDoubleScope(MacroAssembler& masm) {}
};

static constexpr Register OsrFrameReg{Registers::invalid_reg};
static constexpr Register PreBarrierReg{Registers::invalid_reg};
static constexpr Register InterpreterPCReg{Registers::invalid_reg};
static constexpr Register CallTempReg0{Registers::invalid_reg};
static constexpr Register CallTempReg1{Registers::invalid_reg};
static constexpr Register CallTempReg2{Registers::invalid_reg};
static constexpr Register CallTempReg3{Registers::invalid_reg};
static constexpr Register CallTempReg4{Registers::invalid_reg};
static constexpr Register CallTempReg5{Registers::invalid_reg};
static constexpr Register InvalidReg{Registers::invalid_reg};
static constexpr Register CallTempNonArgRegs[] = {InvalidReg, InvalidReg};
static const uint32_t NumCallTempNonArgRegs = std::size(CallTempNonArgRegs);

static constexpr Register IntArgReg0{Registers::invalid_reg};
static constexpr Register IntArgReg1{Registers::invalid_reg};
static constexpr Register IntArgReg2{Registers::invalid_reg};
static constexpr Register IntArgReg3{Registers::invalid_reg};
static constexpr Register HeapReg{Registers::invalid_reg};

static constexpr Register RegExpMatcherRegExpReg{Registers::invalid_reg};
static constexpr Register RegExpMatcherStringReg{Registers::invalid_reg};
static constexpr Register RegExpMatcherLastIndexReg{Registers::invalid_reg};

static constexpr Register RegExpExecTestRegExpReg{Registers::invalid_reg};
static constexpr Register RegExpExecTestStringReg{Registers::invalid_reg};

static constexpr Register RegExpSearcherRegExpReg{Registers::invalid_reg};
static constexpr Register RegExpSearcherStringReg{Registers::invalid_reg};
static constexpr Register RegExpSearcherLastIndexReg{Registers::invalid_reg};

// Uses |invalid_reg2| to avoid static_assert failures.
static constexpr Register JSReturnReg_Type{Registers::invalid_reg2};
static constexpr Register JSReturnReg_Data{Registers::invalid_reg2};
static constexpr Register JSReturnReg{Registers::invalid_reg2};

#if defined(JS_NUNBOX32)
static constexpr ValueOperand JSReturnOperand(InvalidReg, InvalidReg);
static constexpr Register64 ReturnReg64(InvalidReg, InvalidReg);
#elif defined(JS_PUNBOX64)
static constexpr ValueOperand JSReturnOperand(InvalidReg);
static constexpr Register64 ReturnReg64(InvalidReg);
#else
#  error "Bad architecture"
#endif

static constexpr Register ABINonArgReg0{Registers::invalid_reg};
static constexpr Register ABINonArgReg1{Registers::invalid_reg};
static constexpr Register ABINonArgReg2{Registers::invalid_reg};
static constexpr Register ABINonArgReg3{Registers::invalid_reg};
static constexpr Register ABINonArgReturnReg0{Registers::invalid_reg};
static constexpr Register ABINonArgReturnReg1{Registers::invalid_reg};
static constexpr Register ABINonVolatileReg{Registers::invalid_reg};
static constexpr Register ABINonArgReturnVolatileReg{Registers::invalid_reg};

static constexpr FloatRegister ABINonArgDoubleReg = {
    FloatRegisters::invalid_reg};

static constexpr Register WasmTableCallScratchReg0{Registers::invalid_reg};
static constexpr Register WasmTableCallScratchReg1{Registers::invalid_reg};
static constexpr Register WasmTableCallSigReg{Registers::invalid_reg};
static constexpr Register WasmTableCallIndexReg{Registers::invalid_reg};
static constexpr Register InstanceReg{Registers::invalid_reg};
static constexpr Register WasmJitEntryReturnScratch{Registers::invalid_reg};
static constexpr Register WasmCallRefCallScratchReg0{Registers::invalid_reg};
static constexpr Register WasmCallRefCallScratchReg1{Registers::invalid_reg};
static constexpr Register WasmCallRefReg{Registers::invalid_reg};
static constexpr Register WasmTailCallInstanceScratchReg{
    Registers::invalid_reg};
static constexpr Register WasmTailCallRAScratchReg{Registers::invalid_reg};
static constexpr Register WasmTailCallFPScratchReg{Registers::invalid_reg};

static constexpr uint32_t ABIStackAlignment = 4;
static constexpr uint32_t CodeAlignment = 16;
#ifdef ENABLE_PORTABLE_BASELINE_INTERP
static constexpr uint32_t JitStackAlignment = sizeof(void*);
static constexpr uint32_t JitStackValueAlignment = 1;
#else
static constexpr uint32_t JitStackAlignment = 8;
static constexpr uint32_t JitStackValueAlignment =
    JitStackAlignment / sizeof(Value);
#endif

static const Scale ScalePointer = TimesOne;

class Assembler : public AssemblerShared {
 public:
  enum Condition {
    Equal,
    NotEqual,
    Above,
    AboveOrEqual,
    Below,
    BelowOrEqual,
    GreaterThan,
    GreaterThanOrEqual,
    LessThan,
    LessThanOrEqual,
    Overflow,
    CarrySet,
    CarryClear,
    Signed,
    NotSigned,
    Zero,
    NonZero,
    Always,
  };

  enum DoubleCondition {
    DoubleOrdered,
    DoubleEqual,
    DoubleNotEqual,
    DoubleGreaterThan,
    DoubleGreaterThanOrEqual,
    DoubleLessThan,
    DoubleLessThanOrEqual,
    DoubleUnordered,
    DoubleEqualOrUnordered,
    DoubleNotEqualOrUnordered,
    DoubleGreaterThanOrUnordered,
    DoubleGreaterThanOrEqualOrUnordered,
    DoubleLessThanOrUnordered,
    DoubleLessThanOrEqualOrUnordered
  };

  static Condition InvertCondition(Condition) { MOZ_CRASH(); }

  static DoubleCondition InvertCondition(DoubleCondition) { MOZ_CRASH(); }

  template <typename T, typename S>
  static void PatchDataWithValueCheck(CodeLocationLabel, T, S) {
    MOZ_CRASH();
  }
  static void PatchWrite_Imm32(CodeLocationLabel, Imm32) { MOZ_CRASH(); }

  static void PatchWrite_NearCall(CodeLocationLabel, CodeLocationLabel) {
    MOZ_CRASH();
  }
  static uint32_t PatchWrite_NearCallSize() { MOZ_CRASH(); }

  static void ToggleToJmp(CodeLocationLabel) { MOZ_CRASH(); }
  static void ToggleToCmp(CodeLocationLabel) { MOZ_CRASH(); }
  static void ToggleCall(CodeLocationLabel, bool) { MOZ_CRASH(); }

  static void Bind(uint8_t*, const CodeLabel&) { MOZ_CRASH(); }

  static uintptr_t GetPointer(uint8_t*) { MOZ_CRASH(); }

  static bool HasRoundInstruction(RoundingMode) { return false; }

  void verifyHeapAccessDisassembly(uint32_t begin, uint32_t end,
                                   const Disassembler::HeapAccess& heapAccess) {
    MOZ_CRASH();
  }

  void setUnlimitedBuffer() { MOZ_CRASH(); }
};

class Operand {
 public:
  explicit Operand(const Address&) { MOZ_CRASH(); }
  explicit Operand(const Register) { MOZ_CRASH(); }
  explicit Operand(const FloatRegister) { MOZ_CRASH(); }
  explicit Operand(Register, Imm32) { MOZ_CRASH(); }
  explicit Operand(Register, int32_t) { MOZ_CRASH(); }
};

class ABIArgGenerator {
 public:
  ABIArgGenerator() { MOZ_CRASH(); }
  ABIArg next(MIRType) { MOZ_CRASH(); }
  ABIArg& current() { MOZ_CRASH(); }
  uint32_t stackBytesConsumedSoFar() const { MOZ_CRASH(); }
  void increaseStackOffset(uint32_t) { MOZ_CRASH(); }
};

}  // namespace jit
}  // namespace js

#endif /* jit_none_Assembler_none_h */
