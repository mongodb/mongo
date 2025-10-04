/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_none_MacroAssembler_none_h
#define jit_none_MacroAssembler_none_h

#include <iterator>

#include "jit/MoveResolver.h"
#include "jit/none/Assembler-none.h"
#include "wasm/WasmCodegenTypes.h"
#include "wasm/WasmTypeDecls.h"

using js::wasm::FaultingCodeOffsetPair;

namespace js {
namespace jit {

class CompactBufferReader;

class ScratchTagScope {
 public:
  ScratchTagScope(MacroAssembler&, const ValueOperand) {}
  operator Register() { MOZ_CRASH(); }
  void release() { MOZ_CRASH(); }
  void reacquire() { MOZ_CRASH(); }
};

class ScratchTagScopeRelease {
 public:
  explicit ScratchTagScopeRelease(ScratchTagScope*) {}
};

class MacroAssemblerNone : public Assembler {
 public:
  MacroAssemblerNone() { MOZ_CRASH(); }

  MoveResolver moveResolver_;

  size_t size() const { MOZ_CRASH(); }
  size_t bytesNeeded() const { MOZ_CRASH(); }
  size_t jumpRelocationTableBytes() const { MOZ_CRASH(); }
  size_t dataRelocationTableBytes() const { MOZ_CRASH(); }
  size_t preBarrierTableBytes() const { MOZ_CRASH(); }

  size_t numCodeLabels() const { MOZ_CRASH(); }
  CodeLabel codeLabel(size_t) { MOZ_CRASH(); }

  bool reserve(size_t size) { MOZ_CRASH(); }
  bool appendRawCode(const uint8_t* code, size_t numBytes) { MOZ_CRASH(); }
  bool swapBuffer(wasm::Bytes& bytes) { MOZ_CRASH(); }

  void assertNoGCThings() const { MOZ_CRASH(); }

  static void TraceJumpRelocations(JSTracer*, JitCode*, CompactBufferReader&) {
    MOZ_CRASH();
  }
  static void TraceDataRelocations(JSTracer*, JitCode*, CompactBufferReader&) {
    MOZ_CRASH();
  }

  static bool SupportsFloatingPoint() { return false; }
  static bool SupportsUnalignedAccesses() { return false; }
  static bool SupportsFastUnalignedFPAccesses() { return false; }

  void executableCopy(void*, bool = true) { MOZ_CRASH(); }
  void copyJumpRelocationTable(uint8_t*) { MOZ_CRASH(); }
  void copyDataRelocationTable(uint8_t*) { MOZ_CRASH(); }
  void copyPreBarrierTable(uint8_t*) { MOZ_CRASH(); }
  void processCodeLabels(uint8_t*) { MOZ_CRASH(); }

  void flushBuffer() { MOZ_CRASH(); }

  template <typename T>
  void bind(T) {
    MOZ_CRASH();
  }
  template <typename T>
  void j(Condition, T) {
    MOZ_CRASH();
  }
  template <typename T>
  void jump(T) {
    MOZ_CRASH();
  }
  void writeCodePointer(CodeLabel* label) { MOZ_CRASH(); }
  void haltingAlign(size_t) { MOZ_CRASH(); }
  void nopAlign(size_t) { MOZ_CRASH(); }
  void checkStackAlignment() { MOZ_CRASH(); }
  uint32_t currentOffset() { MOZ_CRASH(); }

  void nop() { MOZ_CRASH(); }
  void breakpoint() { MOZ_CRASH(); }
  void abiret() { MOZ_CRASH(); }
  void ret() { MOZ_CRASH(); }

  CodeOffset toggledJump(Label*) { MOZ_CRASH(); }
  CodeOffset toggledCall(JitCode*, bool) { MOZ_CRASH(); }
  static size_t ToggledCallSize(uint8_t*) { MOZ_CRASH(); }

  void finish() { MOZ_CRASH(); }

  template <typename T, typename S>
  void moveValue(T, S) {
    MOZ_CRASH();
  }
  template <typename T, typename S, typename U>
  void moveValue(T, S, U) {
    MOZ_CRASH();
  }
  template <typename T, typename S>
  void storeValue(const T&, const S&) {
    MOZ_CRASH();
  }
  template <typename T, typename S, typename U>
  void storeValue(T, S, U) {
    MOZ_CRASH();
  }
  template <typename T, typename S>
  void storePrivateValue(const T&, const S&) {
    MOZ_CRASH();
  }
  template <typename T, typename S>
  void loadValue(T, S) {
    MOZ_CRASH();
  }
  template <typename T, typename S>
  void loadUnalignedValue(T, S) {
    MOZ_CRASH();
  }
  template <typename T>
  void pushValue(const T&) {
    MOZ_CRASH();
  }
  template <typename T, typename S>
  void pushValue(T, S) {
    MOZ_CRASH();
  }
  void popValue(ValueOperand) { MOZ_CRASH(); }
  void tagValue(JSValueType, Register, ValueOperand) { MOZ_CRASH(); }
  void retn(Imm32 n) { MOZ_CRASH(); }
  template <typename T>
  void push(const T&) {
    MOZ_CRASH();
  }
  template <typename T>
  void Push(T) {
    MOZ_CRASH();
  }
  template <typename T>
  void pop(T) {
    MOZ_CRASH();
  }
  template <typename T>
  void Pop(T) {
    MOZ_CRASH();
  }
  template <typename T>
  CodeOffset pushWithPatch(T) {
    MOZ_CRASH();
  }

  void testNullSet(Condition, ValueOperand, Register) { MOZ_CRASH(); }
  void testObjectSet(Condition, ValueOperand, Register) { MOZ_CRASH(); }
  void testUndefinedSet(Condition, ValueOperand, Register) { MOZ_CRASH(); }

  template <typename T, typename S>
  void cmpPtrSet(Condition, T, S, Register) {
    MOZ_CRASH();
  }
  void cmp8Set(Condition, Address, Imm32, Register) { MOZ_CRASH(); }
  void cmp16Set(Condition, Address, Imm32, Register) { MOZ_CRASH(); }
  template <typename T, typename S>
  void cmp32Set(Condition, T, S, Register) {
    MOZ_CRASH();
  }
  void cmp64Set(Condition, Address, Imm64, Register) { MOZ_CRASH(); }

  template <typename T>
  void mov(T, Register) {
    MOZ_CRASH();
  }
  template <typename T>
  void movePtr(T, Register) {
    MOZ_CRASH();
  }
  template <typename T>
  void move32(const T&, Register) {
    MOZ_CRASH();
  }
  template <typename T, typename S>
  void movq(T, S) {
    MOZ_CRASH();
  }
  template <typename T, typename S>
  void moveFloat32(T, S) {
    MOZ_CRASH();
  }
  template <typename T, typename S>
  void moveDouble(T, S) {
    MOZ_CRASH();
  }
  template <typename T, typename S>
  void move64(T, S) {
    MOZ_CRASH();
  }
  template <typename T>
  CodeOffset movWithPatch(T, Register) {
    MOZ_CRASH();
  }

  template <typename T>
  FaultingCodeOffset loadPtr(T, Register) {
    MOZ_CRASH();
  }
  template <typename T>
  FaultingCodeOffset load32(T, Register) {
    MOZ_CRASH();
  }
  template <typename T>
  void load32Unaligned(T, Register) {
    MOZ_CRASH();
  }
  template <typename T>
  FaultingCodeOffset loadFloat32(T, FloatRegister) {
    MOZ_CRASH();
  }
  template <typename T>
  FaultingCodeOffset loadDouble(T, FloatRegister) {
    MOZ_CRASH();
  }
  template <typename T>
  void loadPrivate(T, Register) {
    MOZ_CRASH();
  }
  template <typename T>
  FaultingCodeOffset load8SignExtend(T, Register) {
    MOZ_CRASH();
  }
  template <typename T>
  FaultingCodeOffset load8ZeroExtend(T, Register) {
    MOZ_CRASH();
  }
  template <typename T>
  FaultingCodeOffset load16SignExtend(T, Register) {
    MOZ_CRASH();
  }
  template <typename T>
  void load16UnalignedSignExtend(T, Register) {
    MOZ_CRASH();
  }
  template <typename T>
  FaultingCodeOffset load16ZeroExtend(T, Register) {
    MOZ_CRASH();
  }
  template <typename T>
  void load16UnalignedZeroExtend(T, Register) {
    MOZ_CRASH();
  }
#ifdef JS_64BIT
  template <typename T>
  FaultingCodeOffset load64(T, Register64) {
    MOZ_CRASH();
  }
#else
  template <typename T>
  FaultingCodeOffsetPair load64(T, Register64) {
    MOZ_CRASH();
  }
#endif
  template <typename T>
  void load64Unaligned(T, Register64) {
    MOZ_CRASH();
  }

  template <typename T, typename S>
  FaultingCodeOffset storePtr(const T&, S) {
    MOZ_CRASH();
  }
  template <typename T, typename S>
  FaultingCodeOffset store32(T, S) {
    MOZ_CRASH();
  }
  template <typename T, typename S>
  void store32Unaligned(T, S) {
    MOZ_CRASH();
  }
  template <typename T, typename S>
  void storeFloat32(T, S) {
    MOZ_CRASH();
  }
  template <typename T, typename S>
  void storeDouble(T, S) {
    MOZ_CRASH();
  }
  template <typename T, typename S>
  FaultingCodeOffset store8(T, S) {
    MOZ_CRASH();
  }
  template <typename T, typename S>
  FaultingCodeOffset store16(T, S) {
    MOZ_CRASH();
  }
  template <typename T, typename S>
  void store16Unaligned(T, S) {
    MOZ_CRASH();
  }
#ifdef JS_64BIT
  template <typename T, typename S>
  FaultingCodeOffset store64(T, S) {
    MOZ_CRASH();
  }
#else
  template <typename T, typename S>
  FaultingCodeOffsetPair store64(T, S) {
    MOZ_CRASH();
  }
#endif
  template <typename T, typename S>
  void store64Unaligned(T, S) {
    MOZ_CRASH();
  }

  template <typename T>
  void computeEffectiveAddress(T, Register) {
    MOZ_CRASH();
  }

  void splitTagForTest(ValueOperand, ScratchTagScope&) { MOZ_CRASH(); }

  void boxDouble(FloatRegister, ValueOperand, FloatRegister) { MOZ_CRASH(); }
  void boxNonDouble(JSValueType, Register, ValueOperand) { MOZ_CRASH(); }
  template <typename T>
  void boxDouble(FloatRegister src, const T& dest) {
    MOZ_CRASH();
  }
  template <typename T>
  void unboxInt32(T, Register) {
    MOZ_CRASH();
  }
  template <typename T>
  void unboxBoolean(T, Register) {
    MOZ_CRASH();
  }
  template <typename T>
  void unboxString(T, Register) {
    MOZ_CRASH();
  }
  template <typename T>
  void unboxSymbol(T, Register) {
    MOZ_CRASH();
  }
  template <typename T>
  void unboxBigInt(T, Register) {
    MOZ_CRASH();
  }
  template <typename T>
  void unboxObject(T, Register) {
    MOZ_CRASH();
  }
  template <typename T>
  void unboxDouble(T, FloatRegister) {
    MOZ_CRASH();
  }
  void unboxValue(const ValueOperand&, AnyRegister, JSValueType) {
    MOZ_CRASH();
  }
  void unboxNonDouble(const ValueOperand&, Register, JSValueType) {
    MOZ_CRASH();
  }
  void unboxNonDouble(const Address&, Register, JSValueType) { MOZ_CRASH(); }
  template <typename T>
  void unboxGCThingForGCBarrier(const T&, Register) {
    MOZ_CRASH();
  }

  template <typename T>
  void unboxWasmAnyRefGCThingForGCBarrier(const T&, Register) {
    MOZ_CRASH();
  }

  void getWasmAnyRefGCThingChunk(Register, Register) { MOZ_CRASH(); }

  template <typename T>
  void unboxObjectOrNull(const T& src, Register dest) {
    MOZ_CRASH();
  }
  void notBoolean(ValueOperand) { MOZ_CRASH(); }
  [[nodiscard]] Register extractObject(Address, Register) { MOZ_CRASH(); }
  [[nodiscard]] Register extractObject(ValueOperand, Register) { MOZ_CRASH(); }
  [[nodiscard]] Register extractSymbol(ValueOperand, Register) { MOZ_CRASH(); }
  [[nodiscard]] Register extractInt32(ValueOperand, Register) { MOZ_CRASH(); }
  [[nodiscard]] Register extractBoolean(ValueOperand, Register) { MOZ_CRASH(); }
  template <typename T>
  [[nodiscard]] Register extractTag(T, Register) {
    MOZ_CRASH();
  }

  void convertFloat32ToInt32(FloatRegister, Register, Label*, bool v = true) {
    MOZ_CRASH();
  }
  void convertDoubleToInt32(FloatRegister, Register, Label*, bool v = true) {
    MOZ_CRASH();
  }
  void convertDoubleToPtr(FloatRegister, Register, Label*, bool v = true) {
    MOZ_CRASH();
  }
  void convertBoolToInt32(Register, Register) { MOZ_CRASH(); }

  void convertDoubleToFloat32(FloatRegister, FloatRegister) { MOZ_CRASH(); }
  void convertInt32ToFloat32(Register, FloatRegister) { MOZ_CRASH(); }

  template <typename T>
  void convertInt32ToDouble(T, FloatRegister) {
    MOZ_CRASH();
  }
  void convertFloat32ToDouble(FloatRegister, FloatRegister) { MOZ_CRASH(); }

  void boolValueToDouble(ValueOperand, FloatRegister) { MOZ_CRASH(); }
  void boolValueToFloat32(ValueOperand, FloatRegister) { MOZ_CRASH(); }
  void int32ValueToDouble(ValueOperand, FloatRegister) { MOZ_CRASH(); }
  void int32ValueToFloat32(ValueOperand, FloatRegister) { MOZ_CRASH(); }

  void loadConstantDouble(double, FloatRegister) { MOZ_CRASH(); }
  void loadConstantFloat32(float, FloatRegister) { MOZ_CRASH(); }
  Condition testInt32Truthy(bool, ValueOperand) { MOZ_CRASH(); }
  Condition testStringTruthy(bool, ValueOperand) { MOZ_CRASH(); }
  Condition testBigIntTruthy(bool, ValueOperand) { MOZ_CRASH(); }

  template <typename T>
  void loadUnboxedValue(T, MIRType, AnyRegister) {
    MOZ_CRASH();
  }
  template <typename T>
  void storeUnboxedValue(const ConstantOrRegister&, MIRType, T) {
    MOZ_CRASH();
  }
  template <typename T>
  void storeUnboxedPayload(ValueOperand value, T, size_t, JSValueType) {
    MOZ_CRASH();
  }

  void convertUInt32ToDouble(Register, FloatRegister) { MOZ_CRASH(); }
  void convertUInt32ToFloat32(Register, FloatRegister) { MOZ_CRASH(); }
  void incrementInt32Value(Address) { MOZ_CRASH(); }
  void ensureDouble(ValueOperand, FloatRegister, Label*) { MOZ_CRASH(); }
  void handleFailureWithHandlerTail(Label*, Label*) { MOZ_CRASH(); }

  void buildFakeExitFrame(Register, uint32_t*) { MOZ_CRASH(); }
  bool buildOOLFakeExitFrame(void*) { MOZ_CRASH(); }

  void setPrinter(Sprinter*) { MOZ_CRASH(); }
  Operand ToPayload(Operand base) { MOZ_CRASH(); }
  Address ToPayload(Address) { MOZ_CRASH(); }

  Register getStackPointer() const { MOZ_CRASH(); }

  // Instrumentation for entering and leaving the profiler.
  void profilerEnterFrame(Register, Register) { MOZ_CRASH(); }
  void profilerExitFrame() { MOZ_CRASH(); }

#ifdef JS_NUNBOX32
  Address ToType(Address) { MOZ_CRASH(); }
#endif
};

typedef MacroAssemblerNone MacroAssemblerSpecific;

static inline bool GetTempRegForIntArg(uint32_t, uint32_t, Register*) {
  MOZ_CRASH();
}

}  // namespace jit
}  // namespace js

#endif /* jit_none_MacroAssembler_none_h */
