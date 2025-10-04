/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_mips64_Assembler_mips64_h
#define jit_mips64_Assembler_mips64_h

#include <iterator>

#include "jit/mips-shared/Assembler-mips-shared.h"

#include "jit/mips64/Architecture-mips64.h"

namespace js {
namespace jit {

static constexpr Register CallTempReg4 = a4;
static constexpr Register CallTempReg5 = a5;

static constexpr Register CallTempNonArgRegs[] = {t0, t1, t2, t3};
static const uint32_t NumCallTempNonArgRegs = std::size(CallTempNonArgRegs);

class ABIArgGenerator {
  unsigned regIndex_;
  uint32_t stackOffset_;
  ABIArg current_;

 public:
  ABIArgGenerator();
  ABIArg next(MIRType argType);
  ABIArg& current() { return current_; }

  uint32_t stackBytesConsumedSoFar() const { return stackOffset_; }
  void increaseStackOffset(uint32_t bytes) { stackOffset_ += bytes; }
};

// These registers may be volatile or nonvolatile.
static constexpr Register ABINonArgReg0 = t0;
static constexpr Register ABINonArgReg1 = t1;
static constexpr Register ABINonArgReg2 = t2;
static constexpr Register ABINonArgReg3 = t3;

// This register may be volatile or nonvolatile. Avoid f23 which is the
// ScratchDoubleReg.
static constexpr FloatRegister ABINonArgDoubleReg{FloatRegisters::f21,
                                                  FloatRegisters::Double};

// These registers may be volatile or nonvolatile.
// Note: these three registers are all guaranteed to be different
static constexpr Register ABINonArgReturnReg0 = t0;
static constexpr Register ABINonArgReturnReg1 = t1;
static constexpr Register ABINonVolatileReg = s0;

// This register is guaranteed to be clobberable during the prologue and
// epilogue of an ABI call which must preserve both ABI argument, return
// and non-volatile registers.
static constexpr Register ABINonArgReturnVolatileReg = t0;

// TLS pointer argument register for WebAssembly functions. This must not alias
// any other register used for passing function arguments or return values.
// Preserved by WebAssembly functions.
static constexpr Register InstanceReg = s5;

// Registers used for wasm table calls. These registers must be disjoint
// from the ABI argument registers, InstanceReg and each other.
static constexpr Register WasmTableCallScratchReg0 = ABINonArgReg0;
static constexpr Register WasmTableCallScratchReg1 = ABINonArgReg1;
static constexpr Register WasmTableCallSigReg = ABINonArgReg2;
static constexpr Register WasmTableCallIndexReg = ABINonArgReg3;

// Registers used for ref calls.
static constexpr Register WasmCallRefCallScratchReg0 = ABINonArgReg0;
static constexpr Register WasmCallRefCallScratchReg1 = ABINonArgReg1;
static constexpr Register WasmCallRefReg = ABINonArgReg3;

// Registers used for wasm tail calls operations.
static constexpr Register WasmTailCallInstanceScratchReg = ABINonArgReg1;
static constexpr Register WasmTailCallRAScratchReg = ra;
static constexpr Register WasmTailCallFPScratchReg = ABINonArgReg3;

// Register used as a scratch along the return path in the fast js -> wasm stub
// code. This must not overlap ReturnReg, JSReturnOperand, or InstanceReg.
// It must be a volatile register.
static constexpr Register WasmJitEntryReturnScratch = t1;

static constexpr Register InterpreterPCReg = t5;

static constexpr Register JSReturnReg = v1;
static constexpr Register JSReturnReg_Type = JSReturnReg;
static constexpr Register JSReturnReg_Data = JSReturnReg;
static constexpr Register64 ReturnReg64(ReturnReg);
static constexpr FloatRegister ReturnFloat32Reg = {FloatRegisters::f0,
                                                   FloatRegisters::Single};
static constexpr FloatRegister ReturnDoubleReg = {FloatRegisters::f0,
                                                  FloatRegisters::Double};
static constexpr FloatRegister ScratchFloat32Reg = {FloatRegisters::f23,
                                                    FloatRegisters::Single};
static constexpr FloatRegister ScratchDoubleReg = {FloatRegisters::f23,
                                                   FloatRegisters::Double};

struct ScratchFloat32Scope : public AutoFloatRegisterScope {
  explicit ScratchFloat32Scope(MacroAssembler& masm)
      : AutoFloatRegisterScope(masm, ScratchFloat32Reg) {}
};

struct ScratchDoubleScope : public AutoFloatRegisterScope {
  explicit ScratchDoubleScope(MacroAssembler& masm)
      : AutoFloatRegisterScope(masm, ScratchDoubleReg) {}
};

static constexpr FloatRegister f0 = {FloatRegisters::f0,
                                     FloatRegisters::Double};
static constexpr FloatRegister f1 = {FloatRegisters::f1,
                                     FloatRegisters::Double};
static constexpr FloatRegister f2 = {FloatRegisters::f2,
                                     FloatRegisters::Double};
static constexpr FloatRegister f3 = {FloatRegisters::f3,
                                     FloatRegisters::Double};
static constexpr FloatRegister f4 = {FloatRegisters::f4,
                                     FloatRegisters::Double};
static constexpr FloatRegister f5 = {FloatRegisters::f5,
                                     FloatRegisters::Double};
static constexpr FloatRegister f6 = {FloatRegisters::f6,
                                     FloatRegisters::Double};
static constexpr FloatRegister f7 = {FloatRegisters::f7,
                                     FloatRegisters::Double};
static constexpr FloatRegister f8 = {FloatRegisters::f8,
                                     FloatRegisters::Double};
static constexpr FloatRegister f9 = {FloatRegisters::f9,
                                     FloatRegisters::Double};
static constexpr FloatRegister f10 = {FloatRegisters::f10,
                                      FloatRegisters::Double};
static constexpr FloatRegister f11 = {FloatRegisters::f11,
                                      FloatRegisters::Double};
static constexpr FloatRegister f12 = {FloatRegisters::f12,
                                      FloatRegisters::Double};
static constexpr FloatRegister f13 = {FloatRegisters::f13,
                                      FloatRegisters::Double};
static constexpr FloatRegister f14 = {FloatRegisters::f14,
                                      FloatRegisters::Double};
static constexpr FloatRegister f15 = {FloatRegisters::f15,
                                      FloatRegisters::Double};
static constexpr FloatRegister f16 = {FloatRegisters::f16,
                                      FloatRegisters::Double};
static constexpr FloatRegister f17 = {FloatRegisters::f17,
                                      FloatRegisters::Double};
static constexpr FloatRegister f18 = {FloatRegisters::f18,
                                      FloatRegisters::Double};
static constexpr FloatRegister f19 = {FloatRegisters::f19,
                                      FloatRegisters::Double};
static constexpr FloatRegister f20 = {FloatRegisters::f20,
                                      FloatRegisters::Double};
static constexpr FloatRegister f21 = {FloatRegisters::f21,
                                      FloatRegisters::Double};
static constexpr FloatRegister f22 = {FloatRegisters::f22,
                                      FloatRegisters::Double};
static constexpr FloatRegister f23 = {FloatRegisters::f23,
                                      FloatRegisters::Double};
static constexpr FloatRegister f24 = {FloatRegisters::f24,
                                      FloatRegisters::Double};
static constexpr FloatRegister f25 = {FloatRegisters::f25,
                                      FloatRegisters::Double};
static constexpr FloatRegister f26 = {FloatRegisters::f26,
                                      FloatRegisters::Double};
static constexpr FloatRegister f27 = {FloatRegisters::f27,
                                      FloatRegisters::Double};
static constexpr FloatRegister f28 = {FloatRegisters::f28,
                                      FloatRegisters::Double};
static constexpr FloatRegister f29 = {FloatRegisters::f29,
                                      FloatRegisters::Double};
static constexpr FloatRegister f30 = {FloatRegisters::f30,
                                      FloatRegisters::Double};
static constexpr FloatRegister f31 = {FloatRegisters::f31,
                                      FloatRegisters::Double};

// MIPS64 CPUs can only load multibyte data that is "naturally"
// eight-byte-aligned, sp register should be sixteen-byte-aligned.
static constexpr uint32_t ABIStackAlignment = 16;
static constexpr uint32_t JitStackAlignment = 16;

static constexpr uint32_t JitStackValueAlignment =
    JitStackAlignment / sizeof(Value);
static_assert(JitStackAlignment % sizeof(Value) == 0 &&
                  JitStackValueAlignment >= 1,
              "Stack alignment should be a non-zero multiple of sizeof(Value)");

// TODO this is just a filler to prevent a build failure. The MIPS SIMD
// alignment requirements still need to be explored.
// TODO Copy the static_asserts from x64/x86 assembler files.
static constexpr uint32_t SimdMemoryAlignment = 16;

static constexpr uint32_t WasmStackAlignment = SimdMemoryAlignment;
static const uint32_t WasmTrapInstructionLength = 4;

// See comments in wasm::GenerateFunctionPrologue.  The difference between these
// is the size of the largest callable prologue on the platform.
static constexpr uint32_t WasmCheckedCallEntryOffset = 0u;

static constexpr Scale ScalePointer = TimesEight;

class Assembler : public AssemblerMIPSShared {
 public:
  Assembler() : AssemblerMIPSShared() {}

  static uintptr_t GetPointer(uint8_t*);

  using AssemblerMIPSShared::bind;

  static void Bind(uint8_t* rawCode, const CodeLabel& label);

  void processCodeLabels(uint8_t* rawCode);

  static void TraceJumpRelocations(JSTracer* trc, JitCode* code,
                                   CompactBufferReader& reader);
  static void TraceDataRelocations(JSTracer* trc, JitCode* code,
                                   CompactBufferReader& reader);

  void bind(InstImm* inst, uintptr_t branch, uintptr_t target);

  // Copy the assembly code to the given buffer, and perform any pending
  // relocations relying on the target address.
  void executableCopy(uint8_t* buffer);

  static uint32_t PatchWrite_NearCallSize();

  static uint64_t ExtractLoad64Value(Instruction* inst0);
  static void UpdateLoad64Value(Instruction* inst0, uint64_t value);
  static void WriteLoad64Instructions(Instruction* inst0, Register reg,
                                      uint64_t value);

  static void PatchWrite_NearCall(CodeLocationLabel start,
                                  CodeLocationLabel toCall);
  static void PatchDataWithValueCheck(CodeLocationLabel label, ImmPtr newValue,
                                      ImmPtr expectedValue);
  static void PatchDataWithValueCheck(CodeLocationLabel label,
                                      PatchedImmPtr newValue,
                                      PatchedImmPtr expectedValue);

  static uint64_t ExtractInstructionImmediate(uint8_t* code);

  static void ToggleCall(CodeLocationLabel inst_, bool enabled);
};  // Assembler

static const uint32_t NumIntArgRegs = 8;
static const uint32_t NumFloatArgRegs = NumIntArgRegs;

static inline bool GetIntArgReg(uint32_t usedArgSlots, Register* out) {
  if (usedArgSlots < NumIntArgRegs) {
    *out = Register::FromCode(a0.code() + usedArgSlots);
    return true;
  }
  return false;
}

static inline bool GetFloatArgReg(uint32_t usedArgSlots, FloatRegister* out) {
  if (usedArgSlots < NumFloatArgRegs) {
    *out = FloatRegister::FromCode(f12.code() + usedArgSlots);
    return true;
  }
  return false;
}

// Get a register in which we plan to put a quantity that will be used as an
// integer argument. This differs from GetIntArgReg in that if we have no more
// actual argument registers to use we will fall back on using whatever
// CallTempReg* don't overlap the argument registers, and only fail once those
// run out too.
static inline bool GetTempRegForIntArg(uint32_t usedIntArgs,
                                       uint32_t usedFloatArgs, Register* out) {
  // NOTE: We can't properly determine which regs are used if there are
  // float arguments. If this is needed, we will have to guess.
  MOZ_ASSERT(usedFloatArgs == 0);

  if (GetIntArgReg(usedIntArgs, out)) {
    return true;
  }
  // Unfortunately, we have to assume things about the point at which
  // GetIntArgReg returns false, because we need to know how many registers it
  // can allocate.
  usedIntArgs -= NumIntArgRegs;
  if (usedIntArgs >= NumCallTempNonArgRegs) {
    return false;
  }
  *out = CallTempNonArgRegs[usedIntArgs];
  return true;
}

}  // namespace jit
}  // namespace js

#endif /* jit_mips64_Assembler_mips64_h */
