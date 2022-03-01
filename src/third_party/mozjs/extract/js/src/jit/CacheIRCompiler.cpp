/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/CacheIRCompiler.h"

#include "mozilla/ArrayUtils.h"
#include "mozilla/FunctionTypeTraits.h"
#include "mozilla/MaybeOneOf.h"
#include "mozilla/ScopeExit.h"

#include <type_traits>
#include <utility>

#include "jslibmath.h"
#include "jsmath.h"

#include "builtin/DataViewObject.h"
#include "builtin/MapObject.h"
#include "builtin/Object.h"
#include "gc/Allocator.h"
#include "jit/BaselineCacheIRCompiler.h"
#include "jit/IonCacheIRCompiler.h"
#include "jit/IonIC.h"
#include "jit/JitFrames.h"
#include "jit/JitRuntime.h"
#include "jit/JitZone.h"
#include "jit/SharedICHelpers.h"
#include "jit/SharedICRegisters.h"
#include "jit/TemplateObject.h"
#include "jit/VMFunctions.h"
#include "js/friend/DOMProxy.h"     // JS::ExpandoAndGeneration
#include "js/friend/XrayJitInfo.h"  // js::jit::GetXrayJitInfo
#include "js/ScalarType.h"          // js::Scalar::Type
#include "proxy/DOMProxy.h"
#include "proxy/Proxy.h"
#include "vm/ArgumentsObject.h"
#include "vm/ArrayBufferObject.h"
#include "vm/ArrayBufferViewObject.h"
#include "vm/BigIntType.h"
#include "vm/FunctionFlags.h"  // js::FunctionFlags
#include "vm/GeneratorObject.h"
#include "vm/GetterSetter.h"
#include "vm/Uint8Clamped.h"

#include "builtin/Boolean-inl.h"
#include "gc/ObjectKind-inl.h"
#include "jit/MacroAssembler-inl.h"
#include "jit/SharedICHelpers-inl.h"
#include "jit/VMFunctionList-inl.h"
#include "vm/BytecodeUtil-inl.h"
#include "vm/Realm-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::BitwiseCast;
using mozilla::Maybe;

using JS::ExpandoAndGeneration;

ValueOperand CacheRegisterAllocator::useValueRegister(MacroAssembler& masm,
                                                      ValOperandId op) {
  OperandLocation& loc = operandLocations_[op.id()];

  switch (loc.kind()) {
    case OperandLocation::ValueReg:
      currentOpRegs_.add(loc.valueReg());
      return loc.valueReg();

    case OperandLocation::ValueStack: {
      ValueOperand reg = allocateValueRegister(masm);
      popValue(masm, &loc, reg);
      return reg;
    }

    case OperandLocation::BaselineFrame: {
      ValueOperand reg = allocateValueRegister(masm);
      Address addr = addressOf(masm, loc.baselineFrameSlot());
      masm.loadValue(addr, reg);
      loc.setValueReg(reg);
      return reg;
    }

    case OperandLocation::Constant: {
      ValueOperand reg = allocateValueRegister(masm);
      masm.moveValue(loc.constant(), reg);
      loc.setValueReg(reg);
      return reg;
    }

    case OperandLocation::PayloadReg: {
      // Temporarily add the payload register to currentOpRegs_ so
      // allocateValueRegister will stay away from it.
      currentOpRegs_.add(loc.payloadReg());
      ValueOperand reg = allocateValueRegister(masm);
      masm.tagValue(loc.payloadType(), loc.payloadReg(), reg);
      currentOpRegs_.take(loc.payloadReg());
      availableRegs_.add(loc.payloadReg());
      loc.setValueReg(reg);
      return reg;
    }

    case OperandLocation::PayloadStack: {
      ValueOperand reg = allocateValueRegister(masm);
      popPayload(masm, &loc, reg.scratchReg());
      masm.tagValue(loc.payloadType(), reg.scratchReg(), reg);
      loc.setValueReg(reg);
      return reg;
    }

    case OperandLocation::DoubleReg: {
      ValueOperand reg = allocateValueRegister(masm);
      {
        ScratchDoubleScope fpscratch(masm);
        masm.boxDouble(loc.doubleReg(), reg, fpscratch);
      }
      loc.setValueReg(reg);
      return reg;
    }

    case OperandLocation::Uninitialized:
      break;
  }

  MOZ_CRASH();
}

// Load a value operand directly into a float register. Caller must have
// guarded isNumber on the provided val.
void CacheRegisterAllocator::ensureDoubleRegister(MacroAssembler& masm,
                                                  NumberOperandId op,
                                                  FloatRegister dest) const {
  // If AutoScratchFloatRegister is active, we have to add sizeof(double) to
  // any stack slot offsets below.
  int32_t stackOffset = hasAutoScratchFloatRegisterSpill() ? sizeof(double) : 0;

  const OperandLocation& loc = operandLocations_[op.id()];

  Label failure, done;
  switch (loc.kind()) {
    case OperandLocation::ValueReg: {
      masm.ensureDouble(loc.valueReg(), dest, &failure);
      break;
    }

    case OperandLocation::ValueStack: {
      Address addr = valueAddress(masm, &loc);
      addr.offset += stackOffset;
      masm.ensureDouble(addr, dest, &failure);
      break;
    }

    case OperandLocation::BaselineFrame: {
      Address addr = addressOf(masm, loc.baselineFrameSlot());
      addr.offset += stackOffset;
      masm.ensureDouble(addr, dest, &failure);
      break;
    }

    case OperandLocation::DoubleReg: {
      masm.moveDouble(loc.doubleReg(), dest);
      return;
    }

    case OperandLocation::Constant: {
      MOZ_ASSERT(loc.constant().isNumber(),
                 "Caller must ensure the operand is a number value");
      masm.loadConstantDouble(loc.constant().toNumber(), dest);
      return;
    }

    case OperandLocation::PayloadReg: {
      // Doubles can't be stored in payload registers, so this must be an int32.
      MOZ_ASSERT(loc.payloadType() == JSVAL_TYPE_INT32,
                 "Caller must ensure the operand is a number value");
      masm.convertInt32ToDouble(loc.payloadReg(), dest);
      return;
    }

    case OperandLocation::PayloadStack: {
      // Doubles can't be stored in payload registers, so this must be an int32.
      MOZ_ASSERT(loc.payloadType() == JSVAL_TYPE_INT32,
                 "Caller must ensure the operand is a number value");
      MOZ_ASSERT(loc.payloadStack() <= stackPushed_);
      Address addr = payloadAddress(masm, &loc);
      addr.offset += stackOffset;
      masm.convertInt32ToDouble(addr, dest);
      return;
    }

    case OperandLocation::Uninitialized:
      MOZ_CRASH("Unhandled operand type in ensureDoubleRegister");
      return;
  }
  masm.jump(&done);
  masm.bind(&failure);
  masm.assumeUnreachable(
      "Missing guard allowed non-number to hit ensureDoubleRegister");
  masm.bind(&done);
}

void CacheRegisterAllocator::copyToScratchRegister(MacroAssembler& masm,
                                                   TypedOperandId typedId,
                                                   Register dest) const {
  // If AutoScratchFloatRegister is active, we have to add sizeof(double) to
  // any stack slot offsets below.
  int32_t stackOffset = hasAutoScratchFloatRegisterSpill() ? sizeof(double) : 0;

  const OperandLocation& loc = operandLocations_[typedId.id()];

  Label failure, done;
  switch (loc.kind()) {
    case OperandLocation::ValueReg: {
      masm.unboxNonDouble(loc.valueReg(), dest, typedId.type());
      break;
    }
    case OperandLocation::ValueStack: {
      Address addr = valueAddress(masm, &loc);
      addr.offset += stackOffset;
      masm.unboxNonDouble(addr, dest, typedId.type());
      break;
    }
    case OperandLocation::BaselineFrame: {
      Address addr = addressOf(masm, loc.baselineFrameSlot());
      addr.offset += stackOffset;
      masm.unboxNonDouble(addr, dest, typedId.type());
      break;
    }
    case OperandLocation::PayloadReg: {
      MOZ_ASSERT(loc.payloadType() == typedId.type());
      masm.mov(loc.payloadReg(), dest);
      return;
    }
    case OperandLocation::PayloadStack: {
      MOZ_ASSERT(loc.payloadType() == typedId.type());
      MOZ_ASSERT(loc.payloadStack() <= stackPushed_);
      Address addr = payloadAddress(masm, &loc);
      addr.offset += stackOffset;
      masm.loadPtr(addr, dest);
      return;
    }
    case OperandLocation::DoubleReg:
    case OperandLocation::Constant:
    case OperandLocation::Uninitialized:
      MOZ_CRASH("Unhandled operand location");
  }
}

void CacheRegisterAllocator::copyToScratchValueRegister(
    MacroAssembler& masm, ValOperandId valId, ValueOperand dest) const {
  MOZ_ASSERT(!addedFailurePath_);
  MOZ_ASSERT(!hasAutoScratchFloatRegisterSpill());

  const OperandLocation& loc = operandLocations_[valId.id()];
  switch (loc.kind()) {
    case OperandLocation::ValueReg:
      masm.moveValue(loc.valueReg(), dest);
      break;
    case OperandLocation::ValueStack: {
      Address addr = valueAddress(masm, &loc);
      masm.loadValue(addr, dest);
      break;
    }
    case OperandLocation::BaselineFrame: {
      Address addr = addressOf(masm, loc.baselineFrameSlot());
      masm.loadValue(addr, dest);
      break;
    }
    case OperandLocation::Constant:
      masm.moveValue(loc.constant(), dest);
      break;
    case OperandLocation::PayloadReg:
      masm.tagValue(loc.payloadType(), loc.payloadReg(), dest);
      break;
    case OperandLocation::PayloadStack: {
      Address addr = payloadAddress(masm, &loc);
      masm.loadPtr(addr, dest.scratchReg());
      masm.tagValue(loc.payloadType(), dest.scratchReg(), dest);
      break;
    }
    case OperandLocation::DoubleReg: {
      ScratchDoubleScope fpscratch(masm);
      masm.boxDouble(loc.doubleReg(), dest, fpscratch);
      break;
    }
    case OperandLocation::Uninitialized:
      MOZ_CRASH();
  }
}

Register CacheRegisterAllocator::useRegister(MacroAssembler& masm,
                                             TypedOperandId typedId) {
  MOZ_ASSERT(!addedFailurePath_);
  MOZ_ASSERT(!hasAutoScratchFloatRegisterSpill());

  OperandLocation& loc = operandLocations_[typedId.id()];
  switch (loc.kind()) {
    case OperandLocation::PayloadReg:
      currentOpRegs_.add(loc.payloadReg());
      return loc.payloadReg();

    case OperandLocation::ValueReg: {
      // It's possible the value is still boxed: as an optimization, we unbox
      // the first time we use a value as object.
      ValueOperand val = loc.valueReg();
      availableRegs_.add(val);
      Register reg = val.scratchReg();
      availableRegs_.take(reg);
      masm.unboxNonDouble(val, reg, typedId.type());
      loc.setPayloadReg(reg, typedId.type());
      currentOpRegs_.add(reg);
      return reg;
    }

    case OperandLocation::PayloadStack: {
      Register reg = allocateRegister(masm);
      popPayload(masm, &loc, reg);
      return reg;
    }

    case OperandLocation::ValueStack: {
      // The value is on the stack, but boxed. If it's on top of the stack we
      // unbox it and then remove it from the stack, else we just unbox.
      Register reg = allocateRegister(masm);
      if (loc.valueStack() == stackPushed_) {
        masm.unboxNonDouble(Address(masm.getStackPointer(), 0), reg,
                            typedId.type());
        masm.addToStackPtr(Imm32(sizeof(js::Value)));
        MOZ_ASSERT(stackPushed_ >= sizeof(js::Value));
        stackPushed_ -= sizeof(js::Value);
      } else {
        MOZ_ASSERT(loc.valueStack() < stackPushed_);
        masm.unboxNonDouble(
            Address(masm.getStackPointer(), stackPushed_ - loc.valueStack()),
            reg, typedId.type());
      }
      loc.setPayloadReg(reg, typedId.type());
      return reg;
    }

    case OperandLocation::BaselineFrame: {
      Register reg = allocateRegister(masm);
      Address addr = addressOf(masm, loc.baselineFrameSlot());
      masm.unboxNonDouble(addr, reg, typedId.type());
      loc.setPayloadReg(reg, typedId.type());
      return reg;
    };

    case OperandLocation::Constant: {
      Value v = loc.constant();
      Register reg = allocateRegister(masm);
      if (v.isString()) {
        masm.movePtr(ImmGCPtr(v.toString()), reg);
      } else if (v.isSymbol()) {
        masm.movePtr(ImmGCPtr(v.toSymbol()), reg);
      } else if (v.isBigInt()) {
        masm.movePtr(ImmGCPtr(v.toBigInt()), reg);
      } else {
        MOZ_CRASH("Unexpected Value");
      }
      loc.setPayloadReg(reg, v.extractNonDoubleType());
      return reg;
    }

    case OperandLocation::DoubleReg:
    case OperandLocation::Uninitialized:
      break;
  }

  MOZ_CRASH();
}

ConstantOrRegister CacheRegisterAllocator::useConstantOrRegister(
    MacroAssembler& masm, ValOperandId val) {
  MOZ_ASSERT(!addedFailurePath_);
  MOZ_ASSERT(!hasAutoScratchFloatRegisterSpill());

  OperandLocation& loc = operandLocations_[val.id()];
  switch (loc.kind()) {
    case OperandLocation::Constant:
      return loc.constant();

    case OperandLocation::PayloadReg:
    case OperandLocation::PayloadStack: {
      JSValueType payloadType = loc.payloadType();
      Register reg = useRegister(masm, TypedOperandId(val, payloadType));
      return TypedOrValueRegister(MIRTypeFromValueType(payloadType),
                                  AnyRegister(reg));
    }

    case OperandLocation::ValueReg:
    case OperandLocation::ValueStack:
    case OperandLocation::BaselineFrame:
      return TypedOrValueRegister(useValueRegister(masm, val));

    case OperandLocation::DoubleReg:
      return TypedOrValueRegister(MIRType::Double,
                                  AnyRegister(loc.doubleReg()));

    case OperandLocation::Uninitialized:
      break;
  }

  MOZ_CRASH();
}

Register CacheRegisterAllocator::defineRegister(MacroAssembler& masm,
                                                TypedOperandId typedId) {
  MOZ_ASSERT(!addedFailurePath_);
  MOZ_ASSERT(!hasAutoScratchFloatRegisterSpill());

  OperandLocation& loc = operandLocations_[typedId.id()];
  MOZ_ASSERT(loc.kind() == OperandLocation::Uninitialized);

  Register reg = allocateRegister(masm);
  loc.setPayloadReg(reg, typedId.type());
  return reg;
}

ValueOperand CacheRegisterAllocator::defineValueRegister(MacroAssembler& masm,
                                                         ValOperandId val) {
  MOZ_ASSERT(!addedFailurePath_);
  MOZ_ASSERT(!hasAutoScratchFloatRegisterSpill());

  OperandLocation& loc = operandLocations_[val.id()];
  MOZ_ASSERT(loc.kind() == OperandLocation::Uninitialized);

  ValueOperand reg = allocateValueRegister(masm);
  loc.setValueReg(reg);
  return reg;
}

void CacheRegisterAllocator::freeDeadOperandLocations(MacroAssembler& masm) {
  // See if any operands are dead so we can reuse their registers. Note that
  // we skip the input operands, as those are also used by failure paths, and
  // we currently don't track those uses.
  for (size_t i = writer_.numInputOperands(); i < operandLocations_.length();
       i++) {
    if (!writer_.operandIsDead(i, currentInstruction_)) {
      continue;
    }

    OperandLocation& loc = operandLocations_[i];
    switch (loc.kind()) {
      case OperandLocation::PayloadReg:
        availableRegs_.add(loc.payloadReg());
        break;
      case OperandLocation::ValueReg:
        availableRegs_.add(loc.valueReg());
        break;
      case OperandLocation::PayloadStack:
        masm.propagateOOM(freePayloadSlots_.append(loc.payloadStack()));
        break;
      case OperandLocation::ValueStack:
        masm.propagateOOM(freeValueSlots_.append(loc.valueStack()));
        break;
      case OperandLocation::Uninitialized:
      case OperandLocation::BaselineFrame:
      case OperandLocation::Constant:
      case OperandLocation::DoubleReg:
        break;
    }
    loc.setUninitialized();
  }
}

void CacheRegisterAllocator::discardStack(MacroAssembler& masm) {
  // This should only be called when we are no longer using the operands,
  // as we're discarding everything from the native stack. Set all operand
  // locations to Uninitialized to catch bugs.
  for (size_t i = 0; i < operandLocations_.length(); i++) {
    operandLocations_[i].setUninitialized();
  }

  if (stackPushed_ > 0) {
    masm.addToStackPtr(Imm32(stackPushed_));
    stackPushed_ = 0;
  }
  freePayloadSlots_.clear();
  freeValueSlots_.clear();
}

Register CacheRegisterAllocator::allocateRegister(MacroAssembler& masm) {
  MOZ_ASSERT(!addedFailurePath_);
  MOZ_ASSERT(!hasAutoScratchFloatRegisterSpill());

  if (availableRegs_.empty()) {
    freeDeadOperandLocations(masm);
  }

  if (availableRegs_.empty()) {
    // Still no registers available, try to spill unused operands to
    // the stack.
    for (size_t i = 0; i < operandLocations_.length(); i++) {
      OperandLocation& loc = operandLocations_[i];
      if (loc.kind() == OperandLocation::PayloadReg) {
        Register reg = loc.payloadReg();
        if (currentOpRegs_.has(reg)) {
          continue;
        }

        spillOperandToStack(masm, &loc);
        availableRegs_.add(reg);
        break;  // We got a register, so break out of the loop.
      }
      if (loc.kind() == OperandLocation::ValueReg) {
        ValueOperand reg = loc.valueReg();
        if (currentOpRegs_.aliases(reg)) {
          continue;
        }

        spillOperandToStack(masm, &loc);
        availableRegs_.add(reg);
        break;  // Break out of the loop.
      }
    }
  }

  if (availableRegs_.empty() && !availableRegsAfterSpill_.empty()) {
    Register reg = availableRegsAfterSpill_.takeAny();
    masm.push(reg);
    stackPushed_ += sizeof(uintptr_t);

    masm.propagateOOM(spilledRegs_.append(SpilledRegister(reg, stackPushed_)));

    availableRegs_.add(reg);
  }

  // At this point, there must be a free register.
  MOZ_RELEASE_ASSERT(!availableRegs_.empty());

  Register reg = availableRegs_.takeAny();
  currentOpRegs_.add(reg);
  return reg;
}

void CacheRegisterAllocator::allocateFixedRegister(MacroAssembler& masm,
                                                   Register reg) {
  MOZ_ASSERT(!addedFailurePath_);
  MOZ_ASSERT(!hasAutoScratchFloatRegisterSpill());

  // Fixed registers should be allocated first, to ensure they're
  // still available.
  MOZ_ASSERT(!currentOpRegs_.has(reg), "Register is in use");

  freeDeadOperandLocations(masm);

  if (availableRegs_.has(reg)) {
    availableRegs_.take(reg);
    currentOpRegs_.add(reg);
    return;
  }

  // Register may be available only after spilling contents.
  if (availableRegsAfterSpill_.has(reg)) {
    availableRegsAfterSpill_.take(reg);
    masm.push(reg);
    stackPushed_ += sizeof(uintptr_t);

    masm.propagateOOM(spilledRegs_.append(SpilledRegister(reg, stackPushed_)));
    currentOpRegs_.add(reg);
    return;
  }

  // The register must be used by some operand. Spill it to the stack.
  for (size_t i = 0; i < operandLocations_.length(); i++) {
    OperandLocation& loc = operandLocations_[i];
    if (loc.kind() == OperandLocation::PayloadReg) {
      if (loc.payloadReg() != reg) {
        continue;
      }

      spillOperandToStackOrRegister(masm, &loc);
      currentOpRegs_.add(reg);
      return;
    }
    if (loc.kind() == OperandLocation::ValueReg) {
      if (!loc.valueReg().aliases(reg)) {
        continue;
      }

      ValueOperand valueReg = loc.valueReg();
      spillOperandToStackOrRegister(masm, &loc);

      availableRegs_.add(valueReg);
      availableRegs_.take(reg);
      currentOpRegs_.add(reg);
      return;
    }
  }

  MOZ_CRASH("Invalid register");
}

void CacheRegisterAllocator::allocateFixedValueRegister(MacroAssembler& masm,
                                                        ValueOperand reg) {
#ifdef JS_NUNBOX32
  allocateFixedRegister(masm, reg.payloadReg());
  allocateFixedRegister(masm, reg.typeReg());
#else
  allocateFixedRegister(masm, reg.valueReg());
#endif
}

#ifdef JS_NUNBOX32
// Possible miscompilation in clang-12 (bug 1689641)
MOZ_NEVER_INLINE
#endif
ValueOperand CacheRegisterAllocator::allocateValueRegister(
    MacroAssembler& masm) {
#ifdef JS_NUNBOX32
  Register reg1 = allocateRegister(masm);
  Register reg2 = allocateRegister(masm);
  return ValueOperand(reg1, reg2);
#else
  Register reg = allocateRegister(masm);
  return ValueOperand(reg);
#endif
}

bool CacheRegisterAllocator::init() {
  if (!origInputLocations_.resize(writer_.numInputOperands())) {
    return false;
  }
  if (!operandLocations_.resize(writer_.numOperandIds())) {
    return false;
  }
  return true;
}

void CacheRegisterAllocator::initAvailableRegsAfterSpill() {
  // Registers not in availableRegs_ and not used by input operands are
  // available after being spilled.
  availableRegsAfterSpill_.set() = GeneralRegisterSet::Intersect(
      GeneralRegisterSet::Not(availableRegs_.set()),
      GeneralRegisterSet::Not(inputRegisterSet()));
}

void CacheRegisterAllocator::fixupAliasedInputs(MacroAssembler& masm) {
  // If IC inputs alias each other, make sure they are stored in different
  // locations so we don't have to deal with this complexity in the rest of
  // the allocator.
  //
  // Note that this can happen in IonMonkey with something like |o.foo = o|
  // or |o[i] = i|.

  size_t numInputs = writer_.numInputOperands();
  MOZ_ASSERT(origInputLocations_.length() == numInputs);

  for (size_t i = 1; i < numInputs; i++) {
    OperandLocation& loc1 = operandLocations_[i];
    if (!loc1.isInRegister()) {
      continue;
    }

    for (size_t j = 0; j < i; j++) {
      OperandLocation& loc2 = operandLocations_[j];
      if (!loc1.aliasesReg(loc2)) {
        continue;
      }

      // loc1 and loc2 alias so we spill one of them. If one is a
      // ValueReg and the other is a PayloadReg, we have to spill the
      // PayloadReg: spilling the ValueReg instead would leave its type
      // register unallocated on 32-bit platforms.
      if (loc1.kind() == OperandLocation::ValueReg) {
        spillOperandToStack(masm, &loc2);
      } else {
        MOZ_ASSERT(loc1.kind() == OperandLocation::PayloadReg);
        spillOperandToStack(masm, &loc1);
        break;  // Spilled loc1, so nothing else will alias it.
      }
    }
  }

#ifdef DEBUG
  assertValidState();
#endif
}

GeneralRegisterSet CacheRegisterAllocator::inputRegisterSet() const {
  MOZ_ASSERT(origInputLocations_.length() == writer_.numInputOperands());

  AllocatableGeneralRegisterSet result;
  for (size_t i = 0; i < writer_.numInputOperands(); i++) {
    const OperandLocation& loc = operandLocations_[i];
    MOZ_ASSERT(loc == origInputLocations_[i]);

    switch (loc.kind()) {
      case OperandLocation::PayloadReg:
        result.addUnchecked(loc.payloadReg());
        continue;
      case OperandLocation::ValueReg:
        result.addUnchecked(loc.valueReg());
        continue;
      case OperandLocation::PayloadStack:
      case OperandLocation::ValueStack:
      case OperandLocation::BaselineFrame:
      case OperandLocation::Constant:
      case OperandLocation::DoubleReg:
        continue;
      case OperandLocation::Uninitialized:
        break;
    }
    MOZ_CRASH("Invalid kind");
  }

  return result.set();
}

JSValueType CacheRegisterAllocator::knownType(ValOperandId val) const {
  const OperandLocation& loc = operandLocations_[val.id()];

  switch (loc.kind()) {
    case OperandLocation::ValueReg:
    case OperandLocation::ValueStack:
    case OperandLocation::BaselineFrame:
      return JSVAL_TYPE_UNKNOWN;

    case OperandLocation::PayloadStack:
    case OperandLocation::PayloadReg:
      return loc.payloadType();

    case OperandLocation::Constant:
      return loc.constant().isDouble() ? JSVAL_TYPE_DOUBLE
                                       : loc.constant().extractNonDoubleType();

    case OperandLocation::DoubleReg:
      return JSVAL_TYPE_DOUBLE;

    case OperandLocation::Uninitialized:
      break;
  }

  MOZ_CRASH("Invalid kind");
}

void CacheRegisterAllocator::initInputLocation(
    size_t i, const TypedOrValueRegister& reg) {
  if (reg.hasValue()) {
    initInputLocation(i, reg.valueReg());
  } else if (reg.typedReg().isFloat()) {
    MOZ_ASSERT(reg.type() == MIRType::Double);
    initInputLocation(i, reg.typedReg().fpu());
  } else {
    initInputLocation(i, reg.typedReg().gpr(),
                      ValueTypeFromMIRType(reg.type()));
  }
}

void CacheRegisterAllocator::initInputLocation(
    size_t i, const ConstantOrRegister& value) {
  if (value.constant()) {
    initInputLocation(i, value.value());
  } else {
    initInputLocation(i, value.reg());
  }
}

void CacheRegisterAllocator::spillOperandToStack(MacroAssembler& masm,
                                                 OperandLocation* loc) {
  MOZ_ASSERT(loc >= operandLocations_.begin() && loc < operandLocations_.end());

  if (loc->kind() == OperandLocation::ValueReg) {
    if (!freeValueSlots_.empty()) {
      uint32_t stackPos = freeValueSlots_.popCopy();
      MOZ_ASSERT(stackPos <= stackPushed_);
      masm.storeValue(loc->valueReg(),
                      Address(masm.getStackPointer(), stackPushed_ - stackPos));
      loc->setValueStack(stackPos);
      return;
    }
    stackPushed_ += sizeof(js::Value);
    masm.pushValue(loc->valueReg());
    loc->setValueStack(stackPushed_);
    return;
  }

  MOZ_ASSERT(loc->kind() == OperandLocation::PayloadReg);

  if (!freePayloadSlots_.empty()) {
    uint32_t stackPos = freePayloadSlots_.popCopy();
    MOZ_ASSERT(stackPos <= stackPushed_);
    masm.storePtr(loc->payloadReg(),
                  Address(masm.getStackPointer(), stackPushed_ - stackPos));
    loc->setPayloadStack(stackPos, loc->payloadType());
    return;
  }
  stackPushed_ += sizeof(uintptr_t);
  masm.push(loc->payloadReg());
  loc->setPayloadStack(stackPushed_, loc->payloadType());
}

void CacheRegisterAllocator::spillOperandToStackOrRegister(
    MacroAssembler& masm, OperandLocation* loc) {
  MOZ_ASSERT(loc >= operandLocations_.begin() && loc < operandLocations_.end());

  // If enough registers are available, use them.
  if (loc->kind() == OperandLocation::ValueReg) {
    static const size_t BoxPieces = sizeof(Value) / sizeof(uintptr_t);
    if (availableRegs_.set().size() >= BoxPieces) {
      ValueOperand reg = availableRegs_.takeAnyValue();
      masm.moveValue(loc->valueReg(), reg);
      loc->setValueReg(reg);
      return;
    }
  } else {
    MOZ_ASSERT(loc->kind() == OperandLocation::PayloadReg);
    if (!availableRegs_.empty()) {
      Register reg = availableRegs_.takeAny();
      masm.movePtr(loc->payloadReg(), reg);
      loc->setPayloadReg(reg, loc->payloadType());
      return;
    }
  }

  // Not enough registers available, spill to the stack.
  spillOperandToStack(masm, loc);
}

void CacheRegisterAllocator::popPayload(MacroAssembler& masm,
                                        OperandLocation* loc, Register dest) {
  MOZ_ASSERT(loc >= operandLocations_.begin() && loc < operandLocations_.end());
  MOZ_ASSERT(stackPushed_ >= sizeof(uintptr_t));

  // The payload is on the stack. If it's on top of the stack we can just
  // pop it, else we emit a load.
  if (loc->payloadStack() == stackPushed_) {
    masm.pop(dest);
    stackPushed_ -= sizeof(uintptr_t);
  } else {
    MOZ_ASSERT(loc->payloadStack() < stackPushed_);
    masm.loadPtr(payloadAddress(masm, loc), dest);
    masm.propagateOOM(freePayloadSlots_.append(loc->payloadStack()));
  }

  loc->setPayloadReg(dest, loc->payloadType());
}

Address CacheRegisterAllocator::valueAddress(MacroAssembler& masm,
                                             const OperandLocation* loc) const {
  MOZ_ASSERT(loc >= operandLocations_.begin() && loc < operandLocations_.end());
  return Address(masm.getStackPointer(), stackPushed_ - loc->valueStack());
}

Address CacheRegisterAllocator::payloadAddress(
    MacroAssembler& masm, const OperandLocation* loc) const {
  MOZ_ASSERT(loc >= operandLocations_.begin() && loc < operandLocations_.end());
  return Address(masm.getStackPointer(), stackPushed_ - loc->payloadStack());
}

void CacheRegisterAllocator::popValue(MacroAssembler& masm,
                                      OperandLocation* loc, ValueOperand dest) {
  MOZ_ASSERT(loc >= operandLocations_.begin() && loc < operandLocations_.end());
  MOZ_ASSERT(stackPushed_ >= sizeof(js::Value));

  // The Value is on the stack. If it's on top of the stack we can just
  // pop it, else we emit a load.
  if (loc->valueStack() == stackPushed_) {
    masm.popValue(dest);
    stackPushed_ -= sizeof(js::Value);
  } else {
    MOZ_ASSERT(loc->valueStack() < stackPushed_);
    masm.loadValue(
        Address(masm.getStackPointer(), stackPushed_ - loc->valueStack()),
        dest);
    masm.propagateOOM(freeValueSlots_.append(loc->valueStack()));
  }

  loc->setValueReg(dest);
}

#ifdef DEBUG
void CacheRegisterAllocator::assertValidState() const {
  // Assert different operands don't have aliasing storage. We depend on this
  // when spilling registers, for instance.

  if (!JitOptions.fullDebugChecks) {
    return;
  }

  for (size_t i = 0; i < operandLocations_.length(); i++) {
    const auto& loc1 = operandLocations_[i];
    if (loc1.isUninitialized()) {
      continue;
    }

    for (size_t j = 0; j < i; j++) {
      const auto& loc2 = operandLocations_[j];
      if (loc2.isUninitialized()) {
        continue;
      }
      MOZ_ASSERT(!loc1.aliasesReg(loc2));
    }
  }
}
#endif

bool OperandLocation::aliasesReg(const OperandLocation& other) const {
  MOZ_ASSERT(&other != this);

  switch (other.kind_) {
    case PayloadReg:
      return aliasesReg(other.payloadReg());
    case ValueReg:
      return aliasesReg(other.valueReg());
    case PayloadStack:
    case ValueStack:
    case BaselineFrame:
    case Constant:
    case DoubleReg:
      return false;
    case Uninitialized:
      break;
  }

  MOZ_CRASH("Invalid kind");
}

void CacheRegisterAllocator::restoreInputState(MacroAssembler& masm,
                                               bool shouldDiscardStack) {
  size_t numInputOperands = origInputLocations_.length();
  MOZ_ASSERT(writer_.numInputOperands() == numInputOperands);

  for (size_t j = 0; j < numInputOperands; j++) {
    const OperandLocation& dest = origInputLocations_[j];
    OperandLocation& cur = operandLocations_[j];
    if (dest == cur) {
      continue;
    }

    auto autoAssign = mozilla::MakeScopeExit([&] { cur = dest; });

    // We have a cycle if a destination register will be used later
    // as source register. If that happens, just push the current value
    // on the stack and later get it from there.
    for (size_t k = j + 1; k < numInputOperands; k++) {
      OperandLocation& laterSource = operandLocations_[k];
      if (dest.aliasesReg(laterSource)) {
        spillOperandToStack(masm, &laterSource);
      }
    }

    if (dest.kind() == OperandLocation::ValueReg) {
      // We have to restore a Value register.
      switch (cur.kind()) {
        case OperandLocation::ValueReg:
          masm.moveValue(cur.valueReg(), dest.valueReg());
          continue;
        case OperandLocation::PayloadReg:
          masm.tagValue(cur.payloadType(), cur.payloadReg(), dest.valueReg());
          continue;
        case OperandLocation::PayloadStack: {
          Register scratch = dest.valueReg().scratchReg();
          popPayload(masm, &cur, scratch);
          masm.tagValue(cur.payloadType(), scratch, dest.valueReg());
          continue;
        }
        case OperandLocation::ValueStack:
          popValue(masm, &cur, dest.valueReg());
          continue;
        case OperandLocation::DoubleReg:
          masm.boxDouble(cur.doubleReg(), dest.valueReg(), cur.doubleReg());
          continue;
        case OperandLocation::Constant:
        case OperandLocation::BaselineFrame:
        case OperandLocation::Uninitialized:
          break;
      }
    } else if (dest.kind() == OperandLocation::PayloadReg) {
      // We have to restore a payload register.
      switch (cur.kind()) {
        case OperandLocation::ValueReg:
          MOZ_ASSERT(dest.payloadType() != JSVAL_TYPE_DOUBLE);
          masm.unboxNonDouble(cur.valueReg(), dest.payloadReg(),
                              dest.payloadType());
          continue;
        case OperandLocation::PayloadReg:
          MOZ_ASSERT(cur.payloadType() == dest.payloadType());
          masm.mov(cur.payloadReg(), dest.payloadReg());
          continue;
        case OperandLocation::PayloadStack: {
          MOZ_ASSERT(cur.payloadType() == dest.payloadType());
          popPayload(masm, &cur, dest.payloadReg());
          continue;
        }
        case OperandLocation::ValueStack:
          MOZ_ASSERT(stackPushed_ >= sizeof(js::Value));
          MOZ_ASSERT(cur.valueStack() <= stackPushed_);
          MOZ_ASSERT(dest.payloadType() != JSVAL_TYPE_DOUBLE);
          masm.unboxNonDouble(
              Address(masm.getStackPointer(), stackPushed_ - cur.valueStack()),
              dest.payloadReg(), dest.payloadType());
          continue;
        case OperandLocation::Constant:
        case OperandLocation::BaselineFrame:
        case OperandLocation::DoubleReg:
        case OperandLocation::Uninitialized:
          break;
      }
    } else if (dest.kind() == OperandLocation::Constant ||
               dest.kind() == OperandLocation::BaselineFrame ||
               dest.kind() == OperandLocation::DoubleReg) {
      // Nothing to do.
      continue;
    }

    MOZ_CRASH("Invalid kind");
  }

  for (const SpilledRegister& spill : spilledRegs_) {
    MOZ_ASSERT(stackPushed_ >= sizeof(uintptr_t));

    if (spill.stackPushed == stackPushed_) {
      masm.pop(spill.reg);
      stackPushed_ -= sizeof(uintptr_t);
    } else {
      MOZ_ASSERT(spill.stackPushed < stackPushed_);
      masm.loadPtr(
          Address(masm.getStackPointer(), stackPushed_ - spill.stackPushed),
          spill.reg);
    }
  }

  if (shouldDiscardStack) {
    discardStack(masm);
  }
}

size_t CacheIRStubInfo::stubDataSize() const {
  size_t field = 0;
  size_t size = 0;
  while (true) {
    StubField::Type type = fieldType(field++);
    if (type == StubField::Type::Limit) {
      return size;
    }
    size += StubField::sizeInBytes(type);
  }
}

template <typename T>
static GCPtr<T>* AsGCPtr(uintptr_t* ptr) {
  return reinterpret_cast<GCPtr<T>*>(ptr);
}

uintptr_t CacheIRStubInfo::getStubRawWord(const uint8_t* stubData,
                                          uint32_t offset) const {
  MOZ_ASSERT(uintptr_t(stubData + offset) % sizeof(uintptr_t) == 0);
  return *reinterpret_cast<const uintptr_t*>(stubData + offset);
}

uintptr_t CacheIRStubInfo::getStubRawWord(ICCacheIRStub* stub,
                                          uint32_t offset) const {
  uint8_t* stubData = (uint8_t*)stub + stubDataOffset_;
  return getStubRawWord(stubData, offset);
}

int64_t CacheIRStubInfo::getStubRawInt64(const uint8_t* stubData,
                                         uint32_t offset) const {
  MOZ_ASSERT(uintptr_t(stubData + offset) % sizeof(int64_t) == 0);
  return *reinterpret_cast<const int64_t*>(stubData + offset);
}

int64_t CacheIRStubInfo::getStubRawInt64(ICCacheIRStub* stub,
                                         uint32_t offset) const {
  uint8_t* stubData = (uint8_t*)stub + stubDataOffset_;
  return getStubRawInt64(stubData, offset);
}

void CacheIRStubInfo::replaceStubRawWord(uint8_t* stubData, uint32_t offset,
                                         uintptr_t oldWord,
                                         uintptr_t newWord) const {
  MOZ_ASSERT(uintptr_t(stubData + offset) % sizeof(uintptr_t) == 0);
  uintptr_t* addr = reinterpret_cast<uintptr_t*>(stubData + offset);
  MOZ_ASSERT(*addr == oldWord);
  *addr = newWord;
}

template <class Stub, class T>
GCPtr<T>& CacheIRStubInfo::getStubField(Stub* stub, uint32_t offset) const {
  uint8_t* stubData = (uint8_t*)stub + stubDataOffset_;
  MOZ_ASSERT(uintptr_t(stubData + offset) % sizeof(uintptr_t) == 0);

  return *AsGCPtr<T>((uintptr_t*)(stubData + offset));
}

template GCPtr<Shape*>& CacheIRStubInfo::getStubField<ICCacheIRStub>(
    ICCacheIRStub* stub, uint32_t offset) const;
template GCPtr<GetterSetter*>& CacheIRStubInfo::getStubField<ICCacheIRStub>(
    ICCacheIRStub* stub, uint32_t offset) const;
template GCPtr<JSObject*>& CacheIRStubInfo::getStubField<ICCacheIRStub>(
    ICCacheIRStub* stub, uint32_t offset) const;
template GCPtr<JSString*>& CacheIRStubInfo::getStubField<ICCacheIRStub>(
    ICCacheIRStub* stub, uint32_t offset) const;
template GCPtr<JSFunction*>& CacheIRStubInfo::getStubField<ICCacheIRStub>(
    ICCacheIRStub* stub, uint32_t offset) const;
template GCPtr<JS::Symbol*>& CacheIRStubInfo::getStubField<ICCacheIRStub>(
    ICCacheIRStub* stub, uint32_t offset) const;
template GCPtr<JS::Value>& CacheIRStubInfo::getStubField<ICCacheIRStub>(
    ICCacheIRStub* stub, uint32_t offset) const;
template GCPtr<jsid>& CacheIRStubInfo::getStubField<ICCacheIRStub>(
    ICCacheIRStub* stub, uint32_t offset) const;
template GCPtr<JSClass*>& CacheIRStubInfo::getStubField<ICCacheIRStub>(
    ICCacheIRStub* stub, uint32_t offset) const;
template GCPtr<ArrayObject*>& CacheIRStubInfo::getStubField<ICCacheIRStub>(
    ICCacheIRStub* stub, uint32_t offset) const;

template <class Stub, class T>
T* CacheIRStubInfo::getPtrStubField(Stub* stub, uint32_t offset) const {
  uint8_t* stubData = (uint8_t*)stub + stubDataOffset_;
  MOZ_ASSERT(uintptr_t(stubData + offset) % sizeof(uintptr_t) == 0);

  return *reinterpret_cast<T**>(stubData + offset);
}

template gc::AllocSite* CacheIRStubInfo::getPtrStubField(ICCacheIRStub* stub,
                                                         uint32_t offset) const;

template <typename T, typename V>
static void InitGCPtr(uintptr_t* ptr, V val) {
  AsGCPtr<T>(ptr)->init(mozilla::BitwiseCast<T>(val));
}

void CacheIRWriter::copyStubData(uint8_t* dest) const {
  MOZ_ASSERT(!failed());

  uintptr_t* destWords = reinterpret_cast<uintptr_t*>(dest);

  for (const StubField& field : stubFields_) {
    MOZ_ASSERT((uintptr_t(destWords) % field.sizeInBytes()) == 0,
               "Unaligned stub field");

    switch (field.type()) {
      case StubField::Type::RawInt32:
      case StubField::Type::RawPointer:
      case StubField::Type::AllocSite:
        *destWords = field.asWord();
        break;
      case StubField::Type::Shape:
        InitGCPtr<Shape*>(destWords, field.asWord());
        break;
      case StubField::Type::GetterSetter:
        InitGCPtr<GetterSetter*>(destWords, field.asWord());
        break;
      case StubField::Type::JSObject:
        InitGCPtr<JSObject*>(destWords, field.asWord());
        break;
      case StubField::Type::Symbol:
        InitGCPtr<JS::Symbol*>(destWords, field.asWord());
        break;
      case StubField::Type::String:
        InitGCPtr<JSString*>(destWords, field.asWord());
        break;
      case StubField::Type::BaseScript:
        InitGCPtr<BaseScript*>(destWords, field.asWord());
        break;
      case StubField::Type::Id:
        AsGCPtr<jsid>(destWords)->init(jsid::fromRawBits(field.asWord()));
        break;
      case StubField::Type::RawInt64:
        *reinterpret_cast<uint64_t*>(destWords) = field.asInt64();
        break;
      case StubField::Type::Value:
        AsGCPtr<Value>(destWords)->init(
            Value::fromRawBits(uint64_t(field.asInt64())));
        break;
      case StubField::Type::Limit:
        MOZ_CRASH("Invalid type");
    }
    destWords += StubField::sizeInBytes(field.type()) / sizeof(uintptr_t);
  }
}

template <typename T>
void jit::TraceCacheIRStub(JSTracer* trc, T* stub,
                           const CacheIRStubInfo* stubInfo) {
  uint32_t field = 0;
  size_t offset = 0;
  while (true) {
    StubField::Type fieldType = stubInfo->fieldType(field);
    switch (fieldType) {
      case StubField::Type::RawInt32:
      case StubField::Type::RawPointer:
      case StubField::Type::RawInt64:
        break;
      case StubField::Type::Shape: {
        // For CCW IC stubs, we can store same-zone but cross-compartment
        // shapes. Use TraceSameZoneCrossCompartmentEdge to not assert in the
        // GC. Note: CacheIRWriter::writeShapeField asserts we never store
        // cross-zone shapes.
        GCPtrShape& shapeField =
            stubInfo->getStubField<T, Shape*>(stub, offset);
        TraceSameZoneCrossCompartmentEdge(trc, &shapeField, "cacheir-shape");
        break;
      }
      case StubField::Type::GetterSetter:
        TraceEdge(trc, &stubInfo->getStubField<T, GetterSetter*>(stub, offset),
                  "cacheir-getter-setter");
        break;
      case StubField::Type::JSObject:
        TraceEdge(trc, &stubInfo->getStubField<T, JSObject*>(stub, offset),
                  "cacheir-object");
        break;
      case StubField::Type::Symbol:
        TraceEdge(trc, &stubInfo->getStubField<T, JS::Symbol*>(stub, offset),
                  "cacheir-symbol");
        break;
      case StubField::Type::String:
        TraceEdge(trc, &stubInfo->getStubField<T, JSString*>(stub, offset),
                  "cacheir-string");
        break;
      case StubField::Type::BaseScript:
        TraceEdge(trc, &stubInfo->getStubField<T, BaseScript*>(stub, offset),
                  "cacheir-script");
        break;
      case StubField::Type::Id:
        TraceEdge(trc, &stubInfo->getStubField<T, jsid>(stub, offset),
                  "cacheir-id");
        break;
      case StubField::Type::Value:
        TraceEdge(trc, &stubInfo->getStubField<T, JS::Value>(stub, offset),
                  "cacheir-value");
        break;
      case StubField::Type::AllocSite: {
        gc::AllocSite* site =
            stubInfo->getPtrStubField<T, gc::AllocSite>(stub, offset);
        site->trace(trc);
        break;
      }
      case StubField::Type::Limit:
        return;  // Done.
    }
    field++;
    offset += StubField::sizeInBytes(fieldType);
  }
}

template void jit::TraceCacheIRStub(JSTracer* trc, ICCacheIRStub* stub,
                                    const CacheIRStubInfo* stubInfo);

template void jit::TraceCacheIRStub(JSTracer* trc, IonICStub* stub,
                                    const CacheIRStubInfo* stubInfo);

bool CacheIRWriter::stubDataEquals(const uint8_t* stubData) const {
  MOZ_ASSERT(!failed());

  const uintptr_t* stubDataWords = reinterpret_cast<const uintptr_t*>(stubData);

  for (const StubField& field : stubFields_) {
    if (field.sizeIsWord()) {
      if (field.asWord() != *stubDataWords) {
        return false;
      }
      stubDataWords++;
      continue;
    }

    if (field.asInt64() != *reinterpret_cast<const uint64_t*>(stubDataWords)) {
      return false;
    }
    stubDataWords += sizeof(uint64_t) / sizeof(uintptr_t);
  }

  return true;
}

HashNumber CacheIRStubKey::hash(const CacheIRStubKey::Lookup& l) {
  HashNumber hash = mozilla::HashBytes(l.code, l.length);
  hash = mozilla::AddToHash(hash, uint32_t(l.kind));
  hash = mozilla::AddToHash(hash, uint32_t(l.engine));
  return hash;
}

bool CacheIRStubKey::match(const CacheIRStubKey& entry,
                           const CacheIRStubKey::Lookup& l) {
  if (entry.stubInfo->kind() != l.kind) {
    return false;
  }

  if (entry.stubInfo->engine() != l.engine) {
    return false;
  }

  if (entry.stubInfo->codeLength() != l.length) {
    return false;
  }

  if (!mozilla::ArrayEqual(entry.stubInfo->code(), l.code, l.length)) {
    return false;
  }

  return true;
}

CacheIRReader::CacheIRReader(const CacheIRStubInfo* stubInfo)
    : CacheIRReader(stubInfo->code(),
                    stubInfo->code() + stubInfo->codeLength()) {}

CacheIRStubInfo* CacheIRStubInfo::New(CacheKind kind, ICStubEngine engine,
                                      bool makesGCCalls,
                                      uint32_t stubDataOffset,
                                      const CacheIRWriter& writer) {
  size_t numStubFields = writer.numStubFields();
  size_t bytesNeeded =
      sizeof(CacheIRStubInfo) + writer.codeLength() +
      (numStubFields + 1);  // +1 for the GCType::Limit terminator.
  uint8_t* p = js_pod_malloc<uint8_t>(bytesNeeded);
  if (!p) {
    return nullptr;
  }

  // Copy the CacheIR code.
  uint8_t* codeStart = p + sizeof(CacheIRStubInfo);
  mozilla::PodCopy(codeStart, writer.codeStart(), writer.codeLength());

  static_assert(sizeof(StubField::Type) == sizeof(uint8_t),
                "StubField::Type must fit in uint8_t");

  // Copy the stub field types.
  uint8_t* fieldTypes = codeStart + writer.codeLength();
  for (size_t i = 0; i < numStubFields; i++) {
    fieldTypes[i] = uint8_t(writer.stubFieldType(i));
  }
  fieldTypes[numStubFields] = uint8_t(StubField::Type::Limit);

  return new (p) CacheIRStubInfo(kind, engine, makesGCCalls, stubDataOffset,
                                 codeStart, writer.codeLength(), fieldTypes);
}

bool OperandLocation::operator==(const OperandLocation& other) const {
  if (kind_ != other.kind_) {
    return false;
  }

  switch (kind()) {
    case Uninitialized:
      return true;
    case PayloadReg:
      return payloadReg() == other.payloadReg() &&
             payloadType() == other.payloadType();
    case ValueReg:
      return valueReg() == other.valueReg();
    case PayloadStack:
      return payloadStack() == other.payloadStack() &&
             payloadType() == other.payloadType();
    case ValueStack:
      return valueStack() == other.valueStack();
    case BaselineFrame:
      return baselineFrameSlot() == other.baselineFrameSlot();
    case Constant:
      return constant() == other.constant();
    case DoubleReg:
      return doubleReg() == other.doubleReg();
  }

  MOZ_CRASH("Invalid OperandLocation kind");
}

AutoOutputRegister::AutoOutputRegister(CacheIRCompiler& compiler)
    : output_(compiler.outputUnchecked_.ref()), alloc_(compiler.allocator) {
  if (output_.hasValue()) {
    alloc_.allocateFixedValueRegister(compiler.masm, output_.valueReg());
  } else if (!output_.typedReg().isFloat()) {
    alloc_.allocateFixedRegister(compiler.masm, output_.typedReg().gpr());
  }
}

AutoOutputRegister::~AutoOutputRegister() {
  if (output_.hasValue()) {
    alloc_.releaseValueRegister(output_.valueReg());
  } else if (!output_.typedReg().isFloat()) {
    alloc_.releaseRegister(output_.typedReg().gpr());
  }
}

bool FailurePath::canShareFailurePath(const FailurePath& other) const {
  if (stackPushed_ != other.stackPushed_) {
    return false;
  }

  if (spilledRegs_.length() != other.spilledRegs_.length()) {
    return false;
  }

  for (size_t i = 0; i < spilledRegs_.length(); i++) {
    if (spilledRegs_[i] != other.spilledRegs_[i]) {
      return false;
    }
  }

  MOZ_ASSERT(inputs_.length() == other.inputs_.length());

  for (size_t i = 0; i < inputs_.length(); i++) {
    if (inputs_[i] != other.inputs_[i]) {
      return false;
    }
  }
  return true;
}

bool CacheIRCompiler::addFailurePath(FailurePath** failure) {
#ifdef DEBUG
  allocator.setAddedFailurePath();
#endif
  MOZ_ASSERT(!allocator.hasAutoScratchFloatRegisterSpill());

  FailurePath newFailure;
  for (size_t i = 0; i < writer_.numInputOperands(); i++) {
    if (!newFailure.appendInput(allocator.operandLocation(i))) {
      return false;
    }
  }
  if (!newFailure.setSpilledRegs(allocator.spilledRegs())) {
    return false;
  }
  newFailure.setStackPushed(allocator.stackPushed());

  // Reuse the previous failure path if the current one is the same, to
  // avoid emitting duplicate code.
  if (failurePaths.length() > 0 &&
      failurePaths.back().canShareFailurePath(newFailure)) {
    *failure = &failurePaths.back();
    return true;
  }

  if (!failurePaths.append(std::move(newFailure))) {
    return false;
  }

  *failure = &failurePaths.back();
  return true;
}

bool CacheIRCompiler::emitFailurePath(size_t index) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  FailurePath& failure = failurePaths[index];

  allocator.setStackPushed(failure.stackPushed());

  for (size_t i = 0; i < writer_.numInputOperands(); i++) {
    allocator.setOperandLocation(i, failure.input(i));
  }

  if (!allocator.setSpilledRegs(failure.spilledRegs())) {
    return false;
  }

  masm.bind(failure.label());
  allocator.restoreInputState(masm);
  return true;
}

bool CacheIRCompiler::emitGuardIsNumber(ValOperandId inputId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  JSValueType knownType = allocator.knownType(inputId);

  // Doubles and ints are numbers!
  if (knownType == JSVAL_TYPE_DOUBLE || knownType == JSVAL_TYPE_INT32) {
    return true;
  }

  ValueOperand input = allocator.useValueRegister(masm, inputId);
  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.branchTestNumber(Assembler::NotEqual, input, failure->label());
  return true;
}

bool CacheIRCompiler::emitGuardToObject(ValOperandId inputId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  if (allocator.knownType(inputId) == JSVAL_TYPE_OBJECT) {
    return true;
  }

  ValueOperand input = allocator.useValueRegister(masm, inputId);
  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }
  masm.branchTestObject(Assembler::NotEqual, input, failure->label());
  return true;
}

bool CacheIRCompiler::emitGuardIsNullOrUndefined(ValOperandId inputId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  JSValueType knownType = allocator.knownType(inputId);
  if (knownType == JSVAL_TYPE_UNDEFINED || knownType == JSVAL_TYPE_NULL) {
    return true;
  }

  ValueOperand input = allocator.useValueRegister(masm, inputId);
  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  Label success;
  masm.branchTestNull(Assembler::Equal, input, &success);
  masm.branchTestUndefined(Assembler::NotEqual, input, failure->label());

  masm.bind(&success);
  return true;
}

bool CacheIRCompiler::emitGuardIsNull(ValOperandId inputId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  JSValueType knownType = allocator.knownType(inputId);
  if (knownType == JSVAL_TYPE_NULL) {
    return true;
  }

  ValueOperand input = allocator.useValueRegister(masm, inputId);
  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  Label success;
  masm.branchTestNull(Assembler::NotEqual, input, failure->label());
  return true;
}

bool CacheIRCompiler::emitGuardIsUndefined(ValOperandId inputId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  JSValueType knownType = allocator.knownType(inputId);
  if (knownType == JSVAL_TYPE_UNDEFINED) {
    return true;
  }

  ValueOperand input = allocator.useValueRegister(masm, inputId);
  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.branchTestUndefined(Assembler::NotEqual, input, failure->label());
  return true;
}

bool CacheIRCompiler::emitGuardBooleanToInt32(ValOperandId inputId,
                                              Int32OperandId resultId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register output = allocator.defineRegister(masm, resultId);

  if (allocator.knownType(inputId) == JSVAL_TYPE_BOOLEAN) {
    Register input =
        allocator.useRegister(masm, BooleanOperandId(inputId.id()));
    masm.move32(input, output);
    return true;
  }
  ValueOperand input = allocator.useValueRegister(masm, inputId);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.fallibleUnboxBoolean(input, output, failure->label());
  return true;
}

bool CacheIRCompiler::emitGuardToString(ValOperandId inputId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  if (allocator.knownType(inputId) == JSVAL_TYPE_STRING) {
    return true;
  }

  ValueOperand input = allocator.useValueRegister(masm, inputId);
  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }
  masm.branchTestString(Assembler::NotEqual, input, failure->label());
  return true;
}

bool CacheIRCompiler::emitGuardToSymbol(ValOperandId inputId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  if (allocator.knownType(inputId) == JSVAL_TYPE_SYMBOL) {
    return true;
  }

  ValueOperand input = allocator.useValueRegister(masm, inputId);
  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }
  masm.branchTestSymbol(Assembler::NotEqual, input, failure->label());
  return true;
}

bool CacheIRCompiler::emitGuardToBigInt(ValOperandId inputId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  if (allocator.knownType(inputId) == JSVAL_TYPE_BIGINT) {
    return true;
  }

  ValueOperand input = allocator.useValueRegister(masm, inputId);
  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }
  masm.branchTestBigInt(Assembler::NotEqual, input, failure->label());
  return true;
}

bool CacheIRCompiler::emitGuardToBoolean(ValOperandId inputId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  if (allocator.knownType(inputId) == JSVAL_TYPE_BOOLEAN) {
    return true;
  }

  ValueOperand input = allocator.useValueRegister(masm, inputId);
  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }
  masm.branchTestBoolean(Assembler::NotEqual, input, failure->label());
  return true;
}

bool CacheIRCompiler::emitGuardToInt32(ValOperandId inputId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  if (allocator.knownType(inputId) == JSVAL_TYPE_INT32) {
    return true;
  }

  ValueOperand input = allocator.useValueRegister(masm, inputId);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.branchTestInt32(Assembler::NotEqual, input, failure->label());
  return true;
}

// Infallible |emitDouble| emitters can use this implementation to avoid
// generating extra clean-up instructions to restore the scratch float register.
// To select this function simply omit the |Label* fail| parameter for the
// emitter lambda function.
template <typename EmitDouble>
static std::enable_if_t<mozilla::FunctionTypeTraits<EmitDouble>::arity == 1,
                        void>
EmitGuardDouble(CacheIRCompiler* compiler, MacroAssembler& masm,
                ValueOperand input, FailurePath* failure,
                EmitDouble emitDouble) {
  AutoScratchFloatRegister floatReg(compiler);

  masm.unboxDouble(input, floatReg);
  emitDouble(floatReg.get());
}

template <typename EmitDouble>
static std::enable_if_t<mozilla::FunctionTypeTraits<EmitDouble>::arity == 2,
                        void>
EmitGuardDouble(CacheIRCompiler* compiler, MacroAssembler& masm,
                ValueOperand input, FailurePath* failure,
                EmitDouble emitDouble) {
  AutoScratchFloatRegister floatReg(compiler, failure);

  masm.unboxDouble(input, floatReg);
  emitDouble(floatReg.get(), floatReg.failure());
}

template <typename EmitInt32, typename EmitDouble>
static void EmitGuardInt32OrDouble(CacheIRCompiler* compiler,
                                   MacroAssembler& masm, ValueOperand input,
                                   Register output, FailurePath* failure,
                                   EmitInt32 emitInt32, EmitDouble emitDouble) {
  Label done;

  {
    ScratchTagScope tag(masm, input);
    masm.splitTagForTest(input, tag);

    Label notInt32;
    masm.branchTestInt32(Assembler::NotEqual, tag, &notInt32);
    {
      ScratchTagScopeRelease _(&tag);

      masm.unboxInt32(input, output);
      emitInt32();

      masm.jump(&done);
    }
    masm.bind(&notInt32);

    masm.branchTestDouble(Assembler::NotEqual, tag, failure->label());
    {
      ScratchTagScopeRelease _(&tag);

      EmitGuardDouble(compiler, masm, input, failure, emitDouble);
    }
  }

  masm.bind(&done);
}

bool CacheIRCompiler::emitGuardToInt32Index(ValOperandId inputId,
                                            Int32OperandId resultId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register output = allocator.defineRegister(masm, resultId);

  if (allocator.knownType(inputId) == JSVAL_TYPE_INT32) {
    Register input = allocator.useRegister(masm, Int32OperandId(inputId.id()));
    masm.move32(input, output);
    return true;
  }

  ValueOperand input = allocator.useValueRegister(masm, inputId);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  EmitGuardInt32OrDouble(
      this, masm, input, output, failure,
      []() {
        // No-op if the value is already an int32.
      },
      [&](FloatRegister floatReg, Label* fail) {
        // ToPropertyKey(-0.0) is "0", so we can truncate -0.0 to 0 here.
        masm.convertDoubleToInt32(floatReg, output, fail, false);
      });

  return true;
}

bool CacheIRCompiler::emitInt32ToIntPtr(Int32OperandId inputId,
                                        IntPtrOperandId resultId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  Register input = allocator.useRegister(masm, inputId);
  Register output = allocator.defineRegister(masm, resultId);

  masm.move32SignExtendToPtr(input, output);
  return true;
}

bool CacheIRCompiler::emitGuardNumberToIntPtrIndex(NumberOperandId inputId,
                                                   bool supportOOB,
                                                   IntPtrOperandId resultId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  Register output = allocator.defineRegister(masm, resultId);

  FailurePath* failure = nullptr;
  if (!supportOOB) {
    if (!addFailurePath(&failure)) {
      return false;
    }
  }

  AutoScratchFloatRegister floatReg(this, failure);
  allocator.ensureDoubleRegister(masm, inputId, floatReg);

  // ToPropertyKey(-0.0) is "0", so we can truncate -0.0 to 0 here.
  if (supportOOB) {
    Label done, fail;
    masm.convertDoubleToPtr(floatReg, output, &fail, false);
    masm.jump(&done);

    // Substitute the invalid index with an arbitrary out-of-bounds index.
    masm.bind(&fail);
    masm.movePtr(ImmWord(-1), output);

    masm.bind(&done);
  } else {
    masm.convertDoubleToPtr(floatReg, output, floatReg.failure(), false);
  }

  return true;
}

bool CacheIRCompiler::emitGuardToInt32ModUint32(ValOperandId inputId,
                                                Int32OperandId resultId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register output = allocator.defineRegister(masm, resultId);

  if (allocator.knownType(inputId) == JSVAL_TYPE_INT32) {
    ConstantOrRegister input = allocator.useConstantOrRegister(masm, inputId);
    if (input.constant()) {
      masm.move32(Imm32(input.value().toInt32()), output);
    } else {
      MOZ_ASSERT(input.reg().type() == MIRType::Int32);
      masm.move32(input.reg().typedReg().gpr(), output);
    }
    return true;
  }

  ValueOperand input = allocator.useValueRegister(masm, inputId);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  EmitGuardInt32OrDouble(
      this, masm, input, output, failure,
      []() {
        // No-op if the value is already an int32.
      },
      [&](FloatRegister floatReg, Label* fail) {
        masm.branchTruncateDoubleMaybeModUint32(floatReg, output, fail);
      });

  return true;
}

bool CacheIRCompiler::emitGuardToUint8Clamped(ValOperandId inputId,
                                              Int32OperandId resultId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register output = allocator.defineRegister(masm, resultId);

  if (allocator.knownType(inputId) == JSVAL_TYPE_INT32) {
    ConstantOrRegister input = allocator.useConstantOrRegister(masm, inputId);
    if (input.constant()) {
      masm.move32(Imm32(ClampDoubleToUint8(input.value().toInt32())), output);
    } else {
      MOZ_ASSERT(input.reg().type() == MIRType::Int32);
      masm.move32(input.reg().typedReg().gpr(), output);
      masm.clampIntToUint8(output);
    }
    return true;
  }

  ValueOperand input = allocator.useValueRegister(masm, inputId);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  EmitGuardInt32OrDouble(
      this, masm, input, output, failure,
      [&]() {
        // |output| holds the unboxed int32 value.
        masm.clampIntToUint8(output);
      },
      [&](FloatRegister floatReg) {
        masm.clampDoubleToUint8(floatReg, output);
      });

  return true;
}

bool CacheIRCompiler::emitGuardNonDoubleType(ValOperandId inputId,
                                             ValueType type) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  if (allocator.knownType(inputId) == JSValueType(type)) {
    return true;
  }

  ValueOperand input = allocator.useValueRegister(masm, inputId);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  switch (type) {
    case ValueType::String:
      masm.branchTestString(Assembler::NotEqual, input, failure->label());
      break;
    case ValueType::Symbol:
      masm.branchTestSymbol(Assembler::NotEqual, input, failure->label());
      break;
    case ValueType::BigInt:
      masm.branchTestBigInt(Assembler::NotEqual, input, failure->label());
      break;
    case ValueType::Int32:
      masm.branchTestInt32(Assembler::NotEqual, input, failure->label());
      break;
    case ValueType::Boolean:
      masm.branchTestBoolean(Assembler::NotEqual, input, failure->label());
      break;
    case ValueType::Undefined:
      masm.branchTestUndefined(Assembler::NotEqual, input, failure->label());
      break;
    case ValueType::Null:
      masm.branchTestNull(Assembler::NotEqual, input, failure->label());
      break;
    case ValueType::Double:
    case ValueType::Magic:
    case ValueType::PrivateGCThing:
    case ValueType::Object:
      MOZ_CRASH("unexpected type");
  }

  return true;
}

bool CacheIRCompiler::emitGuardClass(ObjOperandId objId, GuardClassKind kind) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register obj = allocator.useRegister(masm, objId);
  AutoScratchRegister scratch(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  const JSClass* clasp = nullptr;
  switch (kind) {
    case GuardClassKind::Array:
      clasp = &ArrayObject::class_;
      break;
    case GuardClassKind::ArrayBuffer:
      clasp = &ArrayBufferObject::class_;
      break;
    case GuardClassKind::SharedArrayBuffer:
      clasp = &SharedArrayBufferObject::class_;
      break;
    case GuardClassKind::DataView:
      clasp = &DataViewObject::class_;
      break;
    case GuardClassKind::MappedArguments:
      clasp = &MappedArgumentsObject::class_;
      break;
    case GuardClassKind::UnmappedArguments:
      clasp = &UnmappedArgumentsObject::class_;
      break;
    case GuardClassKind::WindowProxy:
      clasp = cx_->runtime()->maybeWindowProxyClass();
      break;
    case GuardClassKind::JSFunction:
      clasp = &JSFunction::class_;
      break;
  }
  MOZ_ASSERT(clasp);

  if (objectGuardNeedsSpectreMitigations(objId)) {
    masm.branchTestObjClass(Assembler::NotEqual, obj, clasp, scratch, obj,
                            failure->label());
  } else {
    masm.branchTestObjClassNoSpectreMitigations(Assembler::NotEqual, obj, clasp,
                                                scratch, failure->label());
  }

  return true;
}

bool CacheIRCompiler::emitGuardNullProto(ObjOperandId objId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register obj = allocator.useRegister(masm, objId);
  AutoScratchRegister scratch(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.loadObjProto(obj, scratch);
  masm.branchTestPtr(Assembler::NonZero, scratch, scratch, failure->label());
  return true;
}

bool CacheIRCompiler::emitGuardIsExtensible(ObjOperandId objId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register obj = allocator.useRegister(masm, objId);
  AutoScratchRegister scratch(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.branchIfObjectNotExtensible(obj, scratch, failure->label());
  return true;
}

bool CacheIRCompiler::emitGuardDynamicSlotIsSpecificObject(
    ObjOperandId objId, ObjOperandId expectedId, uint32_t slotOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register obj = allocator.useRegister(masm, objId);
  Register expectedObject = allocator.useRegister(masm, expectedId);

  // Allocate registers before the failure path to make sure they're registered
  // by addFailurePath.
  AutoScratchRegister scratch1(allocator, masm);
  AutoScratchRegister scratch2(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  // Guard on the expected object.
  StubFieldOffset slot(slotOffset, StubField::Type::RawInt32);
  masm.loadPtr(Address(obj, NativeObject::offsetOfSlots()), scratch1);
  emitLoadStubField(slot, scratch2);
  BaseObjectSlotIndex expectedSlot(scratch1, scratch2);
  masm.fallibleUnboxObject(expectedSlot, scratch1, failure->label());
  masm.branchPtr(Assembler::NotEqual, expectedObject, scratch1,
                 failure->label());

  return true;
}

bool CacheIRCompiler::emitGuardFixedSlotValue(ObjOperandId objId,
                                              uint32_t offsetOffset,
                                              uint32_t valOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  Register obj = allocator.useRegister(masm, objId);

  AutoScratchRegister scratch(allocator, masm);
  AutoScratchValueRegister scratchVal(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  StubFieldOffset offset(offsetOffset, StubField::Type::RawInt32);
  emitLoadStubField(offset, scratch);

  StubFieldOffset val(valOffset, StubField::Type::Value);
  emitLoadValueStubField(val, scratchVal);

  BaseIndex slotVal(obj, scratch, TimesOne);
  masm.branchTestValue(Assembler::NotEqual, slotVal, scratchVal,
                       failure->label());
  return true;
}

bool CacheIRCompiler::emitGuardDynamicSlotValue(ObjOperandId objId,
                                                uint32_t offsetOffset,
                                                uint32_t valOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  Register obj = allocator.useRegister(masm, objId);

  AutoScratchRegister scratch1(allocator, masm);
  AutoScratchRegister scratch2(allocator, masm);
  AutoScratchValueRegister scratchVal(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.loadPtr(Address(obj, NativeObject::offsetOfSlots()), scratch1);

  StubFieldOffset offset(offsetOffset, StubField::Type::RawInt32);
  emitLoadStubField(offset, scratch2);

  StubFieldOffset val(valOffset, StubField::Type::Value);
  emitLoadValueStubField(val, scratchVal);

  BaseIndex slotVal(scratch1, scratch2, TimesOne);
  masm.branchTestValue(Assembler::NotEqual, slotVal, scratchVal,
                       failure->label());
  return true;
}

bool CacheIRCompiler::emitGuardIsNativeObject(ObjOperandId objId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  Register obj = allocator.useRegister(masm, objId);
  AutoScratchRegister scratch(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.branchIfNonNativeObj(obj, scratch, failure->label());
  return true;
}

bool CacheIRCompiler::emitGuardIsProxy(ObjOperandId objId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  Register obj = allocator.useRegister(masm, objId);
  AutoScratchRegister scratch(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.branchTestObjectIsProxy(false, obj, scratch, failure->label());
  return true;
}

bool CacheIRCompiler::emitGuardIsNotProxy(ObjOperandId objId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  Register obj = allocator.useRegister(masm, objId);
  AutoScratchRegister scratch(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.branchTestObjectIsProxy(true, obj, scratch, failure->label());
  return true;
}

bool CacheIRCompiler::emitGuardIsNotArrayBufferMaybeShared(ObjOperandId objId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  Register obj = allocator.useRegister(masm, objId);
  AutoScratchRegister scratch(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.loadObjClassUnsafe(obj, scratch);
  masm.branchPtr(Assembler::Equal, scratch, ImmPtr(&ArrayBufferObject::class_),
                 failure->label());
  masm.branchPtr(Assembler::Equal, scratch,
                 ImmPtr(&SharedArrayBufferObject::class_), failure->label());
  return true;
}

bool CacheIRCompiler::emitGuardIsTypedArray(ObjOperandId objId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  Register obj = allocator.useRegister(masm, objId);
  AutoScratchRegister scratch(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.loadObjClassUnsafe(obj, scratch);
  masm.branchIfClassIsNotTypedArray(scratch, failure->label());
  return true;
}

bool CacheIRCompiler::emitGuardIsNotDOMProxy(ObjOperandId objId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register obj = allocator.useRegister(masm, objId);
  AutoScratchRegister scratch(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.branchTestProxyHandlerFamily(Assembler::Equal, obj, scratch,
                                    GetDOMProxyHandlerFamily(),
                                    failure->label());
  return true;
}

bool CacheIRCompiler::emitGuardNoDenseElements(ObjOperandId objId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register obj = allocator.useRegister(masm, objId);
  AutoScratchRegister scratch(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  // Load obj->elements.
  masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), scratch);

  // Make sure there are no dense elements.
  Address initLength(scratch, ObjectElements::offsetOfInitializedLength());
  masm.branch32(Assembler::NotEqual, initLength, Imm32(0), failure->label());
  return true;
}

bool CacheIRCompiler::emitGuardStringToInt32(StringOperandId strId,
                                             Int32OperandId resultId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register str = allocator.useRegister(masm, strId);
  Register output = allocator.defineRegister(masm, resultId);
  AutoScratchRegister scratch(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  LiveRegisterSet volatileRegs(GeneralRegisterSet::Volatile(),
                               liveVolatileFloatRegs());
  masm.guardStringToInt32(str, output, scratch, volatileRegs, failure->label());
  return true;
}

bool CacheIRCompiler::emitGuardStringToNumber(StringOperandId strId,
                                              NumberOperandId resultId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register str = allocator.useRegister(masm, strId);
  ValueOperand output = allocator.defineValueRegister(masm, resultId);
  AutoScratchRegister scratch(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  Label vmCall, done;
  // Use indexed value as fast path if possible.
  masm.loadStringIndexValue(str, scratch, &vmCall);
  masm.tagValue(JSVAL_TYPE_INT32, scratch, output);
  masm.jump(&done);
  {
    masm.bind(&vmCall);

    // Reserve stack for holding the result value of the call.
    masm.reserveStack(sizeof(double));
    masm.moveStackPtrTo(output.payloadOrValueReg());

    // We cannot use callVM, as callVM expects to be able to clobber all
    // operands, however, since this op is not the last in the generated IC, we
    // want to be able to reference other live values.
    LiveRegisterSet volatileRegs(GeneralRegisterSet::Volatile(),
                                 liveVolatileFloatRegs());
    masm.PushRegsInMask(volatileRegs);

    using Fn = bool (*)(JSContext * cx, JSString * str, double* result);
    masm.setupUnalignedABICall(scratch);
    masm.loadJSContext(scratch);
    masm.passABIArg(scratch);
    masm.passABIArg(str);
    masm.passABIArg(output.payloadOrValueReg());
    masm.callWithABI<Fn, js::StringToNumberPure>();
    masm.mov(ReturnReg, scratch);

    LiveRegisterSet ignore;
    ignore.add(scratch);
    masm.PopRegsInMaskIgnore(volatileRegs, ignore);

    Label ok;
    masm.branchIfTrueBool(scratch, &ok);
    {
      // OOM path, recovered by StringToNumberPure.
      //
      // Use addToStackPtr instead of freeStack as freeStack tracks stack height
      // flow-insensitively, and using it twice would confuse the stack height
      // tracking.
      masm.addToStackPtr(Imm32(sizeof(double)));
      masm.jump(failure->label());
    }
    masm.bind(&ok);

    {
      ScratchDoubleScope fpscratch(masm);
      masm.loadDouble(Address(output.payloadOrValueReg(), 0), fpscratch);
      masm.boxDouble(fpscratch, output, fpscratch);
    }
    masm.freeStack(sizeof(double));
  }
  masm.bind(&done);
  return true;
}

bool CacheIRCompiler::emitBooleanToNumber(BooleanOperandId booleanId,
                                          NumberOperandId resultId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register boolean = allocator.useRegister(masm, booleanId);
  ValueOperand output = allocator.defineValueRegister(masm, resultId);
  masm.tagValue(JSVAL_TYPE_INT32, boolean, output);
  return true;
}

bool CacheIRCompiler::emitGuardStringToIndex(StringOperandId strId,
                                             Int32OperandId resultId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register str = allocator.useRegister(masm, strId);
  Register output = allocator.defineRegister(masm, resultId);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  Label vmCall, done;
  masm.loadStringIndexValue(str, output, &vmCall);
  masm.jump(&done);

  {
    masm.bind(&vmCall);
    LiveRegisterSet save(GeneralRegisterSet::Volatile(),
                         liveVolatileFloatRegs());
    masm.PushRegsInMask(save);

    using Fn = int32_t (*)(JSString * str);
    masm.setupUnalignedABICall(output);
    masm.passABIArg(str);
    masm.callWithABI<Fn, GetIndexFromString>();
    masm.storeCallInt32Result(output);

    LiveRegisterSet ignore;
    ignore.add(output);
    masm.PopRegsInMaskIgnore(save, ignore);

    // GetIndexFromString returns a negative value on failure.
    masm.branchTest32(Assembler::Signed, output, output, failure->label());
  }

  masm.bind(&done);
  return true;
}

bool CacheIRCompiler::emitLoadProto(ObjOperandId objId, ObjOperandId resultId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register obj = allocator.useRegister(masm, objId);
  Register reg = allocator.defineRegister(masm, resultId);
  masm.loadObjProto(obj, reg);

#ifdef DEBUG
  // We shouldn't encounter a null or lazy proto.
  MOZ_ASSERT(uintptr_t(TaggedProto::LazyProto) == 1);

  Label done;
  masm.branchPtr(Assembler::Above, reg, ImmWord(1), &done);
  masm.assumeUnreachable("Unexpected null or lazy proto in CacheIR LoadProto");
  masm.bind(&done);
#endif
  return true;
}

bool CacheIRCompiler::emitLoadEnclosingEnvironment(ObjOperandId objId,
                                                   ObjOperandId resultId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register obj = allocator.useRegister(masm, objId);
  Register reg = allocator.defineRegister(masm, resultId);
  masm.unboxObject(
      Address(obj, EnvironmentObject::offsetOfEnclosingEnvironment()), reg);
  return true;
}

bool CacheIRCompiler::emitLoadWrapperTarget(ObjOperandId objId,
                                            ObjOperandId resultId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register obj = allocator.useRegister(masm, objId);
  Register reg = allocator.defineRegister(masm, resultId);

  masm.loadPtr(Address(obj, ProxyObject::offsetOfReservedSlots()), reg);
  masm.unboxObject(
      Address(reg, js::detail::ProxyReservedSlots::offsetOfPrivateSlot()), reg);
  return true;
}

bool CacheIRCompiler::emitLoadValueTag(ValOperandId valId,
                                       ValueTagOperandId resultId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  ValueOperand val = allocator.useValueRegister(masm, valId);
  Register res = allocator.defineRegister(masm, resultId);

  Register tag = masm.extractTag(val, res);
  if (tag != res) {
    masm.mov(tag, res);
  }
  return true;
}

bool CacheIRCompiler::emitLoadDOMExpandoValue(ObjOperandId objId,
                                              ValOperandId resultId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register obj = allocator.useRegister(masm, objId);
  ValueOperand val = allocator.defineValueRegister(masm, resultId);

  masm.loadPtr(Address(obj, ProxyObject::offsetOfReservedSlots()),
               val.scratchReg());
  masm.loadValue(Address(val.scratchReg(),
                         js::detail::ProxyReservedSlots::offsetOfPrivateSlot()),
                 val);
  return true;
}

bool CacheIRCompiler::emitLoadDOMExpandoValueIgnoreGeneration(
    ObjOperandId objId, ValOperandId resultId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register obj = allocator.useRegister(masm, objId);
  ValueOperand output = allocator.defineValueRegister(masm, resultId);

  // Determine the expando's Address.
  Register scratch = output.scratchReg();
  masm.loadPtr(Address(obj, ProxyObject::offsetOfReservedSlots()), scratch);
  Address expandoAddr(scratch,
                      js::detail::ProxyReservedSlots::offsetOfPrivateSlot());

#ifdef DEBUG
  // Private values are stored as doubles, so assert we have a double.
  Label ok;
  masm.branchTestDouble(Assembler::Equal, expandoAddr, &ok);
  masm.assumeUnreachable("DOM expando is not a PrivateValue!");
  masm.bind(&ok);
#endif

  // Load the ExpandoAndGeneration* from the PrivateValue.
  masm.loadPrivate(expandoAddr, scratch);

  // Load expandoAndGeneration->expando into the output Value register.
  masm.loadValue(Address(scratch, ExpandoAndGeneration::offsetOfExpando()),
                 output);
  return true;
}

bool CacheIRCompiler::emitLoadUndefinedResult() {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  masm.moveValue(UndefinedValue(), output.valueReg());
  return true;
}

static void EmitStoreBoolean(MacroAssembler& masm, bool b,
                             const AutoOutputRegister& output) {
  if (output.hasValue()) {
    Value val = BooleanValue(b);
    masm.moveValue(val, output.valueReg());
  } else {
    MOZ_ASSERT(output.type() == JSVAL_TYPE_BOOLEAN);
    masm.movePtr(ImmWord(b), output.typedReg().gpr());
  }
}

bool CacheIRCompiler::emitLoadBooleanResult(bool val) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  EmitStoreBoolean(masm, val, output);
  return true;
}

bool CacheIRCompiler::emitLoadOperandResult(ValOperandId inputId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  ValueOperand input = allocator.useValueRegister(masm, inputId);
  masm.moveValue(input, output.valueReg());
  return true;
}

static void EmitStoreResult(MacroAssembler& masm, Register reg,
                            JSValueType type,
                            const AutoOutputRegister& output) {
  if (output.hasValue()) {
    masm.tagValue(type, reg, output.valueReg());
    return;
  }
  if (type == JSVAL_TYPE_INT32 && output.typedReg().isFloat()) {
    masm.convertInt32ToDouble(reg, output.typedReg().fpu());
    return;
  }
  if (type == output.type()) {
    masm.mov(reg, output.typedReg().gpr());
    return;
  }
  masm.assumeUnreachable("Should have monitored result");
}

bool CacheIRCompiler::emitLoadInt32ArrayLengthResult(ObjOperandId objId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register obj = allocator.useRegister(masm, objId);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), scratch);
  masm.load32(Address(scratch, ObjectElements::offsetOfLength()), scratch);

  // Guard length fits in an int32.
  masm.branchTest32(Assembler::Signed, scratch, scratch, failure->label());
  masm.tagValue(JSVAL_TYPE_INT32, scratch, output.valueReg());
  return true;
}

bool CacheIRCompiler::emitLoadInt32ArrayLength(ObjOperandId objId,
                                               Int32OperandId resultId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register obj = allocator.useRegister(masm, objId);
  Register res = allocator.defineRegister(masm, resultId);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), res);
  masm.load32(Address(res, ObjectElements::offsetOfLength()), res);

  // Guard length fits in an int32.
  masm.branchTest32(Assembler::Signed, res, res, failure->label());
  return true;
}

bool CacheIRCompiler::emitDoubleAddResult(NumberOperandId lhsId,
                                          NumberOperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);

  // Float register must be preserved. The BinaryArith ICs use
  // the fact that baseline has them available, as well as fixed temps on
  // LBinaryCache.
  AutoAvailableFloatRegister floatScratch0(*this, FloatReg0);
  AutoAvailableFloatRegister floatScratch1(*this, FloatReg1);

  allocator.ensureDoubleRegister(masm, lhsId, floatScratch0);
  allocator.ensureDoubleRegister(masm, rhsId, floatScratch1);

  masm.addDouble(floatScratch1, floatScratch0);
  masm.boxDouble(floatScratch0, output.valueReg(), floatScratch0);

  return true;
}
bool CacheIRCompiler::emitDoubleSubResult(NumberOperandId lhsId,
                                          NumberOperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);

  AutoAvailableFloatRegister floatScratch0(*this, FloatReg0);
  AutoAvailableFloatRegister floatScratch1(*this, FloatReg1);

  allocator.ensureDoubleRegister(masm, lhsId, floatScratch0);
  allocator.ensureDoubleRegister(masm, rhsId, floatScratch1);

  masm.subDouble(floatScratch1, floatScratch0);
  masm.boxDouble(floatScratch0, output.valueReg(), floatScratch0);

  return true;
}
bool CacheIRCompiler::emitDoubleMulResult(NumberOperandId lhsId,
                                          NumberOperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);

  AutoAvailableFloatRegister floatScratch0(*this, FloatReg0);
  AutoAvailableFloatRegister floatScratch1(*this, FloatReg1);

  allocator.ensureDoubleRegister(masm, lhsId, floatScratch0);
  allocator.ensureDoubleRegister(masm, rhsId, floatScratch1);

  masm.mulDouble(floatScratch1, floatScratch0);
  masm.boxDouble(floatScratch0, output.valueReg(), floatScratch0);

  return true;
}
bool CacheIRCompiler::emitDoubleDivResult(NumberOperandId lhsId,
                                          NumberOperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);

  AutoAvailableFloatRegister floatScratch0(*this, FloatReg0);
  AutoAvailableFloatRegister floatScratch1(*this, FloatReg1);

  allocator.ensureDoubleRegister(masm, lhsId, floatScratch0);
  allocator.ensureDoubleRegister(masm, rhsId, floatScratch1);

  masm.divDouble(floatScratch1, floatScratch0);
  masm.boxDouble(floatScratch0, output.valueReg(), floatScratch0);

  return true;
}
bool CacheIRCompiler::emitDoubleModResult(NumberOperandId lhsId,
                                          NumberOperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  AutoAvailableFloatRegister floatScratch0(*this, FloatReg0);
  AutoAvailableFloatRegister floatScratch1(*this, FloatReg1);

  allocator.ensureDoubleRegister(masm, lhsId, floatScratch0);
  allocator.ensureDoubleRegister(masm, rhsId, floatScratch1);

  LiveRegisterSet save(GeneralRegisterSet::Volatile(), liveVolatileFloatRegs());
  masm.PushRegsInMask(save);

  using Fn = double (*)(double a, double b);
  masm.setupUnalignedABICall(scratch);
  masm.passABIArg(floatScratch0, MoveOp::DOUBLE);
  masm.passABIArg(floatScratch1, MoveOp::DOUBLE);
  masm.callWithABI<Fn, js::NumberMod>(MoveOp::DOUBLE);
  masm.storeCallFloatResult(floatScratch0);

  LiveRegisterSet ignore;
  ignore.add(floatScratch0);
  masm.PopRegsInMaskIgnore(save, ignore);

  masm.boxDouble(floatScratch0, output.valueReg(), floatScratch0);

  return true;
}
bool CacheIRCompiler::emitDoublePowResult(NumberOperandId lhsId,
                                          NumberOperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  AutoAvailableFloatRegister floatScratch0(*this, FloatReg0);
  AutoAvailableFloatRegister floatScratch1(*this, FloatReg1);

  allocator.ensureDoubleRegister(masm, lhsId, floatScratch0);
  allocator.ensureDoubleRegister(masm, rhsId, floatScratch1);

  LiveRegisterSet save(GeneralRegisterSet::Volatile(), liveVolatileFloatRegs());
  masm.PushRegsInMask(save);

  using Fn = double (*)(double x, double y);
  masm.setupUnalignedABICall(scratch);
  masm.passABIArg(floatScratch0, MoveOp::DOUBLE);
  masm.passABIArg(floatScratch1, MoveOp::DOUBLE);
  masm.callWithABI<Fn, js::ecmaPow>(MoveOp::DOUBLE);
  masm.storeCallFloatResult(floatScratch0);

  LiveRegisterSet ignore;
  ignore.add(floatScratch0);
  masm.PopRegsInMaskIgnore(save, ignore);

  masm.boxDouble(floatScratch0, output.valueReg(), floatScratch0);

  return true;
}

bool CacheIRCompiler::emitInt32AddResult(Int32OperandId lhsId,
                                         Int32OperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  Register lhs = allocator.useRegister(masm, lhsId);
  Register rhs = allocator.useRegister(masm, rhsId);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.mov(rhs, scratch);
  masm.branchAdd32(Assembler::Overflow, lhs, scratch, failure->label());
  masm.tagValue(JSVAL_TYPE_INT32, scratch, output.valueReg());

  return true;
}
bool CacheIRCompiler::emitInt32SubResult(Int32OperandId lhsId,
                                         Int32OperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);
  Register lhs = allocator.useRegister(masm, lhsId);
  Register rhs = allocator.useRegister(masm, rhsId);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.mov(lhs, scratch);
  masm.branchSub32(Assembler::Overflow, rhs, scratch, failure->label());
  masm.tagValue(JSVAL_TYPE_INT32, scratch, output.valueReg());

  return true;
}

bool CacheIRCompiler::emitInt32MulResult(Int32OperandId lhsId,
                                         Int32OperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register lhs = allocator.useRegister(masm, lhsId);
  Register rhs = allocator.useRegister(masm, rhsId);
  AutoScratchRegister scratch(allocator, masm);
  AutoScratchRegisterMaybeOutput scratch2(allocator, masm, output);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  Label maybeNegZero, done;
  masm.mov(lhs, scratch);
  masm.branchMul32(Assembler::Overflow, rhs, scratch, failure->label());
  masm.branchTest32(Assembler::Zero, scratch, scratch, &maybeNegZero);
  masm.jump(&done);

  masm.bind(&maybeNegZero);
  masm.mov(lhs, scratch2);
  // Result is -0 if exactly one of lhs or rhs is negative.
  masm.or32(rhs, scratch2);
  masm.branchTest32(Assembler::Signed, scratch2, scratch2, failure->label());

  masm.bind(&done);
  masm.tagValue(JSVAL_TYPE_INT32, scratch, output.valueReg());
  return true;
}

bool CacheIRCompiler::emitInt32DivResult(Int32OperandId lhsId,
                                         Int32OperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register lhs = allocator.useRegister(masm, lhsId);
  Register rhs = allocator.useRegister(masm, rhsId);
  AutoScratchRegister rem(allocator, masm);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  // Prevent division by 0.
  masm.branchTest32(Assembler::Zero, rhs, rhs, failure->label());

  // Prevent -2147483648 / -1.
  Label notOverflow;
  masm.branch32(Assembler::NotEqual, lhs, Imm32(INT32_MIN), &notOverflow);
  masm.branch32(Assembler::Equal, rhs, Imm32(-1), failure->label());
  masm.bind(&notOverflow);

  // Prevent negative 0.
  Label notZero;
  masm.branchTest32(Assembler::NonZero, lhs, lhs, &notZero);
  masm.branchTest32(Assembler::Signed, rhs, rhs, failure->label());
  masm.bind(&notZero);

  masm.mov(lhs, scratch);
  LiveRegisterSet volatileRegs(GeneralRegisterSet::Volatile(),
                               liveVolatileFloatRegs());
  masm.flexibleDivMod32(rhs, scratch, rem, false, volatileRegs);

  // A remainder implies a double result.
  masm.branchTest32(Assembler::NonZero, rem, rem, failure->label());
  masm.tagValue(JSVAL_TYPE_INT32, scratch, output.valueReg());
  return true;
}

bool CacheIRCompiler::emitInt32ModResult(Int32OperandId lhsId,
                                         Int32OperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register lhs = allocator.useRegister(masm, lhsId);
  Register rhs = allocator.useRegister(masm, rhsId);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  // x % 0 results in NaN
  masm.branchTest32(Assembler::Zero, rhs, rhs, failure->label());

  // Prevent -2147483648 % -1.
  //
  // Traps on x86 and has undefined behavior on ARM32 (when __aeabi_idivmod is
  // called).
  Label notOverflow;
  masm.branch32(Assembler::NotEqual, lhs, Imm32(INT32_MIN), &notOverflow);
  masm.branch32(Assembler::Equal, rhs, Imm32(-1), failure->label());
  masm.bind(&notOverflow);

  masm.mov(lhs, scratch);
  LiveRegisterSet volatileRegs(GeneralRegisterSet::Volatile(),
                               liveVolatileFloatRegs());
  masm.flexibleRemainder32(rhs, scratch, false, volatileRegs);

  // Modulo takes the sign of the dividend; we can't return negative zero here.
  Label notZero;
  masm.branchTest32(Assembler::NonZero, scratch, scratch, &notZero);
  masm.branchTest32(Assembler::Signed, lhs, lhs, failure->label());
  masm.bind(&notZero);

  masm.tagValue(JSVAL_TYPE_INT32, scratch, output.valueReg());

  return true;
}

bool CacheIRCompiler::emitInt32PowResult(Int32OperandId lhsId,
                                         Int32OperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register base = allocator.useRegister(masm, lhsId);
  Register power = allocator.useRegister(masm, rhsId);
  AutoScratchRegisterMaybeOutput scratch1(allocator, masm, output);
  AutoScratchRegisterMaybeOutputType scratch2(allocator, masm, output);
  AutoScratchRegister scratch3(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.pow32(base, power, scratch1, scratch2, scratch3, failure->label());

  masm.tagValue(JSVAL_TYPE_INT32, scratch1, output.valueReg());
  return true;
}

bool CacheIRCompiler::emitInt32BitOrResult(Int32OperandId lhsId,
                                           Int32OperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  Register lhs = allocator.useRegister(masm, lhsId);
  Register rhs = allocator.useRegister(masm, rhsId);

  masm.mov(rhs, scratch);
  masm.or32(lhs, scratch);
  masm.tagValue(JSVAL_TYPE_INT32, scratch, output.valueReg());

  return true;
}
bool CacheIRCompiler::emitInt32BitXorResult(Int32OperandId lhsId,
                                            Int32OperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  Register lhs = allocator.useRegister(masm, lhsId);
  Register rhs = allocator.useRegister(masm, rhsId);

  masm.mov(rhs, scratch);
  masm.xor32(lhs, scratch);
  masm.tagValue(JSVAL_TYPE_INT32, scratch, output.valueReg());

  return true;
}
bool CacheIRCompiler::emitInt32BitAndResult(Int32OperandId lhsId,
                                            Int32OperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  Register lhs = allocator.useRegister(masm, lhsId);
  Register rhs = allocator.useRegister(masm, rhsId);

  masm.mov(rhs, scratch);
  masm.and32(lhs, scratch);
  masm.tagValue(JSVAL_TYPE_INT32, scratch, output.valueReg());

  return true;
}
bool CacheIRCompiler::emitInt32LeftShiftResult(Int32OperandId lhsId,
                                               Int32OperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register lhs = allocator.useRegister(masm, lhsId);
  Register rhs = allocator.useRegister(masm, rhsId);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  masm.mov(lhs, scratch);
  masm.flexibleLshift32(rhs, scratch);
  masm.tagValue(JSVAL_TYPE_INT32, scratch, output.valueReg());

  return true;
}

bool CacheIRCompiler::emitInt32RightShiftResult(Int32OperandId lhsId,
                                                Int32OperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register lhs = allocator.useRegister(masm, lhsId);
  Register rhs = allocator.useRegister(masm, rhsId);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  masm.mov(lhs, scratch);
  masm.flexibleRshift32Arithmetic(rhs, scratch);
  masm.tagValue(JSVAL_TYPE_INT32, scratch, output.valueReg());

  return true;
}

bool CacheIRCompiler::emitInt32URightShiftResult(Int32OperandId lhsId,
                                                 Int32OperandId rhsId,
                                                 bool forceDouble) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);

  Register lhs = allocator.useRegister(masm, lhsId);
  Register rhs = allocator.useRegister(masm, rhsId);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.mov(lhs, scratch);
  masm.flexibleRshift32(rhs, scratch);
  if (forceDouble) {
    ScratchDoubleScope fpscratch(masm);
    masm.convertUInt32ToDouble(scratch, fpscratch);
    masm.boxDouble(fpscratch, output.valueReg(), fpscratch);
  } else {
    masm.branchTest32(Assembler::Signed, scratch, scratch, failure->label());
    masm.tagValue(JSVAL_TYPE_INT32, scratch, output.valueReg());
  }
  return true;
}

bool CacheIRCompiler::emitInt32NegationResult(Int32OperandId inputId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register val = allocator.useRegister(masm, inputId);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  // Guard against 0 and MIN_INT by checking if low 31-bits are all zero.
  // Both of these result in a double.
  masm.branchTest32(Assembler::Zero, val, Imm32(0x7fffffff), failure->label());
  masm.mov(val, scratch);
  masm.neg32(scratch);
  masm.tagValue(JSVAL_TYPE_INT32, scratch, output.valueReg());
  return true;
}

bool CacheIRCompiler::emitInt32IncResult(Int32OperandId inputId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register input = allocator.useRegister(masm, inputId);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.mov(input, scratch);
  masm.branchAdd32(Assembler::Overflow, Imm32(1), scratch, failure->label());
  masm.tagValue(JSVAL_TYPE_INT32, scratch, output.valueReg());

  return true;
}

bool CacheIRCompiler::emitInt32DecResult(Int32OperandId inputId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register input = allocator.useRegister(masm, inputId);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.mov(input, scratch);
  masm.branchSub32(Assembler::Overflow, Imm32(1), scratch, failure->label());
  masm.tagValue(JSVAL_TYPE_INT32, scratch, output.valueReg());

  return true;
}

bool CacheIRCompiler::emitInt32NotResult(Int32OperandId inputId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register val = allocator.useRegister(masm, inputId);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  masm.mov(val, scratch);
  masm.not32(scratch);
  masm.tagValue(JSVAL_TYPE_INT32, scratch, output.valueReg());
  return true;
}

bool CacheIRCompiler::emitDoubleNegationResult(NumberOperandId inputId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  ValueOperand val = allocator.useValueRegister(masm, inputId);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  AutoScratchFloatRegister floatReg(this, failure);

  masm.ensureDouble(val, floatReg, floatReg.failure());
  masm.negateDouble(floatReg);
  masm.boxDouble(floatReg, output.valueReg(), floatReg);

  return true;
}

bool CacheIRCompiler::emitDoubleIncDecResult(bool isInc,
                                             NumberOperandId inputId) {
  AutoOutputRegister output(*this);
  ValueOperand val = allocator.useValueRegister(masm, inputId);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  AutoScratchFloatRegister floatReg(this, failure);

  masm.ensureDouble(val, floatReg, floatReg.failure());
  {
    ScratchDoubleScope fpscratch(masm);
    masm.loadConstantDouble(1.0, fpscratch);
    if (isInc) {
      masm.addDouble(fpscratch, floatReg);
    } else {
      masm.subDouble(fpscratch, floatReg);
    }
  }
  masm.boxDouble(floatReg, output.valueReg(), floatReg);

  return true;
}

bool CacheIRCompiler::emitDoubleIncResult(NumberOperandId inputId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  return emitDoubleIncDecResult(true, inputId);
}

bool CacheIRCompiler::emitDoubleDecResult(NumberOperandId inputId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  return emitDoubleIncDecResult(false, inputId);
}

template <typename Fn, Fn fn>
bool CacheIRCompiler::emitBigIntBinaryOperationShared(BigIntOperandId lhsId,
                                                      BigIntOperandId rhsId) {
  AutoCallVM callvm(masm, this, allocator);
  Register lhs = allocator.useRegister(masm, lhsId);
  Register rhs = allocator.useRegister(masm, rhsId);

  callvm.prepare();

  masm.Push(rhs);
  masm.Push(lhs);

  callvm.call<Fn, fn>();
  return true;
}

bool CacheIRCompiler::emitBigIntAddResult(BigIntOperandId lhsId,
                                          BigIntOperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  using Fn = BigInt* (*)(JSContext*, HandleBigInt, HandleBigInt);
  return emitBigIntBinaryOperationShared<Fn, BigInt::add>(lhsId, rhsId);
}

bool CacheIRCompiler::emitBigIntSubResult(BigIntOperandId lhsId,
                                          BigIntOperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  using Fn = BigInt* (*)(JSContext*, HandleBigInt, HandleBigInt);
  return emitBigIntBinaryOperationShared<Fn, BigInt::sub>(lhsId, rhsId);
}

bool CacheIRCompiler::emitBigIntMulResult(BigIntOperandId lhsId,
                                          BigIntOperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  using Fn = BigInt* (*)(JSContext*, HandleBigInt, HandleBigInt);
  return emitBigIntBinaryOperationShared<Fn, BigInt::mul>(lhsId, rhsId);
}

bool CacheIRCompiler::emitBigIntDivResult(BigIntOperandId lhsId,
                                          BigIntOperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  using Fn = BigInt* (*)(JSContext*, HandleBigInt, HandleBigInt);
  return emitBigIntBinaryOperationShared<Fn, BigInt::div>(lhsId, rhsId);
}

bool CacheIRCompiler::emitBigIntModResult(BigIntOperandId lhsId,
                                          BigIntOperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  using Fn = BigInt* (*)(JSContext*, HandleBigInt, HandleBigInt);
  return emitBigIntBinaryOperationShared<Fn, BigInt::mod>(lhsId, rhsId);
}

bool CacheIRCompiler::emitBigIntPowResult(BigIntOperandId lhsId,
                                          BigIntOperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  using Fn = BigInt* (*)(JSContext*, HandleBigInt, HandleBigInt);
  return emitBigIntBinaryOperationShared<Fn, BigInt::pow>(lhsId, rhsId);
}

bool CacheIRCompiler::emitBigIntBitAndResult(BigIntOperandId lhsId,
                                             BigIntOperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  using Fn = BigInt* (*)(JSContext*, HandleBigInt, HandleBigInt);
  return emitBigIntBinaryOperationShared<Fn, BigInt::bitAnd>(lhsId, rhsId);
}

bool CacheIRCompiler::emitBigIntBitOrResult(BigIntOperandId lhsId,
                                            BigIntOperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  using Fn = BigInt* (*)(JSContext*, HandleBigInt, HandleBigInt);
  return emitBigIntBinaryOperationShared<Fn, BigInt::bitOr>(lhsId, rhsId);
}

bool CacheIRCompiler::emitBigIntBitXorResult(BigIntOperandId lhsId,
                                             BigIntOperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  using Fn = BigInt* (*)(JSContext*, HandleBigInt, HandleBigInt);
  return emitBigIntBinaryOperationShared<Fn, BigInt::bitXor>(lhsId, rhsId);
}

bool CacheIRCompiler::emitBigIntLeftShiftResult(BigIntOperandId lhsId,
                                                BigIntOperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  using Fn = BigInt* (*)(JSContext*, HandleBigInt, HandleBigInt);
  return emitBigIntBinaryOperationShared<Fn, BigInt::lsh>(lhsId, rhsId);
}

bool CacheIRCompiler::emitBigIntRightShiftResult(BigIntOperandId lhsId,
                                                 BigIntOperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  using Fn = BigInt* (*)(JSContext*, HandleBigInt, HandleBigInt);
  return emitBigIntBinaryOperationShared<Fn, BigInt::rsh>(lhsId, rhsId);
}

template <typename Fn, Fn fn>
bool CacheIRCompiler::emitBigIntUnaryOperationShared(BigIntOperandId inputId) {
  AutoCallVM callvm(masm, this, allocator);
  Register val = allocator.useRegister(masm, inputId);

  callvm.prepare();

  masm.Push(val);

  callvm.call<Fn, fn>();
  return true;
}

bool CacheIRCompiler::emitBigIntNotResult(BigIntOperandId inputId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  using Fn = BigInt* (*)(JSContext*, HandleBigInt);
  return emitBigIntUnaryOperationShared<Fn, BigInt::bitNot>(inputId);
}

bool CacheIRCompiler::emitBigIntNegationResult(BigIntOperandId inputId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  using Fn = BigInt* (*)(JSContext*, HandleBigInt);
  return emitBigIntUnaryOperationShared<Fn, BigInt::neg>(inputId);
}

bool CacheIRCompiler::emitBigIntIncResult(BigIntOperandId inputId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  using Fn = BigInt* (*)(JSContext*, HandleBigInt);
  return emitBigIntUnaryOperationShared<Fn, BigInt::inc>(inputId);
}

bool CacheIRCompiler::emitBigIntDecResult(BigIntOperandId inputId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  using Fn = BigInt* (*)(JSContext*, HandleBigInt);
  return emitBigIntUnaryOperationShared<Fn, BigInt::dec>(inputId);
}

bool CacheIRCompiler::emitTruncateDoubleToUInt32(NumberOperandId inputId,
                                                 Int32OperandId resultId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register res = allocator.defineRegister(masm, resultId);

  AutoScratchFloatRegister floatReg(this);

  allocator.ensureDoubleRegister(masm, inputId, floatReg);

  Label done, truncateABICall;

  masm.branchTruncateDoubleMaybeModUint32(floatReg, res, &truncateABICall);
  masm.jump(&done);

  masm.bind(&truncateABICall);
  LiveRegisterSet save(GeneralRegisterSet::Volatile(), liveVolatileFloatRegs());
  save.takeUnchecked(floatReg);
  // Bug 1451976
  save.takeUnchecked(floatReg.get().asSingle());
  masm.PushRegsInMask(save);

  using Fn = int32_t (*)(double);
  masm.setupUnalignedABICall(res);
  masm.passABIArg(floatReg, MoveOp::DOUBLE);
  masm.callWithABI<Fn, JS::ToInt32>(MoveOp::GENERAL,
                                    CheckUnsafeCallWithABI::DontCheckOther);
  masm.storeCallInt32Result(res);

  LiveRegisterSet ignore;
  ignore.add(res);
  masm.PopRegsInMaskIgnore(save, ignore);

  masm.bind(&done);
  return true;
}

bool CacheIRCompiler::emitLoadArgumentsObjectLengthResult(ObjOperandId objId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register obj = allocator.useRegister(masm, objId);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.loadArgumentsObjectLength(obj, scratch, failure->label());

  masm.tagValue(JSVAL_TYPE_INT32, scratch, output.valueReg());
  return true;
}

bool CacheIRCompiler::emitLoadArrayBufferByteLengthInt32Result(
    ObjOperandId objId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register obj = allocator.useRegister(masm, objId);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.loadArrayBufferByteLengthIntPtr(obj, scratch);
  masm.guardNonNegativeIntPtrToInt32(scratch, failure->label());
  masm.tagValue(JSVAL_TYPE_INT32, scratch, output.valueReg());
  return true;
}

bool CacheIRCompiler::emitLoadArrayBufferByteLengthDoubleResult(
    ObjOperandId objId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register obj = allocator.useRegister(masm, objId);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  ScratchDoubleScope fpscratch(masm);
  masm.loadArrayBufferByteLengthIntPtr(obj, scratch);
  masm.convertIntPtrToDouble(scratch, fpscratch);
  masm.boxDouble(fpscratch, output.valueReg(), fpscratch);
  return true;
}

bool CacheIRCompiler::emitLoadArrayBufferViewLengthInt32Result(
    ObjOperandId objId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register obj = allocator.useRegister(masm, objId);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.loadArrayBufferViewLengthIntPtr(obj, scratch);
  masm.guardNonNegativeIntPtrToInt32(scratch, failure->label());
  masm.tagValue(JSVAL_TYPE_INT32, scratch, output.valueReg());
  return true;
}

bool CacheIRCompiler::emitLoadArrayBufferViewLengthDoubleResult(
    ObjOperandId objId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register obj = allocator.useRegister(masm, objId);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  ScratchDoubleScope fpscratch(masm);
  masm.loadArrayBufferViewLengthIntPtr(obj, scratch);
  masm.convertIntPtrToDouble(scratch, fpscratch);
  masm.boxDouble(fpscratch, output.valueReg(), fpscratch);
  return true;
}

bool CacheIRCompiler::emitLoadFunctionLengthResult(ObjOperandId objId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register obj = allocator.useRegister(masm, objId);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  // Get the JSFunction flags.
  masm.load16ZeroExtend(Address(obj, JSFunction::offsetOfFlags()), scratch);

  // Functions with a SelfHostedLazyScript must be compiled with the slow-path
  // before the function length is known. If the length was previously resolved,
  // the length property may be shadowed.
  masm.branchTest32(
      Assembler::NonZero, scratch,
      Imm32(FunctionFlags::SELFHOSTLAZY | FunctionFlags::RESOLVED_LENGTH),
      failure->label());

  masm.loadFunctionLength(obj, scratch, scratch, failure->label());
  masm.tagValue(JSVAL_TYPE_INT32, scratch, output.valueReg());
  return true;
}

bool CacheIRCompiler::emitLoadFunctionNameResult(ObjOperandId objId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register obj = allocator.useRegister(masm, objId);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.loadFunctionName(obj, scratch, ImmGCPtr(cx_->names().empty),
                        failure->label());

  masm.tagValue(JSVAL_TYPE_STRING, scratch, output.valueReg());
  return true;
}

bool CacheIRCompiler::emitLoadStringLengthResult(StringOperandId strId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register str = allocator.useRegister(masm, strId);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  masm.loadStringLength(str, scratch);
  masm.tagValue(JSVAL_TYPE_INT32, scratch, output.valueReg());
  return true;
}

bool CacheIRCompiler::emitLoadStringCharCodeResult(StringOperandId strId,
                                                   Int32OperandId indexId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register str = allocator.useRegister(masm, strId);
  Register index = allocator.useRegister(masm, indexId);
  AutoScratchRegisterMaybeOutput scratch1(allocator, masm, output);
  AutoScratchRegister scratch2(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  // Bounds check, load string char.
  masm.spectreBoundsCheck32(index, Address(str, JSString::offsetOfLength()),
                            scratch1, failure->label());
  masm.loadStringChar(str, index, scratch1, scratch2, failure->label());

  masm.tagValue(JSVAL_TYPE_INT32, scratch1, output.valueReg());
  return true;
}

bool CacheIRCompiler::emitNewStringObjectResult(uint32_t templateObjectOffset,
                                                StringOperandId strId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoCallVM callvm(masm, this, allocator);

  Register str = allocator.useRegister(masm, strId);

  callvm.prepare();
  masm.Push(str);

  using Fn = JSObject* (*)(JSContext*, HandleString);
  callvm.call<Fn, NewStringObject>();
  return true;
}

bool CacheIRCompiler::emitStringToLowerCaseResult(StringOperandId strId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoCallVM callvm(masm, this, allocator);

  Register str = allocator.useRegister(masm, strId);

  callvm.prepare();
  masm.Push(str);

  using Fn = JSString* (*)(JSContext*, HandleString);
  callvm.call<Fn, js::StringToLowerCase>();
  return true;
}

bool CacheIRCompiler::emitStringToUpperCaseResult(StringOperandId strId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoCallVM callvm(masm, this, allocator);

  Register str = allocator.useRegister(masm, strId);

  callvm.prepare();
  masm.Push(str);

  using Fn = JSString* (*)(JSContext*, HandleString);
  callvm.call<Fn, js::StringToUpperCase>();
  return true;
}

bool CacheIRCompiler::emitLoadArgumentsObjectArgResult(ObjOperandId objId,
                                                       Int32OperandId indexId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register obj = allocator.useRegister(masm, objId);
  Register index = allocator.useRegister(masm, indexId);
  AutoScratchRegister scratch(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.loadArgumentsObjectElement(obj, index, output.valueReg(), scratch,
                                  failure->label());
  return true;
}

bool CacheIRCompiler::emitLoadDenseElementResult(ObjOperandId objId,
                                                 Int32OperandId indexId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register obj = allocator.useRegister(masm, objId);
  Register index = allocator.useRegister(masm, indexId);
  AutoScratchRegister scratch1(allocator, masm);
  AutoScratchRegisterMaybeOutput scratch2(allocator, masm, output);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  // Load obj->elements.
  masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), scratch1);

  // Bounds check.
  Address initLength(scratch1, ObjectElements::offsetOfInitializedLength());
  masm.spectreBoundsCheck32(index, initLength, scratch2, failure->label());

  // Hole check.
  BaseObjectElementIndex element(scratch1, index);
  masm.branchTestMagic(Assembler::Equal, element, failure->label());
  masm.loadTypedOrValue(element, output);
  return true;
}

bool CacheIRCompiler::emitGuardInt32IsNonNegative(Int32OperandId indexId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register index = allocator.useRegister(masm, indexId);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.branch32(Assembler::LessThan, index, Imm32(0), failure->label());
  return true;
}

bool CacheIRCompiler::emitGuardIndexGreaterThanDenseInitLength(
    ObjOperandId objId, Int32OperandId indexId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register obj = allocator.useRegister(masm, objId);
  Register index = allocator.useRegister(masm, indexId);
  AutoScratchRegister scratch(allocator, masm);
  AutoSpectreBoundsScratchRegister spectreScratch(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  // Load obj->elements.
  masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), scratch);

  // Ensure index >= initLength.
  Label outOfBounds;
  Address capacity(scratch, ObjectElements::offsetOfInitializedLength());
  masm.spectreBoundsCheck32(index, capacity, spectreScratch, &outOfBounds);
  masm.jump(failure->label());
  masm.bind(&outOfBounds);

  return true;
}

bool CacheIRCompiler::emitGuardIndexIsValidUpdateOrAdd(ObjOperandId objId,
                                                       Int32OperandId indexId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register obj = allocator.useRegister(masm, objId);
  Register index = allocator.useRegister(masm, indexId);
  AutoScratchRegister scratch(allocator, masm);
  AutoSpectreBoundsScratchRegister spectreScratch(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  // Load obj->elements.
  masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), scratch);

  Label success;

  // If length is writable, branch to &success.  All indices are writable.
  Address flags(scratch, ObjectElements::offsetOfFlags());
  masm.branchTest32(Assembler::Zero, flags,
                    Imm32(ObjectElements::Flags::NONWRITABLE_ARRAY_LENGTH),
                    &success);

  // Otherwise, ensure index is in bounds.
  Address length(scratch, ObjectElements::offsetOfLength());
  masm.spectreBoundsCheck32(index, length, spectreScratch,
                            /* failure = */ failure->label());
  masm.bind(&success);
  return true;
}

bool CacheIRCompiler::emitGuardTagNotEqual(ValueTagOperandId lhsId,
                                           ValueTagOperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register lhs = allocator.useRegister(masm, lhsId);
  Register rhs = allocator.useRegister(masm, rhsId);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  Label done;
  masm.branch32(Assembler::Equal, lhs, rhs, failure->label());

  // If both lhs and rhs are numbers, can't use tag comparison to do inequality
  // comparison
  masm.branchTestNumber(Assembler::NotEqual, lhs, &done);
  masm.branchTestNumber(Assembler::NotEqual, rhs, &done);
  masm.jump(failure->label());

  masm.bind(&done);
  return true;
}

bool CacheIRCompiler::emitGuardXrayExpandoShapeAndDefaultProto(
    ObjOperandId objId, uint32_t shapeWrapperOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  Register obj = allocator.useRegister(masm, objId);
  StubFieldOffset shapeWrapper(shapeWrapperOffset, StubField::Type::JSObject);

  AutoScratchRegister scratch(allocator, masm);
  AutoScratchRegister scratch2(allocator, masm);
  AutoScratchRegister scratch3(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.loadPtr(Address(obj, ProxyObject::offsetOfReservedSlots()), scratch);
  Address holderAddress(scratch,
                        sizeof(Value) * GetXrayJitInfo()->xrayHolderSlot);
  Address expandoAddress(scratch, NativeObject::getFixedSlotOffset(
                                      GetXrayJitInfo()->holderExpandoSlot));

  masm.fallibleUnboxObject(holderAddress, scratch, failure->label());
  masm.fallibleUnboxObject(expandoAddress, scratch, failure->label());

  // Unwrap the expando before checking its shape.
  masm.loadPtr(Address(scratch, ProxyObject::offsetOfReservedSlots()), scratch);
  masm.unboxObject(
      Address(scratch, js::detail::ProxyReservedSlots::offsetOfPrivateSlot()),
      scratch);

  emitLoadStubField(shapeWrapper, scratch2);
  LoadShapeWrapperContents(masm, scratch2, scratch2, failure->label());
  masm.branchTestObjShape(Assembler::NotEqual, scratch, scratch2, scratch3,
                          scratch, failure->label());

  // The reserved slots on the expando should all be in fixed slots.
  Address protoAddress(scratch, NativeObject::getFixedSlotOffset(
                                    GetXrayJitInfo()->expandoProtoSlot));
  masm.branchTestUndefined(Assembler::NotEqual, protoAddress, failure->label());

  return true;
}

bool CacheIRCompiler::emitGuardXrayNoExpando(ObjOperandId objId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  Register obj = allocator.useRegister(masm, objId);
  AutoScratchRegister scratch(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.loadPtr(Address(obj, ProxyObject::offsetOfReservedSlots()), scratch);
  Address holderAddress(scratch,
                        sizeof(Value) * GetXrayJitInfo()->xrayHolderSlot);
  Address expandoAddress(scratch, NativeObject::getFixedSlotOffset(
                                      GetXrayJitInfo()->holderExpandoSlot));

  Label done;
  masm.fallibleUnboxObject(holderAddress, scratch, &done);
  masm.branchTestObject(Assembler::Equal, expandoAddress, failure->label());
  masm.bind(&done);

  return true;
}

bool CacheIRCompiler::emitGuardNoAllocationMetadataBuilder(
    uint32_t builderAddrOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoScratchRegister scratch(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  StubFieldOffset builderField(builderAddrOffset, StubField::Type::RawPointer);
  emitLoadStubField(builderField, scratch);
  masm.branchPtr(Assembler::NotEqual, Address(scratch, 0), ImmWord(0),
                 failure->label());

  return true;
}

bool CacheIRCompiler::emitGuardFunctionHasJitEntry(ObjOperandId funId,
                                                   bool constructing) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register fun = allocator.useRegister(masm, funId);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.branchIfFunctionHasNoJitEntry(fun, constructing, failure->label());
  return true;
}

bool CacheIRCompiler::emitGuardFunctionHasNoJitEntry(ObjOperandId funId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register obj = allocator.useRegister(masm, funId);
  AutoScratchRegister scratch(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.branchIfFunctionHasJitEntry(obj, /*isConstructing =*/false,
                                   failure->label());
  return true;
}

bool CacheIRCompiler::emitGuardFunctionIsNonBuiltinCtor(ObjOperandId funId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  Register fun = allocator.useRegister(masm, funId);
  AutoScratchRegister scratch(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.branchIfNotFunctionIsNonBuiltinCtor(fun, scratch, failure->label());
  return true;
}

bool CacheIRCompiler::emitGuardFunctionIsConstructor(ObjOperandId funId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register funcReg = allocator.useRegister(masm, funId);
  AutoScratchRegister scratch(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  // Ensure obj is a constructor
  masm.branchTestFunctionFlags(funcReg, FunctionFlags::CONSTRUCTOR,
                               Assembler::Zero, failure->label());
  return true;
}

bool CacheIRCompiler::emitGuardNotClassConstructor(ObjOperandId funId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register fun = allocator.useRegister(masm, funId);
  AutoScratchRegister scratch(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.branchFunctionKind(Assembler::Equal, FunctionFlags::ClassConstructor,
                          fun, scratch, failure->label());
  return true;
}

bool CacheIRCompiler::emitGuardArrayIsPacked(ObjOperandId arrayId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register array = allocator.useRegister(masm, arrayId);
  AutoScratchRegister scratch(allocator, masm);
  AutoScratchRegister scratch2(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.branchArrayIsNotPacked(array, scratch, scratch2, failure->label());
  return true;
}

bool CacheIRCompiler::emitGuardArgumentsObjectFlags(ObjOperandId objId,
                                                    uint8_t flags) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register obj = allocator.useRegister(masm, objId);
  AutoScratchRegister scratch(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.branchTestArgumentsObjectFlags(obj, scratch, flags, Assembler::NonZero,
                                      failure->label());
  return true;
}

bool CacheIRCompiler::emitLoadDenseElementHoleResult(ObjOperandId objId,
                                                     Int32OperandId indexId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register obj = allocator.useRegister(masm, objId);
  Register index = allocator.useRegister(masm, indexId);
  AutoScratchRegister scratch1(allocator, masm);
  AutoScratchRegisterMaybeOutput scratch2(allocator, masm, output);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  // Make sure the index is nonnegative.
  masm.branch32(Assembler::LessThan, index, Imm32(0), failure->label());

  // Load obj->elements.
  masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), scratch1);

  // Guard on the initialized length.
  Label hole;
  Address initLength(scratch1, ObjectElements::offsetOfInitializedLength());
  masm.spectreBoundsCheck32(index, initLength, scratch2, &hole);

  // Load the value.
  Label done;
  masm.loadValue(BaseObjectElementIndex(scratch1, index), output.valueReg());
  masm.branchTestMagic(Assembler::NotEqual, output.valueReg(), &done);

  // Load undefined for the hole.
  masm.bind(&hole);
  masm.moveValue(UndefinedValue(), output.valueReg());

  masm.bind(&done);
  return true;
}

bool CacheIRCompiler::emitLoadTypedArrayElementExistsResult(
    ObjOperandId objId, IntPtrOperandId indexId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register obj = allocator.useRegister(masm, objId);
  Register index = allocator.useRegister(masm, indexId);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  Label outOfBounds, done;

  // Bounds check.
  masm.loadArrayBufferViewLengthIntPtr(obj, scratch);
  masm.branchPtr(Assembler::BelowOrEqual, scratch, index, &outOfBounds);
  EmitStoreBoolean(masm, true, output);
  masm.jump(&done);

  masm.bind(&outOfBounds);
  EmitStoreBoolean(masm, false, output);

  masm.bind(&done);
  return true;
}

bool CacheIRCompiler::emitLoadDenseElementExistsResult(ObjOperandId objId,
                                                       Int32OperandId indexId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register obj = allocator.useRegister(masm, objId);
  Register index = allocator.useRegister(masm, indexId);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  // Load obj->elements.
  masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), scratch);

  // Bounds check. Unsigned compare sends negative indices to next IC.
  Address initLength(scratch, ObjectElements::offsetOfInitializedLength());
  masm.branch32(Assembler::BelowOrEqual, initLength, index, failure->label());

  // Hole check.
  BaseObjectElementIndex element(scratch, index);
  masm.branchTestMagic(Assembler::Equal, element, failure->label());

  EmitStoreBoolean(masm, true, output);
  return true;
}

bool CacheIRCompiler::emitLoadDenseElementHoleExistsResult(
    ObjOperandId objId, Int32OperandId indexId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register obj = allocator.useRegister(masm, objId);
  Register index = allocator.useRegister(masm, indexId);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  // Make sure the index is nonnegative.
  masm.branch32(Assembler::LessThan, index, Imm32(0), failure->label());

  // Load obj->elements.
  masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), scratch);

  // Guard on the initialized length.
  Label hole;
  Address initLength(scratch, ObjectElements::offsetOfInitializedLength());
  masm.branch32(Assembler::BelowOrEqual, initLength, index, &hole);

  // Load value and replace with true.
  Label done;
  BaseObjectElementIndex element(scratch, index);
  masm.branchTestMagic(Assembler::Equal, element, &hole);
  EmitStoreBoolean(masm, true, output);
  masm.jump(&done);

  // Load false for the hole.
  masm.bind(&hole);
  EmitStoreBoolean(masm, false, output);

  masm.bind(&done);
  return true;
}

bool CacheIRCompiler::emitPackedArrayPopResult(ObjOperandId arrayId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  Register array = allocator.useRegister(masm, arrayId);
  AutoScratchRegister scratch1(allocator, masm);
  AutoScratchRegister scratch2(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.packedArrayPop(array, output.valueReg(), scratch1, scratch2,
                      failure->label());
  return true;
}

bool CacheIRCompiler::emitPackedArrayShiftResult(ObjOperandId arrayId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  Register array = allocator.useRegister(masm, arrayId);
  AutoScratchRegister scratch1(allocator, masm);
  AutoScratchRegister scratch2(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  LiveRegisterSet volatileRegs(GeneralRegisterSet::Volatile(),
                               liveVolatileFloatRegs());
  masm.packedArrayShift(array, output.valueReg(), scratch1, scratch2,
                        volatileRegs, failure->label());
  return true;
}

bool CacheIRCompiler::emitIsObjectResult(ValOperandId inputId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  ValueOperand val = allocator.useValueRegister(masm, inputId);

  masm.testObjectSet(Assembler::Equal, val, scratch);

  masm.tagValue(JSVAL_TYPE_BOOLEAN, scratch, output.valueReg());
  return true;
}

bool CacheIRCompiler::emitIsPackedArrayResult(ObjOperandId objId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  Register obj = allocator.useRegister(masm, objId);
  AutoScratchRegister scratch(allocator, masm);

  Register outputScratch = output.valueReg().scratchReg();
  masm.setIsPackedArray(obj, outputScratch, scratch);
  masm.tagValue(JSVAL_TYPE_BOOLEAN, outputScratch, output.valueReg());
  return true;
}

bool CacheIRCompiler::emitIsCallableResult(ValOperandId inputId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  AutoScratchRegister scratch1(allocator, masm);
  AutoScratchRegisterMaybeOutput scratch2(allocator, masm, output);

  ValueOperand val = allocator.useValueRegister(masm, inputId);

  Label isObject, done;
  masm.branchTestObject(Assembler::Equal, val, &isObject);
  // Primitives are never callable.
  masm.move32(Imm32(0), scratch2);
  masm.jump(&done);

  masm.bind(&isObject);
  masm.unboxObject(val, scratch1);

  Label isProxy;
  masm.isCallable(scratch1, scratch2, &isProxy);
  masm.jump(&done);

  masm.bind(&isProxy);
  {
    LiveRegisterSet volatileRegs(GeneralRegisterSet::Volatile(),
                                 liveVolatileFloatRegs());
    masm.PushRegsInMask(volatileRegs);

    using Fn = bool (*)(JSObject * obj);
    masm.setupUnalignedABICall(scratch2);
    masm.passABIArg(scratch1);
    masm.callWithABI<Fn, ObjectIsCallable>();
    masm.storeCallBoolResult(scratch2);

    LiveRegisterSet ignore;
    ignore.add(scratch2);
    masm.PopRegsInMaskIgnore(volatileRegs, ignore);
  }

  masm.bind(&done);
  masm.tagValue(JSVAL_TYPE_BOOLEAN, scratch2, output.valueReg());
  return true;
}

bool CacheIRCompiler::emitIsConstructorResult(ObjOperandId objId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  Register obj = allocator.useRegister(masm, objId);

  Label isProxy, done;
  masm.isConstructor(obj, scratch, &isProxy);
  masm.jump(&done);

  masm.bind(&isProxy);
  {
    LiveRegisterSet volatileRegs(GeneralRegisterSet::Volatile(),
                                 liveVolatileFloatRegs());
    masm.PushRegsInMask(volatileRegs);

    using Fn = bool (*)(JSObject * obj);
    masm.setupUnalignedABICall(scratch);
    masm.passABIArg(obj);
    masm.callWithABI<Fn, ObjectIsConstructor>();
    masm.storeCallBoolResult(scratch);

    LiveRegisterSet ignore;
    ignore.add(scratch);
    masm.PopRegsInMaskIgnore(volatileRegs, ignore);
  }

  masm.bind(&done);
  masm.tagValue(JSVAL_TYPE_BOOLEAN, scratch, output.valueReg());
  return true;
}

bool CacheIRCompiler::emitIsCrossRealmArrayConstructorResult(
    ObjOperandId objId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);
  Register obj = allocator.useRegister(masm, objId);

  masm.setIsCrossRealmArrayConstructor(obj, scratch);
  masm.tagValue(JSVAL_TYPE_BOOLEAN, scratch, output.valueReg());
  return true;
}

bool CacheIRCompiler::emitArrayBufferViewByteOffsetInt32Result(
    ObjOperandId objId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);
  Register obj = allocator.useRegister(masm, objId);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.loadArrayBufferViewByteOffsetIntPtr(obj, scratch);
  masm.guardNonNegativeIntPtrToInt32(scratch, failure->label());
  masm.tagValue(JSVAL_TYPE_INT32, scratch, output.valueReg());
  return true;
}

bool CacheIRCompiler::emitArrayBufferViewByteOffsetDoubleResult(
    ObjOperandId objId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  Register obj = allocator.useRegister(masm, objId);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  ScratchDoubleScope fpscratch(masm);
  masm.loadArrayBufferViewByteOffsetIntPtr(obj, scratch);
  masm.convertIntPtrToDouble(scratch, fpscratch);
  masm.boxDouble(fpscratch, output.valueReg(), fpscratch);
  return true;
}

bool CacheIRCompiler::emitTypedArrayByteLengthInt32Result(ObjOperandId objId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  AutoScratchRegisterMaybeOutput scratch1(allocator, masm, output);
  AutoScratchRegister scratch2(allocator, masm);
  Register obj = allocator.useRegister(masm, objId);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.loadArrayBufferViewLengthIntPtr(obj, scratch1);
  masm.guardNonNegativeIntPtrToInt32(scratch1, failure->label());
  masm.typedArrayElementSize(obj, scratch2);

  masm.branchMul32(Assembler::Overflow, scratch2.get(), scratch1,
                   failure->label());

  masm.tagValue(JSVAL_TYPE_INT32, scratch1, output.valueReg());
  return true;
}

bool CacheIRCompiler::emitTypedArrayByteLengthDoubleResult(ObjOperandId objId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  AutoScratchRegisterMaybeOutput scratch1(allocator, masm, output);
  AutoScratchRegister scratch2(allocator, masm);
  Register obj = allocator.useRegister(masm, objId);

  masm.loadArrayBufferViewLengthIntPtr(obj, scratch1);
  masm.typedArrayElementSize(obj, scratch2);
  masm.mulPtr(scratch2, scratch1);

  ScratchDoubleScope fpscratch(masm);
  masm.convertIntPtrToDouble(scratch1, fpscratch);
  masm.boxDouble(fpscratch, output.valueReg(), fpscratch);
  return true;
}

bool CacheIRCompiler::emitTypedArrayElementSizeResult(ObjOperandId objId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);
  Register obj = allocator.useRegister(masm, objId);

  masm.typedArrayElementSize(obj, scratch);
  masm.tagValue(JSVAL_TYPE_INT32, scratch, output.valueReg());
  return true;
}

bool CacheIRCompiler::emitGuardHasAttachedArrayBuffer(ObjOperandId objId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoScratchRegister scratch(allocator, masm);
  Register obj = allocator.useRegister(masm, objId);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.branchIfHasDetachedArrayBuffer(obj, scratch, failure->label());
  return true;
}

bool CacheIRCompiler::emitIsTypedArrayConstructorResult(ObjOperandId objId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);
  Register obj = allocator.useRegister(masm, objId);

  masm.setIsDefinitelyTypedArrayConstructor(obj, scratch);
  masm.tagValue(JSVAL_TYPE_BOOLEAN, scratch, output.valueReg());
  return true;
}

bool CacheIRCompiler::emitGetNextMapSetEntryForIteratorResult(
    ObjOperandId iterId, ObjOperandId resultArrId, bool isMap) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);
  Register iter = allocator.useRegister(masm, iterId);
  Register resultArr = allocator.useRegister(masm, resultArrId);

  LiveRegisterSet save(GeneralRegisterSet::Volatile(), liveVolatileFloatRegs());
  save.takeUnchecked(output.valueReg());
  save.takeUnchecked(scratch);
  masm.PushRegsInMask(save);

  masm.setupUnalignedABICall(scratch);
  masm.passABIArg(iter);
  masm.passABIArg(resultArr);
  if (isMap) {
    using Fn = bool (*)(MapIteratorObject * iter, ArrayObject * resultPairObj);
    masm.callWithABI<Fn, MapIteratorObject::next>();
  } else {
    using Fn = bool (*)(SetIteratorObject * iter, ArrayObject * resultObj);
    masm.callWithABI<Fn, SetIteratorObject::next>();
  }
  masm.storeCallBoolResult(scratch);

  masm.PopRegsInMask(save);

  masm.tagValue(JSVAL_TYPE_BOOLEAN, scratch, output.valueReg());
  return true;
}

bool CacheIRCompiler::emitFinishBoundFunctionInitResult(
    ObjOperandId boundId, ObjOperandId targetId, Int32OperandId argCountId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoCallVM callvm(masm, this, allocator);

  Register bound = allocator.useRegister(masm, boundId);
  Register target = allocator.useRegister(masm, targetId);
  Register argCount = allocator.useRegister(masm, argCountId);

  callvm.prepare();

  masm.Push(argCount);
  masm.Push(target);
  masm.Push(bound);

  using Fn = bool (*)(JSContext * cx, HandleFunction bound, HandleObject target,
                      int32_t argCount);
  callvm.callNoResult<Fn, JSFunction::finishBoundFunctionInit>();

  masm.moveValue(UndefinedValue(), callvm.outputValueReg());
  return true;
}

bool CacheIRCompiler::emitNewArrayIteratorResult(
    uint32_t templateObjectOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoCallVM callvm(masm, this, allocator);

  callvm.prepare();

  using Fn = ArrayIteratorObject* (*)(JSContext*);
  callvm.call<Fn, NewArrayIterator>();
  return true;
}

bool CacheIRCompiler::emitNewStringIteratorResult(
    uint32_t templateObjectOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoCallVM callvm(masm, this, allocator);

  callvm.prepare();

  using Fn = StringIteratorObject* (*)(JSContext*);
  callvm.call<Fn, NewStringIterator>();
  return true;
}

bool CacheIRCompiler::emitNewRegExpStringIteratorResult(
    uint32_t templateObjectOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoCallVM callvm(masm, this, allocator);

  callvm.prepare();

  using Fn = RegExpStringIteratorObject* (*)(JSContext*);
  callvm.call<Fn, NewRegExpStringIterator>();
  return true;
}

bool CacheIRCompiler::emitObjectCreateResult(uint32_t templateObjectOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoCallVM callvm(masm, this, allocator);
  AutoScratchRegister scratch(allocator, masm);

  StubFieldOffset objectField(templateObjectOffset, StubField::Type::JSObject);
  emitLoadStubField(objectField, scratch);

  callvm.prepare();
  masm.Push(scratch);

  using Fn = PlainObject* (*)(JSContext*, HandlePlainObject);
  callvm.call<Fn, ObjectCreateWithTemplate>();
  return true;
}

bool CacheIRCompiler::emitNewArrayFromLengthResult(
    uint32_t templateObjectOffset, Int32OperandId lengthId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoCallVM callvm(masm, this, allocator);
  AutoScratchRegister scratch(allocator, masm);
  Register length = allocator.useRegister(masm, lengthId);

  StubFieldOffset objectField(templateObjectOffset, StubField::Type::JSObject);
  emitLoadStubField(objectField, scratch);

  callvm.prepare();
  masm.Push(length);
  masm.Push(scratch);

  using Fn = ArrayObject* (*)(JSContext*, HandleArrayObject, int32_t length);
  callvm.call<Fn, ArrayConstructorOneArg>();
  return true;
}

bool CacheIRCompiler::emitNewTypedArrayFromLengthResult(
    uint32_t templateObjectOffset, Int32OperandId lengthId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoCallVM callvm(masm, this, allocator);
  AutoScratchRegister scratch(allocator, masm);
  Register length = allocator.useRegister(masm, lengthId);

  StubFieldOffset objectField(templateObjectOffset, StubField::Type::JSObject);
  emitLoadStubField(objectField, scratch);

  callvm.prepare();
  masm.Push(length);
  masm.Push(scratch);

  using Fn = TypedArrayObject* (*)(JSContext*, HandleObject, int32_t length);
  callvm.call<Fn, NewTypedArrayWithTemplateAndLength>();
  return true;
}

bool CacheIRCompiler::emitNewTypedArrayFromArrayBufferResult(
    uint32_t templateObjectOffset, ObjOperandId bufferId,
    ValOperandId byteOffsetId, ValOperandId lengthId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

#ifdef JS_CODEGEN_X86
  MOZ_CRASH("Instruction not supported on 32-bit x86, not enough registers");
#endif

  AutoCallVM callvm(masm, this, allocator);
  AutoScratchRegister scratch(allocator, masm);
  Register buffer = allocator.useRegister(masm, bufferId);
  ValueOperand byteOffset = allocator.useValueRegister(masm, byteOffsetId);
  ValueOperand length = allocator.useValueRegister(masm, lengthId);

  StubFieldOffset objectField(templateObjectOffset, StubField::Type::JSObject);
  emitLoadStubField(objectField, scratch);

  callvm.prepare();
  masm.Push(length);
  masm.Push(byteOffset);
  masm.Push(buffer);
  masm.Push(scratch);

  using Fn = TypedArrayObject* (*)(JSContext*, HandleObject, HandleObject,
                                   HandleValue, HandleValue);
  callvm.call<Fn, NewTypedArrayWithTemplateAndBuffer>();
  return true;
}

bool CacheIRCompiler::emitNewTypedArrayFromArrayResult(
    uint32_t templateObjectOffset, ObjOperandId arrayId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoCallVM callvm(masm, this, allocator);
  AutoScratchRegister scratch(allocator, masm);
  Register array = allocator.useRegister(masm, arrayId);

  StubFieldOffset objectField(templateObjectOffset, StubField::Type::JSObject);
  emitLoadStubField(objectField, scratch);

  callvm.prepare();
  masm.Push(array);
  masm.Push(scratch);

  using Fn = TypedArrayObject* (*)(JSContext*, HandleObject, HandleObject);
  callvm.call<Fn, NewTypedArrayWithTemplateAndArray>();
  return true;
}

bool CacheIRCompiler::emitMathAbsInt32Result(Int32OperandId inputId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  Register input = allocator.useRegister(masm, inputId);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.mov(input, scratch);
  // Don't negate already positive values.
  Label positive;
  masm.branchTest32(Assembler::NotSigned, scratch, scratch, &positive);
  // neg32 might overflow for INT_MIN.
  masm.branchNeg32(Assembler::Overflow, scratch, failure->label());
  masm.bind(&positive);

  masm.tagValue(JSVAL_TYPE_INT32, scratch, output.valueReg());
  return true;
}

bool CacheIRCompiler::emitMathAbsNumberResult(NumberOperandId inputId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  AutoAvailableFloatRegister scratch(*this, FloatReg0);

  allocator.ensureDoubleRegister(masm, inputId, scratch);

  masm.absDouble(scratch, scratch);
  masm.boxDouble(scratch, output.valueReg(), scratch);
  return true;
}

bool CacheIRCompiler::emitMathClz32Result(Int32OperandId inputId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);
  Register input = allocator.useRegister(masm, inputId);

  masm.clz32(input, scratch, /* knownNotZero = */ false);
  masm.tagValue(JSVAL_TYPE_INT32, scratch, output.valueReg());
  return true;
}

bool CacheIRCompiler::emitMathSignInt32Result(Int32OperandId inputId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);
  Register input = allocator.useRegister(masm, inputId);

  masm.signInt32(input, scratch);
  masm.tagValue(JSVAL_TYPE_INT32, scratch, output.valueReg());
  return true;
}

bool CacheIRCompiler::emitMathSignNumberResult(NumberOperandId inputId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  AutoAvailableFloatRegister floatScratch1(*this, FloatReg0);
  AutoAvailableFloatRegister floatScratch2(*this, FloatReg1);

  allocator.ensureDoubleRegister(masm, inputId, floatScratch1);

  masm.signDouble(floatScratch1, floatScratch2);
  masm.boxDouble(floatScratch2, output.valueReg(), floatScratch2);
  return true;
}

bool CacheIRCompiler::emitMathSignNumberToInt32Result(NumberOperandId inputId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);
  AutoAvailableFloatRegister floatScratch1(*this, FloatReg0);
  AutoAvailableFloatRegister floatScratch2(*this, FloatReg1);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  allocator.ensureDoubleRegister(masm, inputId, floatScratch1);

  masm.signDoubleToInt32(floatScratch1, scratch, floatScratch2,
                         failure->label());
  masm.tagValue(JSVAL_TYPE_INT32, scratch, output.valueReg());
  return true;
}

bool CacheIRCompiler::emitMathImulResult(Int32OperandId lhsId,
                                         Int32OperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);
  Register lhs = allocator.useRegister(masm, lhsId);
  Register rhs = allocator.useRegister(masm, rhsId);

  masm.mov(lhs, scratch);
  masm.mul32(rhs, scratch);
  masm.tagValue(JSVAL_TYPE_INT32, scratch, output.valueReg());
  return true;
}

bool CacheIRCompiler::emitMathSqrtNumberResult(NumberOperandId inputId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  AutoAvailableFloatRegister scratch(*this, FloatReg0);

  allocator.ensureDoubleRegister(masm, inputId, scratch);

  masm.sqrtDouble(scratch, scratch);
  masm.boxDouble(scratch, output.valueReg(), scratch);
  return true;
}

bool CacheIRCompiler::emitMathFloorNumberResult(NumberOperandId inputId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  AutoAvailableFloatRegister scratch(*this, FloatReg0);

  allocator.ensureDoubleRegister(masm, inputId, scratch);

  if (Assembler::HasRoundInstruction(RoundingMode::Down)) {
    masm.nearbyIntDouble(RoundingMode::Down, scratch, scratch);
    masm.boxDouble(scratch, output.valueReg(), scratch);
    return true;
  }

  return emitMathFunctionNumberResultShared(UnaryMathFunction::Floor, scratch,
                                            output.valueReg());
}

bool CacheIRCompiler::emitMathCeilNumberResult(NumberOperandId inputId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  AutoAvailableFloatRegister scratch(*this, FloatReg0);

  allocator.ensureDoubleRegister(masm, inputId, scratch);

  if (Assembler::HasRoundInstruction(RoundingMode::Up)) {
    masm.nearbyIntDouble(RoundingMode::Up, scratch, scratch);
    masm.boxDouble(scratch, output.valueReg(), scratch);
    return true;
  }

  return emitMathFunctionNumberResultShared(UnaryMathFunction::Ceil, scratch,
                                            output.valueReg());
}

bool CacheIRCompiler::emitMathTruncNumberResult(NumberOperandId inputId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  AutoAvailableFloatRegister scratch(*this, FloatReg0);

  allocator.ensureDoubleRegister(masm, inputId, scratch);

  if (Assembler::HasRoundInstruction(RoundingMode::TowardsZero)) {
    masm.nearbyIntDouble(RoundingMode::TowardsZero, scratch, scratch);
    masm.boxDouble(scratch, output.valueReg(), scratch);
    return true;
  }

  return emitMathFunctionNumberResultShared(UnaryMathFunction::Trunc, scratch,
                                            output.valueReg());
}

bool CacheIRCompiler::emitMathFRoundNumberResult(NumberOperandId inputId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  AutoAvailableFloatRegister scratch(*this, FloatReg0);
  FloatRegister scratchFloat32 = scratch.get().asSingle();

  allocator.ensureDoubleRegister(masm, inputId, scratch);

  masm.convertDoubleToFloat32(scratch, scratchFloat32);
  masm.convertFloat32ToDouble(scratchFloat32, scratch);

  masm.boxDouble(scratch, output.valueReg(), scratch);
  return true;
}

bool CacheIRCompiler::emitMathHypot2NumberResult(NumberOperandId first,
                                                 NumberOperandId second) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  AutoAvailableFloatRegister floatScratch0(*this, FloatReg0);
  AutoAvailableFloatRegister floatScratch1(*this, FloatReg1);

  allocator.ensureDoubleRegister(masm, first, floatScratch0);
  allocator.ensureDoubleRegister(masm, second, floatScratch1);

  LiveRegisterSet save(GeneralRegisterSet::Volatile(), liveVolatileFloatRegs());
  masm.PushRegsInMask(save);

  using Fn = double (*)(double x, double y);
  masm.setupUnalignedABICall(scratch);
  masm.passABIArg(floatScratch0, MoveOp::DOUBLE);
  masm.passABIArg(floatScratch1, MoveOp::DOUBLE);

  masm.callWithABI<Fn, ecmaHypot>(MoveOp::DOUBLE);
  masm.storeCallFloatResult(floatScratch0);

  LiveRegisterSet ignore;
  ignore.add(floatScratch0);
  masm.PopRegsInMaskIgnore(save, ignore);

  masm.boxDouble(floatScratch0, output.valueReg(), floatScratch0);
  return true;
}

bool CacheIRCompiler::emitMathHypot3NumberResult(NumberOperandId first,
                                                 NumberOperandId second,
                                                 NumberOperandId third) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  AutoAvailableFloatRegister floatScratch0(*this, FloatReg0);
  AutoAvailableFloatRegister floatScratch1(*this, FloatReg1);
  AutoAvailableFloatRegister floatScratch2(*this, FloatReg2);

  allocator.ensureDoubleRegister(masm, first, floatScratch0);
  allocator.ensureDoubleRegister(masm, second, floatScratch1);
  allocator.ensureDoubleRegister(masm, third, floatScratch2);

  LiveRegisterSet save(GeneralRegisterSet::Volatile(), liveVolatileFloatRegs());
  masm.PushRegsInMask(save);

  using Fn = double (*)(double x, double y, double z);
  masm.setupUnalignedABICall(scratch);
  masm.passABIArg(floatScratch0, MoveOp::DOUBLE);
  masm.passABIArg(floatScratch1, MoveOp::DOUBLE);
  masm.passABIArg(floatScratch2, MoveOp::DOUBLE);

  masm.callWithABI<Fn, hypot3>(MoveOp::DOUBLE);
  masm.storeCallFloatResult(floatScratch0);

  LiveRegisterSet ignore;
  ignore.add(floatScratch0);
  masm.PopRegsInMaskIgnore(save, ignore);

  masm.boxDouble(floatScratch0, output.valueReg(), floatScratch0);
  return true;
}

bool CacheIRCompiler::emitMathHypot4NumberResult(NumberOperandId first,
                                                 NumberOperandId second,
                                                 NumberOperandId third,
                                                 NumberOperandId fourth) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  AutoAvailableFloatRegister floatScratch0(*this, FloatReg0);
  AutoAvailableFloatRegister floatScratch1(*this, FloatReg1);
  AutoAvailableFloatRegister floatScratch2(*this, FloatReg2);
  AutoAvailableFloatRegister floatScratch3(*this, FloatReg3);

  allocator.ensureDoubleRegister(masm, first, floatScratch0);
  allocator.ensureDoubleRegister(masm, second, floatScratch1);
  allocator.ensureDoubleRegister(masm, third, floatScratch2);
  allocator.ensureDoubleRegister(masm, fourth, floatScratch3);

  LiveRegisterSet save(GeneralRegisterSet::Volatile(), liveVolatileFloatRegs());
  masm.PushRegsInMask(save);

  using Fn = double (*)(double x, double y, double z, double w);
  masm.setupUnalignedABICall(scratch);
  masm.passABIArg(floatScratch0, MoveOp::DOUBLE);
  masm.passABIArg(floatScratch1, MoveOp::DOUBLE);
  masm.passABIArg(floatScratch2, MoveOp::DOUBLE);
  masm.passABIArg(floatScratch3, MoveOp::DOUBLE);

  masm.callWithABI<Fn, hypot4>(MoveOp::DOUBLE);
  masm.storeCallFloatResult(floatScratch0);

  LiveRegisterSet ignore;
  ignore.add(floatScratch0);
  masm.PopRegsInMaskIgnore(save, ignore);

  masm.boxDouble(floatScratch0, output.valueReg(), floatScratch0);
  return true;
}

bool CacheIRCompiler::emitMathAtan2NumberResult(NumberOperandId yId,
                                                NumberOperandId xId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  AutoAvailableFloatRegister floatScratch0(*this, FloatReg0);
  AutoAvailableFloatRegister floatScratch1(*this, FloatReg1);

  allocator.ensureDoubleRegister(masm, yId, floatScratch0);
  allocator.ensureDoubleRegister(masm, xId, floatScratch1);

  LiveRegisterSet save(GeneralRegisterSet::Volatile(), liveVolatileFloatRegs());
  masm.PushRegsInMask(save);

  using Fn = double (*)(double x, double y);
  masm.setupUnalignedABICall(scratch);
  masm.passABIArg(floatScratch0, MoveOp::DOUBLE);
  masm.passABIArg(floatScratch1, MoveOp::DOUBLE);
  masm.callWithABI<Fn, js::ecmaAtan2>(MoveOp::DOUBLE);
  masm.storeCallFloatResult(floatScratch0);

  LiveRegisterSet ignore;
  ignore.add(floatScratch0);
  masm.PopRegsInMaskIgnore(save, ignore);

  masm.boxDouble(floatScratch0, output.valueReg(), floatScratch0);

  return true;
}

bool CacheIRCompiler::emitMathFloorToInt32Result(NumberOperandId inputId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  AutoAvailableFloatRegister scratchFloat(*this, FloatReg0);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  allocator.ensureDoubleRegister(masm, inputId, scratchFloat);

  masm.floorDoubleToInt32(scratchFloat, scratch, failure->label());

  masm.tagValue(JSVAL_TYPE_INT32, scratch, output.valueReg());
  return true;
}

bool CacheIRCompiler::emitMathCeilToInt32Result(NumberOperandId inputId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  AutoAvailableFloatRegister scratchFloat(*this, FloatReg0);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  allocator.ensureDoubleRegister(masm, inputId, scratchFloat);

  masm.ceilDoubleToInt32(scratchFloat, scratch, failure->label());

  masm.tagValue(JSVAL_TYPE_INT32, scratch, output.valueReg());
  return true;
}

bool CacheIRCompiler::emitMathTruncToInt32Result(NumberOperandId inputId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  AutoAvailableFloatRegister scratchFloat(*this, FloatReg0);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  allocator.ensureDoubleRegister(masm, inputId, scratchFloat);

  masm.truncDoubleToInt32(scratchFloat, scratch, failure->label());

  masm.tagValue(JSVAL_TYPE_INT32, scratch, output.valueReg());
  return true;
}

bool CacheIRCompiler::emitMathRoundToInt32Result(NumberOperandId inputId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  AutoAvailableFloatRegister scratchFloat0(*this, FloatReg0);
  AutoAvailableFloatRegister scratchFloat1(*this, FloatReg1);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  allocator.ensureDoubleRegister(masm, inputId, scratchFloat0);

  masm.roundDoubleToInt32(scratchFloat0, scratch, scratchFloat1,
                          failure->label());

  masm.tagValue(JSVAL_TYPE_INT32, scratch, output.valueReg());
  return true;
}

bool CacheIRCompiler::emitInt32MinMax(bool isMax, Int32OperandId firstId,
                                      Int32OperandId secondId,
                                      Int32OperandId resultId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  Register first = allocator.useRegister(masm, firstId);
  Register second = allocator.useRegister(masm, secondId);
  Register result = allocator.defineRegister(masm, resultId);

  Assembler::Condition cond =
      isMax ? Assembler::GreaterThan : Assembler::LessThan;
  masm.move32(first, result);
  masm.cmp32Move32(cond, second, first, second, result);
  return true;
}

bool CacheIRCompiler::emitNumberMinMax(bool isMax, NumberOperandId firstId,
                                       NumberOperandId secondId,
                                       NumberOperandId resultId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  ValueOperand output = allocator.defineValueRegister(masm, resultId);

  AutoAvailableFloatRegister scratch1(*this, FloatReg0);
  AutoAvailableFloatRegister scratch2(*this, FloatReg1);

  allocator.ensureDoubleRegister(masm, firstId, scratch1);
  allocator.ensureDoubleRegister(masm, secondId, scratch2);

  if (isMax) {
    masm.maxDouble(scratch2, scratch1, /* handleNaN = */ true);
  } else {
    masm.minDouble(scratch2, scratch1, /* handleNaN = */ true);
  }

  masm.boxDouble(scratch1, output, scratch1);
  return true;
}

bool CacheIRCompiler::emitInt32MinMaxArrayResult(ObjOperandId arrayId,
                                                 bool isMax) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  Register array = allocator.useRegister(masm, arrayId);

  AutoScratchRegister scratch(allocator, masm);
  AutoScratchRegister scratch2(allocator, masm);
  AutoScratchRegisterMaybeOutputType scratch3(allocator, masm, output);
  AutoScratchRegisterMaybeOutput result(allocator, masm, output);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.minMaxArrayInt32(array, result, scratch, scratch2, scratch3, isMax,
                        failure->label());
  masm.tagValue(JSVAL_TYPE_INT32, result, output.valueReg());
  return true;
}

bool CacheIRCompiler::emitNumberMinMaxArrayResult(ObjOperandId arrayId,
                                                  bool isMax) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  Register array = allocator.useRegister(masm, arrayId);

  AutoAvailableFloatRegister result(*this, FloatReg0);
  AutoAvailableFloatRegister floatScratch(*this, FloatReg1);

  AutoScratchRegister scratch1(allocator, masm);
  AutoScratchRegister scratch2(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.minMaxArrayNumber(array, result, floatScratch, scratch1, scratch2, isMax,
                         failure->label());
  masm.boxDouble(result, output.valueReg(), result);
  return true;
}

bool CacheIRCompiler::emitMathFunctionNumberResultShared(
    UnaryMathFunction fun, FloatRegister inputScratch, ValueOperand output) {
  UnaryMathFunctionType funPtr = GetUnaryMathFunctionPtr(fun);

  LiveRegisterSet save(GeneralRegisterSet::Volatile(), liveVolatileFloatRegs());
  save.takeUnchecked(inputScratch);
  masm.PushRegsInMask(save);

  masm.setupUnalignedABICall(output.scratchReg());
  masm.passABIArg(inputScratch, MoveOp::DOUBLE);
  masm.callWithABI(DynamicFunction<UnaryMathFunctionType>(funPtr),
                   MoveOp::DOUBLE);
  masm.storeCallFloatResult(inputScratch);

  masm.PopRegsInMask(save);

  masm.boxDouble(inputScratch, output, inputScratch);
  return true;
}

bool CacheIRCompiler::emitMathFunctionNumberResult(NumberOperandId inputId,
                                                   UnaryMathFunction fun) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  AutoAvailableFloatRegister scratch(*this, FloatReg0);

  allocator.ensureDoubleRegister(masm, inputId, scratch);

  return emitMathFunctionNumberResultShared(fun, scratch, output.valueReg());
}

static void EmitStoreDenseElement(MacroAssembler& masm,
                                  const ConstantOrRegister& value,
                                  BaseObjectElementIndex target) {
  if (value.constant()) {
    Value v = value.value();
    masm.storeValue(v, target);
    return;
  }

  TypedOrValueRegister reg = value.reg();
  masm.storeTypedOrValue(reg, target);
}

bool CacheIRCompiler::emitStoreDenseElement(ObjOperandId objId,
                                            Int32OperandId indexId,
                                            ValOperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  Register obj = allocator.useRegister(masm, objId);
  Register index = allocator.useRegister(masm, indexId);
  ConstantOrRegister val = allocator.useConstantOrRegister(masm, rhsId);

  AutoScratchRegister scratch(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  // Load obj->elements in scratch.
  masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), scratch);

  // Bounds check. Unfortunately we don't have more registers available on
  // x86, so use InvalidReg and emit slightly slower code on x86.
  Register spectreTemp = InvalidReg;
  Address initLength(scratch, ObjectElements::offsetOfInitializedLength());
  masm.spectreBoundsCheck32(index, initLength, spectreTemp, failure->label());

  // Hole check.
  BaseObjectElementIndex element(scratch, index);
  masm.branchTestMagic(Assembler::Equal, element, failure->label());

  // Perform the store.
  EmitPreBarrier(masm, element, MIRType::Value);
  EmitStoreDenseElement(masm, val, element);

  emitPostBarrierElement(obj, val, scratch, index);
  return true;
}

static void EmitAssertExtensibleElements(MacroAssembler& masm,
                                         Register elementsReg) {
#ifdef DEBUG
  // Preceding shape guards ensure the object elements are extensible.
  Address elementsFlags(elementsReg, ObjectElements::offsetOfFlags());
  Label ok;
  masm.branchTest32(Assembler::Zero, elementsFlags,
                    Imm32(ObjectElements::Flags::NOT_EXTENSIBLE), &ok);
  masm.assumeUnreachable("Unexpected non-extensible elements");
  masm.bind(&ok);
#endif
}

static void EmitAssertWritableArrayLengthElements(MacroAssembler& masm,
                                                  Register elementsReg) {
#ifdef DEBUG
  // Preceding shape guards ensure the array length is writable.
  Address elementsFlags(elementsReg, ObjectElements::offsetOfFlags());
  Label ok;
  masm.branchTest32(Assembler::Zero, elementsFlags,
                    Imm32(ObjectElements::Flags::NONWRITABLE_ARRAY_LENGTH),
                    &ok);
  masm.assumeUnreachable("Unexpected non-writable array length elements");
  masm.bind(&ok);
#endif
}

bool CacheIRCompiler::emitStoreDenseElementHole(ObjOperandId objId,
                                                Int32OperandId indexId,
                                                ValOperandId rhsId,
                                                bool handleAdd) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  Register obj = allocator.useRegister(masm, objId);
  Register index = allocator.useRegister(masm, indexId);
  ConstantOrRegister val = allocator.useConstantOrRegister(masm, rhsId);

  AutoScratchRegister scratch(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  // Load obj->elements in scratch.
  masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), scratch);

  EmitAssertExtensibleElements(masm, scratch);
  if (handleAdd) {
    EmitAssertWritableArrayLengthElements(masm, scratch);
  }

  BaseObjectElementIndex element(scratch, index);
  Address initLength(scratch, ObjectElements::offsetOfInitializedLength());
  Address elementsFlags(scratch, ObjectElements::offsetOfFlags());

  // We don't have enough registers on x86 so use InvalidReg. This will emit
  // slightly less efficient code on x86.
  Register spectreTemp = InvalidReg;

  Label storeSkipPreBarrier;
  if (handleAdd) {
    // Bounds check.
    Label inBounds, outOfBounds;
    masm.spectreBoundsCheck32(index, initLength, spectreTemp, &outOfBounds);
    masm.jump(&inBounds);

    // If we're out-of-bounds, only handle the index == initLength case.
    masm.bind(&outOfBounds);
    masm.branch32(Assembler::NotEqual, initLength, index, failure->label());

    // If index < capacity, we can add a dense element inline. If not we
    // need to allocate more elements.
    Label allocElement, addNewElement;
    Address capacity(scratch, ObjectElements::offsetOfCapacity());
    masm.spectreBoundsCheck32(index, capacity, spectreTemp, &allocElement);
    masm.jump(&addNewElement);

    masm.bind(&allocElement);

    LiveRegisterSet save(GeneralRegisterSet::Volatile(),
                         liveVolatileFloatRegs());
    save.takeUnchecked(scratch);
    masm.PushRegsInMask(save);

    using Fn = bool (*)(JSContext * cx, NativeObject * obj);
    masm.setupUnalignedABICall(scratch);
    masm.loadJSContext(scratch);
    masm.passABIArg(scratch);
    masm.passABIArg(obj);
    masm.callWithABI<Fn, NativeObject::addDenseElementPure>();
    masm.mov(ReturnReg, scratch);

    masm.PopRegsInMask(save);
    masm.branchIfFalseBool(scratch, failure->label());

    // Load the reallocated elements pointer.
    masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), scratch);

    masm.bind(&addNewElement);

    // Increment initLength.
    masm.add32(Imm32(1), initLength);

    // If length is now <= index, increment length too.
    Label skipIncrementLength;
    Address length(scratch, ObjectElements::offsetOfLength());
    masm.branch32(Assembler::Above, length, index, &skipIncrementLength);
    masm.add32(Imm32(1), length);
    masm.bind(&skipIncrementLength);

    // Skip EmitPreBarrier as the memory is uninitialized.
    masm.jump(&storeSkipPreBarrier);

    masm.bind(&inBounds);
  } else {
    // Fail if index >= initLength.
    masm.spectreBoundsCheck32(index, initLength, spectreTemp, failure->label());
  }

  EmitPreBarrier(masm, element, MIRType::Value);

  masm.bind(&storeSkipPreBarrier);
  EmitStoreDenseElement(masm, val, element);

  emitPostBarrierElement(obj, val, scratch, index);
  return true;
}

bool CacheIRCompiler::emitArrayPush(ObjOperandId objId, ValOperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  Register obj = allocator.useRegister(masm, objId);
  ValueOperand val = allocator.useValueRegister(masm, rhsId);

  AutoScratchRegisterMaybeOutput scratchLength(allocator, masm, output);
  AutoScratchRegisterMaybeOutputType scratch(allocator, masm, output);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  // Load obj->elements in scratch.
  masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), scratch);

  EmitAssertExtensibleElements(masm, scratch);
  EmitAssertWritableArrayLengthElements(masm, scratch);

  Address elementsInitLength(scratch,
                             ObjectElements::offsetOfInitializedLength());
  Address elementsLength(scratch, ObjectElements::offsetOfLength());
  Address elementsFlags(scratch, ObjectElements::offsetOfFlags());

  // Fail if length != initLength.
  masm.load32(elementsInitLength, scratchLength);
  masm.branch32(Assembler::NotEqual, elementsLength, scratchLength,
                failure->label());

  // If scratchLength < capacity, we can add a dense element inline. If not we
  // need to allocate more elements.
  Label allocElement, addNewElement;
  Address capacity(scratch, ObjectElements::offsetOfCapacity());
  masm.spectreBoundsCheck32(scratchLength, capacity, InvalidReg, &allocElement);
  masm.jump(&addNewElement);

  masm.bind(&allocElement);

  LiveRegisterSet save(GeneralRegisterSet::Volatile(), liveVolatileFloatRegs());
  save.takeUnchecked(scratch);
  masm.PushRegsInMask(save);

  using Fn = bool (*)(JSContext * cx, NativeObject * obj);
  masm.setupUnalignedABICall(scratch);
  masm.loadJSContext(scratch);
  masm.passABIArg(scratch);
  masm.passABIArg(obj);
  masm.callWithABI<Fn, NativeObject::addDenseElementPure>();
  masm.mov(ReturnReg, scratch);

  masm.PopRegsInMask(save);
  masm.branchIfFalseBool(scratch, failure->label());

  // Load the reallocated elements pointer.
  masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), scratch);

  masm.bind(&addNewElement);

  // Increment initLength and length.
  masm.add32(Imm32(1), elementsInitLength);
  masm.add32(Imm32(1), elementsLength);

  // Store the value.
  BaseObjectElementIndex element(scratch, scratchLength);
  masm.storeValue(val, element);
  emitPostBarrierElement(obj, val, scratch, scratchLength);

  // Return value is new length.
  masm.add32(Imm32(1), scratchLength);
  masm.tagValue(JSVAL_TYPE_INT32, scratchLength, output.valueReg());

  return true;
}

bool CacheIRCompiler::emitStoreTypedArrayElement(ObjOperandId objId,
                                                 Scalar::Type elementType,
                                                 IntPtrOperandId indexId,
                                                 uint32_t rhsId,
                                                 bool handleOOB) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register obj = allocator.useRegister(masm, objId);
  Register index = allocator.useRegister(masm, indexId);

  AutoAvailableFloatRegister floatScratch0(*this, FloatReg0);

  Maybe<Register> valInt32;
  Maybe<Register> valBigInt;
  switch (elementType) {
    case Scalar::Int8:
    case Scalar::Uint8:
    case Scalar::Int16:
    case Scalar::Uint16:
    case Scalar::Int32:
    case Scalar::Uint32:
    case Scalar::Uint8Clamped:
      valInt32.emplace(allocator.useRegister(masm, Int32OperandId(rhsId)));
      break;

    case Scalar::Float32:
    case Scalar::Float64:
      allocator.ensureDoubleRegister(masm, NumberOperandId(rhsId),
                                     floatScratch0);
      break;

    case Scalar::BigInt64:
    case Scalar::BigUint64:
      valBigInt.emplace(allocator.useRegister(masm, BigIntOperandId(rhsId)));
      break;

    case Scalar::MaxTypedArrayViewType:
    case Scalar::Int64:
    case Scalar::Simd128:
      MOZ_CRASH("Unsupported TypedArray type");
  }

  AutoScratchRegister scratch1(allocator, masm);
  Maybe<AutoScratchRegister> scratch2;
  Maybe<AutoSpectreBoundsScratchRegister> spectreScratch;
  if (Scalar::isBigIntType(elementType)) {
    scratch2.emplace(allocator, masm);
  } else {
    spectreScratch.emplace(allocator, masm);
  }

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  // Bounds check.
  Label done;
  Register spectreTemp = scratch2 ? scratch2->get() : spectreScratch->get();
  masm.loadArrayBufferViewLengthIntPtr(obj, scratch1);
  masm.spectreBoundsCheckPtr(index, scratch1, spectreTemp,
                             handleOOB ? &done : failure->label());

  // Load the elements vector.
  masm.loadPtr(Address(obj, ArrayBufferViewObject::dataOffset()), scratch1);

  BaseIndex dest(scratch1, index, ScaleFromScalarType(elementType));

  if (Scalar::isBigIntType(elementType)) {
#ifdef JS_PUNBOX64
    Register64 temp(scratch2->get());
#else
    // We don't have more registers available on x86, so spill |obj|.
    masm.push(obj);
    Register64 temp(scratch2->get(), obj);
#endif

    masm.loadBigInt64(*valBigInt, temp);
    masm.storeToTypedBigIntArray(elementType, temp, dest);

#ifndef JS_PUNBOX64
    masm.pop(obj);
#endif
  } else if (elementType == Scalar::Float32) {
    ScratchFloat32Scope fpscratch(masm);
    masm.convertDoubleToFloat32(floatScratch0, fpscratch);
    masm.storeToTypedFloatArray(elementType, fpscratch, dest);
  } else if (elementType == Scalar::Float64) {
    masm.storeToTypedFloatArray(elementType, floatScratch0, dest);
  } else {
    masm.storeToTypedIntArray(elementType, *valInt32, dest);
  }

  masm.bind(&done);
  return true;
}

static bool CanNurseryAllocateBigInt(JSContext* cx) {
  JS::Zone* zone = cx->zone();
  return zone->runtimeFromAnyThread()->gc.nursery().canAllocateBigInts() &&
         zone->allocNurseryBigInts;
}

static void EmitAllocateBigInt(MacroAssembler& masm, Register result,
                               Register temp, const LiveRegisterSet& liveSet,
                               Label* fail, bool attemptNursery) {
  Label fallback, done;
  masm.newGCBigInt(result, temp, &fallback, attemptNursery);
  masm.jump(&done);
  {
    masm.bind(&fallback);
    masm.PushRegsInMask(liveSet);

    using Fn = void* (*)(JSContext * cx, bool requestMinorGC);
    masm.setupUnalignedABICall(temp);
    masm.loadJSContext(temp);
    masm.passABIArg(temp);
    masm.move32(Imm32(attemptNursery), result);
    masm.passABIArg(result);
    masm.callWithABI<Fn, jit::AllocateBigIntNoGC>();
    masm.storeCallPointerResult(result);

    masm.PopRegsInMask(liveSet);
    masm.branchPtr(Assembler::Equal, result, ImmWord(0), fail);
  }
  masm.bind(&done);
}

bool CacheIRCompiler::emitLoadTypedArrayElementResult(
    ObjOperandId objId, IntPtrOperandId indexId, Scalar::Type elementType,
    bool handleOOB, bool forceDoubleForUint32) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register obj = allocator.useRegister(masm, objId);
  Register index = allocator.useRegister(masm, indexId);

  AutoScratchRegister scratch1(allocator, masm);
#ifdef JS_PUNBOX64
  AutoScratchRegister scratch2(allocator, masm);
#else
  // There are too few registers available on x86, so we may need to reuse the
  // output's scratch register.
  AutoScratchRegisterMaybeOutput scratch2(allocator, masm, output);
#endif

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  // Bounds check.
  Label outOfBounds;
  masm.loadArrayBufferViewLengthIntPtr(obj, scratch1);
  masm.spectreBoundsCheckPtr(index, scratch1, scratch2,
                             handleOOB ? &outOfBounds : failure->label());

  // Allocate BigInt if needed. The code after this should be infallible.
  Maybe<Register> bigInt;
  if (Scalar::isBigIntType(elementType)) {
    bigInt.emplace(output.valueReg().scratchReg());

    LiveRegisterSet save(GeneralRegisterSet::Volatile(),
                         liveVolatileFloatRegs());
    save.takeUnchecked(scratch1);
    save.takeUnchecked(scratch2);
    save.takeUnchecked(output);

    bool attemptNursery = CanNurseryAllocateBigInt(cx_);
    EmitAllocateBigInt(masm, *bigInt, scratch1, save, failure->label(),
                       attemptNursery);
  }

  // Load the elements vector.
  masm.loadPtr(Address(obj, ArrayBufferViewObject::dataOffset()), scratch1);

  // Load the value.
  BaseIndex source(scratch1, index, ScaleFromScalarType(elementType));

  if (Scalar::isBigIntType(elementType)) {
#ifdef JS_PUNBOX64
    Register64 temp(scratch2);
#else
    // We don't have more registers available on x86, so spill |obj| and
    // additionally use the output's type register.
    MOZ_ASSERT(output.valueReg().scratchReg() != output.valueReg().typeReg());
    masm.push(obj);
    Register64 temp(output.valueReg().typeReg(), obj);
#endif

    masm.loadFromTypedBigIntArray(elementType, source, *bigInt, temp);

#ifndef JS_PUNBOX64
    masm.pop(obj);
#endif

    masm.tagValue(JSVAL_TYPE_BIGINT, *bigInt, output.valueReg());
  } else {
    MacroAssembler::Uint32Mode uint32Mode =
        forceDoubleForUint32 ? MacroAssembler::Uint32Mode::ForceDouble
                             : MacroAssembler::Uint32Mode::FailOnDouble;
    masm.loadFromTypedArray(elementType, source, output.valueReg(), uint32Mode,
                            scratch1, failure->label());
  }

  if (handleOOB) {
    Label done;
    masm.jump(&done);

    masm.bind(&outOfBounds);
    masm.moveValue(UndefinedValue(), output.valueReg());

    masm.bind(&done);
  }

  return true;
}

static void EmitDataViewBoundsCheck(MacroAssembler& masm, size_t byteSize,
                                    Register obj, Register offset,
                                    Register scratch, Label* fail) {
  // Ensure both offset < length and offset + (byteSize - 1) < length.
  masm.loadArrayBufferViewLengthIntPtr(obj, scratch);
  if (byteSize == 1) {
    masm.spectreBoundsCheckPtr(offset, scratch, InvalidReg, fail);
  } else {
    // temp := length - (byteSize - 1)
    // if temp < 0: fail
    // if offset >= temp: fail
    masm.branchSubPtr(Assembler::Signed, Imm32(byteSize - 1), scratch, fail);
    masm.spectreBoundsCheckPtr(offset, scratch, InvalidReg, fail);
  }
}

bool CacheIRCompiler::emitLoadDataViewValueResult(
    ObjOperandId objId, IntPtrOperandId offsetId,
    BooleanOperandId littleEndianId, Scalar::Type elementType,
    bool forceDoubleForUint32) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  Register obj = allocator.useRegister(masm, objId);
  Register offset = allocator.useRegister(masm, offsetId);
  Register littleEndian = allocator.useRegister(masm, littleEndianId);

  AutoAvailableFloatRegister floatScratch0(*this, FloatReg0);

  Register64 outputReg64 = output.valueReg().toRegister64();
  Register outputScratch = outputReg64.scratchReg();

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  const size_t byteSize = Scalar::byteSize(elementType);

  EmitDataViewBoundsCheck(masm, byteSize, obj, offset, outputScratch,
                          failure->label());

  masm.loadPtr(Address(obj, DataViewObject::dataOffset()), outputScratch);

  // Load the value.
  BaseIndex source(outputScratch, offset, TimesOne);
  switch (elementType) {
    case Scalar::Int8:
      masm.load8SignExtend(source, outputScratch);
      break;
    case Scalar::Uint8:
      masm.load8ZeroExtend(source, outputScratch);
      break;
    case Scalar::Int16:
      masm.load16UnalignedSignExtend(source, outputScratch);
      break;
    case Scalar::Uint16:
      masm.load16UnalignedZeroExtend(source, outputScratch);
      break;
    case Scalar::Int32:
    case Scalar::Uint32:
    case Scalar::Float32:
      masm.load32Unaligned(source, outputScratch);
      break;
    case Scalar::Float64:
    case Scalar::BigInt64:
    case Scalar::BigUint64:
      masm.load64Unaligned(source, outputReg64);
      break;
    case Scalar::Uint8Clamped:
    default:
      MOZ_CRASH("Invalid typed array type");
  }

  // Swap the bytes in the loaded value.
  if (byteSize > 1) {
    Label skip;
    masm.branch32(MOZ_LITTLE_ENDIAN() ? Assembler::NotEqual : Assembler::Equal,
                  littleEndian, Imm32(0), &skip);

    switch (elementType) {
      case Scalar::Int16:
        masm.byteSwap16SignExtend(outputScratch);
        break;
      case Scalar::Uint16:
        masm.byteSwap16ZeroExtend(outputScratch);
        break;
      case Scalar::Int32:
      case Scalar::Uint32:
      case Scalar::Float32:
        masm.byteSwap32(outputScratch);
        break;
      case Scalar::Float64:
      case Scalar::BigInt64:
      case Scalar::BigUint64:
        masm.byteSwap64(outputReg64);
        break;
      case Scalar::Int8:
      case Scalar::Uint8:
      case Scalar::Uint8Clamped:
      default:
        MOZ_CRASH("Invalid type");
    }

    masm.bind(&skip);
  }

  // Move the value into the output register.
  switch (elementType) {
    case Scalar::Int8:
    case Scalar::Uint8:
    case Scalar::Int16:
    case Scalar::Uint16:
    case Scalar::Int32:
      masm.tagValue(JSVAL_TYPE_INT32, outputScratch, output.valueReg());
      break;
    case Scalar::Uint32: {
      MacroAssembler::Uint32Mode uint32Mode =
          forceDoubleForUint32 ? MacroAssembler::Uint32Mode::ForceDouble
                               : MacroAssembler::Uint32Mode::FailOnDouble;
      masm.boxUint32(outputScratch, output.valueReg(), uint32Mode,
                     failure->label());
      break;
    }
    case Scalar::Float32: {
      FloatRegister scratchFloat32 = floatScratch0.get().asSingle();
      masm.moveGPRToFloat32(outputScratch, scratchFloat32);
      masm.canonicalizeFloat(scratchFloat32);
      masm.convertFloat32ToDouble(scratchFloat32, floatScratch0);
      masm.boxDouble(floatScratch0, output.valueReg(), floatScratch0);
      break;
    }
    case Scalar::Float64:
      masm.moveGPR64ToDouble(outputReg64, floatScratch0);
      masm.canonicalizeDouble(floatScratch0);
      masm.boxDouble(floatScratch0, output.valueReg(), floatScratch0);
      break;
    case Scalar::BigInt64:
    case Scalar::BigUint64: {
      // We need two extra registers. Reuse the obj/littleEndian registers.
      Register bigInt = obj;
      Register bigIntScratch = littleEndian;
      masm.push(bigInt);
      masm.push(bigIntScratch);
      Label fail, done;
      LiveRegisterSet save(GeneralRegisterSet::Volatile(),
                           liveVolatileFloatRegs());
      save.takeUnchecked(bigInt);
      save.takeUnchecked(bigIntScratch);
      bool attemptNursery = CanNurseryAllocateBigInt(cx_);
      EmitAllocateBigInt(masm, bigInt, bigIntScratch, save, &fail,
                         attemptNursery);
      masm.jump(&done);

      masm.bind(&fail);
      masm.pop(bigIntScratch);
      masm.pop(bigInt);
      masm.jump(failure->label());

      masm.bind(&done);
      masm.initializeBigInt64(elementType, bigInt, outputReg64);
      masm.tagValue(JSVAL_TYPE_BIGINT, bigInt, output.valueReg());
      masm.pop(bigIntScratch);
      masm.pop(bigInt);
      break;
    }
    case Scalar::Uint8Clamped:
    default:
      MOZ_CRASH("Invalid typed array type");
  }

  return true;
}

bool CacheIRCompiler::emitStoreDataViewValueResult(
    ObjOperandId objId, IntPtrOperandId offsetId, uint32_t valueId,
    BooleanOperandId littleEndianId, Scalar::Type elementType) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
#ifdef JS_CODEGEN_X86
  // Use a scratch register to avoid running out of the registers.
  Register obj = output.valueReg().typeReg();
  allocator.copyToScratchRegister(masm, objId, obj);
#else
  Register obj = allocator.useRegister(masm, objId);
#endif
  Register offset = allocator.useRegister(masm, offsetId);
  Register littleEndian = allocator.useRegister(masm, littleEndianId);

  AutoAvailableFloatRegister floatScratch0(*this, FloatReg0);
  Maybe<Register> valInt32;
  Maybe<Register> valBigInt;
  switch (elementType) {
    case Scalar::Int8:
    case Scalar::Uint8:
    case Scalar::Int16:
    case Scalar::Uint16:
    case Scalar::Int32:
    case Scalar::Uint32:
    case Scalar::Uint8Clamped:
      valInt32.emplace(allocator.useRegister(masm, Int32OperandId(valueId)));
      break;

    case Scalar::Float32:
    case Scalar::Float64:
      allocator.ensureDoubleRegister(masm, NumberOperandId(valueId),
                                     floatScratch0);
      break;

    case Scalar::BigInt64:
    case Scalar::BigUint64:
      valBigInt.emplace(allocator.useRegister(masm, BigIntOperandId(valueId)));
      break;

    case Scalar::MaxTypedArrayViewType:
    case Scalar::Int64:
    case Scalar::Simd128:
      MOZ_CRASH("Unsupported type");
  }

  Register scratch1 = output.valueReg().scratchReg();
  MOZ_ASSERT(scratch1 != obj, "scratchReg must not be typeReg");

  // On platforms with enough registers, |scratch2| is an extra scratch register
  // (pair) used for byte-swapping the value.
#ifndef JS_CODEGEN_X86
  mozilla::MaybeOneOf<AutoScratchRegister, AutoScratchRegister64> scratch2;
  switch (elementType) {
    case Scalar::Int8:
    case Scalar::Uint8:
      break;
    case Scalar::Int16:
    case Scalar::Uint16:
    case Scalar::Int32:
    case Scalar::Uint32:
    case Scalar::Float32:
      scratch2.construct<AutoScratchRegister>(allocator, masm);
      break;
    case Scalar::Float64:
    case Scalar::BigInt64:
    case Scalar::BigUint64:
      scratch2.construct<AutoScratchRegister64>(allocator, masm);
      break;
    case Scalar::Uint8Clamped:
    default:
      MOZ_CRASH("Invalid type");
  }
#endif

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  const size_t byteSize = Scalar::byteSize(elementType);

  EmitDataViewBoundsCheck(masm, byteSize, obj, offset, scratch1,
                          failure->label());

  masm.loadPtr(Address(obj, DataViewObject::dataOffset()), scratch1);
  BaseIndex dest(scratch1, offset, TimesOne);

  if (byteSize == 1) {
    // Byte swapping has no effect, so just do the byte store.
    masm.store8(*valInt32, dest);
    masm.moveValue(UndefinedValue(), output.valueReg());
    return true;
  }

  // On 32-bit x86, |obj| is already a scratch register so use that. If we need
  // a Register64 we also use the littleEndian register and use the stack
  // location for the check below.
  bool pushedLittleEndian = false;
#ifdef JS_CODEGEN_X86
  if (byteSize == 8) {
    masm.push(littleEndian);
    pushedLittleEndian = true;
  }
  auto valScratch32 = [&]() -> Register { return obj; };
  auto valScratch64 = [&]() -> Register64 {
    return Register64(obj, littleEndian);
  };
#else
  auto valScratch32 = [&]() -> Register {
    return scratch2.ref<AutoScratchRegister>();
  };
  auto valScratch64 = [&]() -> Register64 {
    return scratch2.ref<AutoScratchRegister64>();
  };
#endif

  // Load the value into a gpr register.
  switch (elementType) {
    case Scalar::Int16:
    case Scalar::Uint16:
    case Scalar::Int32:
    case Scalar::Uint32:
      masm.move32(*valInt32, valScratch32());
      break;
    case Scalar::Float32: {
      FloatRegister scratchFloat32 = floatScratch0.get().asSingle();
      masm.convertDoubleToFloat32(floatScratch0, scratchFloat32);
      masm.canonicalizeFloatIfDeterministic(scratchFloat32);
      masm.moveFloat32ToGPR(scratchFloat32, valScratch32());
      break;
    }
    case Scalar::Float64: {
      masm.canonicalizeDoubleIfDeterministic(floatScratch0);
      masm.moveDoubleToGPR64(floatScratch0, valScratch64());
      break;
    }
    case Scalar::BigInt64:
    case Scalar::BigUint64:
      masm.loadBigInt64(*valBigInt, valScratch64());
      break;
    case Scalar::Int8:
    case Scalar::Uint8:
    case Scalar::Uint8Clamped:
    default:
      MOZ_CRASH("Invalid type");
  }

  // Swap the bytes in the loaded value.
  Label skip;
  if (pushedLittleEndian) {
    masm.branch32(MOZ_LITTLE_ENDIAN() ? Assembler::NotEqual : Assembler::Equal,
                  Address(masm.getStackPointer(), 0), Imm32(0), &skip);
  } else {
    masm.branch32(MOZ_LITTLE_ENDIAN() ? Assembler::NotEqual : Assembler::Equal,
                  littleEndian, Imm32(0), &skip);
  }
  switch (elementType) {
    case Scalar::Int16:
      masm.byteSwap16SignExtend(valScratch32());
      break;
    case Scalar::Uint16:
      masm.byteSwap16ZeroExtend(valScratch32());
      break;
    case Scalar::Int32:
    case Scalar::Uint32:
    case Scalar::Float32:
      masm.byteSwap32(valScratch32());
      break;
    case Scalar::Float64:
    case Scalar::BigInt64:
    case Scalar::BigUint64:
      masm.byteSwap64(valScratch64());
      break;
    case Scalar::Int8:
    case Scalar::Uint8:
    case Scalar::Uint8Clamped:
    default:
      MOZ_CRASH("Invalid type");
  }
  masm.bind(&skip);

  // Store the value.
  switch (elementType) {
    case Scalar::Int16:
    case Scalar::Uint16:
      masm.store16Unaligned(valScratch32(), dest);
      break;
    case Scalar::Int32:
    case Scalar::Uint32:
    case Scalar::Float32:
      masm.store32Unaligned(valScratch32(), dest);
      break;
    case Scalar::Float64:
    case Scalar::BigInt64:
    case Scalar::BigUint64:
      masm.store64Unaligned(valScratch64(), dest);
      break;
    case Scalar::Int8:
    case Scalar::Uint8:
    case Scalar::Uint8Clamped:
    default:
      MOZ_CRASH("Invalid typed array type");
  }

#ifdef JS_CODEGEN_X86
  // Restore registers.
  if (pushedLittleEndian) {
    masm.pop(littleEndian);
  }
#endif

  masm.moveValue(UndefinedValue(), output.valueReg());
  return true;
}

bool CacheIRCompiler::emitStoreFixedSlotUndefinedResult(ObjOperandId objId,
                                                        uint32_t offsetOffset,
                                                        ValOperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);
  Register obj = allocator.useRegister(masm, objId);
  ValueOperand val = allocator.useValueRegister(masm, rhsId);

  StubFieldOffset offset(offsetOffset, StubField::Type::RawInt32);
  emitLoadStubField(offset, scratch);

  BaseIndex slot(obj, scratch, TimesOne);
  EmitPreBarrier(masm, slot, MIRType::Value);
  masm.storeValue(val, slot);
  emitPostBarrierSlot(obj, val, scratch);

  masm.moveValue(UndefinedValue(), output.valueReg());
  return true;
}

bool CacheIRCompiler::emitLoadObjectResult(ObjOperandId objId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register obj = allocator.useRegister(masm, objId);

  EmitStoreResult(masm, obj, JSVAL_TYPE_OBJECT, output);

  return true;
}

bool CacheIRCompiler::emitLoadStringResult(StringOperandId strId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register str = allocator.useRegister(masm, strId);

  masm.tagValue(JSVAL_TYPE_STRING, str, output.valueReg());

  return true;
}

bool CacheIRCompiler::emitLoadSymbolResult(SymbolOperandId symId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register sym = allocator.useRegister(masm, symId);

  masm.tagValue(JSVAL_TYPE_SYMBOL, sym, output.valueReg());

  return true;
}

bool CacheIRCompiler::emitLoadInt32Result(Int32OperandId valId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register val = allocator.useRegister(masm, valId);

  masm.tagValue(JSVAL_TYPE_INT32, val, output.valueReg());

  return true;
}

bool CacheIRCompiler::emitLoadBigIntResult(BigIntOperandId valId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register val = allocator.useRegister(masm, valId);

  masm.tagValue(JSVAL_TYPE_BIGINT, val, output.valueReg());

  return true;
}

bool CacheIRCompiler::emitLoadDoubleResult(NumberOperandId valId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  ValueOperand val = allocator.useValueRegister(masm, valId);

#ifdef DEBUG
  Label ok;
  masm.branchTestDouble(Assembler::Equal, val, &ok);
  masm.branchTestInt32(Assembler::Equal, val, &ok);
  masm.assumeUnreachable("input must be double or int32");
  masm.bind(&ok);
#endif

  masm.moveValue(val, output.valueReg());
  masm.convertInt32ValueToDouble(output.valueReg());

  return true;
}

bool CacheIRCompiler::emitLoadTypeOfObjectResult(ObjOperandId objId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register obj = allocator.useRegister(masm, objId);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  Label slowCheck, isObject, isCallable, isUndefined, done;
  masm.typeOfObject(obj, scratch, &slowCheck, &isObject, &isCallable,
                    &isUndefined);

  masm.bind(&isCallable);
  masm.moveValue(StringValue(cx_->names().function), output.valueReg());
  masm.jump(&done);

  masm.bind(&isUndefined);
  masm.moveValue(StringValue(cx_->names().undefined), output.valueReg());
  masm.jump(&done);

  masm.bind(&isObject);
  masm.moveValue(StringValue(cx_->names().object), output.valueReg());
  masm.jump(&done);

  {
    masm.bind(&slowCheck);
    LiveRegisterSet save(GeneralRegisterSet::Volatile(),
                         liveVolatileFloatRegs());
    masm.PushRegsInMask(save);

    using Fn = JSString* (*)(JSObject * obj, JSRuntime * rt);
    masm.setupUnalignedABICall(scratch);
    masm.passABIArg(obj);
    masm.movePtr(ImmPtr(cx_->runtime()), scratch);
    masm.passABIArg(scratch);
    masm.callWithABI<Fn, TypeOfObject>();
    masm.mov(ReturnReg, scratch);

    LiveRegisterSet ignore;
    ignore.add(scratch);
    masm.PopRegsInMaskIgnore(save, ignore);

    masm.tagValue(JSVAL_TYPE_STRING, scratch, output.valueReg());
  }

  masm.bind(&done);
  return true;
}

bool CacheIRCompiler::emitLoadInt32TruthyResult(ValOperandId inputId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  ValueOperand val = allocator.useValueRegister(masm, inputId);

  Label ifFalse, done;
  masm.branchTestInt32Truthy(false, val, &ifFalse);
  masm.moveValue(BooleanValue(true), output.valueReg());
  masm.jump(&done);

  masm.bind(&ifFalse);
  masm.moveValue(BooleanValue(false), output.valueReg());

  masm.bind(&done);
  return true;
}

bool CacheIRCompiler::emitLoadStringTruthyResult(StringOperandId strId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register str = allocator.useRegister(masm, strId);

  Label ifFalse, done;
  masm.branch32(Assembler::Equal, Address(str, JSString::offsetOfLength()),
                Imm32(0), &ifFalse);
  masm.moveValue(BooleanValue(true), output.valueReg());
  masm.jump(&done);

  masm.bind(&ifFalse);
  masm.moveValue(BooleanValue(false), output.valueReg());

  masm.bind(&done);
  return true;
}

bool CacheIRCompiler::emitLoadDoubleTruthyResult(NumberOperandId inputId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);

  AutoScratchFloatRegister floatReg(this);

  allocator.ensureDoubleRegister(masm, inputId, floatReg);

  Label ifFalse, done;

  masm.branchTestDoubleTruthy(false, floatReg, &ifFalse);
  masm.moveValue(BooleanValue(true), output.valueReg());
  masm.jump(&done);

  masm.bind(&ifFalse);
  masm.moveValue(BooleanValue(false), output.valueReg());

  masm.bind(&done);
  return true;
}

bool CacheIRCompiler::emitLoadObjectTruthyResult(ObjOperandId objId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register obj = allocator.useRegister(masm, objId);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  Label emulatesUndefined, slowPath, done;
  masm.branchIfObjectEmulatesUndefined(obj, scratch, &slowPath,
                                       &emulatesUndefined);
  masm.moveValue(BooleanValue(true), output.valueReg());
  masm.jump(&done);

  masm.bind(&emulatesUndefined);
  masm.moveValue(BooleanValue(false), output.valueReg());
  masm.jump(&done);

  masm.bind(&slowPath);
  {
    LiveRegisterSet volatileRegs(GeneralRegisterSet::Volatile(),
                                 liveVolatileFloatRegs());
    volatileRegs.takeUnchecked(scratch);
    volatileRegs.takeUnchecked(output);
    masm.PushRegsInMask(volatileRegs);

    using Fn = bool (*)(JSObject * obj);
    masm.setupUnalignedABICall(scratch);
    masm.passABIArg(obj);
    masm.callWithABI<Fn, js::EmulatesUndefined>();
    masm.convertBoolToInt32(ReturnReg, scratch);
    masm.xor32(Imm32(1), scratch);

    masm.PopRegsInMask(volatileRegs);

    masm.tagValue(JSVAL_TYPE_BOOLEAN, scratch, output.valueReg());
  }

  masm.bind(&done);
  return true;
}

bool CacheIRCompiler::emitLoadBigIntTruthyResult(BigIntOperandId bigIntId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register bigInt = allocator.useRegister(masm, bigIntId);

  Label ifFalse, done;
  masm.branch32(Assembler::Equal,
                Address(bigInt, BigInt::offsetOfDigitLength()), Imm32(0),
                &ifFalse);
  masm.moveValue(BooleanValue(true), output.valueReg());
  masm.jump(&done);

  masm.bind(&ifFalse);
  masm.moveValue(BooleanValue(false), output.valueReg());

  masm.bind(&done);
  return true;
}

bool CacheIRCompiler::emitLoadValueTruthyResult(ValOperandId inputId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  ValueOperand value = allocator.useValueRegister(masm, inputId);
  AutoScratchRegisterMaybeOutput scratch1(allocator, masm, output);
  AutoScratchRegister scratch2(allocator, masm);
  AutoScratchFloatRegister floatReg(this);

  Label ifFalse, ifTrue, done;

  {
    ScratchTagScope tag(masm, value);
    masm.splitTagForTest(value, tag);

    masm.branchTestUndefined(Assembler::Equal, tag, &ifFalse);
    masm.branchTestNull(Assembler::Equal, tag, &ifFalse);

    Label notBoolean;
    masm.branchTestBoolean(Assembler::NotEqual, tag, &notBoolean);
    {
      ScratchTagScopeRelease _(&tag);
      masm.branchTestBooleanTruthy(false, value, &ifFalse);
      masm.jump(&ifTrue);
    }
    masm.bind(&notBoolean);

    Label notInt32;
    masm.branchTestInt32(Assembler::NotEqual, tag, &notInt32);
    {
      ScratchTagScopeRelease _(&tag);
      masm.branchTestInt32Truthy(false, value, &ifFalse);
      masm.jump(&ifTrue);
    }
    masm.bind(&notInt32);

    Label notObject;
    masm.branchTestObject(Assembler::NotEqual, tag, &notObject);
    {
      ScratchTagScopeRelease _(&tag);

      Register obj = masm.extractObject(value, scratch1);

      Label slowPath;
      masm.branchIfObjectEmulatesUndefined(obj, scratch2, &slowPath, &ifFalse);
      masm.jump(&ifTrue);

      masm.bind(&slowPath);
      {
        LiveRegisterSet volatileRegs(GeneralRegisterSet::Volatile(),
                                     liveVolatileFloatRegs());
        volatileRegs.takeUnchecked(scratch1);
        volatileRegs.takeUnchecked(scratch2);
        volatileRegs.takeUnchecked(output);
        masm.PushRegsInMask(volatileRegs);

        using Fn = bool (*)(JSObject * obj);
        masm.setupUnalignedABICall(scratch2);
        masm.passABIArg(obj);
        masm.callWithABI<Fn, js::EmulatesUndefined>();
        masm.storeCallBoolResult(scratch2);

        masm.PopRegsInMask(volatileRegs);

        masm.branchTest32(Assembler::NonZero, scratch2, scratch2, &ifFalse);
        masm.jump(&ifTrue);
      }
    }
    masm.bind(&notObject);

    Label notString;
    masm.branchTestString(Assembler::NotEqual, tag, &notString);
    {
      ScratchTagScopeRelease _(&tag);
      masm.branchTestStringTruthy(false, value, &ifFalse);
      masm.jump(&ifTrue);
    }
    masm.bind(&notString);

    Label notBigInt;
    masm.branchTestBigInt(Assembler::NotEqual, tag, &notBigInt);
    {
      ScratchTagScopeRelease _(&tag);
      masm.branchTestBigIntTruthy(false, value, &ifFalse);
      masm.jump(&ifTrue);
    }
    masm.bind(&notBigInt);

    masm.branchTestSymbol(Assembler::Equal, tag, &ifTrue);

#ifdef DEBUG
    Label isDouble;
    masm.branchTestDouble(Assembler::Equal, tag, &isDouble);
    masm.assumeUnreachable("Unexpected value type");
    masm.bind(&isDouble);
#endif

    {
      ScratchTagScopeRelease _(&tag);
      masm.unboxDouble(value, floatReg);
      masm.branchTestDoubleTruthy(false, floatReg, &ifFalse);
    }

    // Fall through to true case.
  }

  masm.bind(&ifTrue);
  masm.moveValue(BooleanValue(true), output.valueReg());
  masm.jump(&done);

  masm.bind(&ifFalse);
  masm.moveValue(BooleanValue(false), output.valueReg());

  masm.bind(&done);
  return true;
}

bool CacheIRCompiler::emitComparePointerResultShared(JSOp op,
                                                     TypedOperandId lhsId,
                                                     TypedOperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);

  Register left = allocator.useRegister(masm, lhsId);
  Register right = allocator.useRegister(masm, rhsId);

  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  Label ifTrue, done;
  masm.branchPtr(JSOpToCondition(op, /* signed = */ true), left, right,
                 &ifTrue);

  EmitStoreBoolean(masm, false, output);
  masm.jump(&done);

  masm.bind(&ifTrue);
  EmitStoreBoolean(masm, true, output);
  masm.bind(&done);
  return true;
}

bool CacheIRCompiler::emitCompareObjectResult(JSOp op, ObjOperandId lhsId,
                                              ObjOperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  return emitComparePointerResultShared(op, lhsId, rhsId);
}

bool CacheIRCompiler::emitCompareSymbolResult(JSOp op, SymbolOperandId lhsId,
                                              SymbolOperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  return emitComparePointerResultShared(op, lhsId, rhsId);
}

bool CacheIRCompiler::emitCompareInt32Result(JSOp op, Int32OperandId lhsId,
                                             Int32OperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register left = allocator.useRegister(masm, lhsId);
  Register right = allocator.useRegister(masm, rhsId);

  Label ifTrue, done;
  masm.branch32(JSOpToCondition(op, /* signed = */ true), left, right, &ifTrue);

  EmitStoreBoolean(masm, false, output);
  masm.jump(&done);

  masm.bind(&ifTrue);
  EmitStoreBoolean(masm, true, output);
  masm.bind(&done);
  return true;
}

bool CacheIRCompiler::emitCompareDoubleResult(JSOp op, NumberOperandId lhsId,
                                              NumberOperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);

  // Float register must be preserved. The Compare ICs use the fact that
  // baseline has them available, as well as fixed temps on LBinaryBoolCache.
  AutoAvailableFloatRegister floatScratch0(*this, FloatReg0);
  AutoAvailableFloatRegister floatScratch1(*this, FloatReg1);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  allocator.ensureDoubleRegister(masm, lhsId, floatScratch0);
  allocator.ensureDoubleRegister(masm, rhsId, floatScratch1);

  Label done, ifTrue;
  masm.branchDouble(JSOpToDoubleCondition(op), floatScratch0, floatScratch1,
                    &ifTrue);
  EmitStoreBoolean(masm, false, output);
  masm.jump(&done);

  masm.bind(&ifTrue);
  EmitStoreBoolean(masm, true, output);
  masm.bind(&done);
  return true;
}

bool CacheIRCompiler::emitCompareBigIntResult(JSOp op, BigIntOperandId lhsId,
                                              BigIntOperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);

  Register lhs = allocator.useRegister(masm, lhsId);
  Register rhs = allocator.useRegister(masm, rhsId);

  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  LiveRegisterSet save(GeneralRegisterSet::Volatile(), liveVolatileFloatRegs());
  masm.PushRegsInMask(save);

  masm.setupUnalignedABICall(scratch);

  // Push the operands in reverse order for JSOp::Le and JSOp::Gt:
  // - |left <= right| is implemented as |right >= left|.
  // - |left > right| is implemented as |right < left|.
  if (op == JSOp::Le || op == JSOp::Gt) {
    masm.passABIArg(rhs);
    masm.passABIArg(lhs);
  } else {
    masm.passABIArg(lhs);
    masm.passABIArg(rhs);
  }

  using Fn = bool (*)(BigInt*, BigInt*);
  Fn fn;
  if (op == JSOp::Eq || op == JSOp::StrictEq) {
    fn = jit::BigIntEqual<EqualityKind::Equal>;
  } else if (op == JSOp::Ne || op == JSOp::StrictNe) {
    fn = jit::BigIntEqual<EqualityKind::NotEqual>;
  } else if (op == JSOp::Lt || op == JSOp::Gt) {
    fn = jit::BigIntCompare<ComparisonKind::LessThan>;
  } else {
    MOZ_ASSERT(op == JSOp::Le || op == JSOp::Ge);
    fn = jit::BigIntCompare<ComparisonKind::GreaterThanOrEqual>;
  }

  masm.callWithABI(DynamicFunction<Fn>(fn));
  masm.storeCallBoolResult(scratch);

  LiveRegisterSet ignore;
  ignore.add(scratch);
  masm.PopRegsInMaskIgnore(save, ignore);

  EmitStoreResult(masm, scratch, JSVAL_TYPE_BOOLEAN, output);
  return true;
}

bool CacheIRCompiler::emitCompareBigIntInt32Result(JSOp op,
                                                   BigIntOperandId lhsId,
                                                   Int32OperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register bigInt = allocator.useRegister(masm, lhsId);
  Register int32 = allocator.useRegister(masm, rhsId);

  AutoScratchRegisterMaybeOutput scratch1(allocator, masm, output);
  AutoScratchRegister scratch2(allocator, masm);

  Label ifTrue, ifFalse;
  masm.compareBigIntAndInt32(op, bigInt, int32, scratch1, scratch2, &ifTrue,
                             &ifFalse);

  Label done;
  masm.bind(&ifFalse);
  EmitStoreBoolean(masm, false, output);
  masm.jump(&done);

  masm.bind(&ifTrue);
  EmitStoreBoolean(masm, true, output);

  masm.bind(&done);
  return true;
}

bool CacheIRCompiler::emitCompareBigIntNumberResult(JSOp op,
                                                    BigIntOperandId lhsId,
                                                    NumberOperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);

  // Float register must be preserved. The Compare ICs use the fact that
  // baseline has them available, as well as fixed temps on LBinaryBoolCache.
  AutoAvailableFloatRegister floatScratch0(*this, FloatReg0);

  Register lhs = allocator.useRegister(masm, lhsId);
  allocator.ensureDoubleRegister(masm, rhsId, floatScratch0);

  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  LiveRegisterSet save(GeneralRegisterSet::Volatile(), liveVolatileFloatRegs());
  masm.PushRegsInMask(save);

  masm.setupUnalignedABICall(scratch);

  // Push the operands in reverse order for JSOp::Le and JSOp::Gt:
  // - |left <= right| is implemented as |right >= left|.
  // - |left > right| is implemented as |right < left|.
  if (op == JSOp::Le || op == JSOp::Gt) {
    masm.passABIArg(floatScratch0, MoveOp::DOUBLE);
    masm.passABIArg(lhs);
  } else {
    masm.passABIArg(lhs);
    masm.passABIArg(floatScratch0, MoveOp::DOUBLE);
  }

  using FnBigIntNumber = bool (*)(BigInt*, double);
  using FnNumberBigInt = bool (*)(double, BigInt*);
  switch (op) {
    case JSOp::Eq: {
      masm.callWithABI<FnBigIntNumber,
                       jit::BigIntNumberEqual<EqualityKind::Equal>>();
      break;
    }
    case JSOp::Ne: {
      masm.callWithABI<FnBigIntNumber,
                       jit::BigIntNumberEqual<EqualityKind::NotEqual>>();
      break;
    }
    case JSOp::Lt: {
      masm.callWithABI<FnBigIntNumber,
                       jit::BigIntNumberCompare<ComparisonKind::LessThan>>();
      break;
    }
    case JSOp::Gt: {
      masm.callWithABI<FnNumberBigInt,
                       jit::NumberBigIntCompare<ComparisonKind::LessThan>>();
      break;
    }
    case JSOp::Le: {
      masm.callWithABI<
          FnNumberBigInt,
          jit::NumberBigIntCompare<ComparisonKind::GreaterThanOrEqual>>();
      break;
    }
    case JSOp::Ge: {
      masm.callWithABI<
          FnBigIntNumber,
          jit::BigIntNumberCompare<ComparisonKind::GreaterThanOrEqual>>();
      break;
    }
    default:
      MOZ_CRASH("unhandled op");
  }

  masm.storeCallBoolResult(scratch);

  LiveRegisterSet ignore;
  ignore.add(scratch);
  masm.PopRegsInMaskIgnore(save, ignore);

  EmitStoreResult(masm, scratch, JSVAL_TYPE_BOOLEAN, output);
  return true;
}

bool CacheIRCompiler::emitCompareBigIntStringResult(JSOp op,
                                                    BigIntOperandId lhsId,
                                                    StringOperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoCallVM callvm(masm, this, allocator);

  Register lhs = allocator.useRegister(masm, lhsId);
  Register rhs = allocator.useRegister(masm, rhsId);

  callvm.prepare();

  // Push the operands in reverse order for JSOp::Le and JSOp::Gt:
  // - |left <= right| is implemented as |right >= left|.
  // - |left > right| is implemented as |right < left|.
  if (op == JSOp::Le || op == JSOp::Gt) {
    masm.Push(lhs);
    masm.Push(rhs);
  } else {
    masm.Push(rhs);
    masm.Push(lhs);
  }

  using FnBigIntString =
      bool (*)(JSContext*, HandleBigInt, HandleString, bool*);
  using FnStringBigInt =
      bool (*)(JSContext*, HandleString, HandleBigInt, bool*);

  switch (op) {
    case JSOp::Eq: {
#ifndef _MSC_VER
      constexpr auto Equal = EqualityKind::Equal;
#else
      // The static_cast works around an internal compiler error in MSVC.
      constexpr auto Equal = static_cast<bool>(EqualityKind::Equal);
#endif
      callvm.call<FnBigIntString, BigIntStringEqual<Equal>>();
      break;
    }
    case JSOp::Ne: {
#ifndef _MSC_VER
      constexpr auto NotEqual = EqualityKind::NotEqual;
#else
      // The static_cast works around an internal compiler error in MSVC.
      constexpr auto NotEqual = static_cast<bool>(EqualityKind::NotEqual);
#endif
      callvm.call<FnBigIntString, BigIntStringEqual<NotEqual>>();
      break;
    }
    case JSOp::Lt: {
      constexpr auto LessThan = ComparisonKind::LessThan;
      callvm.call<FnBigIntString, BigIntStringCompare<LessThan>>();
      break;
    }
    case JSOp::Gt: {
      constexpr auto LessThan = ComparisonKind::LessThan;
      callvm.call<FnStringBigInt, StringBigIntCompare<LessThan>>();
      break;
    }
    case JSOp::Le: {
      constexpr auto GreaterThanOrEqual = ComparisonKind::GreaterThanOrEqual;
      callvm.call<FnStringBigInt, StringBigIntCompare<GreaterThanOrEqual>>();
      break;
    }
    case JSOp::Ge: {
      constexpr auto GreaterThanOrEqual = ComparisonKind::GreaterThanOrEqual;
      callvm.call<FnBigIntString, BigIntStringCompare<GreaterThanOrEqual>>();
      break;
    }
    default:
      MOZ_CRASH("unhandled op");
  }
  return true;
}

bool CacheIRCompiler::emitCompareNullUndefinedResult(JSOp op, bool isUndefined,
                                                     ValOperandId inputId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  ValueOperand input = allocator.useValueRegister(masm, inputId);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  if (IsStrictEqualityOp(op)) {
    if (isUndefined) {
      masm.testUndefinedSet(JSOpToCondition(op, false), input, scratch);
    } else {
      masm.testNullSet(JSOpToCondition(op, false), input, scratch);
    }
    EmitStoreResult(masm, scratch, JSVAL_TYPE_BOOLEAN, output);
    return true;
  }

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  MOZ_ASSERT(IsLooseEqualityOp(op));

  Label nullOrLikeUndefined, notNullOrLikeUndefined, done;
  {
    ScratchTagScope tag(masm, input);
    masm.splitTagForTest(input, tag);

    if (isUndefined) {
      masm.branchTestUndefined(Assembler::Equal, tag, &nullOrLikeUndefined);
      masm.branchTestNull(Assembler::Equal, tag, &nullOrLikeUndefined);
    } else {
      masm.branchTestNull(Assembler::Equal, tag, &nullOrLikeUndefined);
      masm.branchTestUndefined(Assembler::Equal, tag, &nullOrLikeUndefined);
    }
    masm.branchTestObject(Assembler::NotEqual, tag, &notNullOrLikeUndefined);

    {
      ScratchTagScopeRelease _(&tag);

      masm.unboxObject(input, scratch);
      masm.branchIfObjectEmulatesUndefined(scratch, scratch, failure->label(),
                                           &nullOrLikeUndefined);
      masm.jump(&notNullOrLikeUndefined);
    }
  }

  masm.bind(&nullOrLikeUndefined);
  EmitStoreBoolean(masm, op == JSOp::Eq, output);
  masm.jump(&done);

  masm.bind(&notNullOrLikeUndefined);
  EmitStoreBoolean(masm, op == JSOp::Ne, output);

  masm.bind(&done);
  return true;
}

bool CacheIRCompiler::emitCompareDoubleSameValueResult(NumberOperandId lhsId,
                                                       NumberOperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);
  AutoAvailableFloatRegister floatScratch0(*this, FloatReg0);
  AutoAvailableFloatRegister floatScratch1(*this, FloatReg1);
  AutoAvailableFloatRegister floatScratch2(*this, FloatReg2);

  allocator.ensureDoubleRegister(masm, lhsId, floatScratch0);
  allocator.ensureDoubleRegister(masm, rhsId, floatScratch1);

  masm.sameValueDouble(floatScratch0, floatScratch1, floatScratch2, scratch);
  masm.tagValue(JSVAL_TYPE_BOOLEAN, scratch, output.valueReg());
  return true;
}

bool CacheIRCompiler::emitIndirectTruncateInt32Result(Int32OperandId valId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  Register val = allocator.useRegister(masm, valId);

  if (output.hasValue()) {
    masm.tagValue(JSVAL_TYPE_INT32, val, output.valueReg());
  } else {
    masm.mov(val, output.typedReg().gpr());
  }
  return true;
}

bool CacheIRCompiler::emitCallPrintString(const char* str) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  masm.printf(str);
  return true;
}

bool CacheIRCompiler::emitBreakpoint() {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  masm.breakpoint();
  return true;
}

void CacheIRCompiler::emitRegisterEnumerator(Register enumeratorsList,
                                             Register iter, Register scratch) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  // iter->next = list
  masm.storePtr(enumeratorsList, Address(iter, NativeIterator::offsetOfNext()));

  // iter->prev = list->prev
  masm.loadPtr(Address(enumeratorsList, NativeIterator::offsetOfPrev()),
               scratch);
  masm.storePtr(scratch, Address(iter, NativeIterator::offsetOfPrev()));

  // list->prev->next = iter
  masm.storePtr(iter, Address(scratch, NativeIterator::offsetOfNext()));

  // list->prev = ni
  masm.storePtr(iter, Address(enumeratorsList, NativeIterator::offsetOfPrev()));
}

void CacheIRCompiler::emitPostBarrierShared(Register obj,
                                            const ConstantOrRegister& val,
                                            Register scratch,
                                            Register maybeIndex) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  if (val.constant()) {
    MOZ_ASSERT_IF(val.value().isGCThing(),
                  !IsInsideNursery(val.value().toGCThing()));
    return;
  }

  TypedOrValueRegister reg = val.reg();
  if (reg.hasTyped() && !NeedsPostBarrier(reg.type())) {
    return;
  }

  Label skipBarrier;
  if (reg.hasValue()) {
    masm.branchValueIsNurseryCell(Assembler::NotEqual, reg.valueReg(), scratch,
                                  &skipBarrier);
  } else {
    masm.branchPtrInNurseryChunk(Assembler::NotEqual, reg.typedReg().gpr(),
                                 scratch, &skipBarrier);
  }
  masm.branchPtrInNurseryChunk(Assembler::Equal, obj, scratch, &skipBarrier);

  // Call one of these, depending on maybeIndex:
  //
  //   void PostWriteBarrier(JSRuntime* rt, JSObject* obj);
  //   void PostWriteElementBarrier(JSRuntime* rt, JSObject* obj,
  //                                int32_t index);
  LiveRegisterSet save(GeneralRegisterSet::Volatile(), liveVolatileFloatRegs());
  masm.PushRegsInMask(save);
  masm.setupUnalignedABICall(scratch);
  masm.movePtr(ImmPtr(cx_->runtime()), scratch);
  masm.passABIArg(scratch);
  masm.passABIArg(obj);
  if (maybeIndex != InvalidReg) {
    masm.passABIArg(maybeIndex);
    using Fn = void (*)(JSRuntime * rt, JSObject * obj, int32_t index);
    masm.callWithABI<Fn, PostWriteElementBarrier<IndexInBounds::Yes>>();
  } else {
    using Fn = void (*)(JSRuntime * rt, js::gc::Cell * cell);
    masm.callWithABI<Fn, PostWriteBarrier>();
  }
  masm.PopRegsInMask(save);

  masm.bind(&skipBarrier);
}

bool CacheIRCompiler::emitWrapResult() {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  AutoScratchRegister scratch(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  Label done;
  // We only have to wrap objects, because we are in the same zone.
  masm.branchTestObject(Assembler::NotEqual, output.valueReg(), &done);

  Register obj = output.valueReg().scratchReg();
  masm.unboxObject(output.valueReg(), obj);

  LiveRegisterSet save(GeneralRegisterSet::Volatile(), liveVolatileFloatRegs());
  masm.PushRegsInMask(save);

  using Fn = JSObject* (*)(JSContext * cx, JSObject * obj);
  masm.setupUnalignedABICall(scratch);
  masm.loadJSContext(scratch);
  masm.passABIArg(scratch);
  masm.passABIArg(obj);
  masm.callWithABI<Fn, WrapObjectPure>();
  masm.mov(ReturnReg, obj);

  LiveRegisterSet ignore;
  ignore.add(obj);
  masm.PopRegsInMaskIgnore(save, ignore);

  // We could not get a wrapper for this object.
  masm.branchTestPtr(Assembler::Zero, obj, obj, failure->label());

  // We clobbered the output register, so we have to retag.
  masm.tagValue(JSVAL_TYPE_OBJECT, obj, output.valueReg());

  masm.bind(&done);
  return true;
}

bool CacheIRCompiler::emitMegamorphicLoadSlotByValueResult(ObjOperandId objId,
                                                           ValOperandId idId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);

  Register obj = allocator.useRegister(masm, objId);
  ValueOperand idVal = allocator.useValueRegister(masm, idId);

  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  // The object must be Native.
  masm.branchIfNonNativeObj(obj, scratch, failure->label());

  // idVal will be in vp[0], result will be stored in vp[1].
  masm.reserveStack(sizeof(Value));
  masm.Push(idVal);
  masm.moveStackPtrTo(idVal.scratchReg());

  LiveRegisterSet volatileRegs(GeneralRegisterSet::Volatile(),
                               liveVolatileFloatRegs());
  volatileRegs.takeUnchecked(scratch);
  volatileRegs.takeUnchecked(idVal);
  masm.PushRegsInMask(volatileRegs);

  using Fn = bool (*)(JSContext * cx, JSObject * obj, Value * vp);
  masm.setupUnalignedABICall(scratch);
  masm.loadJSContext(scratch);
  masm.passABIArg(scratch);
  masm.passABIArg(obj);
  masm.passABIArg(idVal.scratchReg());
  masm.callWithABI<Fn, GetNativeDataPropertyByValuePure>();

  masm.mov(ReturnReg, scratch);
  masm.PopRegsInMask(volatileRegs);

  masm.Pop(idVal);

  Label ok;
  uint32_t framePushed = masm.framePushed();
  masm.branchIfTrueBool(scratch, &ok);
  masm.adjustStack(sizeof(Value));
  masm.jump(failure->label());

  masm.bind(&ok);
  if (JitOptions.spectreJitToCxxCalls) {
    masm.speculationBarrier();
  }
  masm.setFramePushed(framePushed);
  masm.loadTypedOrValue(Address(masm.getStackPointer(), 0), output);
  masm.adjustStack(sizeof(Value));
  return true;
}

bool CacheIRCompiler::emitMegamorphicHasPropResult(ObjOperandId objId,
                                                   ValOperandId idId,
                                                   bool hasOwn) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);

  Register obj = allocator.useRegister(masm, objId);
  ValueOperand idVal = allocator.useValueRegister(masm, idId);

  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  // idVal will be in vp[0], result will be stored in vp[1].
  masm.reserveStack(sizeof(Value));
  masm.Push(idVal);
  masm.moveStackPtrTo(idVal.scratchReg());

  LiveRegisterSet volatileRegs(GeneralRegisterSet::Volatile(),
                               liveVolatileFloatRegs());
  volatileRegs.takeUnchecked(scratch);
  volatileRegs.takeUnchecked(idVal);
  masm.PushRegsInMask(volatileRegs);

  using Fn = bool (*)(JSContext * cx, JSObject * obj, Value * vp);
  masm.setupUnalignedABICall(scratch);
  masm.loadJSContext(scratch);
  masm.passABIArg(scratch);
  masm.passABIArg(obj);
  masm.passABIArg(idVal.scratchReg());
  if (hasOwn) {
    masm.callWithABI<Fn, HasNativeDataPropertyPure<true>>();
  } else {
    masm.callWithABI<Fn, HasNativeDataPropertyPure<false>>();
  }
  masm.mov(ReturnReg, scratch);
  masm.PopRegsInMask(volatileRegs);

  masm.Pop(idVal);

  Label ok;
  uint32_t framePushed = masm.framePushed();
  masm.branchIfTrueBool(scratch, &ok);
  masm.adjustStack(sizeof(Value));
  masm.jump(failure->label());

  masm.bind(&ok);
  masm.setFramePushed(framePushed);
  masm.loadTypedOrValue(Address(masm.getStackPointer(), 0), output);
  masm.adjustStack(sizeof(Value));
  return true;
}

bool CacheIRCompiler::emitCallObjectHasSparseElementResult(
    ObjOperandId objId, Int32OperandId indexId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);

  Register obj = allocator.useRegister(masm, objId);
  Register index = allocator.useRegister(masm, indexId);

  AutoScratchRegisterMaybeOutput scratch1(allocator, masm, output);
  AutoScratchRegister scratch2(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.reserveStack(sizeof(Value));
  masm.moveStackPtrTo(scratch2.get());

  LiveRegisterSet volatileRegs(GeneralRegisterSet::Volatile(),
                               liveVolatileFloatRegs());
  volatileRegs.takeUnchecked(scratch1);
  volatileRegs.takeUnchecked(index);
  masm.PushRegsInMask(volatileRegs);

  using Fn =
      bool (*)(JSContext * cx, NativeObject * obj, int32_t index, Value * vp);
  masm.setupUnalignedABICall(scratch1);
  masm.loadJSContext(scratch1);
  masm.passABIArg(scratch1);
  masm.passABIArg(obj);
  masm.passABIArg(index);
  masm.passABIArg(scratch2);
  masm.callWithABI<Fn, HasNativeElementPure>();
  masm.mov(ReturnReg, scratch1);
  masm.PopRegsInMask(volatileRegs);

  Label ok;
  uint32_t framePushed = masm.framePushed();
  masm.branchIfTrueBool(scratch1, &ok);
  masm.adjustStack(sizeof(Value));
  masm.jump(failure->label());

  masm.bind(&ok);
  masm.setFramePushed(framePushed);
  masm.loadTypedOrValue(Address(masm.getStackPointer(), 0), output);
  masm.adjustStack(sizeof(Value));
  return true;
}

/*
 * Move a constant value into register dest.
 */
void CacheIRCompiler::emitLoadStubFieldConstant(StubFieldOffset val,
                                                Register dest) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  MOZ_ASSERT(mode_ == Mode::Ion);
  switch (val.getStubFieldType()) {
    case StubField::Type::Shape:
      masm.movePtr(ImmGCPtr(shapeStubField(val.getOffset())), dest);
      break;
    case StubField::Type::GetterSetter:
      masm.movePtr(ImmGCPtr(getterSetterStubField(val.getOffset())), dest);
      break;
    case StubField::Type::String:
      masm.movePtr(ImmGCPtr(stringStubField(val.getOffset())), dest);
      break;
    case StubField::Type::JSObject:
      masm.movePtr(ImmGCPtr(objectStubField(val.getOffset())), dest);
      break;
    case StubField::Type::RawPointer:
      masm.movePtr(ImmPtr(pointerStubField(val.getOffset())), dest);
      break;
    case StubField::Type::RawInt32:
      masm.move32(Imm32(int32StubField(val.getOffset())), dest);
      break;
    case StubField::Type::Id:
      masm.movePropertyKey(idStubField(val.getOffset()), dest);
      break;
    default:
      MOZ_CRASH("Unhandled stub field constant type");
  }
}

/*
 * After this is done executing, dest contains the value; either through a
 * constant load or through the load from the stub data.
 *
 * The current policy is that Baseline will use loads from the stub data (to
 * allow IC sharing), where as Ion doesn't share ICs, and so we can safely use
 * constants in the IC.
 */
void CacheIRCompiler::emitLoadStubField(StubFieldOffset val, Register dest) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  if (stubFieldPolicy_ == StubFieldPolicy::Constant) {
    emitLoadStubFieldConstant(val, dest);
  } else {
    Address load(ICStubReg, stubDataOffset_ + val.getOffset());

    switch (val.getStubFieldType()) {
      case StubField::Type::RawPointer:
      case StubField::Type::Shape:
      case StubField::Type::GetterSetter:
      case StubField::Type::JSObject:
      case StubField::Type::Symbol:
      case StubField::Type::String:
      case StubField::Type::Id:
        masm.loadPtr(load, dest);
        break;
      case StubField::Type::RawInt32:
        masm.load32(load, dest);
        break;
      default:
        MOZ_CRASH("Unhandled stub field constant type");
    }
  }
}

void CacheIRCompiler::emitLoadValueStubField(StubFieldOffset val,
                                             ValueOperand dest) {
  MOZ_ASSERT(val.getStubFieldType() == StubField::Type::Value);

  if (stubFieldPolicy_ == StubFieldPolicy::Constant) {
    MOZ_ASSERT(mode_ == Mode::Ion);
    masm.moveValue(valueStubField(val.getOffset()), dest);
  } else {
    Address addr(ICStubReg, stubDataOffset_ + val.getOffset());
    masm.loadValue(addr, dest);
  }
}

bool CacheIRCompiler::emitLoadInstanceOfObjectResult(ValOperandId lhsId,
                                                     ObjOperandId protoId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  ValueOperand lhs = allocator.useValueRegister(masm, lhsId);
  Register proto = allocator.useRegister(masm, protoId);

  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  Label returnFalse, returnTrue, done;
  masm.fallibleUnboxObject(lhs, scratch, &returnFalse);

  // LHS is an object. Load its proto.
  masm.loadObjProto(scratch, scratch);
  {
    // Walk the proto chain until we either reach the target object,
    // nullptr or LazyProto.
    Label loop;
    masm.bind(&loop);

    masm.branchPtr(Assembler::Equal, scratch, proto, &returnTrue);
    masm.branchTestPtr(Assembler::Zero, scratch, scratch, &returnFalse);

    MOZ_ASSERT(uintptr_t(TaggedProto::LazyProto) == 1);
    masm.branchPtr(Assembler::Equal, scratch, ImmWord(1), failure->label());

    masm.loadObjProto(scratch, scratch);
    masm.jump(&loop);
  }

  masm.bind(&returnFalse);
  EmitStoreBoolean(masm, false, output);
  masm.jump(&done);

  masm.bind(&returnTrue);
  EmitStoreBoolean(masm, true, output);
  // fallthrough
  masm.bind(&done);
  return true;
}

bool CacheIRCompiler::emitMegamorphicLoadSlotResult(ObjOperandId objId,
                                                    uint32_t nameOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);

  Register obj = allocator.useRegister(masm, objId);
  StubFieldOffset name(nameOffset, StubField::Type::String);

  AutoScratchRegisterMaybeOutput scratch1(allocator, masm, output);
  AutoScratchRegister scratch2(allocator, masm);
  AutoScratchRegister scratch3(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  // The object must be Native.
  masm.branchIfNonNativeObj(obj, scratch3, failure->label());

  masm.Push(UndefinedValue());
  masm.moveStackPtrTo(scratch3.get());

  LiveRegisterSet volatileRegs(GeneralRegisterSet::Volatile(),
                               liveVolatileFloatRegs());
  volatileRegs.takeUnchecked(scratch1);
  volatileRegs.takeUnchecked(scratch2);
  volatileRegs.takeUnchecked(scratch3);
  masm.PushRegsInMask(volatileRegs);

  using Fn =
      bool (*)(JSContext * cx, JSObject * obj, PropertyName * name, Value * vp);
  masm.setupUnalignedABICall(scratch1);
  masm.loadJSContext(scratch1);
  masm.passABIArg(scratch1);
  masm.passABIArg(obj);
  emitLoadStubField(name, scratch2);
  masm.passABIArg(scratch2);
  masm.passABIArg(scratch3);
  masm.callWithABI<Fn, GetNativeDataPropertyPure>();

  masm.mov(ReturnReg, scratch2);
  masm.PopRegsInMask(volatileRegs);

  masm.loadTypedOrValue(Address(masm.getStackPointer(), 0), output);
  masm.adjustStack(sizeof(Value));

  masm.branchIfFalseBool(scratch2, failure->label());
  if (JitOptions.spectreJitToCxxCalls) {
    masm.speculationBarrier();
  }

  return true;
}

bool CacheIRCompiler::emitMegamorphicStoreSlot(ObjOperandId objId,
                                               uint32_t nameOffset,
                                               ValOperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register obj = allocator.useRegister(masm, objId);
  StubFieldOffset name(nameOffset, StubField::Type::String);
  ValueOperand val = allocator.useValueRegister(masm, rhsId);

  AutoScratchRegister scratch1(allocator, masm);
  AutoScratchRegister scratch2(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  masm.Push(val);
  masm.moveStackPtrTo(val.scratchReg());

  LiveRegisterSet volatileRegs(GeneralRegisterSet::Volatile(),
                               liveVolatileFloatRegs());
  volatileRegs.takeUnchecked(scratch1);
  volatileRegs.takeUnchecked(scratch2);
  volatileRegs.takeUnchecked(val);
  masm.PushRegsInMask(volatileRegs);

  using Fn = bool (*)(JSContext * cx, JSObject * obj, PropertyName * name,
                      Value * val);
  masm.setupUnalignedABICall(scratch1);
  masm.loadJSContext(scratch1);
  masm.passABIArg(scratch1);
  masm.passABIArg(obj);
  emitLoadStubField(name, scratch2);
  masm.passABIArg(scratch2);
  masm.passABIArg(val.scratchReg());
  masm.callWithABI<Fn, SetNativeDataPropertyPure>();

  masm.mov(ReturnReg, scratch1);
  masm.PopRegsInMask(volatileRegs);

  masm.loadValue(Address(masm.getStackPointer(), 0), val);
  masm.adjustStack(sizeof(Value));

  masm.branchIfFalseBool(scratch1, failure->label());
  return true;
}

bool CacheIRCompiler::emitGuardHasGetterSetter(ObjOperandId objId,
                                               uint32_t idOffset,
                                               uint32_t getterSetterOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  Register obj = allocator.useRegister(masm, objId);

  StubFieldOffset id(idOffset, StubField::Type::Id);
  StubFieldOffset getterSetter(getterSetterOffset,
                               StubField::Type::GetterSetter);

  AutoScratchRegister scratch1(allocator, masm);
  AutoScratchRegister scratch2(allocator, masm);
  AutoScratchRegister scratch3(allocator, masm);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  LiveRegisterSet volatileRegs(GeneralRegisterSet::Volatile(),
                               liveVolatileFloatRegs());
  volatileRegs.takeUnchecked(scratch1);
  volatileRegs.takeUnchecked(scratch2);
  masm.PushRegsInMask(volatileRegs);

  using Fn = bool (*)(JSContext * cx, JSObject * obj, jsid id,
                      GetterSetter * getterSetter);
  masm.setupUnalignedABICall(scratch1);
  masm.loadJSContext(scratch1);
  masm.passABIArg(scratch1);
  masm.passABIArg(obj);
  emitLoadStubField(id, scratch2);
  masm.passABIArg(scratch2);
  emitLoadStubField(getterSetter, scratch3);
  masm.passABIArg(scratch3);
  masm.callWithABI<Fn, ObjectHasGetterSetterPure>();
  masm.mov(ReturnReg, scratch1);
  masm.PopRegsInMask(volatileRegs);

  masm.branchIfFalseBool(scratch1, failure->label());
  return true;
}

bool CacheIRCompiler::emitGuardWasmArg(ValOperandId argId,
                                       wasm::ValType::Kind kind) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  // All values can be boxed as AnyRef.
  if (kind == wasm::ValType::Ref) {
    return true;
  }
  MOZ_ASSERT(kind != wasm::ValType::V128);

  ValueOperand arg = allocator.useValueRegister(masm, argId);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  // Check that the argument can be converted to the Wasm type in Warp code
  // without bailing out.
  Label done;
  switch (kind) {
    case wasm::ValType::I32:
    case wasm::ValType::F32:
    case wasm::ValType::F64: {
      // Argument must be number, bool, or undefined.
      masm.branchTestNumber(Assembler::Equal, arg, &done);
      masm.branchTestBoolean(Assembler::Equal, arg, &done);
      masm.branchTestUndefined(Assembler::NotEqual, arg, failure->label());
      break;
    }
    case wasm::ValType::I64: {
      // Argument must be bigint, bool, or string.
      masm.branchTestBigInt(Assembler::Equal, arg, &done);
      masm.branchTestBoolean(Assembler::Equal, arg, &done);
      masm.branchTestString(Assembler::NotEqual, arg, failure->label());
      break;
    }
    default:
      MOZ_CRASH("Unexpected kind");
  }
  masm.bind(&done);

  return true;
}

bool CacheIRCompiler::emitLoadObject(ObjOperandId resultId,
                                     uint32_t objOffset) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register reg = allocator.defineRegister(masm, resultId);
  StubFieldOffset obj(objOffset, StubField::Type::JSObject);
  emitLoadStubField(obj, reg);
  return true;
}

bool CacheIRCompiler::emitLoadInt32Constant(uint32_t valOffset,
                                            Int32OperandId resultId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register reg = allocator.defineRegister(masm, resultId);
  StubFieldOffset val(valOffset, StubField::Type::RawInt32);
  emitLoadStubField(val, reg);
  return true;
}

bool CacheIRCompiler::emitLoadBooleanConstant(bool val,
                                              BooleanOperandId resultId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register reg = allocator.defineRegister(masm, resultId);
  masm.move32(Imm32(val), reg);
  return true;
}

bool CacheIRCompiler::emitLoadUndefined(ValOperandId resultId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  ValueOperand reg = allocator.defineValueRegister(masm, resultId);
  masm.moveValue(UndefinedValue(), reg);
  return true;
}

bool CacheIRCompiler::emitLoadConstantString(uint32_t strOffset,
                                             StringOperandId resultId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register reg = allocator.defineRegister(masm, resultId);
  StubFieldOffset str(strOffset, StubField::Type::String);
  emitLoadStubField(str, reg);
  return true;
}

bool CacheIRCompiler::emitCallInt32ToString(Int32OperandId inputId,
                                            StringOperandId resultId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register input = allocator.useRegister(masm, inputId);
  Register result = allocator.defineRegister(masm, resultId);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  LiveRegisterSet volatileRegs(GeneralRegisterSet::Volatile(),
                               liveVolatileFloatRegs());
  volatileRegs.takeUnchecked(result);
  masm.PushRegsInMask(volatileRegs);

  using Fn = JSLinearString* (*)(JSContext * cx, int32_t i);
  masm.setupUnalignedABICall(result);
  masm.loadJSContext(result);
  masm.passABIArg(result);
  masm.passABIArg(input);
  masm.callWithABI<Fn, js::Int32ToStringPure>();

  masm.mov(ReturnReg, result);
  masm.PopRegsInMask(volatileRegs);

  masm.branchPtr(Assembler::Equal, result, ImmPtr(0), failure->label());
  return true;
}

bool CacheIRCompiler::emitCallNumberToString(NumberOperandId inputId,
                                             StringOperandId resultId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  // Float register must be preserved. The BinaryArith ICs use
  // the fact that baseline has them available, as well as fixed temps on
  // LBinaryCache.
  AutoAvailableFloatRegister floatScratch0(*this, FloatReg0);

  allocator.ensureDoubleRegister(masm, inputId, floatScratch0);
  Register result = allocator.defineRegister(masm, resultId);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  LiveRegisterSet volatileRegs(GeneralRegisterSet::Volatile(),
                               liveVolatileFloatRegs());
  volatileRegs.takeUnchecked(result);
  volatileRegs.addUnchecked(floatScratch0);
  masm.PushRegsInMask(volatileRegs);

  using Fn = JSString* (*)(JSContext * cx, double d);
  masm.setupUnalignedABICall(result);
  masm.loadJSContext(result);
  masm.passABIArg(result);
  masm.passABIArg(floatScratch0, MoveOp::DOUBLE);
  masm.callWithABI<Fn, js::NumberToStringPure>();

  masm.mov(ReturnReg, result);
  masm.PopRegsInMask(volatileRegs);

  masm.branchPtr(Assembler::Equal, result, ImmPtr(0), failure->label());
  return true;
}

bool CacheIRCompiler::emitBooleanToString(BooleanOperandId inputId,
                                          StringOperandId resultId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  Register boolean = allocator.useRegister(masm, inputId);
  Register result = allocator.defineRegister(masm, resultId);
  const JSAtomState& names = cx_->names();
  Label true_, done;

  masm.branchTest32(Assembler::NonZero, boolean, boolean, &true_);

  // False case
  masm.movePtr(ImmGCPtr(names.false_), result);
  masm.jump(&done);

  // True case
  masm.bind(&true_);
  masm.movePtr(ImmGCPtr(names.true_), result);
  masm.bind(&done);

  return true;
}

bool CacheIRCompiler::emitObjectToStringResult(ObjOperandId objId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  Register obj = allocator.useRegister(masm, objId);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  LiveRegisterSet volatileRegs(GeneralRegisterSet::Volatile(),
                               liveVolatileFloatRegs());
  volatileRegs.takeUnchecked(output.valueReg());
  volatileRegs.takeUnchecked(scratch);
  masm.PushRegsInMask(volatileRegs);

  using Fn = JSString* (*)(JSContext*, JSObject*);
  masm.setupUnalignedABICall(scratch);
  masm.loadJSContext(scratch);
  masm.passABIArg(scratch);
  masm.passABIArg(obj);
  masm.callWithABI<Fn, js::ObjectClassToString>();
  masm.storeCallPointerResult(scratch);

  masm.PopRegsInMask(volatileRegs);

  masm.branchPtr(Assembler::Equal, scratch, ImmPtr(0), failure->label());
  masm.tagValue(JSVAL_TYPE_STRING, scratch, output.valueReg());

  return true;
}

bool CacheIRCompiler::emitCallStringConcatResult(StringOperandId lhsId,
                                                 StringOperandId rhsId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoCallVM callvm(masm, this, allocator);

  Register lhs = allocator.useRegister(masm, lhsId);
  Register rhs = allocator.useRegister(masm, rhsId);

  callvm.prepare();

  masm.Push(static_cast<js::jit::Imm32>(js::gc::DefaultHeap));
  masm.Push(rhs);
  masm.Push(lhs);

  using Fn = JSString* (*)(JSContext*, HandleString, HandleString,
                           js::gc::InitialHeap);
  callvm.call<Fn, ConcatStrings<CanGC>>();

  return true;
}

bool CacheIRCompiler::emitCallIsSuspendedGeneratorResult(ValOperandId valId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoOutputRegister output(*this);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);
  AutoScratchRegister scratch2(allocator, masm);
  ValueOperand input = allocator.useValueRegister(masm, valId);

  // Test if it's an object.
  Label returnFalse, done;
  masm.fallibleUnboxObject(input, scratch, &returnFalse);

  // Test if it's a GeneratorObject.
  masm.branchTestObjClass(Assembler::NotEqual, scratch,
                          &GeneratorObject::class_, scratch2, scratch,
                          &returnFalse);

  // If the resumeIndex slot holds an int32 value < RESUME_INDEX_RUNNING,
  // the generator is suspended.
  Address addr(scratch, AbstractGeneratorObject::offsetOfResumeIndexSlot());
  masm.fallibleUnboxInt32(addr, scratch, &returnFalse);
  masm.branch32(Assembler::AboveOrEqual, scratch,
                Imm32(AbstractGeneratorObject::RESUME_INDEX_RUNNING),
                &returnFalse);

  masm.moveValue(BooleanValue(true), output.valueReg());
  masm.jump(&done);

  masm.bind(&returnFalse);
  masm.moveValue(BooleanValue(false), output.valueReg());

  masm.bind(&done);
  return true;
}

// This op generates no code. It is consumed by the transpiler.
bool CacheIRCompiler::emitMetaTwoByte(uint32_t, uint32_t) { return true; }

bool CacheIRCompiler::emitCallNativeGetElementResult(ObjOperandId objId,
                                                     Int32OperandId indexId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoCallVM callvm(masm, this, allocator);

  Register obj = allocator.useRegister(masm, objId);
  Register index = allocator.useRegister(masm, indexId);

  callvm.prepare();

  masm.Push(index);
  masm.Push(TypedOrValueRegister(MIRType::Object, AnyRegister(obj)));
  masm.Push(obj);

  using Fn = bool (*)(JSContext*, HandleNativeObject, HandleValue, int32_t,
                      MutableHandleValue);
  callvm.call<Fn, NativeGetElement>();

  return true;
}

bool CacheIRCompiler::emitProxyHasPropResult(ObjOperandId objId,
                                             ValOperandId idId, bool hasOwn) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoCallVM callvm(masm, this, allocator);

  Register obj = allocator.useRegister(masm, objId);
  ValueOperand idVal = allocator.useValueRegister(masm, idId);

  callvm.prepare();

  masm.Push(idVal);
  masm.Push(obj);

  using Fn = bool (*)(JSContext*, HandleObject, HandleValue, bool*);
  if (hasOwn) {
    callvm.call<Fn, ProxyHasOwn>();
  } else {
    callvm.call<Fn, ProxyHas>();
  }
  return true;
}

bool CacheIRCompiler::emitProxyGetByValueResult(ObjOperandId objId,
                                                ValOperandId idId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);
  AutoCallVM callvm(masm, this, allocator);

  Register obj = allocator.useRegister(masm, objId);
  ValueOperand idVal = allocator.useValueRegister(masm, idId);

  callvm.prepare();
  masm.Push(idVal);
  masm.Push(obj);

  using Fn =
      bool (*)(JSContext*, HandleObject, HandleValue, MutableHandleValue);
  callvm.call<Fn, ProxyGetPropertyByValue>();
  return true;
}

bool CacheIRCompiler::emitCallGetSparseElementResult(ObjOperandId objId,
                                                     Int32OperandId indexId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoCallVM callvm(masm, this, allocator);

  Register obj = allocator.useRegister(masm, objId);
  Register id = allocator.useRegister(masm, indexId);

  callvm.prepare();
  masm.Push(id);
  masm.Push(obj);

  using Fn = bool (*)(JSContext * cx, HandleArrayObject obj, int32_t int_id,
                      MutableHandleValue result);
  callvm.call<Fn, GetSparseElementHelper>();
  return true;
}

bool CacheIRCompiler::emitCallRegExpMatcherResult(ObjOperandId regexpId,
                                                  StringOperandId inputId,
                                                  Int32OperandId lastIndexId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoCallVM callvm(masm, this, allocator);

  Register regexp = allocator.useRegister(masm, regexpId);
  Register input = allocator.useRegister(masm, inputId);
  Register lastIndex = allocator.useRegister(masm, lastIndexId);

  callvm.prepare();
  masm.Push(ImmWord(0));  // nullptr MatchPairs.
  masm.Push(lastIndex);
  masm.Push(input);
  masm.Push(regexp);

  using Fn = bool (*)(JSContext*, HandleObject regexp, HandleString input,
                      int32_t lastIndex, MatchPairs * pairs,
                      MutableHandleValue output);
  callvm.call<Fn, RegExpMatcherRaw>();
  return true;
}

bool CacheIRCompiler::emitCallRegExpSearcherResult(ObjOperandId regexpId,
                                                   StringOperandId inputId,
                                                   Int32OperandId lastIndexId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoCallVM callvm(masm, this, allocator);

  Register regexp = allocator.useRegister(masm, regexpId);
  Register input = allocator.useRegister(masm, inputId);
  Register lastIndex = allocator.useRegister(masm, lastIndexId);

  callvm.prepare();
  masm.Push(ImmWord(0));  // nullptr MatchPairs.
  masm.Push(lastIndex);
  masm.Push(input);
  masm.Push(regexp);

  using Fn = bool (*)(JSContext*, HandleObject regexp, HandleString input,
                      int32_t lastIndex, MatchPairs * pairs, int32_t * result);
  callvm.call<Fn, RegExpSearcherRaw>();
  return true;
}

bool CacheIRCompiler::emitCallRegExpTesterResult(ObjOperandId regexpId,
                                                 StringOperandId inputId,
                                                 Int32OperandId lastIndexId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoCallVM callvm(masm, this, allocator);

  Register regexp = allocator.useRegister(masm, regexpId);
  Register input = allocator.useRegister(masm, inputId);
  Register lastIndex = allocator.useRegister(masm, lastIndexId);

  callvm.prepare();
  masm.Push(lastIndex);
  masm.Push(input);
  masm.Push(regexp);

  using Fn = bool (*)(JSContext*, HandleObject regexp, HandleString input,
                      int32_t lastIndex, int32_t * result);
  callvm.call<Fn, RegExpTesterRaw>();
  return true;
}

bool CacheIRCompiler::emitRegExpFlagResult(ObjOperandId regexpId,
                                           int32_t flagsMask) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  Register regexp = allocator.useRegister(masm, regexpId);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  Address flagsAddr(
      regexp, NativeObject::getFixedSlotOffset(RegExpObject::flagsSlot()));
  masm.unboxInt32(flagsAddr, scratch);

  Label ifFalse, done;
  masm.branchTest32(Assembler::Zero, scratch, Imm32(flagsMask), &ifFalse);
  masm.moveValue(BooleanValue(true), output.valueReg());
  masm.jump(&done);

  masm.bind(&ifFalse);
  masm.moveValue(BooleanValue(false), output.valueReg());

  masm.bind(&done);
  return true;
}

bool CacheIRCompiler::emitCallSubstringKernelResult(StringOperandId strId,
                                                    Int32OperandId beginId,
                                                    Int32OperandId lengthId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoCallVM callvm(masm, this, allocator);

  Register str = allocator.useRegister(masm, strId);
  Register begin = allocator.useRegister(masm, beginId);
  Register length = allocator.useRegister(masm, lengthId);

  callvm.prepare();
  masm.Push(length);
  masm.Push(begin);
  masm.Push(str);

  using Fn = JSString* (*)(JSContext * cx, HandleString str, int32_t begin,
                           int32_t len);
  callvm.call<Fn, SubstringKernel>();
  return true;
}

bool CacheIRCompiler::emitStringReplaceStringResult(
    StringOperandId strId, StringOperandId patternId,
    StringOperandId replacementId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoCallVM callvm(masm, this, allocator);

  Register str = allocator.useRegister(masm, strId);
  Register pattern = allocator.useRegister(masm, patternId);
  Register replacement = allocator.useRegister(masm, replacementId);

  callvm.prepare();
  masm.Push(replacement);
  masm.Push(pattern);
  masm.Push(str);

  using Fn =
      JSString* (*)(JSContext*, HandleString, HandleString, HandleString);
  callvm.call<Fn, jit::StringReplace>();
  return true;
}

bool CacheIRCompiler::emitStringSplitStringResult(StringOperandId strId,
                                                  StringOperandId separatorId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoCallVM callvm(masm, this, allocator);

  Register str = allocator.useRegister(masm, strId);
  Register separator = allocator.useRegister(masm, separatorId);

  callvm.prepare();
  masm.Push(Imm32(INT32_MAX));
  masm.Push(separator);
  masm.Push(str);

  using Fn = ArrayObject* (*)(JSContext*, HandleString, HandleString, uint32_t);
  callvm.call<Fn, js::StringSplitString>();
  return true;
}

bool CacheIRCompiler::emitRegExpPrototypeOptimizableResult(
    ObjOperandId protoId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  Register proto = allocator.useRegister(masm, protoId);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  Label slow, done;
  masm.branchIfNotRegExpPrototypeOptimizable(proto, scratch, &slow);
  masm.moveValue(BooleanValue(true), output.valueReg());
  masm.jump(&done);

  {
    masm.bind(&slow);

    LiveRegisterSet volatileRegs(GeneralRegisterSet::Volatile(),
                                 liveVolatileFloatRegs());
    volatileRegs.takeUnchecked(scratch);
    masm.PushRegsInMask(volatileRegs);

    using Fn = bool (*)(JSContext * cx, JSObject * proto);
    masm.setupUnalignedABICall(scratch);
    masm.loadJSContext(scratch);
    masm.passABIArg(scratch);
    masm.passABIArg(proto);
    masm.callWithABI<Fn, RegExpPrototypeOptimizableRaw>();
    masm.storeCallBoolResult(scratch);

    masm.PopRegsInMask(volatileRegs);
    masm.tagValue(JSVAL_TYPE_BOOLEAN, scratch, output.valueReg());
  }

  masm.bind(&done);
  return true;
}

bool CacheIRCompiler::emitRegExpInstanceOptimizableResult(
    ObjOperandId regexpId, ObjOperandId protoId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  Register regexp = allocator.useRegister(masm, regexpId);
  Register proto = allocator.useRegister(masm, protoId);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  Label slow, done;
  masm.branchIfNotRegExpInstanceOptimizable(regexp, scratch, &slow);
  masm.moveValue(BooleanValue(true), output.valueReg());
  masm.jump(&done);

  {
    masm.bind(&slow);

    LiveRegisterSet volatileRegs(GeneralRegisterSet::Volatile(),
                                 liveVolatileFloatRegs());
    volatileRegs.takeUnchecked(scratch);
    masm.PushRegsInMask(volatileRegs);

    using Fn = bool (*)(JSContext * cx, JSObject * obj, JSObject * proto);
    masm.setupUnalignedABICall(scratch);
    masm.loadJSContext(scratch);
    masm.passABIArg(scratch);
    masm.passABIArg(regexp);
    masm.passABIArg(proto);
    masm.callWithABI<Fn, RegExpInstanceOptimizableRaw>();
    masm.storeCallBoolResult(scratch);

    masm.PopRegsInMask(volatileRegs);
    masm.tagValue(JSVAL_TYPE_BOOLEAN, scratch, output.valueReg());
  }

  masm.bind(&done);
  return true;
}

bool CacheIRCompiler::emitGetFirstDollarIndexResult(StringOperandId strId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoCallVM callvm(masm, this, allocator);

  Register str = allocator.useRegister(masm, strId);

  callvm.prepare();
  masm.Push(str);

  using Fn = bool (*)(JSContext*, JSString*, int32_t*);
  callvm.call<Fn, GetFirstDollarIndexRaw>();
  return true;
}

bool CacheIRCompiler::emitAtomicsCompareExchangeResult(
    ObjOperandId objId, IntPtrOperandId indexId, uint32_t expectedId,
    uint32_t replacementId, Scalar::Type elementType) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  Maybe<AutoOutputRegister> output;
  Maybe<AutoCallVM> callvm;
  if (!Scalar::isBigIntType(elementType)) {
    output.emplace(*this);
  } else {
    callvm.emplace(masm, this, allocator);
  }
#ifdef JS_CODEGEN_X86
  // Use a scratch register to avoid running out of registers.
  Register obj = output ? output->valueReg().typeReg()
                        : callvm->outputValueReg().typeReg();
  allocator.copyToScratchRegister(masm, objId, obj);
#else
  Register obj = allocator.useRegister(masm, objId);
#endif
  Register index = allocator.useRegister(masm, indexId);
  Register expected;
  Register replacement;
  if (!Scalar::isBigIntType(elementType)) {
    expected = allocator.useRegister(masm, Int32OperandId(expectedId));
    replacement = allocator.useRegister(masm, Int32OperandId(replacementId));
  } else {
    expected = allocator.useRegister(masm, BigIntOperandId(expectedId));
    replacement = allocator.useRegister(masm, BigIntOperandId(replacementId));
  }

  Register scratch = output ? output->valueReg().scratchReg()
                            : callvm->outputValueReg().scratchReg();
  MOZ_ASSERT(scratch != obj, "scratchReg must not be typeReg");

  // Not enough registers on X86.
  Register spectreTemp = Register::Invalid();

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  // AutoCallVM's AutoSaveLiveRegisters aren't accounted for in FailurePath, so
  // we can't use both at the same time. This isn't an issue here, because Ion
  // doesn't support CallICs. If that ever changes, this code must be updated.
  MOZ_ASSERT(isBaseline(), "Can't use FailurePath with AutoCallVM in Ion ICs");

  // Bounds check.
  masm.loadArrayBufferViewLengthIntPtr(obj, scratch);
  masm.spectreBoundsCheckPtr(index, scratch, spectreTemp, failure->label());

  // Atomic operations are highly platform-dependent, for example x86/x64 has
  // specific requirements on which registers are used; MIPS needs multiple
  // additional temporaries. Therefore we're using either an ABI or VM call here
  // instead of handling each platform separately.

  if (Scalar::isBigIntType(elementType)) {
    callvm->prepare();

    masm.Push(replacement);
    masm.Push(expected);
    masm.Push(index);
    masm.Push(obj);

    using Fn =
        BigInt* (*)(JSContext*, TypedArrayObject*, size_t, BigInt*, BigInt*);
    callvm->call<Fn, jit::AtomicsCompareExchange64>();
    return true;
  }

  {
    LiveRegisterSet volatileRegs(GeneralRegisterSet::Volatile(),
                                 liveVolatileFloatRegs());
    volatileRegs.takeUnchecked(output->valueReg());
    volatileRegs.takeUnchecked(scratch);
    masm.PushRegsInMask(volatileRegs);

    masm.setupUnalignedABICall(scratch);
    masm.passABIArg(obj);
    masm.passABIArg(index);
    masm.passABIArg(expected);
    masm.passABIArg(replacement);
    masm.callWithABI(DynamicFunction<AtomicsCompareExchangeFn>(
        AtomicsCompareExchange(elementType)));
    masm.storeCallInt32Result(scratch);

    masm.PopRegsInMask(volatileRegs);
  }

  if (elementType != Scalar::Uint32) {
    masm.tagValue(JSVAL_TYPE_INT32, scratch, output->valueReg());
  } else {
    ScratchDoubleScope fpscratch(masm);
    masm.convertUInt32ToDouble(scratch, fpscratch);
    masm.boxDouble(fpscratch, output->valueReg(), fpscratch);
  }

  return true;
}

bool CacheIRCompiler::emitAtomicsReadModifyWriteResult(
    ObjOperandId objId, IntPtrOperandId indexId, uint32_t valueId,
    Scalar::Type elementType, AtomicsReadWriteModifyFn fn) {
  AutoOutputRegister output(*this);
  Register obj = allocator.useRegister(masm, objId);
  Register index = allocator.useRegister(masm, indexId);
  Register value = allocator.useRegister(masm, Int32OperandId(valueId));
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  // Not enough registers on X86.
  Register spectreTemp = Register::Invalid();

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  // Bounds check.
  masm.loadArrayBufferViewLengthIntPtr(obj, scratch);
  masm.spectreBoundsCheckPtr(index, scratch, spectreTemp, failure->label());

  // See comment in emitAtomicsCompareExchange for why we use an ABI call.
  {
    LiveRegisterSet volatileRegs(GeneralRegisterSet::Volatile(),
                                 liveVolatileFloatRegs());
    volatileRegs.takeUnchecked(output.valueReg());
    volatileRegs.takeUnchecked(scratch);
    masm.PushRegsInMask(volatileRegs);

    masm.setupUnalignedABICall(scratch);
    masm.passABIArg(obj);
    masm.passABIArg(index);
    masm.passABIArg(value);
    masm.callWithABI(DynamicFunction<AtomicsReadWriteModifyFn>(fn));
    masm.storeCallInt32Result(scratch);

    masm.PopRegsInMask(volatileRegs);
  }

  if (elementType != Scalar::Uint32) {
    masm.tagValue(JSVAL_TYPE_INT32, scratch, output.valueReg());
  } else {
    ScratchDoubleScope fpscratch(masm);
    masm.convertUInt32ToDouble(scratch, fpscratch);
    masm.boxDouble(fpscratch, output.valueReg(), fpscratch);
  }

  return true;
}

template <CacheIRCompiler::AtomicsReadWriteModify64Fn fn>
bool CacheIRCompiler::emitAtomicsReadModifyWriteResult64(
    ObjOperandId objId, IntPtrOperandId indexId, uint32_t valueId) {
  AutoCallVM callvm(masm, this, allocator);
  Register obj = allocator.useRegister(masm, objId);
  Register index = allocator.useRegister(masm, indexId);
  Register value = allocator.useRegister(masm, BigIntOperandId(valueId));
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, callvm.output());

  // Not enough registers on X86.
  Register spectreTemp = Register::Invalid();

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  // AutoCallVM's AutoSaveLiveRegisters aren't accounted for in FailurePath, so
  // we can't use both at the same time. This isn't an issue here, because Ion
  // doesn't support CallICs. If that ever changes, this code must be updated.
  MOZ_ASSERT(isBaseline(), "Can't use FailurePath with AutoCallVM in Ion ICs");

  // Bounds check.
  masm.loadArrayBufferViewLengthIntPtr(obj, scratch);
  masm.spectreBoundsCheckPtr(index, scratch, spectreTemp, failure->label());

  // See comment in emitAtomicsCompareExchange for why we use a VM call.

  callvm.prepare();

  masm.Push(value);
  masm.Push(index);
  masm.Push(obj);

  callvm.call<AtomicsReadWriteModify64Fn, fn>();
  return true;
}

bool CacheIRCompiler::emitAtomicsExchangeResult(ObjOperandId objId,
                                                IntPtrOperandId indexId,
                                                uint32_t valueId,
                                                Scalar::Type elementType) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  if (Scalar::isBigIntType(elementType)) {
    return emitAtomicsReadModifyWriteResult64<jit::AtomicsExchange64>(
        objId, indexId, valueId);
  }
  return emitAtomicsReadModifyWriteResult(objId, indexId, valueId, elementType,
                                          AtomicsExchange(elementType));
}

bool CacheIRCompiler::emitAtomicsAddResult(ObjOperandId objId,
                                           IntPtrOperandId indexId,
                                           uint32_t valueId,
                                           Scalar::Type elementType,
                                           bool forEffect) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  if (Scalar::isBigIntType(elementType)) {
    return emitAtomicsReadModifyWriteResult64<jit::AtomicsAdd64>(objId, indexId,
                                                                 valueId);
  }
  return emitAtomicsReadModifyWriteResult(objId, indexId, valueId, elementType,
                                          AtomicsAdd(elementType));
}

bool CacheIRCompiler::emitAtomicsSubResult(ObjOperandId objId,
                                           IntPtrOperandId indexId,
                                           uint32_t valueId,
                                           Scalar::Type elementType,
                                           bool forEffect) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  if (Scalar::isBigIntType(elementType)) {
    return emitAtomicsReadModifyWriteResult64<jit::AtomicsSub64>(objId, indexId,
                                                                 valueId);
  }
  return emitAtomicsReadModifyWriteResult(objId, indexId, valueId, elementType,
                                          AtomicsSub(elementType));
}

bool CacheIRCompiler::emitAtomicsAndResult(ObjOperandId objId,
                                           IntPtrOperandId indexId,
                                           uint32_t valueId,
                                           Scalar::Type elementType,
                                           bool forEffect) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  if (Scalar::isBigIntType(elementType)) {
    return emitAtomicsReadModifyWriteResult64<jit::AtomicsAnd64>(objId, indexId,
                                                                 valueId);
  }
  return emitAtomicsReadModifyWriteResult(objId, indexId, valueId, elementType,
                                          AtomicsAnd(elementType));
}

bool CacheIRCompiler::emitAtomicsOrResult(ObjOperandId objId,
                                          IntPtrOperandId indexId,
                                          uint32_t valueId,
                                          Scalar::Type elementType,
                                          bool forEffect) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  if (Scalar::isBigIntType(elementType)) {
    return emitAtomicsReadModifyWriteResult64<jit::AtomicsOr64>(objId, indexId,
                                                                valueId);
  }
  return emitAtomicsReadModifyWriteResult(objId, indexId, valueId, elementType,
                                          AtomicsOr(elementType));
}

bool CacheIRCompiler::emitAtomicsXorResult(ObjOperandId objId,
                                           IntPtrOperandId indexId,
                                           uint32_t valueId,
                                           Scalar::Type elementType,
                                           bool forEffect) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  if (Scalar::isBigIntType(elementType)) {
    return emitAtomicsReadModifyWriteResult64<jit::AtomicsXor64>(objId, indexId,
                                                                 valueId);
  }
  return emitAtomicsReadModifyWriteResult(objId, indexId, valueId, elementType,
                                          AtomicsXor(elementType));
}

bool CacheIRCompiler::emitAtomicsLoadResult(ObjOperandId objId,
                                            IntPtrOperandId indexId,
                                            Scalar::Type elementType) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  Maybe<AutoOutputRegister> output;
  Maybe<AutoCallVM> callvm;
  if (!Scalar::isBigIntType(elementType)) {
    output.emplace(*this);
  } else {
    callvm.emplace(masm, this, allocator);
  }
  Register obj = allocator.useRegister(masm, objId);
  Register index = allocator.useRegister(masm, indexId);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm,
                                         output ? *output : callvm->output());
  AutoSpectreBoundsScratchRegister spectreTemp(allocator, masm);
  AutoAvailableFloatRegister floatReg(*this, FloatReg0);

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  // AutoCallVM's AutoSaveLiveRegisters aren't accounted for in FailurePath, so
  // we can't use both at the same time. This isn't an issue here, because Ion
  // doesn't support CallICs. If that ever changes, this code must be updated.
  MOZ_ASSERT(isBaseline(), "Can't use FailurePath with AutoCallVM in Ion ICs");

  // Bounds check.
  masm.loadArrayBufferViewLengthIntPtr(obj, scratch);
  masm.spectreBoundsCheckPtr(index, scratch, spectreTemp, failure->label());

  // Atomic operations are highly platform-dependent, for example x86/arm32 has
  // specific requirements on which registers are used. Therefore we're using a
  // VM call here instead of handling each platform separately.
  if (Scalar::isBigIntType(elementType)) {
    callvm->prepare();

    masm.Push(index);
    masm.Push(obj);

    using Fn = BigInt* (*)(JSContext*, TypedArrayObject*, size_t);
    callvm->call<Fn, jit::AtomicsLoad64>();
    return true;
  }

  // Load the elements vector.
  masm.loadPtr(Address(obj, ArrayBufferViewObject::dataOffset()), scratch);

  // Load the value.
  BaseIndex source(scratch, index, ScaleFromScalarType(elementType));

  auto sync = Synchronization::Load();

  masm.memoryBarrierBefore(sync);

  Label* failUint32 = nullptr;
  MacroAssembler::Uint32Mode mode = MacroAssembler::Uint32Mode::ForceDouble;
  masm.loadFromTypedArray(elementType, source, output->valueReg(), mode,
                          scratch, failUint32);
  masm.memoryBarrierAfter(sync);

  return true;
}

bool CacheIRCompiler::emitAtomicsStoreResult(ObjOperandId objId,
                                             IntPtrOperandId indexId,
                                             uint32_t valueId,
                                             Scalar::Type elementType) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  Register obj = allocator.useRegister(masm, objId);
  Register index = allocator.useRegister(masm, indexId);
  Maybe<Register> valueInt32;
  Maybe<Register> valueBigInt;
  if (!Scalar::isBigIntType(elementType)) {
    valueInt32.emplace(allocator.useRegister(masm, Int32OperandId(valueId)));
  } else {
    valueBigInt.emplace(allocator.useRegister(masm, BigIntOperandId(valueId)));
  }
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  // Not enough registers on X86.
  Register spectreTemp = Register::Invalid();

  FailurePath* failure;
  if (!addFailurePath(&failure)) {
    return false;
  }

  // Bounds check.
  masm.loadArrayBufferViewLengthIntPtr(obj, scratch);
  masm.spectreBoundsCheckPtr(index, scratch, spectreTemp, failure->label());

  if (!Scalar::isBigIntType(elementType)) {
    // Load the elements vector.
    masm.loadPtr(Address(obj, ArrayBufferViewObject::dataOffset()), scratch);

    // Store the value.
    BaseIndex dest(scratch, index, ScaleFromScalarType(elementType));

    auto sync = Synchronization::Store();

    masm.memoryBarrierBefore(sync);
    masm.storeToTypedIntArray(elementType, *valueInt32, dest);
    masm.memoryBarrierAfter(sync);

    masm.tagValue(JSVAL_TYPE_INT32, *valueInt32, output.valueReg());
  } else {
    // See comment in emitAtomicsCompareExchange for why we use an ABI call.

    LiveRegisterSet volatileRegs(GeneralRegisterSet::Volatile(),
                                 liveVolatileFloatRegs());
    volatileRegs.takeUnchecked(output.valueReg());
    volatileRegs.takeUnchecked(scratch);
    masm.PushRegsInMask(volatileRegs);

    using Fn = void (*)(TypedArrayObject*, size_t, BigInt*);
    masm.setupUnalignedABICall(scratch);
    masm.passABIArg(obj);
    masm.passABIArg(index);
    masm.passABIArg(*valueBigInt);
    masm.callWithABI<Fn, jit::AtomicsStore64>();

    masm.PopRegsInMask(volatileRegs);

    masm.tagValue(JSVAL_TYPE_BIGINT, *valueBigInt, output.valueReg());
  }

  return true;
}

bool CacheIRCompiler::emitAtomicsIsLockFreeResult(Int32OperandId valueId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);
  Register value = allocator.useRegister(masm, valueId);
  AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

  masm.atomicIsLockFreeJS(value, scratch);
  masm.tagValue(JSVAL_TYPE_BOOLEAN, scratch, output.valueReg());

  return true;
}

bool CacheIRCompiler::emitBigIntAsIntNResult(Int32OperandId bitsId,
                                             BigIntOperandId bigIntId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoCallVM callvm(masm, this, allocator);

  Register bits = allocator.useRegister(masm, bitsId);
  Register bigInt = allocator.useRegister(masm, bigIntId);

  callvm.prepare();
  masm.Push(bits);
  masm.Push(bigInt);

  using Fn = BigInt* (*)(JSContext*, HandleBigInt, int32_t);
  callvm.call<Fn, jit::BigIntAsIntN>();
  return true;
}

bool CacheIRCompiler::emitBigIntAsUintNResult(Int32OperandId bitsId,
                                              BigIntOperandId bigIntId) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoCallVM callvm(masm, this, allocator);

  Register bits = allocator.useRegister(masm, bitsId);
  Register bigInt = allocator.useRegister(masm, bigIntId);

  callvm.prepare();
  masm.Push(bits);
  masm.Push(bigInt);

  using Fn = BigInt* (*)(JSContext*, HandleBigInt, int32_t);
  callvm.call<Fn, jit::BigIntAsUintN>();
  return true;
}

bool CacheIRCompiler::emitBailout() {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  // Generates no code.

  return true;
}

bool CacheIRCompiler::emitAssertRecoveredOnBailoutResult(ValOperandId valId,
                                                         bool mustBeRecovered) {
  JitSpew(JitSpew_Codegen, "%s", __FUNCTION__);

  AutoOutputRegister output(*this);

  // NOP when not in IonMonkey
  masm.moveValue(UndefinedValue(), output.valueReg());

  return true;
}

template <typename Fn, Fn fn>
void CacheIRCompiler::callVM(MacroAssembler& masm) {
  VMFunctionId id = VMFunctionToId<Fn, fn>::id;
  callVMInternal(masm, id);
}

void CacheIRCompiler::callVMInternal(MacroAssembler& masm, VMFunctionId id) {
  if (mode_ == Mode::Ion) {
    MOZ_ASSERT(preparedForVMCall_);
    TrampolinePtr code = cx_->runtime()->jitRuntime()->getVMWrapper(id);
    const VMFunctionData& fun = GetVMFunction(id);
    uint32_t frameSize = fun.explicitStackSlots() * sizeof(void*);
    uint32_t descriptor = MakeFrameDescriptor(frameSize, FrameType::IonICCall,
                                              ExitFrameLayout::Size());
    masm.Push(Imm32(descriptor));
    masm.callJit(code);

    // Remove rest of the frame left on the stack. We remove the return address
    // which is implicitly popped when returning.
    int framePop = sizeof(ExitFrameLayout) - sizeof(void*);

    // Pop arguments from framePushed.
    masm.implicitPop(frameSize + framePop);
    masm.freeStack(IonICCallFrameLayout::Size());
    return;
  }

  MOZ_ASSERT(mode_ == Mode::Baseline);

  MOZ_ASSERT(preparedForVMCall_);

  TrampolinePtr code = cx_->runtime()->jitRuntime()->getVMWrapper(id);
  MOZ_ASSERT(GetVMFunction(id).expectTailCall == NonTailCall);

  EmitBaselineCallVM(code, masm);
}

bool CacheIRCompiler::isBaseline() { return mode_ == Mode::Baseline; }

bool CacheIRCompiler::isIon() { return mode_ == Mode::Ion; }

BaselineCacheIRCompiler* CacheIRCompiler::asBaseline() {
  MOZ_ASSERT(this->isBaseline());
  return static_cast<BaselineCacheIRCompiler*>(this);
}

IonCacheIRCompiler* CacheIRCompiler::asIon() {
  MOZ_ASSERT(this->isIon());
  return static_cast<IonCacheIRCompiler*>(this);
}

#ifdef DEBUG
void CacheIRCompiler::assertFloatRegisterAvailable(FloatRegister reg) {
  if (isBaseline()) {
    // Baseline does not have any FloatRegisters live when calling an IC stub.
    return;
  }

  asIon()->assertFloatRegisterAvailable(reg);
}
#endif

AutoCallVM::AutoCallVM(MacroAssembler& masm, CacheIRCompiler* compiler,
                       CacheRegisterAllocator& allocator)
    : masm_(masm), compiler_(compiler), allocator_(allocator) {
  // Ion needs to `prepareVMCall` before it can callVM and it also needs to
  // initialize AutoSaveLiveRegisters.
  if (compiler_->mode_ == CacheIRCompiler::Mode::Ion) {
    // Will need to use a downcast here as well, in order to pass the
    // stub to AutoSaveLiveRegisters
    save_.emplace(*compiler_->asIon());
  }

  output_.emplace(*compiler);

  if (compiler_->mode_ == CacheIRCompiler::Mode::Baseline) {
    stubFrame_.emplace(*compiler_->asBaseline());
    scratch_.emplace(allocator_, masm_, output_.ref());
  }
}

void AutoCallVM::prepare() {
  allocator_.discardStack(masm_);
  MOZ_ASSERT(compiler_ != nullptr);
  if (compiler_->mode_ == CacheIRCompiler::Mode::Ion) {
    compiler_->asIon()->prepareVMCall(masm_, *save_.ptr());
    return;
  }
  MOZ_ASSERT(compiler_->mode_ == CacheIRCompiler::Mode::Baseline);
  stubFrame_->enter(masm_, scratch_.ref());
}

void AutoCallVM::storeResult(JSValueType returnType) {
  MOZ_ASSERT(returnType != JSVAL_TYPE_DOUBLE);

  if (returnType == JSVAL_TYPE_UNKNOWN) {
    masm_.storeCallResultValue(output_.ref());
  } else {
    if (output_->hasValue()) {
      masm_.tagValue(returnType, ReturnReg, output_->valueReg());
    } else {
      Register out = output_->typedReg().gpr();
      if (out != ReturnReg) {
        masm_.mov(ReturnReg, out);
      }
    }
  }
}

void AutoCallVM::leaveBaselineStubFrame() {
  if (compiler_->mode_ == CacheIRCompiler::Mode::Baseline) {
    stubFrame_->leave(masm_);
  }
}

template <typename...>
struct VMFunctionReturnType;

template <class R, typename... Args>
struct VMFunctionReturnType<R (*)(JSContext*, Args...)> {
  using LastArgument = typename LastArg<Args...>::Type;

  // By convention VMFunctions returning `bool` use an output parameter.
  using ReturnType =
      std::conditional_t<std::is_same_v<R, bool>, LastArgument, R>;
};

template <class>
struct ReturnTypeToJSValueType;

// Definitions for the currently used return types.
template <>
struct ReturnTypeToJSValueType<MutableHandleValue> {
  static constexpr JSValueType result = JSVAL_TYPE_UNKNOWN;
};
template <>
struct ReturnTypeToJSValueType<bool*> {
  static constexpr JSValueType result = JSVAL_TYPE_BOOLEAN;
};
template <>
struct ReturnTypeToJSValueType<int32_t*> {
  static constexpr JSValueType result = JSVAL_TYPE_INT32;
};
template <>
struct ReturnTypeToJSValueType<JSString*> {
  static constexpr JSValueType result = JSVAL_TYPE_STRING;
};
template <>
struct ReturnTypeToJSValueType<BigInt*> {
  static constexpr JSValueType result = JSVAL_TYPE_BIGINT;
};
template <>
struct ReturnTypeToJSValueType<JSObject*> {
  static constexpr JSValueType result = JSVAL_TYPE_OBJECT;
};
template <>
struct ReturnTypeToJSValueType<ArrayIteratorObject*> {
  static constexpr JSValueType result = JSVAL_TYPE_OBJECT;
};
template <>
struct ReturnTypeToJSValueType<StringIteratorObject*> {
  static constexpr JSValueType result = JSVAL_TYPE_OBJECT;
};
template <>
struct ReturnTypeToJSValueType<RegExpStringIteratorObject*> {
  static constexpr JSValueType result = JSVAL_TYPE_OBJECT;
};
template <>
struct ReturnTypeToJSValueType<PlainObject*> {
  static constexpr JSValueType result = JSVAL_TYPE_OBJECT;
};
template <>
struct ReturnTypeToJSValueType<ArrayObject*> {
  static constexpr JSValueType result = JSVAL_TYPE_OBJECT;
};
template <>
struct ReturnTypeToJSValueType<TypedArrayObject*> {
  static constexpr JSValueType result = JSVAL_TYPE_OBJECT;
};

template <typename Fn>
void AutoCallVM::storeResult() {
  using ReturnType = typename VMFunctionReturnType<Fn>::ReturnType;
  storeResult(ReturnTypeToJSValueType<ReturnType>::result);
}

AutoScratchFloatRegister::AutoScratchFloatRegister(CacheIRCompiler* compiler,
                                                   FailurePath* failure)
    : compiler_(compiler), failure_(failure) {
  // If we're compiling a Baseline IC, FloatReg0 is always available.
  if (!compiler_->isBaseline()) {
    MacroAssembler& masm = compiler_->masm;
    masm.push(FloatReg0);
    compiler->allocator.setHasAutoScratchFloatRegisterSpill(true);
  }

  if (failure_) {
    failure_->setHasAutoScratchFloatRegister();
  }
}

AutoScratchFloatRegister::~AutoScratchFloatRegister() {
  if (failure_) {
    failure_->clearHasAutoScratchFloatRegister();
  }

  if (!compiler_->isBaseline()) {
    MacroAssembler& masm = compiler_->masm;
    masm.pop(FloatReg0);
    compiler_->allocator.setHasAutoScratchFloatRegisterSpill(false);

    if (failure_) {
      Label done;
      masm.jump(&done);
      masm.bind(&failurePopReg_);
      masm.pop(FloatReg0);
      masm.jump(failure_->label());
      masm.bind(&done);
    }
  }
}

Label* AutoScratchFloatRegister::failure() {
  MOZ_ASSERT(failure_);

  if (!compiler_->isBaseline()) {
    return &failurePopReg_;
  }
  return failure_->labelUnchecked();
}
