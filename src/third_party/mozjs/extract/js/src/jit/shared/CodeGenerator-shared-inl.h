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

// For argument construction for calls. Argslots are Value-sized.
Address CodeGeneratorShared::AddressOfPassedArg(uint32_t slot) const {
  MOZ_ASSERT(masm.framePushed() == frameSize());

  MOZ_ASSERT(slot > 0);
  MOZ_ASSERT(slot <= graph.argumentSlotCount());

  uint32_t offsetFromBase = offsetOfPassedArgSlots_ + slot * sizeof(Value);
  MOZ_ASSERT(offsetFromBase <= frameSize());

  // Space for passed arguments is reserved below a function's local stack
  // storage. Note that passedArgSlotsOffset_ is aligned to at least
  // sizeof(Value) to ensure proper alignment.
  MOZ_ASSERT((offsetFromBase % sizeof(Value)) == 0);

  if (JitOptions.baseRegForLocals == BaseRegForAddress::SP) {
    return Address(masm.getStackPointer(), frameSize() - offsetFromBase);
  }
  MOZ_ASSERT(JitOptions.baseRegForLocals == BaseRegForAddress::FP);
  return Address(FramePointer, -int32_t(offsetFromBase));
}

uint32_t CodeGeneratorShared::UnusedStackBytesForCall(
    uint32_t numArgSlots) const {
  MOZ_ASSERT(masm.framePushed() == frameSize());
  MOZ_ASSERT(numArgSlots <= graph.argumentSlotCount());
  uint32_t unusedArgSlots = graph.argumentSlotCount() - numArgSlots;
  return unusedArgSlots * sizeof(Value);
}

template <BaseRegForAddress Base>
Address CodeGeneratorShared::ToAddress(const LAllocation& a) const {
  MOZ_ASSERT(a.isMemory() || a.isStackArea());
  MOZ_ASSERT(masm.framePushed() == frameSize());

  if (a.isArgument()) {
    // Use the frame pointer, unless the caller explicitly requested a
    // stack-pointer-relative address.
    uint32_t offsetFromFP = offsetOfArgsFromFP_ + a.toArgument()->index();
    if constexpr (Base == BaseRegForAddress::SP) {
      return Address(masm.getStackPointer(), frameSize() + offsetFromFP);
    } else {
      static_assert(Base == BaseRegForAddress::Default ||
                    Base == BaseRegForAddress::FP);
      return Address(FramePointer, offsetFromFP);
    }
  }

  uint32_t slot =
      a.isStackSlot() ? a.toStackSlot()->slot() : a.toStackArea()->base();
  MOZ_ASSERT(slot > 0 && slot <= graph.localSlotsSize());
  MOZ_ASSERT(slot <= frameSize());

  BaseRegForAddress base = Base;
  if constexpr (Base == BaseRegForAddress::Default) {
    base = JitOptions.baseRegForLocals;
  }

  if (base == BaseRegForAddress::FP) {
    return Address(FramePointer, -int32_t(slot));
  }
  MOZ_ASSERT(base == BaseRegForAddress::SP);
  return Address(masm.getStackPointer(), frameSize() - slot);
}

template <BaseRegForAddress Base>
Address CodeGeneratorShared::ToAddress(const LAllocation* a) const {
  return ToAddress<Base>(*a);
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
