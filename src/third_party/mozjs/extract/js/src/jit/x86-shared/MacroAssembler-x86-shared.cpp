/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/x86-shared/MacroAssembler-x86-shared.h"

#include "mozilla/Casting.h"

#include "jsmath.h"

#include "jit/JitFrames.h"
#include "jit/MacroAssembler.h"
#include "js/ScalarType.h"  // js::Scalar::Type

#include "jit/MacroAssembler-inl.h"

using namespace js;
using namespace js::jit;

// Note: this function clobbers the input register.
void MacroAssembler::clampDoubleToUint8(FloatRegister input, Register output) {
  ScratchDoubleScope scratch(*this);
  MOZ_ASSERT(input != scratch);
  Label positive, done;

  // <= 0 or NaN --> 0
  zeroDouble(scratch);
  branchDouble(DoubleGreaterThan, input, scratch, &positive);
  {
    move32(Imm32(0), output);
    jump(&done);
  }

  bind(&positive);

  if (HasRoundInstruction(RoundingMode::NearestTiesToEven)) {
    // Round input to nearest integer.
    nearbyIntDouble(RoundingMode::NearestTiesToEven, input, input);

    // Truncate to int32 and ensure the result <= 255. This relies on the
    // processor setting output to a value > 255 for doubles outside the int32
    // range (for instance 0x80000000).
    vcvttsd2si(input, output);
    branch32(Assembler::BelowOrEqual, output, Imm32(255), &done);
    move32(Imm32(255), output);
  } else {
    Label outOfRange;

    // Truncate to int32 and ensure the result <= 255. This relies on the
    // processor setting output to a value > 255 for doubles outside the int32
    // range (for instance 0x80000000).
    vcvttsd2si(input, output);
    branch32(Assembler::AboveOrEqual, output, Imm32(255), &outOfRange);
    {
      // Check if we had a tie.
      convertInt32ToDouble(output, scratch);
      subDouble(scratch, input);

      loadConstantDouble(0.5, scratch);

      Label roundUp;
      vucomisd(scratch, input);
      j(Above, &roundUp);
      j(NotEqual, &done);

      // It was a tie. Round up if the output is odd.
      branchTest32(Zero, output, Imm32(1), &done);

      bind(&roundUp);
      add32(Imm32(1), output);
      jump(&done);
    }

    // > 255 --> 255
    bind(&outOfRange);
    move32(Imm32(255), output);
  }

  bind(&done);
}

bool MacroAssemblerX86Shared::buildOOLFakeExitFrame(void* fakeReturnAddr) {
  asMasm().PushFrameDescriptor(FrameType::IonJS);
  asMasm().Push(ImmPtr(fakeReturnAddr));
  asMasm().Push(FramePointer);
  return true;
}

void MacroAssemblerX86Shared::branchNegativeZero(FloatRegister reg,
                                                 Register scratch, Label* label,
                                                 bool maybeNonZero) {
  // Determines whether the low double contained in the XMM register reg
  // is equal to -0.0.

#if defined(JS_CODEGEN_X86)
  Label nonZero;

  // if not already compared to zero
  if (maybeNonZero) {
    ScratchDoubleScope scratchDouble(asMasm());

    // Compare to zero. Lets through {0, -0}.
    zeroDouble(scratchDouble);

    // If reg is non-zero, jump to nonZero.
    asMasm().branchDouble(DoubleNotEqual, reg, scratchDouble, &nonZero);
  }
  // Input register is either zero or negative zero. Retrieve sign of input.
  vmovmskpd(reg, scratch);

  // If reg is 1 or 3, input is negative zero.
  // If reg is 0 or 2, input is a normal zero.
  asMasm().branchTest32(NonZero, scratch, Imm32(1), label);

  bind(&nonZero);
#elif defined(JS_CODEGEN_X64)
  vmovq(reg, scratch);
  cmpq(Imm32(1), scratch);
  j(Overflow, label);
#endif
}

void MacroAssemblerX86Shared::branchNegativeZeroFloat32(FloatRegister reg,
                                                        Register scratch,
                                                        Label* label) {
  vmovd(reg, scratch);
  cmp32(scratch, Imm32(1));
  j(Overflow, label);
}

MacroAssembler& MacroAssemblerX86Shared::asMasm() {
  return *static_cast<MacroAssembler*>(this);
}

const MacroAssembler& MacroAssemblerX86Shared::asMasm() const {
  return *static_cast<const MacroAssembler*>(this);
}

template <class T, class Map>
T* MacroAssemblerX86Shared::getConstant(const typename T::Pod& value, Map& map,
                                        Vector<T, 0, SystemAllocPolicy>& vec) {
  using AddPtr = typename Map::AddPtr;
  size_t index;
  if (AddPtr p = map.lookupForAdd(value)) {
    index = p->value();
  } else {
    index = vec.length();
    enoughMemory_ &= vec.append(T(value));
    if (!enoughMemory_) {
      return nullptr;
    }
    enoughMemory_ &= map.add(p, value, index);
    if (!enoughMemory_) {
      return nullptr;
    }
  }
  return &vec[index];
}

MacroAssemblerX86Shared::Float* MacroAssemblerX86Shared::getFloat(float f) {
  return getConstant<Float, FloatMap>(f, floatMap_, floats_);
}

MacroAssemblerX86Shared::Double* MacroAssemblerX86Shared::getDouble(double d) {
  return getConstant<Double, DoubleMap>(d, doubleMap_, doubles_);
}

MacroAssemblerX86Shared::SimdData* MacroAssemblerX86Shared::getSimdData(
    const SimdConstant& v) {
  return getConstant<SimdData, SimdMap>(v, simdMap_, simds_);
}

void MacroAssemblerX86Shared::binarySimd128(
    const SimdConstant& rhs, FloatRegister lhsDest,
    void (MacroAssembler::*regOp)(const Operand&, FloatRegister, FloatRegister),
    void (MacroAssembler::*constOp)(const SimdConstant&, FloatRegister)) {
  ScratchSimd128Scope scratch(asMasm());
  if (maybeInlineSimd128Int(rhs, scratch)) {
    (asMasm().*regOp)(Operand(scratch), lhsDest, lhsDest);
  } else {
    (asMasm().*constOp)(rhs, lhsDest);
  }
}

void MacroAssemblerX86Shared::binarySimd128(
    FloatRegister lhs, const SimdConstant& rhs, FloatRegister dest,
    void (MacroAssembler::*regOp)(const Operand&, FloatRegister, FloatRegister),
    void (MacroAssembler::*constOp)(const SimdConstant&, FloatRegister,
                                    FloatRegister)) {
  ScratchSimd128Scope scratch(asMasm());
  if (maybeInlineSimd128Int(rhs, scratch)) {
    (asMasm().*regOp)(Operand(scratch), lhs, dest);
  } else {
    (asMasm().*constOp)(rhs, lhs, dest);
  }
}

void MacroAssemblerX86Shared::binarySimd128(
    const SimdConstant& rhs, FloatRegister lhs,
    void (MacroAssembler::*regOp)(const Operand&, FloatRegister),
    void (MacroAssembler::*constOp)(const SimdConstant&, FloatRegister)) {
  ScratchSimd128Scope scratch(asMasm());
  if (maybeInlineSimd128Int(rhs, scratch)) {
    (asMasm().*regOp)(Operand(scratch), lhs);
  } else {
    (asMasm().*constOp)(rhs, lhs);
  }
}

void MacroAssemblerX86Shared::bitwiseTestSimd128(const SimdConstant& rhs,
                                                 FloatRegister lhs) {
  ScratchSimd128Scope scratch(asMasm());
  if (maybeInlineSimd128Int(rhs, scratch)) {
    vptest(scratch, lhs);
  } else {
    asMasm().vptestSimd128(rhs, lhs);
  }
}

void MacroAssemblerX86Shared::minMaxDouble(FloatRegister first,
                                           FloatRegister second, bool canBeNaN,
                                           bool isMax) {
  Label done, nan, minMaxInst;

  // Do a vucomisd to catch equality and NaNs, which both require special
  // handling. If the operands are ordered and inequal, we branch straight to
  // the min/max instruction. If we wanted, we could also branch for less-than
  // or greater-than here instead of using min/max, however these conditions
  // will sometimes be hard on the branch predictor.
  vucomisd(second, first);
  j(Assembler::NotEqual, &minMaxInst);
  if (canBeNaN) {
    j(Assembler::Parity, &nan);
  }

  // Ordered and equal. The operands are bit-identical unless they are zero
  // and negative zero. These instructions merge the sign bits in that
  // case, and are no-ops otherwise.
  if (isMax) {
    vandpd(second, first, first);
  } else {
    vorpd(second, first, first);
  }
  jump(&done);

  // x86's min/max are not symmetric; if either operand is a NaN, they return
  // the read-only operand. We need to return a NaN if either operand is a
  // NaN, so we explicitly check for a NaN in the read-write operand.
  if (canBeNaN) {
    bind(&nan);
    vucomisd(first, first);
    j(Assembler::Parity, &done);
  }

  // When the values are inequal, or second is NaN, x86's min and max will
  // return the value we need.
  bind(&minMaxInst);
  if (isMax) {
    vmaxsd(second, first, first);
  } else {
    vminsd(second, first, first);
  }

  bind(&done);
}

void MacroAssemblerX86Shared::minMaxFloat32(FloatRegister first,
                                            FloatRegister second, bool canBeNaN,
                                            bool isMax) {
  Label done, nan, minMaxInst;

  // Do a vucomiss to catch equality and NaNs, which both require special
  // handling. If the operands are ordered and inequal, we branch straight to
  // the min/max instruction. If we wanted, we could also branch for less-than
  // or greater-than here instead of using min/max, however these conditions
  // will sometimes be hard on the branch predictor.
  vucomiss(second, first);
  j(Assembler::NotEqual, &minMaxInst);
  if (canBeNaN) {
    j(Assembler::Parity, &nan);
  }

  // Ordered and equal. The operands are bit-identical unless they are zero
  // and negative zero. These instructions merge the sign bits in that
  // case, and are no-ops otherwise.
  if (isMax) {
    vandps(second, first, first);
  } else {
    vorps(second, first, first);
  }
  jump(&done);

  // x86's min/max are not symmetric; if either operand is a NaN, they return
  // the read-only operand. We need to return a NaN if either operand is a
  // NaN, so we explicitly check for a NaN in the read-write operand.
  if (canBeNaN) {
    bind(&nan);
    vucomiss(first, first);
    j(Assembler::Parity, &done);
  }

  // When the values are inequal, or second is NaN, x86's min and max will
  // return the value we need.
  bind(&minMaxInst);
  if (isMax) {
    vmaxss(second, first, first);
  } else {
    vminss(second, first, first);
  }

  bind(&done);
}

#ifdef ENABLE_WASM_SIMD
bool MacroAssembler::MustMaskShiftCountSimd128(wasm::SimdOp op, int32_t* mask) {
  switch (op) {
    case wasm::SimdOp::I8x16Shl:
    case wasm::SimdOp::I8x16ShrU:
    case wasm::SimdOp::I8x16ShrS:
      *mask = 7;
      break;
    case wasm::SimdOp::I16x8Shl:
    case wasm::SimdOp::I16x8ShrU:
    case wasm::SimdOp::I16x8ShrS:
      *mask = 15;
      break;
    case wasm::SimdOp::I32x4Shl:
    case wasm::SimdOp::I32x4ShrU:
    case wasm::SimdOp::I32x4ShrS:
      *mask = 31;
      break;
    case wasm::SimdOp::I64x2Shl:
    case wasm::SimdOp::I64x2ShrU:
    case wasm::SimdOp::I64x2ShrS:
      *mask = 63;
      break;
    default:
      MOZ_CRASH("Unexpected shift operation");
  }
  return true;
}
#endif

//{{{ check_macroassembler_style
// ===============================================================
// MacroAssembler high-level usage.

void MacroAssembler::flush() {}

void MacroAssembler::comment(const char* msg) { masm.comment(msg); }

// This operation really consists of five phases, in order to enforce the
// restriction that on x86_shared, srcDest must be eax and edx will be
// clobbered.
//
//     Input: { rhs, lhsOutput }
//
//  [PUSH] Preserve registers
//  [MOVE] Generate moves to specific registers
//
//  [DIV] Input: { regForRhs, EAX }
//  [DIV] extend EAX into EDX
//  [DIV] x86 Division operator
//  [DIV] Ouptut: { EAX, EDX }
//
//  [MOVE] Move specific registers to outputs
//  [POP] Restore registers
//
//    Output: { lhsOutput, remainderOutput }
void MacroAssembler::flexibleDivMod32(Register rhs, Register lhsOutput,
                                      Register remOutput, bool isUnsigned,
                                      const LiveRegisterSet&) {
  // Currently this helper can't handle this situation.
  MOZ_ASSERT(lhsOutput != rhs);
  MOZ_ASSERT(lhsOutput != remOutput);

  // Choose a register that is not edx, or eax to hold the rhs;
  // ebx is chosen arbitrarily, and will be preserved if necessary.
  Register regForRhs = (rhs == eax || rhs == edx) ? ebx : rhs;

  // Add registers we will be clobbering as live, but
  // also remove the set we do not restore.
  LiveRegisterSet preserve;
  preserve.add(edx);
  preserve.add(eax);
  preserve.add(regForRhs);

  preserve.takeUnchecked(lhsOutput);
  preserve.takeUnchecked(remOutput);

  PushRegsInMask(preserve);

  // Shuffle input into place.
  moveRegPair(lhsOutput, rhs, eax, regForRhs);

  // Sign extend eax into edx to make (edx:eax): idiv/udiv are 64-bit.
  if (isUnsigned) {
    mov(ImmWord(0), edx);
    udiv(regForRhs);
  } else {
    cdq();
    idiv(regForRhs);
  }

  moveRegPair(eax, edx, lhsOutput, remOutput);

  PopRegsInMask(preserve);
}

void MacroAssembler::flexibleQuotient32(
    Register rhs, Register srcDest, bool isUnsigned,
    const LiveRegisterSet& volatileLiveRegs) {
  // Choose an arbitrary register that isn't eax, edx, rhs or srcDest;
  AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
  regs.takeUnchecked(eax);
  regs.takeUnchecked(edx);
  regs.takeUnchecked(rhs);
  regs.takeUnchecked(srcDest);

  Register remOut = regs.takeAny();
  push(remOut);
  flexibleDivMod32(rhs, srcDest, remOut, isUnsigned, volatileLiveRegs);
  pop(remOut);
}

void MacroAssembler::flexibleRemainder32(
    Register rhs, Register srcDest, bool isUnsigned,
    const LiveRegisterSet& volatileLiveRegs) {
  // Choose an arbitrary register that isn't eax, edx, rhs or srcDest
  AllocatableGeneralRegisterSet regs(GeneralRegisterSet::All());
  regs.takeUnchecked(eax);
  regs.takeUnchecked(edx);
  regs.takeUnchecked(rhs);
  regs.takeUnchecked(srcDest);

  Register remOut = regs.takeAny();
  push(remOut);
  flexibleDivMod32(rhs, srcDest, remOut, isUnsigned, volatileLiveRegs);
  mov(remOut, srcDest);
  pop(remOut);
}

// ===============================================================
// Stack manipulation functions.

size_t MacroAssembler::PushRegsInMaskSizeInBytes(LiveRegisterSet set) {
  FloatRegisterSet fpuSet(set.fpus().reduceSetForPush());
  return set.gprs().size() * sizeof(intptr_t) + fpuSet.getPushSizeInBytes();
}

void MacroAssembler::PushRegsInMask(LiveRegisterSet set) {
  mozilla::DebugOnly<size_t> framePushedInitial = framePushed();

  FloatRegisterSet fpuSet(set.fpus().reduceSetForPush());
  unsigned numFpu = fpuSet.size();
  int32_t diffF = fpuSet.getPushSizeInBytes();
  int32_t diffG = set.gprs().size() * sizeof(intptr_t);

  // On x86, always use push to push the integer registers, as it's fast
  // on modern hardware and it's a small instruction.
  for (GeneralRegisterBackwardIterator iter(set.gprs()); iter.more(); ++iter) {
    diffG -= sizeof(intptr_t);
    Push(*iter);
  }
  MOZ_ASSERT(diffG == 0);
  (void)diffG;

  reserveStack(diffF);
  for (FloatRegisterBackwardIterator iter(fpuSet); iter.more(); ++iter) {
    FloatRegister reg = *iter;
    diffF -= reg.size();
    numFpu -= 1;
    Address spillAddress(StackPointer, diffF);
    if (reg.isDouble()) {
      storeDouble(reg, spillAddress);
    } else if (reg.isSingle()) {
      storeFloat32(reg, spillAddress);
    } else if (reg.isSimd128()) {
      storeUnalignedSimd128(reg, spillAddress);
    } else {
      MOZ_CRASH("Unknown register type.");
    }
  }
  MOZ_ASSERT(numFpu == 0);
  (void)numFpu;

  // x64 padding to keep the stack aligned on uintptr_t. Keep in sync with
  // GetPushSizeInBytes.
  size_t alignExtra = ((size_t)diffF) % sizeof(uintptr_t);
  MOZ_ASSERT_IF(sizeof(uintptr_t) == 8, alignExtra == 0 || alignExtra == 4);
  MOZ_ASSERT_IF(sizeof(uintptr_t) == 4, alignExtra == 0);
  diffF -= alignExtra;
  MOZ_ASSERT(diffF == 0);

  // The macroassembler will keep the stack sizeof(uintptr_t)-aligned, so
  // we don't need to take into account `alignExtra` here.
  MOZ_ASSERT(framePushed() - framePushedInitial ==
             PushRegsInMaskSizeInBytes(set));
}

void MacroAssembler::storeRegsInMask(LiveRegisterSet set, Address dest,
                                     Register) {
  mozilla::DebugOnly<size_t> offsetInitial = dest.offset;

  FloatRegisterSet fpuSet(set.fpus().reduceSetForPush());
  unsigned numFpu = fpuSet.size();
  int32_t diffF = fpuSet.getPushSizeInBytes();
  int32_t diffG = set.gprs().size() * sizeof(intptr_t);

  MOZ_ASSERT(dest.offset >= diffG + diffF);

  for (GeneralRegisterBackwardIterator iter(set.gprs()); iter.more(); ++iter) {
    diffG -= sizeof(intptr_t);
    dest.offset -= sizeof(intptr_t);
    storePtr(*iter, dest);
  }
  MOZ_ASSERT(diffG == 0);
  (void)diffG;

  for (FloatRegisterBackwardIterator iter(fpuSet); iter.more(); ++iter) {
    FloatRegister reg = *iter;
    diffF -= reg.size();
    numFpu -= 1;
    dest.offset -= reg.size();
    if (reg.isDouble()) {
      storeDouble(reg, dest);
    } else if (reg.isSingle()) {
      storeFloat32(reg, dest);
    } else if (reg.isSimd128()) {
      storeUnalignedSimd128(reg, dest);
    } else {
      MOZ_CRASH("Unknown register type.");
    }
  }
  MOZ_ASSERT(numFpu == 0);
  (void)numFpu;

  // x64 padding to keep the stack aligned on uintptr_t. Keep in sync with
  // GetPushSizeInBytes.
  size_t alignExtra = ((size_t)diffF) % sizeof(uintptr_t);
  MOZ_ASSERT_IF(sizeof(uintptr_t) == 8, alignExtra == 0 || alignExtra == 4);
  MOZ_ASSERT_IF(sizeof(uintptr_t) == 4, alignExtra == 0);
  diffF -= alignExtra;
  MOZ_ASSERT(diffF == 0);

  // What this means is: if `alignExtra` is nonzero, then the save area size
  // actually used is `alignExtra` bytes smaller than what
  // PushRegsInMaskSizeInBytes claims.  Hence we need to compensate for that.
  MOZ_ASSERT(alignExtra + offsetInitial - dest.offset ==
             PushRegsInMaskSizeInBytes(set));
}

void MacroAssembler::PopRegsInMaskIgnore(LiveRegisterSet set,
                                         LiveRegisterSet ignore) {
  mozilla::DebugOnly<size_t> framePushedInitial = framePushed();

  FloatRegisterSet fpuSet(set.fpus().reduceSetForPush());
  unsigned numFpu = fpuSet.size();
  int32_t diffG = set.gprs().size() * sizeof(intptr_t);
  int32_t diffF = fpuSet.getPushSizeInBytes();
  const int32_t reservedG = diffG;
  const int32_t reservedF = diffF;

  for (FloatRegisterBackwardIterator iter(fpuSet); iter.more(); ++iter) {
    FloatRegister reg = *iter;
    diffF -= reg.size();
    numFpu -= 1;
    if (ignore.has(reg)) {
      continue;
    }

    Address spillAddress(StackPointer, diffF);
    if (reg.isDouble()) {
      loadDouble(spillAddress, reg);
    } else if (reg.isSingle()) {
      loadFloat32(spillAddress, reg);
    } else if (reg.isSimd128()) {
      loadUnalignedSimd128(spillAddress, reg);
    } else {
      MOZ_CRASH("Unknown register type.");
    }
  }
  freeStack(reservedF);
  MOZ_ASSERT(numFpu == 0);
  (void)numFpu;
  // x64 padding to keep the stack aligned on uintptr_t. Keep in sync with
  // GetPushBytesInSize.
  diffF -= diffF % sizeof(uintptr_t);
  MOZ_ASSERT(diffF == 0);

  // On x86, use pop to pop the integer registers, if we're not going to
  // ignore any slots, as it's fast on modern hardware and it's a small
  // instruction.
  if (ignore.emptyGeneral()) {
    for (GeneralRegisterForwardIterator iter(set.gprs()); iter.more(); ++iter) {
      diffG -= sizeof(intptr_t);
      Pop(*iter);
    }
  } else {
    for (GeneralRegisterBackwardIterator iter(set.gprs()); iter.more();
         ++iter) {
      diffG -= sizeof(intptr_t);
      if (!ignore.has(*iter)) {
        loadPtr(Address(StackPointer, diffG), *iter);
      }
    }
    freeStack(reservedG);
  }
  MOZ_ASSERT(diffG == 0);

  MOZ_ASSERT(framePushedInitial - framePushed() ==
             PushRegsInMaskSizeInBytes(set));
}

void MacroAssembler::Push(const Operand op) {
  push(op);
  adjustFrame(sizeof(intptr_t));
}

void MacroAssembler::Push(Register reg) {
  push(reg);
  adjustFrame(sizeof(intptr_t));
}

void MacroAssembler::Push(const Imm32 imm) {
  push(imm);
  adjustFrame(sizeof(intptr_t));
}

void MacroAssembler::Push(const ImmWord imm) {
  push(imm);
  adjustFrame(sizeof(intptr_t));
}

void MacroAssembler::Push(const ImmPtr imm) {
  Push(ImmWord(uintptr_t(imm.value)));
}

void MacroAssembler::Push(const ImmGCPtr ptr) {
  push(ptr);
  adjustFrame(sizeof(intptr_t));
}

void MacroAssembler::Push(FloatRegister t) {
  push(t);
  adjustFrame(sizeof(double));
}

void MacroAssembler::PushFlags() {
  pushFlags();
  adjustFrame(sizeof(intptr_t));
}

void MacroAssembler::Pop(const Operand op) {
  pop(op);
  implicitPop(sizeof(intptr_t));
}

void MacroAssembler::Pop(Register reg) {
  pop(reg);
  implicitPop(sizeof(intptr_t));
}

void MacroAssembler::Pop(FloatRegister reg) {
  pop(reg);
  implicitPop(sizeof(double));
}

void MacroAssembler::Pop(const ValueOperand& val) {
  popValue(val);
  implicitPop(sizeof(Value));
}

void MacroAssembler::PopFlags() {
  popFlags();
  implicitPop(sizeof(intptr_t));
}

void MacroAssembler::PopStackPtr() { Pop(StackPointer); }

void MacroAssembler::freeStackTo(uint32_t framePushed) {
  MOZ_ASSERT(framePushed <= framePushed_);
  lea(Operand(FramePointer, -int32_t(framePushed)), StackPointer);
  framePushed_ = framePushed;
}

// ===============================================================
// Simple call functions.

CodeOffset MacroAssembler::call(Register reg) { return Assembler::call(reg); }

CodeOffset MacroAssembler::call(Label* label) { return Assembler::call(label); }

void MacroAssembler::call(const Address& addr) {
  Assembler::call(Operand(addr.base, addr.offset));
}

CodeOffset MacroAssembler::call(wasm::SymbolicAddress target) {
  mov(target, eax);
  return Assembler::call(eax);
}

void MacroAssembler::call(ImmWord target) { Assembler::call(target); }

void MacroAssembler::call(ImmPtr target) { Assembler::call(target); }

void MacroAssembler::call(JitCode* target) { Assembler::call(target); }

CodeOffset MacroAssembler::callWithPatch() {
  return Assembler::callWithPatch();
}
void MacroAssembler::patchCall(uint32_t callerOffset, uint32_t calleeOffset) {
  Assembler::patchCall(callerOffset, calleeOffset);
}

void MacroAssembler::callAndPushReturnAddress(Register reg) { call(reg); }

void MacroAssembler::callAndPushReturnAddress(Label* label) { call(label); }

// ===============================================================
// Patchable near/far jumps.

CodeOffset MacroAssembler::farJumpWithPatch() {
  return Assembler::farJumpWithPatch();
}

void MacroAssembler::patchFarJump(CodeOffset farJump, uint32_t targetOffset) {
  Assembler::patchFarJump(farJump, targetOffset);
}

void MacroAssembler::patchFarJump(uint8_t* farJump, uint8_t* target) {
  Assembler::patchFarJump(farJump, target);
}

CodeOffset MacroAssembler::nopPatchableToCall() {
  masm.nop_five();
  return CodeOffset(currentOffset());
}

void MacroAssembler::patchNopToCall(uint8_t* callsite, uint8_t* target) {
  Assembler::patchFiveByteNopToCall(callsite, target);
}

void MacroAssembler::patchCallToNop(uint8_t* callsite) {
  Assembler::patchCallToFiveByteNop(callsite);
}

CodeOffset MacroAssembler::move32WithPatch(Register dest) {
  movl(Imm32(-1), dest);
  return CodeOffset(currentOffset());
}

void MacroAssembler::patchMove32(CodeOffset offset, Imm32 n) {
  X86Encoding::SetInt32(masm.data() + offset.offset(), n.value);
}

// ===============================================================
// Jit Frames.

uint32_t MacroAssembler::pushFakeReturnAddress(Register scratch) {
  CodeLabel cl;

  mov(&cl, scratch);
  Push(scratch);
  bind(&cl);
  uint32_t retAddr = currentOffset();

  addCodeLabel(cl);
  return retAddr;
}

// ===============================================================
// WebAssembly

FaultingCodeOffset MacroAssembler::wasmTrapInstruction() {
  return FaultingCodeOffset(ud2().offset());
}

void MacroAssembler::wasmBoundsCheck32(Condition cond, Register index,
                                       Register boundsCheckLimit, Label* ok) {
  cmp32(index, boundsCheckLimit);
  j(cond, ok);
  if (JitOptions.spectreIndexMasking) {
    cmovCCl(cond, Operand(boundsCheckLimit), index);
  }
}

void MacroAssembler::wasmBoundsCheck32(Condition cond, Register index,
                                       Address boundsCheckLimit, Label* ok) {
  cmp32(index, Operand(boundsCheckLimit));
  j(cond, ok);
  if (JitOptions.spectreIndexMasking) {
    cmovCCl(cond, Operand(boundsCheckLimit), index);
  }
}

// RAII class that generates the jumps to traps when it's destructed, to
// prevent some code duplication in the outOfLineWasmTruncateXtoY methods.
struct MOZ_RAII AutoHandleWasmTruncateToIntErrors {
  MacroAssembler& masm;
  Label inputIsNaN;
  Label intOverflow;
  const wasm::TrapSiteDesc& trapSiteDesc;

  explicit AutoHandleWasmTruncateToIntErrors(
      MacroAssembler& masm, const wasm::TrapSiteDesc& trapSiteDesc)
      : masm(masm), trapSiteDesc(trapSiteDesc) {}

  ~AutoHandleWasmTruncateToIntErrors() {
    // Handle errors.  These cases are not in arbitrary order: code will
    // fall through to intOverflow.
    masm.bind(&intOverflow);
    masm.wasmTrap(wasm::Trap::IntegerOverflow, trapSiteDesc);

    masm.bind(&inputIsNaN);
    masm.wasmTrap(wasm::Trap::InvalidConversionToInteger, trapSiteDesc);
  }
};

void MacroAssembler::wasmTruncateDoubleToInt32(FloatRegister input,
                                               Register output,
                                               bool isSaturating,
                                               Label* oolEntry) {
  vcvttsd2si(input, output);
  cmp32(output, Imm32(1));
  j(Assembler::Overflow, oolEntry);
}

void MacroAssembler::wasmTruncateFloat32ToInt32(FloatRegister input,
                                                Register output,
                                                bool isSaturating,
                                                Label* oolEntry) {
  vcvttss2si(input, output);
  cmp32(output, Imm32(1));
  j(Assembler::Overflow, oolEntry);
}

void MacroAssembler::oolWasmTruncateCheckF64ToI32(
    FloatRegister input, Register output, TruncFlags flags,
    const wasm::TrapSiteDesc& trapSiteDesc, Label* rejoin) {
  bool isUnsigned = flags & TRUNC_UNSIGNED;
  bool isSaturating = flags & TRUNC_SATURATING;

  if (isSaturating) {
    if (isUnsigned) {
      // Negative overflow and NaN both are converted to 0, and the only
      // other case is positive overflow which is converted to
      // UINT32_MAX.
      Label nonNegative;
      ScratchDoubleScope fpscratch(*this);
      loadConstantDouble(0.0, fpscratch);
      branchDouble(Assembler::DoubleGreaterThanOrEqual, input, fpscratch,
                   &nonNegative);
      move32(Imm32(0), output);
      jump(rejoin);

      bind(&nonNegative);
      move32(Imm32(UINT32_MAX), output);
    } else {
      // Negative overflow is already saturated to INT32_MIN, so we only
      // have to handle NaN and positive overflow here.
      Label notNaN;
      branchDouble(Assembler::DoubleOrdered, input, input, &notNaN);
      move32(Imm32(0), output);
      jump(rejoin);

      bind(&notNaN);
      ScratchDoubleScope fpscratch(*this);
      loadConstantDouble(0.0, fpscratch);
      branchDouble(Assembler::DoubleLessThan, input, fpscratch, rejoin);
      sub32(Imm32(1), output);
    }
    jump(rejoin);
    return;
  }

  AutoHandleWasmTruncateToIntErrors traps(*this, trapSiteDesc);

  // Eagerly take care of NaNs.
  branchDouble(Assembler::DoubleUnordered, input, input, &traps.inputIsNaN);

  // For unsigned, fall through to intOverflow failure case.
  if (isUnsigned) {
    return;
  }

  // Handle special values.

  // We've used vcvttsd2si. The only valid double values that can
  // truncate to INT32_MIN are in ]INT32_MIN - 1; INT32_MIN].
  ScratchDoubleScope fpscratch(*this);
  loadConstantDouble(double(INT32_MIN) - 1.0, fpscratch);
  branchDouble(Assembler::DoubleLessThanOrEqual, input, fpscratch,
               &traps.intOverflow);

  loadConstantDouble(0.0, fpscratch);
  branchDouble(Assembler::DoubleGreaterThan, input, fpscratch,
               &traps.intOverflow);
  jump(rejoin);
}

void MacroAssembler::oolWasmTruncateCheckF32ToI32(
    FloatRegister input, Register output, TruncFlags flags,
    const wasm::TrapSiteDesc& trapSiteDesc, Label* rejoin) {
  bool isUnsigned = flags & TRUNC_UNSIGNED;
  bool isSaturating = flags & TRUNC_SATURATING;

  if (isSaturating) {
    if (isUnsigned) {
      // Negative overflow and NaN both are converted to 0, and the only
      // other case is positive overflow which is converted to
      // UINT32_MAX.
      Label nonNegative;
      ScratchFloat32Scope fpscratch(*this);
      loadConstantFloat32(0.0f, fpscratch);
      branchFloat(Assembler::DoubleGreaterThanOrEqual, input, fpscratch,
                  &nonNegative);
      move32(Imm32(0), output);
      jump(rejoin);

      bind(&nonNegative);
      move32(Imm32(UINT32_MAX), output);
    } else {
      // Negative overflow is already saturated to INT32_MIN, so we only
      // have to handle NaN and positive overflow here.
      Label notNaN;
      branchFloat(Assembler::DoubleOrdered, input, input, &notNaN);
      move32(Imm32(0), output);
      jump(rejoin);

      bind(&notNaN);
      ScratchFloat32Scope fpscratch(*this);
      loadConstantFloat32(0.0f, fpscratch);
      branchFloat(Assembler::DoubleLessThan, input, fpscratch, rejoin);
      sub32(Imm32(1), output);
    }
    jump(rejoin);
    return;
  }

  AutoHandleWasmTruncateToIntErrors traps(*this, trapSiteDesc);

  // Eagerly take care of NaNs.
  branchFloat(Assembler::DoubleUnordered, input, input, &traps.inputIsNaN);

  // For unsigned, fall through to intOverflow failure case.
  if (isUnsigned) {
    return;
  }

  // Handle special values.

  // We've used vcvttss2si. Check that the input wasn't
  // float(INT32_MIN), which is the only legimitate input that
  // would truncate to INT32_MIN.
  ScratchFloat32Scope fpscratch(*this);
  loadConstantFloat32(float(INT32_MIN), fpscratch);
  branchFloat(Assembler::DoubleNotEqual, input, fpscratch, &traps.intOverflow);
  jump(rejoin);
}

void MacroAssembler::oolWasmTruncateCheckF64ToI64(
    FloatRegister input, Register64 output, TruncFlags flags,
    const wasm::TrapSiteDesc& trapSiteDesc, Label* rejoin) {
  bool isUnsigned = flags & TRUNC_UNSIGNED;
  bool isSaturating = flags & TRUNC_SATURATING;

  if (isSaturating) {
    if (isUnsigned) {
      // Negative overflow and NaN both are converted to 0, and the only
      // other case is positive overflow which is converted to
      // UINT64_MAX.
      Label positive;
      ScratchDoubleScope fpscratch(*this);
      loadConstantDouble(0.0, fpscratch);
      branchDouble(Assembler::DoubleGreaterThan, input, fpscratch, &positive);
      move64(Imm64(0), output);
      jump(rejoin);

      bind(&positive);
      move64(Imm64(UINT64_MAX), output);
    } else {
      // Negative overflow is already saturated to INT64_MIN, so we only
      // have to handle NaN and positive overflow here.
      Label notNaN;
      branchDouble(Assembler::DoubleOrdered, input, input, &notNaN);
      move64(Imm64(0), output);
      jump(rejoin);

      bind(&notNaN);
      ScratchDoubleScope fpscratch(*this);
      loadConstantDouble(0.0, fpscratch);
      branchDouble(Assembler::DoubleLessThan, input, fpscratch, rejoin);
      sub64(Imm64(1), output);
    }
    jump(rejoin);
    return;
  }

  AutoHandleWasmTruncateToIntErrors traps(*this, trapSiteDesc);

  // Eagerly take care of NaNs.
  branchDouble(Assembler::DoubleUnordered, input, input, &traps.inputIsNaN);

  // Handle special values.
  if (isUnsigned) {
    ScratchDoubleScope fpscratch(*this);
    loadConstantDouble(0.0, fpscratch);
    branchDouble(Assembler::DoubleGreaterThan, input, fpscratch,
                 &traps.intOverflow);
    loadConstantDouble(-1.0, fpscratch);
    branchDouble(Assembler::DoubleLessThanOrEqual, input, fpscratch,
                 &traps.intOverflow);
    jump(rejoin);
    return;
  }

  // We've used vcvtsd2sq. The only legit value whose i64
  // truncation is INT64_MIN is double(INT64_MIN): exponent is so
  // high that the highest resolution around is much more than 1.
  ScratchDoubleScope fpscratch(*this);
  loadConstantDouble(double(int64_t(INT64_MIN)), fpscratch);
  branchDouble(Assembler::DoubleNotEqual, input, fpscratch, &traps.intOverflow);
  jump(rejoin);
}

void MacroAssembler::oolWasmTruncateCheckF32ToI64(
    FloatRegister input, Register64 output, TruncFlags flags,
    const wasm::TrapSiteDesc& trapSiteDesc, Label* rejoin) {
  bool isUnsigned = flags & TRUNC_UNSIGNED;
  bool isSaturating = flags & TRUNC_SATURATING;

  if (isSaturating) {
    if (isUnsigned) {
      // Negative overflow and NaN both are converted to 0, and the only
      // other case is positive overflow which is converted to
      // UINT64_MAX.
      Label positive;
      ScratchFloat32Scope fpscratch(*this);
      loadConstantFloat32(0.0f, fpscratch);
      branchFloat(Assembler::DoubleGreaterThan, input, fpscratch, &positive);
      move64(Imm64(0), output);
      jump(rejoin);

      bind(&positive);
      move64(Imm64(UINT64_MAX), output);
    } else {
      // Negative overflow is already saturated to INT64_MIN, so we only
      // have to handle NaN and positive overflow here.
      Label notNaN;
      branchFloat(Assembler::DoubleOrdered, input, input, &notNaN);
      move64(Imm64(0), output);
      jump(rejoin);

      bind(&notNaN);
      ScratchFloat32Scope fpscratch(*this);
      loadConstantFloat32(0.0f, fpscratch);
      branchFloat(Assembler::DoubleLessThan, input, fpscratch, rejoin);
      sub64(Imm64(1), output);
    }
    jump(rejoin);
    return;
  }

  AutoHandleWasmTruncateToIntErrors traps(*this, trapSiteDesc);

  // Eagerly take care of NaNs.
  branchFloat(Assembler::DoubleUnordered, input, input, &traps.inputIsNaN);

  // Handle special values.
  if (isUnsigned) {
    ScratchFloat32Scope fpscratch(*this);
    loadConstantFloat32(0.0f, fpscratch);
    branchFloat(Assembler::DoubleGreaterThan, input, fpscratch,
                &traps.intOverflow);
    loadConstantFloat32(-1.0f, fpscratch);
    branchFloat(Assembler::DoubleLessThanOrEqual, input, fpscratch,
                &traps.intOverflow);
    jump(rejoin);
    return;
  }

  // We've used vcvtss2sq. See comment in outOfLineWasmTruncateDoubleToInt64.
  ScratchFloat32Scope fpscratch(*this);
  loadConstantFloat32(float(int64_t(INT64_MIN)), fpscratch);
  branchFloat(Assembler::DoubleNotEqual, input, fpscratch, &traps.intOverflow);
  jump(rejoin);
}

void MacroAssembler::enterFakeExitFrameForWasm(Register cxreg, Register scratch,
                                               ExitFrameType type) {
  enterFakeExitFrame(cxreg, scratch, type);
}

CodeOffset MacroAssembler::sub32FromMemAndBranchIfNegativeWithPatch(
    Address address, Label* label) {
  // -128 is arbitrary, but makes `*address` count upwards, which may help
  // to identify cases where the subsequent ::patch..() call was forgotten.
  int numImmBytes = subl(Imm32(-128), Operand(address));
  // This is vitally important for patching
  MOZ_RELEASE_ASSERT(numImmBytes == 1);
  // Points immediately after the location to patch
  CodeOffset patchPoint = CodeOffset(currentOffset());
  jSrc(Condition::Signed, label);
  return patchPoint;
}

void MacroAssembler::patchSub32FromMemAndBranchIfNegative(CodeOffset offset,
                                                          Imm32 imm) {
  int32_t val = imm.value;
  // Patching it to zero would make the insn pointless
  MOZ_RELEASE_ASSERT(val >= 1 && val <= 127);
  uint8_t* ptr = (uint8_t*)masm.data() + offset.offset() - 1;
  MOZ_RELEASE_ASSERT(*ptr == uint8_t(-128));  // as created above
  *ptr = uint8_t(val) & 0x7F;
}

// ========================================================================
// Primitive atomic operations.

static void ExtendTo32(MacroAssembler& masm, Scalar::Type type, Register r) {
  switch (type) {
    case Scalar::Int8:
      masm.movsbl(r, r);
      break;
    case Scalar::Uint8:
      masm.movzbl(r, r);
      break;
    case Scalar::Int16:
      masm.movswl(r, r);
      break;
    case Scalar::Uint16:
      masm.movzwl(r, r);
      break;
    case Scalar::Int32:
    case Scalar::Uint32:
      break;
    default:
      MOZ_CRASH("unexpected type");
  }
}

#ifdef DEBUG
static inline bool IsByteReg(Register r) {
  AllocatableGeneralRegisterSet byteRegs(Registers::SingleByteRegs);
  return byteRegs.has(r);
}

static inline bool IsByteReg(Imm32 r) {
  // Nothing
  return true;
}
#endif

template <typename T>
static void CompareExchange(MacroAssembler& masm,
                            const wasm::MemoryAccessDesc* access,
                            Scalar::Type type, const T& mem, Register oldval,
                            Register newval, Register output) {
  MOZ_ASSERT(output == eax);

  if (oldval != output) {
    masm.movl(oldval, output);
  }

  if (access) {
    masm.append(*access, wasm::TrapMachineInsn::Atomic,
                FaultingCodeOffset(masm.currentOffset()));
  }

  // NOTE: the generated code must match the assembly code in gen_cmpxchg in
  // GenerateAtomicOperations.py
  switch (Scalar::byteSize(type)) {
    case 1:
      MOZ_ASSERT(IsByteReg(newval));
      masm.lock_cmpxchgb(newval, Operand(mem));
      break;
    case 2:
      masm.lock_cmpxchgw(newval, Operand(mem));
      break;
    case 4:
      masm.lock_cmpxchgl(newval, Operand(mem));
      break;
    default:
      MOZ_CRASH("Invalid");
  }

  ExtendTo32(masm, type, output);
}

void MacroAssembler::compareExchange(Scalar::Type type, Synchronization,
                                     const Address& mem, Register oldval,
                                     Register newval, Register output) {
  CompareExchange(*this, nullptr, type, mem, oldval, newval, output);
}

void MacroAssembler::compareExchange(Scalar::Type type, Synchronization,
                                     const BaseIndex& mem, Register oldval,
                                     Register newval, Register output) {
  CompareExchange(*this, nullptr, type, mem, oldval, newval, output);
}

void MacroAssembler::wasmCompareExchange(const wasm::MemoryAccessDesc& access,
                                         const Address& mem, Register oldval,
                                         Register newval, Register output) {
  CompareExchange(*this, &access, access.type(), mem, oldval, newval, output);
}

void MacroAssembler::wasmCompareExchange(const wasm::MemoryAccessDesc& access,
                                         const BaseIndex& mem, Register oldval,
                                         Register newval, Register output) {
  CompareExchange(*this, &access, access.type(), mem, oldval, newval, output);
}

template <typename T>
static void AtomicExchange(MacroAssembler& masm,
                           const wasm::MemoryAccessDesc* access,
                           Scalar::Type type, const T& mem, Register value,
                           Register output)
// NOTE: the generated code must match the assembly code in gen_exchange in
// GenerateAtomicOperations.py
{
  if (value != output) {
    masm.movl(value, output);
  }

  if (access) {
    masm.append(*access, wasm::TrapMachineInsn::Atomic,
                FaultingCodeOffset(masm.currentOffset()));
  }

  switch (Scalar::byteSize(type)) {
    case 1:
      MOZ_ASSERT(IsByteReg(output));
      masm.xchgb(output, Operand(mem));
      break;
    case 2:
      masm.xchgw(output, Operand(mem));
      break;
    case 4:
      masm.xchgl(output, Operand(mem));
      break;
    default:
      MOZ_CRASH("Invalid");
  }
  ExtendTo32(masm, type, output);
}

void MacroAssembler::atomicExchange(Scalar::Type type, Synchronization,
                                    const Address& mem, Register value,
                                    Register output) {
  AtomicExchange(*this, nullptr, type, mem, value, output);
}

void MacroAssembler::atomicExchange(Scalar::Type type, Synchronization,
                                    const BaseIndex& mem, Register value,
                                    Register output) {
  AtomicExchange(*this, nullptr, type, mem, value, output);
}

void MacroAssembler::wasmAtomicExchange(const wasm::MemoryAccessDesc& access,
                                        const Address& mem, Register value,
                                        Register output) {
  AtomicExchange(*this, &access, access.type(), mem, value, output);
}

void MacroAssembler::wasmAtomicExchange(const wasm::MemoryAccessDesc& access,
                                        const BaseIndex& mem, Register value,
                                        Register output) {
  AtomicExchange(*this, &access, access.type(), mem, value, output);
}

static void SetupValue(MacroAssembler& masm, AtomicOp op, Imm32 src,
                       Register output) {
  if (op == AtomicOp::Sub) {
    masm.movl(Imm32(-src.value), output);
  } else {
    masm.movl(src, output);
  }
}

static void SetupValue(MacroAssembler& masm, AtomicOp op, Register src,
                       Register output) {
  if (src != output) {
    masm.movl(src, output);
  }
  if (op == AtomicOp::Sub) {
    masm.negl(output);
  }
}

static auto WasmTrapMachineInsn(Scalar::Type arrayType, AtomicOp op) {
  switch (op) {
    case AtomicOp::Add:
    case AtomicOp::Sub:
      return wasm::TrapMachineInsn::Atomic;
    case AtomicOp::And:
    case AtomicOp::Or:
    case AtomicOp::Xor:
      switch (arrayType) {
        case Scalar::Int8:
        case Scalar::Uint8:
          return wasm::TrapMachineInsn::Load8;
        case Scalar::Int16:
        case Scalar::Uint16:
          return wasm::TrapMachineInsn::Load16;
        case Scalar::Int32:
        case Scalar::Uint32:
          return wasm::TrapMachineInsn::Load32;
        default:
          break;
      }
      [[fallthrough]];
    default:
      break;
  }
  MOZ_CRASH();
}

template <typename T, typename V>
static void AtomicFetchOp(MacroAssembler& masm,
                          const wasm::MemoryAccessDesc* access,
                          Scalar::Type arrayType, AtomicOp op, V value,
                          const T& mem, Register temp, Register output) {
  // Note value can be an Imm or a Register.

  // NOTE: the generated code must match the assembly code in gen_fetchop in
  // GenerateAtomicOperations.py

  // Setup the output register.
  switch (op) {
    case AtomicOp::Add:
    case AtomicOp::Sub:
      MOZ_ASSERT(temp == InvalidReg);
      MOZ_ASSERT_IF(Scalar::byteSize(arrayType) == 1,
                    IsByteReg(output) && IsByteReg(value));

      SetupValue(masm, op, value, output);
      break;
    case AtomicOp::And:
    case AtomicOp::Or:
    case AtomicOp::Xor:
      MOZ_ASSERT(output != temp && output == eax);
      MOZ_ASSERT_IF(Scalar::byteSize(arrayType) == 1,
                    IsByteReg(output) && IsByteReg(temp));

      // Bitwise operations don't require any additional setup.
      break;
    default:
      MOZ_CRASH();
  }

  auto lock_xadd = [&]() {
    switch (arrayType) {
      case Scalar::Int8:
      case Scalar::Uint8:
        masm.lock_xaddb(output, Operand(mem));
        break;
      case Scalar::Int16:
      case Scalar::Uint16:
        masm.lock_xaddw(output, Operand(mem));
        break;
      case Scalar::Int32:
      case Scalar::Uint32:
        masm.lock_xaddl(output, Operand(mem));
        break;
      default:
        MOZ_CRASH();
    }
  };

  auto load = [&]() {
    switch (arrayType) {
      case Scalar::Int8:
      case Scalar::Uint8:
        masm.movzbl(Operand(mem), eax);
        break;
      case Scalar::Int16:
      case Scalar::Uint16:
        masm.movzwl(Operand(mem), eax);
        break;
      case Scalar::Int32:
      case Scalar::Uint32:
        masm.movl(Operand(mem), eax);
        break;
      default:
        MOZ_CRASH();
    }
  };

  auto bitwiseOp = [&]() {
    switch (op) {
      case AtomicOp::And:
        masm.andl(value, temp);
        break;
      case AtomicOp::Or:
        masm.orl(value, temp);
        break;
      case AtomicOp::Xor:
        masm.xorl(value, temp);
        break;
      default:
        MOZ_CRASH();
    }
  };

  auto lock_cmpxchg = [&]() {
    switch (arrayType) {
      case Scalar::Int8:
      case Scalar::Uint8:
        masm.lock_cmpxchgb(temp, Operand(mem));
        break;
      case Scalar::Int16:
      case Scalar::Uint16:
        masm.lock_cmpxchgw(temp, Operand(mem));
        break;
      case Scalar::Int32:
      case Scalar::Uint32:
        masm.lock_cmpxchgl(temp, Operand(mem));
        break;
      default:
        MOZ_CRASH();
    }
  };

  // Add trap instruction directly before the load.
  if (access) {
    masm.append(*access, WasmTrapMachineInsn(arrayType, op),
                FaultingCodeOffset(masm.currentOffset()));
  }

  switch (op) {
    case AtomicOp::Add:
    case AtomicOp::Sub:
      // `add` and `sub` operations can be optimized with XADD.
      lock_xadd();

      ExtendTo32(masm, arrayType, output);
      break;

    case AtomicOp::And:
    case AtomicOp::Or:
    case AtomicOp::Xor: {
      // Bitwise operations need a CAS loop.

      // Load memory into eax.
      load();

      // Loop.
      Label again;
      masm.bind(&again);
      masm.movl(eax, temp);

      // temp = temp <op> value.
      bitwiseOp();

      // Compare and swap `temp` with memory.
      lock_cmpxchg();

      // Repeat if the comparison failed.
      masm.j(MacroAssembler::NonZero, &again);

      // Sign-extend the zero-extended load.
      if (Scalar::isSignedIntType(arrayType)) {
        ExtendTo32(masm, arrayType, eax);
      }
      break;
    }

    default:
      MOZ_CRASH();
  }
}

void MacroAssembler::atomicFetchOp(Scalar::Type arrayType, Synchronization,
                                   AtomicOp op, Register value,
                                   const BaseIndex& mem, Register temp,
                                   Register output) {
  AtomicFetchOp(*this, nullptr, arrayType, op, value, mem, temp, output);
}

void MacroAssembler::atomicFetchOp(Scalar::Type arrayType, Synchronization,
                                   AtomicOp op, Register value,
                                   const Address& mem, Register temp,
                                   Register output) {
  AtomicFetchOp(*this, nullptr, arrayType, op, value, mem, temp, output);
}

void MacroAssembler::atomicFetchOp(Scalar::Type arrayType, Synchronization,
                                   AtomicOp op, Imm32 value,
                                   const BaseIndex& mem, Register temp,
                                   Register output) {
  AtomicFetchOp(*this, nullptr, arrayType, op, value, mem, temp, output);
}

void MacroAssembler::atomicFetchOp(Scalar::Type arrayType, Synchronization,
                                   AtomicOp op, Imm32 value, const Address& mem,
                                   Register temp, Register output) {
  AtomicFetchOp(*this, nullptr, arrayType, op, value, mem, temp, output);
}

void MacroAssembler::wasmAtomicFetchOp(const wasm::MemoryAccessDesc& access,
                                       AtomicOp op, Register value,
                                       const Address& mem, Register temp,
                                       Register output) {
  AtomicFetchOp(*this, &access, access.type(), op, value, mem, temp, output);
}

void MacroAssembler::wasmAtomicFetchOp(const wasm::MemoryAccessDesc& access,
                                       AtomicOp op, Imm32 value,
                                       const Address& mem, Register temp,
                                       Register output) {
  AtomicFetchOp(*this, &access, access.type(), op, value, mem, temp, output);
}

void MacroAssembler::wasmAtomicFetchOp(const wasm::MemoryAccessDesc& access,
                                       AtomicOp op, Register value,
                                       const BaseIndex& mem, Register temp,
                                       Register output) {
  AtomicFetchOp(*this, &access, access.type(), op, value, mem, temp, output);
}

void MacroAssembler::wasmAtomicFetchOp(const wasm::MemoryAccessDesc& access,
                                       AtomicOp op, Imm32 value,
                                       const BaseIndex& mem, Register temp,
                                       Register output) {
  AtomicFetchOp(*this, &access, access.type(), op, value, mem, temp, output);
}

template <typename T, typename V>
static void AtomicEffectOp(MacroAssembler& masm,
                           const wasm::MemoryAccessDesc* access,
                           Scalar::Type arrayType, AtomicOp op, V value,
                           const T& mem) {
  if (access) {
    masm.append(*access, wasm::TrapMachineInsn::Atomic,
                FaultingCodeOffset(masm.currentOffset()));
  }

  switch (Scalar::byteSize(arrayType)) {
    case 1:
      switch (op) {
        case AtomicOp::Add:
          masm.lock_addb(value, Operand(mem));
          break;
        case AtomicOp::Sub:
          masm.lock_subb(value, Operand(mem));
          break;
        case AtomicOp::And:
          masm.lock_andb(value, Operand(mem));
          break;
        case AtomicOp::Or:
          masm.lock_orb(value, Operand(mem));
          break;
        case AtomicOp::Xor:
          masm.lock_xorb(value, Operand(mem));
          break;
        default:
          MOZ_CRASH();
      }
      break;
    case 2:
      switch (op) {
        case AtomicOp::Add:
          masm.lock_addw(value, Operand(mem));
          break;
        case AtomicOp::Sub:
          masm.lock_subw(value, Operand(mem));
          break;
        case AtomicOp::And:
          masm.lock_andw(value, Operand(mem));
          break;
        case AtomicOp::Or:
          masm.lock_orw(value, Operand(mem));
          break;
        case AtomicOp::Xor:
          masm.lock_xorw(value, Operand(mem));
          break;
        default:
          MOZ_CRASH();
      }
      break;
    case 4:
      switch (op) {
        case AtomicOp::Add:
          masm.lock_addl(value, Operand(mem));
          break;
        case AtomicOp::Sub:
          masm.lock_subl(value, Operand(mem));
          break;
        case AtomicOp::And:
          masm.lock_andl(value, Operand(mem));
          break;
        case AtomicOp::Or:
          masm.lock_orl(value, Operand(mem));
          break;
        case AtomicOp::Xor:
          masm.lock_xorl(value, Operand(mem));
          break;
        default:
          MOZ_CRASH();
      }
      break;
    default:
      MOZ_CRASH();
  }
}

void MacroAssembler::wasmAtomicEffectOp(const wasm::MemoryAccessDesc& access,
                                        AtomicOp op, Register value,
                                        const Address& mem, Register temp) {
  MOZ_ASSERT(temp == InvalidReg);
  AtomicEffectOp(*this, &access, access.type(), op, value, mem);
}

void MacroAssembler::wasmAtomicEffectOp(const wasm::MemoryAccessDesc& access,
                                        AtomicOp op, Imm32 value,
                                        const Address& mem, Register temp) {
  MOZ_ASSERT(temp == InvalidReg);
  AtomicEffectOp(*this, &access, access.type(), op, value, mem);
}

void MacroAssembler::wasmAtomicEffectOp(const wasm::MemoryAccessDesc& access,
                                        AtomicOp op, Register value,
                                        const BaseIndex& mem, Register temp) {
  MOZ_ASSERT(temp == InvalidReg);
  AtomicEffectOp(*this, &access, access.type(), op, value, mem);
}

void MacroAssembler::wasmAtomicEffectOp(const wasm::MemoryAccessDesc& access,
                                        AtomicOp op, Imm32 value,
                                        const BaseIndex& mem, Register temp) {
  MOZ_ASSERT(temp == InvalidReg);
  AtomicEffectOp(*this, &access, access.type(), op, value, mem);
}

// ========================================================================
// JS atomic operations.

template <typename T>
static void CompareExchangeJS(MacroAssembler& masm, Scalar::Type arrayType,
                              Synchronization sync, const T& mem,
                              Register oldval, Register newval, Register temp,
                              AnyRegister output) {
  if (arrayType == Scalar::Uint32) {
    masm.compareExchange(arrayType, sync, mem, oldval, newval, temp);
    masm.convertUInt32ToDouble(temp, output.fpu());
  } else {
    masm.compareExchange(arrayType, sync, mem, oldval, newval, output.gpr());
  }
}

void MacroAssembler::compareExchangeJS(Scalar::Type arrayType,
                                       Synchronization sync, const Address& mem,
                                       Register oldval, Register newval,
                                       Register temp, AnyRegister output) {
  CompareExchangeJS(*this, arrayType, sync, mem, oldval, newval, temp, output);
}

void MacroAssembler::compareExchangeJS(Scalar::Type arrayType,
                                       Synchronization sync,
                                       const BaseIndex& mem, Register oldval,
                                       Register newval, Register temp,
                                       AnyRegister output) {
  CompareExchangeJS(*this, arrayType, sync, mem, oldval, newval, temp, output);
}

template <typename T>
static void AtomicExchangeJS(MacroAssembler& masm, Scalar::Type arrayType,
                             Synchronization sync, const T& mem, Register value,
                             Register temp, AnyRegister output) {
  if (arrayType == Scalar::Uint32) {
    masm.atomicExchange(arrayType, sync, mem, value, temp);
    masm.convertUInt32ToDouble(temp, output.fpu());
  } else {
    masm.atomicExchange(arrayType, sync, mem, value, output.gpr());
  }
}

void MacroAssembler::atomicExchangeJS(Scalar::Type arrayType,
                                      Synchronization sync, const Address& mem,
                                      Register value, Register temp,
                                      AnyRegister output) {
  AtomicExchangeJS(*this, arrayType, sync, mem, value, temp, output);
}

void MacroAssembler::atomicExchangeJS(Scalar::Type arrayType,
                                      Synchronization sync,
                                      const BaseIndex& mem, Register value,
                                      Register temp, AnyRegister output) {
  AtomicExchangeJS(*this, arrayType, sync, mem, value, temp, output);
}

template <typename T>
static void AtomicFetchOpJS(MacroAssembler& masm, Scalar::Type arrayType,
                            Synchronization sync, AtomicOp op, Register value,
                            const T& mem, Register temp1, Register temp2,
                            AnyRegister output) {
  if (arrayType == Scalar::Uint32) {
    masm.atomicFetchOp(arrayType, sync, op, value, mem, temp2, temp1);
    masm.convertUInt32ToDouble(temp1, output.fpu());
  } else {
    masm.atomicFetchOp(arrayType, sync, op, value, mem, temp1, output.gpr());
  }
}

void MacroAssembler::atomicFetchOpJS(Scalar::Type arrayType,
                                     Synchronization sync, AtomicOp op,
                                     Register value, const Address& mem,
                                     Register temp1, Register temp2,
                                     AnyRegister output) {
  AtomicFetchOpJS(*this, arrayType, sync, op, value, mem, temp1, temp2, output);
}

void MacroAssembler::atomicFetchOpJS(Scalar::Type arrayType,
                                     Synchronization sync, AtomicOp op,
                                     Register value, const BaseIndex& mem,
                                     Register temp1, Register temp2,
                                     AnyRegister output) {
  AtomicFetchOpJS(*this, arrayType, sync, op, value, mem, temp1, temp2, output);
}

void MacroAssembler::atomicEffectOpJS(Scalar::Type arrayType, Synchronization,
                                      AtomicOp op, Register value,
                                      const BaseIndex& mem, Register temp) {
  MOZ_ASSERT(temp == InvalidReg);
  AtomicEffectOp(*this, nullptr, arrayType, op, value, mem);
}

void MacroAssembler::atomicEffectOpJS(Scalar::Type arrayType, Synchronization,
                                      AtomicOp op, Register value,
                                      const Address& mem, Register temp) {
  MOZ_ASSERT(temp == InvalidReg);
  AtomicEffectOp(*this, nullptr, arrayType, op, value, mem);
}

void MacroAssembler::atomicEffectOpJS(Scalar::Type arrayType, Synchronization,
                                      AtomicOp op, Imm32 value,
                                      const Address& mem, Register temp) {
  MOZ_ASSERT(temp == InvalidReg);
  AtomicEffectOp(*this, nullptr, arrayType, op, value, mem);
}

void MacroAssembler::atomicEffectOpJS(Scalar::Type arrayType,
                                      Synchronization sync, AtomicOp op,
                                      Imm32 value, const BaseIndex& mem,
                                      Register temp) {
  MOZ_ASSERT(temp == InvalidReg);
  AtomicEffectOp(*this, nullptr, arrayType, op, value, mem);
}

template <typename T>
static void AtomicFetchOpJS(MacroAssembler& masm, Scalar::Type arrayType,
                            Synchronization sync, AtomicOp op, Imm32 value,
                            const T& mem, Register temp1, Register temp2,
                            AnyRegister output) {
  if (arrayType == Scalar::Uint32) {
    masm.atomicFetchOp(arrayType, sync, op, value, mem, temp2, temp1);
    masm.convertUInt32ToDouble(temp1, output.fpu());
  } else {
    masm.atomicFetchOp(arrayType, sync, op, value, mem, temp1, output.gpr());
  }
}

void MacroAssembler::atomicFetchOpJS(Scalar::Type arrayType,
                                     Synchronization sync, AtomicOp op,
                                     Imm32 value, const Address& mem,
                                     Register temp1, Register temp2,
                                     AnyRegister output) {
  AtomicFetchOpJS(*this, arrayType, sync, op, value, mem, temp1, temp2, output);
}

void MacroAssembler::atomicFetchOpJS(Scalar::Type arrayType,
                                     Synchronization sync, AtomicOp op,
                                     Imm32 value, const BaseIndex& mem,
                                     Register temp1, Register temp2,
                                     AnyRegister output) {
  AtomicFetchOpJS(*this, arrayType, sync, op, value, mem, temp1, temp2, output);
}

void MacroAssembler::atomicPause() { masm.pause(); }

// ========================================================================
// Spectre Mitigations.

void MacroAssembler::speculationBarrier() {
  // Spectre mitigation recommended by Intel and AMD suggest to use lfence as
  // a way to force all speculative execution of instructions to end.
  MOZ_ASSERT(HasSSE2());
  masm.lfence();
}

void MacroAssembler::floorFloat32ToInt32(FloatRegister src, Register dest,
                                         Label* fail) {
  if (HasSSE41()) {
    // Fail on negative-zero.
    branchNegativeZeroFloat32(src, dest, fail);

    // Round toward -Infinity.
    {
      ScratchFloat32Scope scratch(*this);
      vroundss(X86Encoding::RoundDown, src, scratch);
      truncateFloat32ToInt32(scratch, dest, fail);
    }
  } else {
    Label negative, end;

    // Branch to a slow path for negative inputs. Doesn't catch NaN or -0.
    {
      ScratchFloat32Scope scratch(*this);
      zeroFloat32(scratch);
      branchFloat(Assembler::DoubleLessThan, src, scratch, &negative);
    }

    // Fail on negative-zero.
    branchNegativeZeroFloat32(src, dest, fail);

    // Input is non-negative, so truncation correctly rounds.
    truncateFloat32ToInt32(src, dest, fail);
    jump(&end);

    // Input is negative, but isn't -0.
    // Negative values go on a comparatively expensive path, since no
    // native rounding mode matches JS semantics. Still better than callVM.
    bind(&negative);
    {
      // Truncate and round toward zero.
      // This is off-by-one for everything but integer-valued inputs.
      //
      // Directly call vcvttss2si instead of truncateFloat32ToInt32 because we
      // want to perform failure handling ourselves.
      vcvttss2si(src, dest);

      // Test whether the input double was integer-valued.
      {
        ScratchFloat32Scope scratch(*this);
        convertInt32ToFloat32(dest, scratch);
        branchFloat(Assembler::DoubleEqualOrUnordered, src, scratch, &end);
      }

      // Input is not integer-valued, so we rounded off-by-one in the
      // wrong direction. Correct by subtraction.
      //
      // Overflows if vcvttss2si returned the failure return value INT_MIN.
      branchSub32(Assembler::Overflow, Imm32(1), dest, fail);
    }

    bind(&end);
  }
}

void MacroAssembler::floorDoubleToInt32(FloatRegister src, Register dest,
                                        Label* fail) {
  if (HasSSE41()) {
    // Fail on negative-zero.
    branchNegativeZero(src, dest, fail);

    // Round toward -Infinity.
    {
      ScratchDoubleScope scratch(*this);
      vroundsd(X86Encoding::RoundDown, src, scratch);
      truncateDoubleToInt32(scratch, dest, fail);
    }
  } else {
    Label negative, end;

    // Branch to a slow path for negative inputs. Doesn't catch NaN or -0.
    {
      ScratchDoubleScope scratch(*this);
      zeroDouble(scratch);
      branchDouble(Assembler::DoubleLessThan, src, scratch, &negative);
    }

    // Fail on negative-zero.
    branchNegativeZero(src, dest, fail);

    // Input is non-negative, so truncation correctly rounds.
    truncateDoubleToInt32(src, dest, fail);
    jump(&end);

    // Input is negative, but isn't -0.
    // Negative values go on a comparatively expensive path, since no
    // native rounding mode matches JS semantics. Still better than callVM.
    bind(&negative);
    {
      // Truncate and round toward zero.
      // This is off-by-one for everything but integer-valued inputs.
      //
      // Directly call vcvttsd2si instead of truncateDoubleToInt32 because we
      // want to perform failure handling ourselves.
      vcvttsd2si(src, dest);

      // Test whether the input double was integer-valued.
      {
        ScratchDoubleScope scratch(*this);
        convertInt32ToDouble(dest, scratch);
        branchDouble(Assembler::DoubleEqualOrUnordered, src, scratch, &end);
      }

      // Input is not integer-valued, so we rounded off-by-one in the
      // wrong direction. Correct by subtraction.
      //
      // Overflows if vcvttsd2si returned the failure return value INT_MIN.
      branchSub32(Assembler::Overflow, Imm32(1), dest, fail);
    }

    bind(&end);
  }
}

void MacroAssembler::ceilFloat32ToInt32(FloatRegister src, Register dest,
                                        Label* fail) {
  ScratchFloat32Scope scratch(*this);

  Label lessThanOrEqualMinusOne;

  // If x is in ]-1,0], ceil(x) is -0, which cannot be represented as an int32.
  // Fail if x > -1 and the sign bit is set.
  loadConstantFloat32(-1.f, scratch);
  branchFloat(Assembler::DoubleLessThanOrEqualOrUnordered, src, scratch,
              &lessThanOrEqualMinusOne);
  vmovmskps(src, dest);
  branchTest32(Assembler::NonZero, dest, Imm32(1), fail);

  if (HasSSE41()) {
    // x <= -1 or x > -0
    bind(&lessThanOrEqualMinusOne);
    // Round toward +Infinity.
    vroundss(X86Encoding::RoundUp, src, scratch);
    truncateFloat32ToInt32(scratch, dest, fail);
    return;
  }

  // No SSE4.1
  Label end;

  // x >= 0 and x is not -0.0. We can truncate integer values, and truncate and
  // add 1 to non-integer values. This will also work for values >= INT_MAX + 1,
  // as the truncate operation will return INT_MIN and we'll fail.
  truncateFloat32ToInt32(src, dest, fail);
  convertInt32ToFloat32(dest, scratch);
  branchFloat(Assembler::DoubleEqualOrUnordered, src, scratch, &end);

  // Input is not integer-valued, add 1 to obtain the ceiling value.
  // If input > INT_MAX, output == INT_MAX so adding 1 will overflow.
  branchAdd32(Assembler::Overflow, Imm32(1), dest, fail);
  jump(&end);

  // x <= -1, truncation is the way to go.
  bind(&lessThanOrEqualMinusOne);
  truncateFloat32ToInt32(src, dest, fail);

  bind(&end);
}

void MacroAssembler::ceilDoubleToInt32(FloatRegister src, Register dest,
                                       Label* fail) {
  ScratchDoubleScope scratch(*this);

  Label lessThanOrEqualMinusOne;

  // If x is in ]-1,0], ceil(x) is -0, which cannot be represented as an int32.
  // Fail if x > -1 and the sign bit is set.
  loadConstantDouble(-1.0, scratch);
  branchDouble(Assembler::DoubleLessThanOrEqualOrUnordered, src, scratch,
               &lessThanOrEqualMinusOne);
  vmovmskpd(src, dest);
  branchTest32(Assembler::NonZero, dest, Imm32(1), fail);

  if (HasSSE41()) {
    // x <= -1 or x > -0
    bind(&lessThanOrEqualMinusOne);
    // Round toward +Infinity.
    vroundsd(X86Encoding::RoundUp, src, scratch);
    truncateDoubleToInt32(scratch, dest, fail);
    return;
  }

  // No SSE4.1
  Label end;

  // x >= 0 and x is not -0.0. We can truncate integer values, and truncate and
  // add 1 to non-integer values. This will also work for values >= INT_MAX + 1,
  // as the truncate operation will return INT_MIN and we'll fail.
  truncateDoubleToInt32(src, dest, fail);
  convertInt32ToDouble(dest, scratch);
  branchDouble(Assembler::DoubleEqualOrUnordered, src, scratch, &end);

  // Input is not integer-valued, add 1 to obtain the ceiling value.
  // If input > INT_MAX, output == INT_MAX so adding 1 will overflow.
  branchAdd32(Assembler::Overflow, Imm32(1), dest, fail);
  jump(&end);

  // x <= -1, truncation is the way to go.
  bind(&lessThanOrEqualMinusOne);
  truncateDoubleToInt32(src, dest, fail);

  bind(&end);
}

void MacroAssembler::truncDoubleToInt32(FloatRegister src, Register dest,
                                        Label* fail) {
  Label lessThanOrEqualMinusOne;

  // Bail on ]-1; -0] range
  {
    ScratchDoubleScope scratch(*this);
    loadConstantDouble(-1, scratch);
    branchDouble(Assembler::DoubleLessThanOrEqualOrUnordered, src, scratch,
                 &lessThanOrEqualMinusOne);
  }

  // Test for remaining values with the sign bit set, i.e. ]-1; -0]
  vmovmskpd(src, dest);
  branchTest32(Assembler::NonZero, dest, Imm32(1), fail);

  // x <= -1 or x >= +0, truncation is the way to go.
  bind(&lessThanOrEqualMinusOne);
  truncateDoubleToInt32(src, dest, fail);
}

void MacroAssembler::truncFloat32ToInt32(FloatRegister src, Register dest,
                                         Label* fail) {
  Label lessThanOrEqualMinusOne;

  // Bail on ]-1; -0] range
  {
    ScratchFloat32Scope scratch(*this);
    loadConstantFloat32(-1.f, scratch);
    branchFloat(Assembler::DoubleLessThanOrEqualOrUnordered, src, scratch,
                &lessThanOrEqualMinusOne);
  }

  // Test for remaining values with the sign bit set, i.e. ]-1; -0]
  vmovmskps(src, dest);
  branchTest32(Assembler::NonZero, dest, Imm32(1), fail);

  // x <= -1 or x >= +0, truncation is the way to go.
  bind(&lessThanOrEqualMinusOne);
  truncateFloat32ToInt32(src, dest, fail);
}

void MacroAssembler::roundFloat32ToInt32(FloatRegister src, Register dest,
                                         FloatRegister temp, Label* fail) {
  ScratchFloat32Scope scratch(*this);

  Label negativeOrZero, negative, end;

  // Branch to a slow path for non-positive inputs. Doesn't catch NaN.
  zeroFloat32(scratch);
  loadConstantFloat32(GetBiggestNumberLessThan(0.5f), temp);
  branchFloat(Assembler::DoubleLessThanOrEqual, src, scratch, &negativeOrZero);
  {
    // Input is strictly positive or NaN. Add the biggest float less than 0.5
    // and truncate, rounding down (because if the input is the biggest float
    // less than 0.5, adding 0.5 would undesirably round up to 1). Note that we
    // have to add the input to the temp register because we're not allowed to
    // modify the input register.
    addFloat32(src, temp);
    truncateFloat32ToInt32(temp, dest, fail);
    jump(&end);
  }

  // Input is negative, +0 or -0.
  bind(&negativeOrZero);
  {
    // Branch on negative input.
    j(Assembler::NotEqual, &negative);

    // Fail on negative-zero.
    branchNegativeZeroFloat32(src, dest, fail);

    // Input is +0.
    xor32(dest, dest);
    jump(&end);
  }

  // Input is negative.
  bind(&negative);
  {
    // Inputs in [-0.5, 0) are rounded to -0. Fail.
    loadConstantFloat32(-0.5f, scratch);
    branchFloat(Assembler::DoubleGreaterThanOrEqual, src, scratch, fail);

    // Other negative inputs need the biggest float less than 0.5 added.
    //
    // The result is stored in the temp register (currently contains the biggest
    // float less than 0.5).
    addFloat32(src, temp);

    if (HasSSE41()) {
      // Round toward -Infinity.
      vroundss(X86Encoding::RoundDown, temp, scratch);

      // Truncate.
      truncateFloat32ToInt32(scratch, dest, fail);
    } else {
      // Round toward -Infinity without the benefit of ROUNDSS.

      // Truncate and round toward zero.
      // This is off-by-one for everything but integer-valued inputs.
      //
      // Directly call vcvttss2si instead of truncateFloat32ToInt32 because we
      // want to perform failure handling ourselves.
      vcvttss2si(temp, dest);

      // Test whether the truncated float was integer-valued.
      convertInt32ToFloat32(dest, scratch);
      branchFloat(Assembler::DoubleEqualOrUnordered, temp, scratch, &end);

      // Input is not integer-valued, so we rounded off-by-one in the
      // wrong direction. Correct by subtraction.
      //
      // Overflows if vcvttss2si returned the failure return value INT_MIN.
      branchSub32(Assembler::Overflow, Imm32(1), dest, fail);
    }
  }

  bind(&end);
}

void MacroAssembler::roundDoubleToInt32(FloatRegister src, Register dest,
                                        FloatRegister temp, Label* fail) {
  ScratchDoubleScope scratch(*this);

  Label negativeOrZero, negative, end;

  // Branch to a slow path for non-positive inputs. Doesn't catch NaN.
  zeroDouble(scratch);
  loadConstantDouble(GetBiggestNumberLessThan(0.5), temp);
  branchDouble(Assembler::DoubleLessThanOrEqual, src, scratch, &negativeOrZero);
  {
    // Input is strictly positive or NaN. Add the biggest double less than 0.5
    // and truncate, rounding down (because if the input is the biggest double
    // less than 0.5, adding 0.5 would undesirably round up to 1). Note that we
    // have to add the input to the temp register because we're not allowed to
    // modify the input register.
    addDouble(src, temp);
    truncateDoubleToInt32(temp, dest, fail);
    jump(&end);
  }

  // Input is negative, +0 or -0.
  bind(&negativeOrZero);
  {
    // Branch on negative input.
    j(Assembler::NotEqual, &negative);

    // Fail on negative-zero.
    branchNegativeZero(src, dest, fail, /* maybeNonZero = */ false);

    // Input is +0
    xor32(dest, dest);
    jump(&end);
  }

  // Input is negative.
  bind(&negative);
  {
    // Inputs in [-0.5, 0) are rounded to -0. Fail.
    loadConstantDouble(-0.5, scratch);
    branchDouble(Assembler::DoubleGreaterThanOrEqual, src, scratch, fail);

    // Other negative inputs need the biggest double less than 0.5 added.
    //
    // The result is stored in the temp register (currently contains the biggest
    // double less than 0.5).
    addDouble(src, temp);

    if (HasSSE41()) {
      // Round toward -Infinity.
      vroundsd(X86Encoding::RoundDown, temp, scratch);

      // Truncate.
      truncateDoubleToInt32(scratch, dest, fail);
    } else {
      // Round toward -Infinity without the benefit of ROUNDSD.

      // Truncate and round toward zero.
      // This is off-by-one for everything but integer-valued inputs.
      //
      // Directly call vcvttsd2si instead of truncateDoubleToInt32 because we
      // want to perform failure handling ourselves.
      vcvttsd2si(temp, dest);

      // Test whether the truncated double was integer-valued.
      convertInt32ToDouble(dest, scratch);
      branchDouble(Assembler::DoubleEqualOrUnordered, temp, scratch, &end);

      // Input is not integer-valued, so we rounded off-by-one in the
      // wrong direction. Correct by subtraction.
      //
      // Overflows if vcvttsd2si returned the failure return value INT_MIN.
      branchSub32(Assembler::Overflow, Imm32(1), dest, fail);
    }
  }

  bind(&end);
}

void MacroAssembler::nearbyIntDouble(RoundingMode mode, FloatRegister src,
                                     FloatRegister dest) {
  MOZ_ASSERT(HasRoundInstruction(mode));
  vroundsd(Assembler::ToX86RoundingMode(mode), src, dest);
}

void MacroAssembler::nearbyIntFloat32(RoundingMode mode, FloatRegister src,
                                      FloatRegister dest) {
  MOZ_ASSERT(HasRoundInstruction(mode));
  vroundss(Assembler::ToX86RoundingMode(mode), src, dest);
}

void MacroAssembler::copySignDouble(FloatRegister lhs, FloatRegister rhs,
                                    FloatRegister output) {
  ScratchDoubleScope scratch(*this);

  // TODO Support AVX2
  if (rhs == output) {
    MOZ_ASSERT(lhs != rhs);
    double keepSignMask = mozilla::BitwiseCast<double>(INT64_MIN);
    loadConstantDouble(keepSignMask, scratch);
    vandpd(scratch, rhs, output);

    double clearSignMask = mozilla::BitwiseCast<double>(INT64_MAX);
    loadConstantDouble(clearSignMask, scratch);
    vandpd(lhs, scratch, scratch);
  } else {
    double clearSignMask = mozilla::BitwiseCast<double>(INT64_MAX);
    loadConstantDouble(clearSignMask, scratch);
    vandpd(scratch, lhs, output);

    double keepSignMask = mozilla::BitwiseCast<double>(INT64_MIN);
    loadConstantDouble(keepSignMask, scratch);
    vandpd(rhs, scratch, scratch);
  }

  vorpd(scratch, output, output);
}

void MacroAssembler::copySignFloat32(FloatRegister lhs, FloatRegister rhs,
                                     FloatRegister output) {
  ScratchFloat32Scope scratch(*this);

  // TODO Support AVX2
  if (rhs == output) {
    MOZ_ASSERT(lhs != rhs);
    float keepSignMask = mozilla::BitwiseCast<float>(INT32_MIN);
    loadConstantFloat32(keepSignMask, scratch);
    vandps(scratch, output, output);

    float clearSignMask = mozilla::BitwiseCast<float>(INT32_MAX);
    loadConstantFloat32(clearSignMask, scratch);
    vandps(lhs, scratch, scratch);
  } else {
    float clearSignMask = mozilla::BitwiseCast<float>(INT32_MAX);
    loadConstantFloat32(clearSignMask, scratch);
    vandps(scratch, lhs, output);

    float keepSignMask = mozilla::BitwiseCast<float>(INT32_MIN);
    loadConstantFloat32(keepSignMask, scratch);
    vandps(rhs, scratch, scratch);
  }

  vorps(scratch, output, output);
}

void MacroAssembler::shiftIndex32AndAdd(Register indexTemp32, int shift,
                                        Register pointer) {
  if (IsShiftInScaleRange(shift)) {
    computeEffectiveAddress(
        BaseIndex(pointer, indexTemp32, ShiftToScale(shift)), pointer);
    return;
  }
  lshift32(Imm32(shift), indexTemp32);
  addPtr(indexTemp32, pointer);
}

CodeOffset MacroAssembler::wasmMarkedSlowCall(const wasm::CallSiteDesc& desc,
                                              const Register reg) {
  CodeOffset offset = call(desc, reg);
  wasmMarkCallAsSlow();
  return offset;
}

//}}} check_macroassembler_style
