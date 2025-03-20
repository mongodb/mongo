/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/wasm32/MacroAssembler-wasm32.h"

namespace js::jit {

void MacroAssembler::subFromStackPtr(Imm32 imm32) { MOZ_CRASH(); }

//{{{ check_macroassembler_style

void MacroAssembler::PushBoxed(FloatRegister reg) { MOZ_CRASH(); }

void MacroAssembler::branchPtrInNurseryChunk(Condition cond, Register ptr,
                                             Register temp, Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::pushReturnAddress() { MOZ_CRASH(); }

void MacroAssembler::popReturnAddress() { MOZ_CRASH(); }

CodeOffset MacroAssembler::moveNearAddressWithPatch(Register dest) {
  MOZ_CRASH();
}

void MacroAssembler::patchNearAddressMove(CodeLocationLabel loc,
                                          CodeLocationLabel target) {
  MOZ_CRASH();
}

size_t MacroAssembler::PushRegsInMaskSizeInBytes(LiveRegisterSet set) {
  MOZ_CRASH();
  return 0;
}

void MacroAssembler::PushRegsInMask(LiveRegisterSet set) { MOZ_CRASH(); }

void MacroAssembler::PopRegsInMaskIgnore(LiveRegisterSet set,
                                         LiveRegisterSet ignore) {
  MOZ_CRASH();
}

void MacroAssembler::PopStackPtr() { MOZ_CRASH(); }

void MacroAssembler::flexibleDivMod32(Register rhs, Register srcDest,
                                      Register remOutput, bool isUnsigned,
                                      const LiveRegisterSet& volatileLiveRegs) {
  MOZ_CRASH();
}

void MacroAssembler::flexibleRemainder32(
    Register rhs, Register srcDest, bool isUnsigned,
    const LiveRegisterSet& volatileLiveRegs) {
  MOZ_CRASH();
}

void MacroAssembler::storeRegsInMask(LiveRegisterSet set, Address dest,
                                     Register scratch) {
  MOZ_CRASH();
}

void MacroAssembler::wasmBoundsCheck32(Condition cond, Register index,
                                       Register boundsCheckLimit, Label* ok) {
  MOZ_CRASH();
}

void MacroAssembler::wasmBoundsCheck32(Condition cond, Register index,
                                       Address boundsCheckLimit, Label* ok) {
  MOZ_CRASH();
}

void MacroAssembler::wasmBoundsCheck64(Condition cond, Register64 index,
                                       Register64 boundsCheckLimit, Label* ok) {
  MOZ_CRASH();
}

void MacroAssembler::wasmBoundsCheck64(Condition cond, Register64 index,
                                       Address boundsCheckLimit, Label* ok) {
  MOZ_CRASH();
}

void MacroAssembler::oolWasmTruncateCheckF32ToI32(FloatRegister input,
                                                  Register output,
                                                  TruncFlags flags,
                                                  wasm::BytecodeOffset off,
                                                  Label* rejoin) {
  MOZ_CRASH();
}

void MacroAssembler::wasmTruncateDoubleToInt64(
    FloatRegister input, Register64 output, bool isSaturating, Label* oolEntry,
    Label* oolRejoin, FloatRegister tempDouble) {
  MOZ_CRASH();
}

void MacroAssembler::wasmTruncateDoubleToUInt64(
    FloatRegister input, Register64 output, bool isSaturating, Label* oolEntry,
    Label* oolRejoin, FloatRegister tempDouble) {
  MOZ_CRASH();
}

void MacroAssembler::oolWasmTruncateCheckF64ToI64(FloatRegister input,
                                                  Register64 output,
                                                  TruncFlags flags,
                                                  wasm::BytecodeOffset off,
                                                  Label* rejoin) {
  MOZ_CRASH();
}

void MacroAssembler::wasmTruncateFloat32ToInt64(
    FloatRegister input, Register64 output, bool isSaturating, Label* oolEntry,
    Label* oolRejoin, FloatRegister tempDouble) {
  MOZ_CRASH();
}

void MacroAssembler::wasmTruncateFloat32ToUInt64(
    FloatRegister input, Register64 output, bool isSaturating, Label* oolEntry,
    Label* oolRejoin, FloatRegister tempDouble) {
  MOZ_CRASH();
}

void MacroAssembler::oolWasmTruncateCheckF32ToI64(FloatRegister input,
                                                  Register64 output,
                                                  TruncFlags flags,
                                                  wasm::BytecodeOffset off,
                                                  Label* rejoin) {
  MOZ_CRASH();
}

void MacroAssembler::oolWasmTruncateCheckF64ToI32(FloatRegister input,
                                                  Register output,
                                                  TruncFlags flags,
                                                  wasm::BytecodeOffset off,
                                                  Label* rejoin) {
  MOZ_CRASH();
}

void MacroAssembler::convertUInt64ToFloat32(Register64 src, FloatRegister dest,
                                            Register temp) {
  MOZ_CRASH();
}

void MacroAssembler::convertInt64ToFloat32(Register64 src, FloatRegister dest) {
  MOZ_CRASH();
}

bool MacroAssembler::convertUInt64ToDoubleNeedsTemp() { MOZ_CRASH(); }

void MacroAssembler::convertUInt64ToDouble(Register64 src, FloatRegister dest,
                                           Register temp) {
  MOZ_CRASH();
}

void MacroAssembler::convertInt64ToDouble(Register64 src, FloatRegister dest) {
  MOZ_CRASH();
}

void MacroAssembler::convertIntPtrToDouble(Register src, FloatRegister dest) {
  MOZ_CRASH();
}

void MacroAssembler::wasmAtomicLoad64(const wasm::MemoryAccessDesc& access,
                                      const Address& mem, Register64 temp,
                                      Register64 output) {
  MOZ_CRASH();
}

void MacroAssembler::wasmAtomicLoad64(const wasm::MemoryAccessDesc& access,
                                      const BaseIndex& mem, Register64 temp,
                                      Register64 output) {
  MOZ_CRASH();
}

void MacroAssembler::patchNopToCall(uint8_t* call, uint8_t* target) {
  MOZ_CRASH();
}

void MacroAssembler::patchCallToNop(uint8_t* call) { MOZ_CRASH(); }

void MacroAssembler::patchCall(uint32_t callerOffset, uint32_t calleeOffset) {
  MOZ_CRASH();
}

CodeOffset MacroAssembler::farJumpWithPatch() {
  MOZ_CRASH();
  return CodeOffset(0);
}

void MacroAssembler::patchFarJump(CodeOffset farJump, uint32_t targetOffset) {
  MOZ_CRASH();
}

CodeOffset MacroAssembler::call(Register reg) {
  MOZ_CRASH();
  return CodeOffset(0);
}

CodeOffset MacroAssembler::call(Label* label) {
  MOZ_CRASH();
  return CodeOffset(0);
}

CodeOffset MacroAssembler::call(wasm::SymbolicAddress imm) {
  MOZ_CRASH();
  return CodeOffset(0);
}

CodeOffset MacroAssembler::callWithPatch() {
  MOZ_CRASH();
  return CodeOffset(0);
}

CodeOffset MacroAssembler::nopPatchableToCall() {
  MOZ_CRASH();
  return CodeOffset(0);
}

FaultingCodeOffset MacroAssembler::wasmTrapInstruction() {
  MOZ_CRASH();
  return FaultingCodeOffset();
}

template void MacroAssembler::storeUnboxedValue(const ConstantOrRegister& value,
                                                MIRType valueType,
                                                const Address& dest);

template void MacroAssembler::storeUnboxedValue(
    const ConstantOrRegister& value, MIRType valueType,
    const BaseObjectElementIndex& dest);

template <typename T>
void MacroAssembler::storeUnboxedValue(const ConstantOrRegister& value,
                                       MIRType valueType, const T& dest) {
  MOZ_CRASH();
}

uint32_t MacroAssembler::pushFakeReturnAddress(Register scratch) {
  MOZ_CRASH();
}

void MacroAssembler::Pop(Register reg) { MOZ_CRASH(); }

void MacroAssembler::Pop(FloatRegister t) { MOZ_CRASH(); }

void MacroAssembler::Pop(const ValueOperand& val) { MOZ_CRASH(); }

void MacroAssembler::Push(Register reg) { MOZ_CRASH(); }

void MacroAssembler::Push(const Imm32 imm) { MOZ_CRASH(); }

void MacroAssembler::Push(const ImmWord imm) { MOZ_CRASH(); }

void MacroAssembler::Push(const ImmPtr imm) { MOZ_CRASH(); }

void MacroAssembler::Push(const ImmGCPtr ptr) { MOZ_CRASH(); }

void MacroAssembler::Push(FloatRegister reg) { MOZ_CRASH(); }

void MacroAssembler::wasmTruncateFloat32ToInt32(FloatRegister input,
                                                Register output,
                                                bool isSaturating,
                                                Label* oolEntry) {
  MOZ_CRASH();
}

void MacroAssembler::wasmTruncateFloat32ToUInt32(FloatRegister input,
                                                 Register output,
                                                 bool isSaturating,
                                                 Label* oolEntry) {
  MOZ_CRASH();
}

void MacroAssembler::wasmTruncateDoubleToUInt32(FloatRegister input,
                                                Register output,
                                                bool isSaturating,
                                                Label* oolEntry) {
  MOZ_CRASH();
}

void MacroAssembler::wasmTruncateDoubleToInt32(FloatRegister input,
                                               Register output,
                                               bool isSaturating,
                                               Label* oolEntry) {
  MOZ_CRASH();
}

void MacroAssembler::wasmAtomicExchange64(const wasm::MemoryAccessDesc& access,
                                          const Address& mem, Register64 value,
                                          Register64 output) {
  MOZ_CRASH();
}

void MacroAssembler::wasmAtomicExchange64(const wasm::MemoryAccessDesc& access,
                                          const BaseIndex& mem,
                                          Register64 value, Register64 output) {
  MOZ_CRASH();
}

void MacroAssembler::speculationBarrier() { MOZ_CRASH(); }

void MacroAssembler::shiftIndex32AndAdd(Register indexTemp32, int shift,
                                        Register pointer) {
  MOZ_CRASH();
}

void MacroAssembler::setupUnalignedABICall(Register scratch) { MOZ_CRASH(); }

void MacroAssembler::enterFakeExitFrameForWasm(Register cxreg, Register scratch,
                                               ExitFrameType type) {
  MOZ_CRASH();
}

void MacroAssembler::floorFloat32ToInt32(FloatRegister src, Register dest,
                                         Label* fail) {
  MOZ_CRASH();
}

void MacroAssembler::floorDoubleToInt32(FloatRegister src, Register dest,
                                        Label* fail) {
  MOZ_CRASH();
}

void MacroAssembler::ceilFloat32ToInt32(FloatRegister src, Register dest,
                                        Label* fail) {
  MOZ_CRASH();
}

void MacroAssembler::ceilDoubleToInt32(FloatRegister src, Register dest,
                                       Label* fail) {
  MOZ_CRASH();
}

void MacroAssembler::roundFloat32ToInt32(FloatRegister src, Register dest,
                                         FloatRegister temp, Label* fail) {
  MOZ_CRASH();
}

void MacroAssembler::roundDoubleToInt32(FloatRegister src, Register dest,
                                        FloatRegister temp, Label* fail) {
  MOZ_CRASH();
}

void MacroAssembler::truncFloat32ToInt32(FloatRegister src, Register dest,
                                         Label* fail) {
  MOZ_CRASH();
}

void MacroAssembler::truncDoubleToInt32(FloatRegister src, Register dest,
                                        Label* fail) {
  MOZ_CRASH();
}

void MacroAssembler::nearbyIntDouble(RoundingMode mode, FloatRegister src,
                                     FloatRegister dest) {
  MOZ_CRASH();
}

void MacroAssembler::nearbyIntFloat32(RoundingMode mode, FloatRegister src,
                                      FloatRegister dest) {
  MOZ_CRASH();
}

void MacroAssembler::copySignDouble(FloatRegister lhs, FloatRegister rhs,
                                    FloatRegister output) {
  MOZ_CRASH();
}

void MacroAssembler::branchTestValue(Condition cond, const ValueOperand& lhs,
                                     const Value& rhs, Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchValueIsNurseryCell(Condition cond,
                                              const Address& address,
                                              Register temp, Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::branchValueIsNurseryCell(Condition cond,
                                              ValueOperand value, Register temp,
                                              Label* label) {
  MOZ_CRASH();
}

void MacroAssembler::callWithABINoProfiler(Register fun, ABIType result) {
  MOZ_CRASH();
}

void MacroAssembler::callWithABINoProfiler(const Address& fun, ABIType result) {
  MOZ_CRASH();
}

void MacroAssembler::call(const Address& addr) { MOZ_CRASH(); }

void MacroAssembler::call(ImmWord imm) { MOZ_CRASH(); }

void MacroAssembler::call(ImmPtr imm) { MOZ_CRASH(); }

void MacroAssembler::call(JitCode* c) { MOZ_CRASH(); }

void MacroAssembler::callWithABIPost(uint32_t stackAdjust, ABIType result,
                                     bool callFromWasm) {
  MOZ_CRASH();
}

void MacroAssembler::callWithABIPre(uint32_t* stackAdjust, bool callFromWasm) {
  MOZ_CRASH();
}

void MacroAssembler::comment(const char* msg) { MOZ_CRASH(); }

void MacroAssembler::flush() { MOZ_CRASH(); }

void MacroAssembler::loadStoreBuffer(Register ptr, Register buffer) {
  MOZ_CRASH();
}

void MacroAssembler::moveValue(const TypedOrValueRegister& src,
                               const ValueOperand& dest) {
  MOZ_CRASH();
}

void MacroAssembler::moveValue(const ValueOperand& src,
                               const ValueOperand& dest) {
  MOZ_CRASH();
}

void MacroAssembler::moveValue(const Value& src, const ValueOperand& dest) {
  MOZ_CRASH();
}

void MacroAssembler::wasmCompareExchange64(const wasm::MemoryAccessDesc& access,
                                           const Address& mem,
                                           Register64 expected,
                                           Register64 replacement,
                                           Register64 output) {
  MOZ_CRASH();
}

void MacroAssembler::wasmCompareExchange64(const wasm::MemoryAccessDesc& access,
                                           const BaseIndex& mem,
                                           Register64 expected,
                                           Register64 replacement,
                                           Register64 output) {
  MOZ_CRASH();
}

//}}} check_macroassembler_style

void MacroAssemblerWasm32::executableCopy(void* buffer) { MOZ_CRASH(); }

void MacroAssemblerWasm32::jump(Label* label) { MOZ_CRASH(); }

void MacroAssemblerWasm32::writeCodePointer(CodeLabel* label) { MOZ_CRASH(); }

void MacroAssemblerWasm32::haltingAlign(size_t) { MOZ_CRASH(); }

void MacroAssemblerWasm32::nopAlign(size_t) { MOZ_CRASH(); }

void MacroAssemblerWasm32::checkStackAlignment() { MOZ_CRASH(); }

uint32_t MacroAssemblerWasm32::currentOffset() {
  MOZ_CRASH();
  return 0;
}

void MacroAssemblerWasm32::nop() { MOZ_CRASH(); }

void MacroAssemblerWasm32::breakpoint() { MOZ_CRASH(); }

void MacroAssemblerWasm32::abiret() { MOZ_CRASH(); }

void MacroAssemblerWasm32::ret() { MOZ_CRASH(); }

CodeOffset MacroAssemblerWasm32::toggledJump(Label*) { MOZ_CRASH(); }

CodeOffset MacroAssemblerWasm32::toggledCall(JitCode*, bool) { MOZ_CRASH(); }

size_t MacroAssemblerWasm32::ToggledCallSize(uint8_t*) { MOZ_CRASH(); }

void MacroAssemblerWasm32::finish() { MOZ_CRASH(); }

void MacroAssemblerWasm32::pushValue(ValueOperand val) { MOZ_CRASH(); }

void MacroAssemblerWasm32::popValue(ValueOperand) { MOZ_CRASH(); }

void MacroAssemblerWasm32::tagValue(JSValueType, Register, ValueOperand) {
  MOZ_CRASH();
}

void MacroAssemblerWasm32::retn(Imm32 n) { MOZ_CRASH(); }

void MacroAssemblerWasm32::push(Register reg) { MOZ_CRASH(); }

Address MacroAssemblerWasm32::ToType(const Address& address) { MOZ_CRASH(); }

}  // namespace js::jit
