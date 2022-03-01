/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_shared_CodeGenerator_shared_inl_h
#define jit_shared_CodeGenerator_shared_inl_h

#include "jit/shared/CodeGenerator-shared.h"

#include "jit/JitFrames.h"
#include "jit/ScalarTypeUtils.h"

#include "jit/MacroAssembler-inl.h"

namespace js {
namespace jit {

static inline bool IsConstant(const LInt64Allocation& a) {
#if JS_BITS_PER_WORD == 32
  if (a.high().isConstantValue()) {
    return true;
  }
  if (a.high().isConstantIndex()) {
    return true;
  }
#else
  if (a.value().isConstantValue()) {
    return true;
  }
  if (a.value().isConstantIndex()) {
    return true;
  }
#endif
  return false;
}

static inline int32_t ToInt32(const LAllocation* a) {
  if (a->isConstantValue()) {
    const MConstant* cst = a->toConstant();
    if (cst->type() == MIRType::Int32) {
      return cst->toInt32();
    }
    intptr_t val = cst->toIntPtr();
    MOZ_ASSERT(INT32_MIN <= val && val <= INT32_MAX);
    return int32_t(val);
  }
  if (a->isConstantIndex()) {
    return a->toConstantIndex()->index();
  }
  MOZ_CRASH("this is not a constant!");
}

static inline int64_t ToInt64(const LAllocation* a) {
  if (a->isConstantValue()) {
    return a->toConstant()->toInt64();
  }
  if (a->isConstantIndex()) {
    return a->toConstantIndex()->index();
  }
  MOZ_CRASH("this is not a constant!");
}

static inline int64_t ToInt64(const LInt64Allocation& a) {
#if JS_BITS_PER_WORD == 32
  if (a.high().isConstantValue()) {
    return a.high().toConstant()->toInt64();
  }
  if (a.high().isConstantIndex()) {
    return a.high().toConstantIndex()->index();
  }
#else
  if (a.value().isConstantValue()) {
    return a.value().toConstant()->toInt64();
  }
  if (a.value().isConstantIndex()) {
    return a.value().toConstantIndex()->index();
  }
#endif
  MOZ_CRASH("this is not a constant!");
}

static inline double ToDouble(const LAllocation* a) {
  return a->toConstant()->numberToDouble();
}

static inline bool ToBoolean(const LAllocation* a) {
  return a->toConstant()->toBoolean();
}

static inline Register ToRegister(const LAllocation& a) {
  MOZ_ASSERT(a.isGeneralReg());
  return a.toGeneralReg()->reg();
}

static inline Register ToRegister(const LAllocation* a) {
  return ToRegister(*a);
}

static inline Register ToRegister(const LDefinition* def) {
  return ToRegister(*def->output());
}

static inline Register64 ToOutRegister64(LInstruction* ins) {
#if JS_BITS_PER_WORD == 32
  Register loReg = ToRegister(ins->getDef(INT64LOW_INDEX));
  Register hiReg = ToRegister(ins->getDef(INT64HIGH_INDEX));
  return Register64(hiReg, loReg);
#else
  return Register64(ToRegister(ins->getDef(0)));
#endif
}

static inline Register64 ToRegister64(const LInt64Allocation& a) {
#if JS_BITS_PER_WORD == 32
  return Register64(ToRegister(a.high()), ToRegister(a.low()));
#else
  return Register64(ToRegister(a.value()));
#endif
}

static inline Register64 ToRegister64(const LInt64Definition& a) {
#if JS_BITS_PER_WORD == 32
  return Register64(ToRegister(a.pointerHigh()), ToRegister(a.pointerLow()));
#else
  return Register64(ToRegister(a.pointer()));
#endif
}

static inline Register ToTempRegisterOrInvalid(const LDefinition* def) {
  if (def->isBogusTemp()) {
    return InvalidReg;
  }
  return ToRegister(def);
}

static inline Register64 ToTempRegister64OrInvalid(
    const LInt64Definition& def) {
  if (def.isBogusTemp()) {
    return Register64::Invalid();
  }
  return ToRegister64(def);
}

static inline Register ToTempUnboxRegister(const LDefinition* def) {
  return ToTempRegisterOrInvalid(def);
}

static inline Register ToRegisterOrInvalid(const LDefinition* a) {
  return a ? ToRegister(a) : InvalidReg;
}

static inline FloatRegister ToFloatRegister(const LAllocation& a) {
  MOZ_ASSERT(a.isFloatReg());
  return a.toFloatReg()->reg();
}

static inline FloatRegister ToFloatRegister(const LAllocation* a) {
  return ToFloatRegister(*a);
}

static inline FloatRegister ToFloatRegister(const LDefinition* def) {
  return ToFloatRegister(*def->output());
}

static inline FloatRegister ToTempFloatRegisterOrInvalid(
    const LDefinition* def) {
  if (def->isBogusTemp()) {
    return InvalidFloatReg;
  }
  return ToFloatRegister(def);
}

static inline AnyRegister ToAnyRegister(const LAllocation& a) {
  MOZ_ASSERT(a.isGeneralReg() || a.isFloatReg());
  if (a.isGeneralReg()) {
    return AnyRegister(ToRegister(a));
  }
  return AnyRegister(ToFloatRegister(a));
}

static inline AnyRegister ToAnyRegister(const LAllocation* a) {
  return ToAnyRegister(*a);
}

static inline AnyRegister ToAnyRegister(const LDefinition* def) {
  return ToAnyRegister(def->output());
}

static inline ValueOperand ToOutValue(LInstruction* ins) {
#if defined(JS_NUNBOX32)
  return ValueOperand(ToRegister(ins->getDef(TYPE_INDEX)),
                      ToRegister(ins->getDef(PAYLOAD_INDEX)));
#elif defined(JS_PUNBOX64)
  return ValueOperand(ToRegister(ins->getDef(0)));
#else
#  error "Unknown"
#endif
}

static inline ValueOperand GetTempValue(Register type, Register payload) {
#if defined(JS_NUNBOX32)
  return ValueOperand(type, payload);
#elif defined(JS_PUNBOX64)
  (void)type;
  return ValueOperand(payload);
#else
#  error "Unknown"
#endif
}

int32_t CodeGeneratorShared::ArgToStackOffset(int32_t slot) const {
  return masm.framePushed() +
         (gen->compilingWasm() ? sizeof(wasm::Frame) : sizeof(JitFrameLayout)) +
         slot;
}

int32_t CodeGeneratorShared::SlotToStackOffset(int32_t slot) const {
  MOZ_ASSERT(slot > 0 && slot <= int32_t(graph.localSlotCount()));
  int32_t offset = masm.framePushed() - slot;
  MOZ_ASSERT(offset >= 0);
  return offset;
}

int32_t CodeGeneratorShared::StackOffsetToSlot(int32_t offset) const {
  // See: SlotToStackOffset. This is used to convert pushed arguments
  // to a slot index that safepoints can use.
  //
  // offset = framePushed - frameInitialAdjustment - slot
  // offset + slot = framePushed - frameInitialAdjustment
  // slot = framePushed - frameInitialAdjustement - offset
  return masm.framePushed() - offset;
}

// For argument construction for calls. Argslots are Value-sized.
int32_t CodeGeneratorShared::StackOffsetOfPassedArg(int32_t slot) const {
  // A slot of 0 is permitted only to calculate %esp offset for calls.
  MOZ_ASSERT(slot >= 0 && slot <= int32_t(graph.argumentSlotCount()));
  int32_t offset = masm.framePushed() - graph.paddedLocalSlotsSize() -
                   (slot * sizeof(Value));

  // Passed arguments go below A function's local stack storage.
  // When arguments are being pushed, there is nothing important on the stack.
  // Therefore, It is safe to push the arguments down arbitrarily.  Pushing
  // by sizeof(Value) is desirable since everything on the stack is a Value.
  // Note that paddedLocalSlotCount() aligns to at least a Value boundary
  // specifically to support this.
  MOZ_ASSERT(offset >= 0);
  MOZ_ASSERT(offset % sizeof(Value) == 0);
  return offset;
}

int32_t CodeGeneratorShared::ToStackOffset(LAllocation a) const {
  if (a.isArgument()) {
    return ArgToStackOffset(a.toArgument()->index());
  }
  return SlotToStackOffset(a.isStackSlot() ? a.toStackSlot()->slot()
                                           : a.toStackArea()->base());
}

int32_t CodeGeneratorShared::ToStackOffset(const LAllocation* a) const {
  return ToStackOffset(*a);
}

Address CodeGeneratorShared::ToAddress(const LAllocation& a) const {
  MOZ_ASSERT(a.isMemory() || a.isStackArea());
  if (useWasmStackArgumentAbi() && a.isArgument()) {
    return Address(FramePointer, ToFramePointerOffset(a));
  }
  return Address(masm.getStackPointer(), ToStackOffset(&a));
}

Address CodeGeneratorShared::ToAddress(const LAllocation* a) const {
  return ToAddress(*a);
}

// static
Address CodeGeneratorShared::ToAddress(Register elements,
                                       const LAllocation* index,
                                       Scalar::Type type,
                                       int32_t offsetAdjustment) {
  int32_t idx = ToInt32(index);
  int32_t offset;
  MOZ_ALWAYS_TRUE(ArrayOffsetFitsInInt32(idx, type, offsetAdjustment, &offset));
  return Address(elements, offset);
}

int32_t CodeGeneratorShared::ToFramePointerOffset(LAllocation a) const {
  MOZ_ASSERT(useWasmStackArgumentAbi());
  MOZ_ASSERT(a.isArgument());
  return a.toArgument()->index() + sizeof(wasm::Frame);
}

int32_t CodeGeneratorShared::ToFramePointerOffset(const LAllocation* a) const {
  return ToFramePointerOffset(*a);
}

void CodeGeneratorShared::saveLive(LInstruction* ins) {
  MOZ_ASSERT(!ins->isCall());
  LSafepoint* safepoint = ins->safepoint();
  masm.PushRegsInMask(safepoint->liveRegs());
}

void CodeGeneratorShared::restoreLive(LInstruction* ins) {
  MOZ_ASSERT(!ins->isCall());
  LSafepoint* safepoint = ins->safepoint();
  masm.PopRegsInMask(safepoint->liveRegs());
}

void CodeGeneratorShared::restoreLiveIgnore(LInstruction* ins,
                                            LiveRegisterSet ignore) {
  MOZ_ASSERT(!ins->isCall());
  LSafepoint* safepoint = ins->safepoint();
  masm.PopRegsInMaskIgnore(safepoint->liveRegs(), ignore);
}

LiveRegisterSet CodeGeneratorShared::liveVolatileRegs(LInstruction* ins) {
  MOZ_ASSERT(!ins->isCall());
  LSafepoint* safepoint = ins->safepoint();
  LiveRegisterSet regs;
  regs.set() = RegisterSet::Intersect(safepoint->liveRegs().set(),
                                      RegisterSet::Volatile());
  return regs;
}

void CodeGeneratorShared::saveLiveVolatile(LInstruction* ins) {
  LiveRegisterSet regs = liveVolatileRegs(ins);
  masm.PushRegsInMask(regs);
}

void CodeGeneratorShared::restoreLiveVolatile(LInstruction* ins) {
  LiveRegisterSet regs = liveVolatileRegs(ins);
  masm.PopRegsInMask(regs);
}

inline bool CodeGeneratorShared::isGlobalObject(JSObject* object) {
  // Calling object->is<GlobalObject>() is racy because this relies on
  // checking the group and this can be changed while we are compiling off the
  // main thread. Note that we only check for the script realm's global here.
  return object == gen->realm->maybeGlobal();
}

}  // namespace jit
}  // namespace js

#endif /* jit_shared_CodeGenerator_shared_inl_h */
