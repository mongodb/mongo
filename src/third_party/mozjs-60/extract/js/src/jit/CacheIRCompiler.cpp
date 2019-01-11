/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/CacheIRCompiler.h"

#include "jit/IonIC.h"
#include "jit/SharedICHelpers.h"

#include "jsboolinlines.h"

#include "jit/MacroAssembler-inl.h"
#include "vm/JSCompartment-inl.h"

using namespace js;
using namespace js::jit;

ValueOperand
CacheRegisterAllocator::useValueRegister(MacroAssembler& masm, ValOperandId op)
{
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
        masm.boxDouble(loc.doubleReg(), reg, ScratchDoubleReg);
        loc.setValueReg(reg);
        return reg;
      }

      case OperandLocation::Uninitialized:
        break;
    }

    MOZ_CRASH();
}

ValueOperand
CacheRegisterAllocator::useFixedValueRegister(MacroAssembler& masm, ValOperandId valId,
                                              ValueOperand reg)
{
    allocateFixedValueRegister(masm, reg);

    OperandLocation& loc = operandLocations_[valId.id()];
    switch (loc.kind()) {
      case OperandLocation::ValueReg:
        masm.moveValue(loc.valueReg(), reg);
        MOZ_ASSERT(!currentOpRegs_.aliases(loc.valueReg()), "Register shouldn't be in use");
        availableRegs_.add(loc.valueReg());
        break;
      case OperandLocation::ValueStack:
        popValue(masm, &loc, reg);
        break;
      case OperandLocation::BaselineFrame: {
        Address addr = addressOf(masm, loc.baselineFrameSlot());
        masm.loadValue(addr, reg);
        break;
      }
      case OperandLocation::Constant:
        masm.moveValue(loc.constant(), reg);
        break;
      case OperandLocation::PayloadReg:
        masm.tagValue(loc.payloadType(), loc.payloadReg(), reg);
        MOZ_ASSERT(!currentOpRegs_.has(loc.payloadReg()), "Register shouldn't be in use");
        availableRegs_.add(loc.payloadReg());
        break;
      case OperandLocation::PayloadStack:
        popPayload(masm, &loc, reg.scratchReg());
        masm.tagValue(loc.payloadType(), reg.scratchReg(), reg);
        break;
      case OperandLocation::DoubleReg:
        masm.boxDouble(loc.doubleReg(), reg, ScratchDoubleReg);
        break;
      case OperandLocation::Uninitialized:
        MOZ_CRASH();
    }

    loc.setValueReg(reg);
    return reg;
}

Register
CacheRegisterAllocator::useRegister(MacroAssembler& masm, TypedOperandId typedId)
{
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
            masm.unboxNonDouble(Address(masm.getStackPointer(), 0), reg, typedId.type());
            masm.addToStackPtr(Imm32(sizeof(js::Value)));
            MOZ_ASSERT(stackPushed_ >= sizeof(js::Value));
            stackPushed_ -= sizeof(js::Value);
        } else {
            MOZ_ASSERT(loc.valueStack() < stackPushed_);
            masm.unboxNonDouble(Address(masm.getStackPointer(), stackPushed_ - loc.valueStack()),
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
        if (v.isString())
            masm.movePtr(ImmGCPtr(v.toString()), reg);
        else if (v.isSymbol())
            masm.movePtr(ImmGCPtr(v.toSymbol()), reg);
        else
            MOZ_CRASH("Unexpected Value");
        loc.setPayloadReg(reg, v.extractNonDoubleType());
        return reg;
      }

      case OperandLocation::DoubleReg:
      case OperandLocation::Uninitialized:
        break;
    }

    MOZ_CRASH();
}

ConstantOrRegister
CacheRegisterAllocator::useConstantOrRegister(MacroAssembler& masm, ValOperandId val)
{
    OperandLocation& loc = operandLocations_[val.id()];
    switch (loc.kind()) {
      case OperandLocation::Constant:
        return loc.constant();

      case OperandLocation::PayloadReg:
      case OperandLocation::PayloadStack: {
        JSValueType payloadType = loc.payloadType();
        Register reg = useRegister(masm, TypedOperandId(val, payloadType));
        return TypedOrValueRegister(MIRTypeFromValueType(payloadType), AnyRegister(reg));
      }

      case OperandLocation::ValueReg:
      case OperandLocation::ValueStack:
      case OperandLocation::BaselineFrame:
        return TypedOrValueRegister(useValueRegister(masm, val));

      case OperandLocation::DoubleReg:
        return TypedOrValueRegister(MIRType::Double, AnyRegister(loc.doubleReg()));

      case OperandLocation::Uninitialized:
        break;
    }

    MOZ_CRASH();
}

Register
CacheRegisterAllocator::defineRegister(MacroAssembler& masm, TypedOperandId typedId)
{
    OperandLocation& loc = operandLocations_[typedId.id()];
    MOZ_ASSERT(loc.kind() == OperandLocation::Uninitialized);

    Register reg = allocateRegister(masm);
    loc.setPayloadReg(reg, typedId.type());
    return reg;
}

ValueOperand
CacheRegisterAllocator::defineValueRegister(MacroAssembler& masm, ValOperandId val)
{
    OperandLocation& loc = operandLocations_[val.id()];
    MOZ_ASSERT(loc.kind() == OperandLocation::Uninitialized);

    ValueOperand reg = allocateValueRegister(masm);
    loc.setValueReg(reg);
    return reg;
}

void
CacheRegisterAllocator::freeDeadOperandLocations(MacroAssembler& masm)
{
    // See if any operands are dead so we can reuse their registers. Note that
    // we skip the input operands, as those are also used by failure paths, and
    // we currently don't track those uses.
    for (size_t i = writer_.numInputOperands(); i < operandLocations_.length(); i++) {
        if (!writer_.operandIsDead(i, currentInstruction_))
            continue;

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

void
CacheRegisterAllocator::discardStack(MacroAssembler& masm)
{
    // This should only be called when we are no longer using the operands,
    // as we're discarding everything from the native stack. Set all operand
    // locations to Uninitialized to catch bugs.
    for (size_t i = 0; i < operandLocations_.length(); i++)
        operandLocations_[i].setUninitialized();

    if (stackPushed_ > 0) {
        masm.addToStackPtr(Imm32(stackPushed_));
        stackPushed_ = 0;
    }
    freePayloadSlots_.clear();
    freeValueSlots_.clear();
}

Register
CacheRegisterAllocator::allocateRegister(MacroAssembler& masm)
{
    if (availableRegs_.empty())
        freeDeadOperandLocations(masm);

    if (availableRegs_.empty()) {
        // Still no registers available, try to spill unused operands to
        // the stack.
        for (size_t i = 0; i < operandLocations_.length(); i++) {
            OperandLocation& loc = operandLocations_[i];
            if (loc.kind() == OperandLocation::PayloadReg) {
                Register reg = loc.payloadReg();
                if (currentOpRegs_.has(reg))
                    continue;

                spillOperandToStack(masm, &loc);
                availableRegs_.add(reg);
                break; // We got a register, so break out of the loop.
            }
            if (loc.kind() == OperandLocation::ValueReg) {
                ValueOperand reg = loc.valueReg();
                if (currentOpRegs_.aliases(reg))
                    continue;

                spillOperandToStack(masm, &loc);
                availableRegs_.add(reg);
                break; // Break out of the loop.
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

void
CacheRegisterAllocator::allocateFixedRegister(MacroAssembler& masm, Register reg)
{
    // Fixed registers should be allocated first, to ensure they're
    // still available.
    MOZ_ASSERT(!currentOpRegs_.has(reg), "Register is in use");

    freeDeadOperandLocations(masm);

    if (availableRegs_.has(reg)) {
        availableRegs_.take(reg);
        currentOpRegs_.add(reg);
        return;
    }

    // The register must be used by some operand. Spill it to the stack.
    for (size_t i = 0; i < operandLocations_.length(); i++) {
        OperandLocation& loc = operandLocations_[i];
        if (loc.kind() == OperandLocation::PayloadReg) {
            if (loc.payloadReg() != reg)
                continue;

            spillOperandToStackOrRegister(masm, &loc);
            currentOpRegs_.add(reg);
            return;
        }
        if (loc.kind() == OperandLocation::ValueReg) {
            if (!loc.valueReg().aliases(reg))
                continue;

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

void
CacheRegisterAllocator::allocateFixedValueRegister(MacroAssembler& masm, ValueOperand reg)
{
#ifdef JS_NUNBOX32
    allocateFixedRegister(masm, reg.payloadReg());
    allocateFixedRegister(masm, reg.typeReg());
#else
    allocateFixedRegister(masm, reg.valueReg());
#endif
}

ValueOperand
CacheRegisterAllocator::allocateValueRegister(MacroAssembler& masm)
{
#ifdef JS_NUNBOX32
    Register reg1 = allocateRegister(masm);
    Register reg2 = allocateRegister(masm);
    return ValueOperand(reg1, reg2);
#else
    Register reg = allocateRegister(masm);
    return ValueOperand(reg);
#endif
}

bool
CacheRegisterAllocator::init()
{
    if (!origInputLocations_.resize(writer_.numInputOperands()))
        return false;
    if (!operandLocations_.resize(writer_.numOperandIds()))
        return false;
    return true;
}

void
CacheRegisterAllocator::initAvailableRegsAfterSpill()
{
    // Registers not in availableRegs_ and not used by input operands are
    // available after being spilled.
    availableRegsAfterSpill_.set() =
        GeneralRegisterSet::Intersect(GeneralRegisterSet::Not(availableRegs_.set()),
                                      GeneralRegisterSet::Not(inputRegisterSet()));
}

void
CacheRegisterAllocator::fixupAliasedInputs(MacroAssembler& masm)
{
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
        if (!loc1.isInRegister())
            continue;

        for (size_t j = 0; j < i; j++) {
            OperandLocation& loc2 = operandLocations_[j];
            if (!loc1.aliasesReg(loc2))
                continue;

            // loc1 and loc2 alias so we spill one of them. If one is a
            // ValueReg and the other is a PayloadReg, we have to spill the
            // PayloadReg: spilling the ValueReg instead would leave its type
            // register unallocated on 32-bit platforms.
            if (loc1.kind() == OperandLocation::ValueReg) {
                spillOperandToStack(masm, &loc2);
            } else {
                MOZ_ASSERT(loc1.kind() == OperandLocation::PayloadReg);
                spillOperandToStack(masm, &loc1);
                break; // Spilled loc1, so nothing else will alias it.
            }
        }
    }
}

GeneralRegisterSet
CacheRegisterAllocator::inputRegisterSet() const
{
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

JSValueType
CacheRegisterAllocator::knownType(ValOperandId val) const
{
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
        return loc.constant().isDouble()
               ? JSVAL_TYPE_DOUBLE
               : loc.constant().extractNonDoubleType();

      case OperandLocation::DoubleReg:
        return JSVAL_TYPE_DOUBLE;

      case OperandLocation::Uninitialized:
        break;
    }

    MOZ_CRASH("Invalid kind");
}

void
CacheRegisterAllocator::initInputLocation(size_t i, const TypedOrValueRegister& reg)
{
    if (reg.hasValue()) {
        initInputLocation(i, reg.valueReg());
    } else if (reg.typedReg().isFloat()) {
        MOZ_ASSERT(reg.type() == MIRType::Double);
        initInputLocation(i, reg.typedReg().fpu());
    } else {
        initInputLocation(i, reg.typedReg().gpr(), ValueTypeFromMIRType(reg.type()));
    }
}

void
CacheRegisterAllocator::initInputLocation(size_t i, const ConstantOrRegister& value)
{
    if (value.constant())
        initInputLocation(i, value.value());
    else
        initInputLocation(i, value.reg());
}

void
CacheRegisterAllocator::spillOperandToStack(MacroAssembler& masm, OperandLocation* loc)
{
    MOZ_ASSERT(loc >= operandLocations_.begin() && loc < operandLocations_.end());

    if (loc->kind() == OperandLocation::ValueReg) {
        if (!freeValueSlots_.empty()) {
            uint32_t stackPos = freeValueSlots_.popCopy();
            MOZ_ASSERT(stackPos <= stackPushed_);
            masm.storeValue(loc->valueReg(), Address(masm.getStackPointer(),
                                                     stackPushed_ - stackPos));
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
        masm.storePtr(loc->payloadReg(), Address(masm.getStackPointer(),
                                                 stackPushed_ - stackPos));
        loc->setPayloadStack(stackPos, loc->payloadType());
        return;
    }
    stackPushed_ += sizeof(uintptr_t);
    masm.push(loc->payloadReg());
    loc->setPayloadStack(stackPushed_, loc->payloadType());
}

void
CacheRegisterAllocator::spillOperandToStackOrRegister(MacroAssembler& masm, OperandLocation* loc)
{
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

void
CacheRegisterAllocator::popPayload(MacroAssembler& masm, OperandLocation* loc, Register dest)
{
    MOZ_ASSERT(loc >= operandLocations_.begin() && loc < operandLocations_.end());
    MOZ_ASSERT(stackPushed_ >= sizeof(uintptr_t));

    // The payload is on the stack. If it's on top of the stack we can just
    // pop it, else we emit a load.
    if (loc->payloadStack() == stackPushed_) {
        masm.pop(dest);
        stackPushed_ -= sizeof(uintptr_t);
    } else {
        MOZ_ASSERT(loc->payloadStack() < stackPushed_);
        masm.loadPtr(Address(masm.getStackPointer(), stackPushed_ - loc->payloadStack()), dest);
        masm.propagateOOM(freePayloadSlots_.append(loc->payloadStack()));
    }

    loc->setPayloadReg(dest, loc->payloadType());
}

void
CacheRegisterAllocator::popValue(MacroAssembler& masm, OperandLocation* loc, ValueOperand dest)
{
    MOZ_ASSERT(loc >= operandLocations_.begin() && loc < operandLocations_.end());
    MOZ_ASSERT(stackPushed_ >= sizeof(js::Value));

    // The Value is on the stack. If it's on top of the stack we can just
    // pop it, else we emit a load.
    if (loc->valueStack() == stackPushed_) {
        masm.popValue(dest);
        stackPushed_ -= sizeof(js::Value);
    } else {
        MOZ_ASSERT(loc->valueStack() < stackPushed_);
        masm.loadValue(Address(masm.getStackPointer(), stackPushed_ - loc->valueStack()), dest);
        masm.propagateOOM(freeValueSlots_.append(loc->valueStack()));
    }

    loc->setValueReg(dest);
}

bool
OperandLocation::aliasesReg(const OperandLocation& other) const
{
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

void
CacheRegisterAllocator::restoreInputState(MacroAssembler& masm, bool shouldDiscardStack)
{
    size_t numInputOperands = origInputLocations_.length();
    MOZ_ASSERT(writer_.numInputOperands() == numInputOperands);

    for (size_t j = 0; j < numInputOperands; j++) {
        const OperandLocation& dest = origInputLocations_[j];
        OperandLocation& cur = operandLocations_[j];
        if (dest == cur)
            continue;

        auto autoAssign = mozilla::MakeScopeExit([&] { cur = dest; });

        // We have a cycle if a destination register will be used later
        // as source register. If that happens, just push the current value
        // on the stack and later get it from there.
        for (size_t k = j + 1; k < numInputOperands; k++) {
            OperandLocation& laterSource = operandLocations_[k];
            if (dest.aliasesReg(laterSource))
                spillOperandToStack(masm, &laterSource);
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
              case OperandLocation::Constant:
              case OperandLocation::BaselineFrame:
              case OperandLocation::DoubleReg:
              case OperandLocation::Uninitialized:
                break;
            }
        } else if (dest.kind() == OperandLocation::PayloadReg) {
            // We have to restore a payload register.
            switch (cur.kind()) {
              case OperandLocation::ValueReg:
                MOZ_ASSERT(dest.payloadType() != JSVAL_TYPE_DOUBLE);
                masm.unboxNonDouble(cur.valueReg(), dest.payloadReg(), dest.payloadType());
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
                masm.unboxNonDouble(Address(masm.getStackPointer(), stackPushed_ - cur.valueStack()),
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
                   dest.kind() == OperandLocation::DoubleReg)
        {
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
            masm.loadPtr(Address(masm.getStackPointer(), stackPushed_ - spill.stackPushed),
                         spill.reg);
        }
    }

    if (shouldDiscardStack)
        discardStack(masm);
}

size_t
CacheIRStubInfo::stubDataSize() const
{
    size_t field = 0;
    size_t size = 0;
    while (true) {
        StubField::Type type = fieldType(field++);
        if (type == StubField::Type::Limit)
            return size;
        size += StubField::sizeInBytes(type);
    }
}

void
CacheIRStubInfo::copyStubData(ICStub* src, ICStub* dest) const
{
    uint8_t* srcBytes = reinterpret_cast<uint8_t*>(src);
    uint8_t* destBytes = reinterpret_cast<uint8_t*>(dest);

    size_t field = 0;
    size_t offset = 0;
    while (true) {
        StubField::Type type = fieldType(field);
        switch (type) {
          case StubField::Type::RawWord:
            *reinterpret_cast<uintptr_t*>(destBytes + offset) =
                *reinterpret_cast<uintptr_t*>(srcBytes + offset);
            break;
          case StubField::Type::RawInt64:
          case StubField::Type::DOMExpandoGeneration:
            *reinterpret_cast<uint64_t*>(destBytes + offset) =
                *reinterpret_cast<uint64_t*>(srcBytes + offset);
            break;
          case StubField::Type::Shape:
            getStubField<ICStub, Shape*>(dest, offset).init(getStubField<ICStub, Shape*>(src, offset));
            break;
          case StubField::Type::JSObject:
            getStubField<ICStub, JSObject*>(dest, offset).init(getStubField<ICStub, JSObject*>(src, offset));
            break;
          case StubField::Type::ObjectGroup:
            getStubField<ICStub, ObjectGroup*>(dest, offset).init(getStubField<ICStub, ObjectGroup*>(src, offset));
            break;
          case StubField::Type::Symbol:
            getStubField<ICStub, JS::Symbol*>(dest, offset).init(getStubField<ICStub, JS::Symbol*>(src, offset));
            break;
          case StubField::Type::String:
            getStubField<ICStub, JSString*>(dest, offset).init(getStubField<ICStub, JSString*>(src, offset));
            break;
          case StubField::Type::Id:
            getStubField<ICStub, jsid>(dest, offset).init(getStubField<ICStub, jsid>(src, offset));
            break;
          case StubField::Type::Value:
            getStubField<ICStub, Value>(dest, offset).init(getStubField<ICStub, Value>(src, offset));
            break;
          case StubField::Type::Limit:
            return; // Done.
        }
        field++;
        offset += StubField::sizeInBytes(type);
    }
}

template <typename T>
static GCPtr<T>*
AsGCPtr(uintptr_t* ptr)
{
    return reinterpret_cast<GCPtr<T>*>(ptr);
}

uintptr_t
CacheIRStubInfo::getStubRawWord(ICStub* stub, uint32_t offset) const {
    uint8_t* stubData = (uint8_t*)stub + stubDataOffset_;
    MOZ_ASSERT(uintptr_t(stubData) % sizeof(uintptr_t) == 0);
    return *(uintptr_t*)(stubData + offset);
}

template<class Stub, class T>
GCPtr<T>&
CacheIRStubInfo::getStubField(Stub* stub, uint32_t offset) const
{
    uint8_t* stubData = (uint8_t*)stub + stubDataOffset_;
    MOZ_ASSERT(uintptr_t(stubData) % sizeof(uintptr_t) == 0);

    return *AsGCPtr<T>((uintptr_t*)(stubData + offset));
}

template GCPtr<Shape*>& CacheIRStubInfo::getStubField<ICStub>(ICStub* stub, uint32_t offset) const;
template GCPtr<ObjectGroup*>& CacheIRStubInfo::getStubField<ICStub>(ICStub* stub, uint32_t offset) const;
template GCPtr<JSObject*>& CacheIRStubInfo::getStubField<ICStub>(ICStub* stub, uint32_t offset) const;
template GCPtr<JSString*>& CacheIRStubInfo::getStubField<ICStub>(ICStub* stub, uint32_t offset) const;
template GCPtr<JS::Symbol*>& CacheIRStubInfo::getStubField<ICStub>(ICStub* stub, uint32_t offset) const;
template GCPtr<JS::Value>& CacheIRStubInfo::getStubField<ICStub>(ICStub* stub, uint32_t offset) const;
template GCPtr<jsid>& CacheIRStubInfo::getStubField<ICStub>(ICStub* stub, uint32_t offset) const;

template <typename T, typename V>
static void
InitGCPtr(uintptr_t* ptr, V val)
{
    AsGCPtr<T>(ptr)->init(mozilla::BitwiseCast<T>(val));
}

void
CacheIRWriter::copyStubData(uint8_t* dest) const
{
    MOZ_ASSERT(!failed());

    uintptr_t* destWords = reinterpret_cast<uintptr_t*>(dest);

    for (const StubField& field : stubFields_) {
        switch (field.type()) {
          case StubField::Type::RawWord:
            *destWords = field.asWord();
            break;
          case StubField::Type::Shape:
            InitGCPtr<Shape*>(destWords, field.asWord());
            break;
          case StubField::Type::JSObject:
            InitGCPtr<JSObject*>(destWords, field.asWord());
            break;
          case StubField::Type::ObjectGroup:
            InitGCPtr<ObjectGroup*>(destWords, field.asWord());
            break;
          case StubField::Type::Symbol:
            InitGCPtr<JS::Symbol*>(destWords, field.asWord());
            break;
          case StubField::Type::String:
            InitGCPtr<JSString*>(destWords, field.asWord());
            break;
          case StubField::Type::Id:
            InitGCPtr<jsid>(destWords, field.asWord());
            break;
          case StubField::Type::RawInt64:
          case StubField::Type::DOMExpandoGeneration:
            *reinterpret_cast<uint64_t*>(destWords) = field.asInt64();
            break;
          case StubField::Type::Value:
            AsGCPtr<Value>(destWords)->init(Value::fromRawBits(uint64_t(field.asInt64())));
            break;
          case StubField::Type::Limit:
            MOZ_CRASH("Invalid type");
        }
        destWords += StubField::sizeInBytes(field.type()) / sizeof(uintptr_t);
    }
}

template <typename T>
void
jit::TraceCacheIRStub(JSTracer* trc, T* stub, const CacheIRStubInfo* stubInfo)
{
    uint32_t field = 0;
    size_t offset = 0;
    while (true) {
        StubField::Type fieldType = stubInfo->fieldType(field);
        switch (fieldType) {
          case StubField::Type::RawWord:
          case StubField::Type::RawInt64:
          case StubField::Type::DOMExpandoGeneration:
            break;
          case StubField::Type::Shape:
            TraceNullableEdge(trc, &stubInfo->getStubField<T, Shape*>(stub, offset),
                              "cacheir-shape");
            break;
          case StubField::Type::ObjectGroup:
            TraceNullableEdge(trc, &stubInfo->getStubField<T, ObjectGroup*>(stub, offset),
                              "cacheir-group");
            break;
          case StubField::Type::JSObject:
            TraceNullableEdge(trc, &stubInfo->getStubField<T, JSObject*>(stub, offset),
                              "cacheir-object");
            break;
          case StubField::Type::Symbol:
            TraceNullableEdge(trc, &stubInfo->getStubField<T, JS::Symbol*>(stub, offset),
                              "cacheir-symbol");
            break;
          case StubField::Type::String:
            TraceNullableEdge(trc, &stubInfo->getStubField<T, JSString*>(stub, offset),
                              "cacheir-string");
            break;
          case StubField::Type::Id:
            TraceEdge(trc, &stubInfo->getStubField<T, jsid>(stub, offset), "cacheir-id");
            break;
          case StubField::Type::Value:
            TraceEdge(trc, &stubInfo->getStubField<T, JS::Value>(stub, offset),
                      "cacheir-value");
            break;
          case StubField::Type::Limit:
            return; // Done.
        }
        field++;
        offset += StubField::sizeInBytes(fieldType);
    }
}

template
void jit::TraceCacheIRStub(JSTracer* trc, ICStub* stub, const CacheIRStubInfo* stubInfo);

template
void jit::TraceCacheIRStub(JSTracer* trc, IonICStub* stub, const CacheIRStubInfo* stubInfo);

bool
CacheIRWriter::stubDataEqualsMaybeUpdate(uint8_t* stubData, bool* updated) const
{
    MOZ_ASSERT(!failed());

    *updated = false;
    const uintptr_t* stubDataWords = reinterpret_cast<const uintptr_t*>(stubData);

    // If DOMExpandoGeneration fields are different but all other stub fields
    // are exactly the same, we overwrite the old stub data instead of attaching
    // a new stub, as the old stub is never going to succeed. This works because
    // even Ion stubs read the DOMExpandoGeneration field from the stub instead
    // of baking it in.
    bool expandoGenerationIsDifferent = false;

    for (const StubField& field : stubFields_) {
        if (field.sizeIsWord()) {
            if (field.asWord() != *stubDataWords)
                return false;
            stubDataWords++;
            continue;
        }

        if (field.asInt64() != *reinterpret_cast<const uint64_t*>(stubDataWords)) {
            if (field.type() != StubField::Type::DOMExpandoGeneration)
                return false;
            expandoGenerationIsDifferent = true;
        }
        stubDataWords += sizeof(uint64_t) / sizeof(uintptr_t);
    }

    if (expandoGenerationIsDifferent) {
        copyStubData(stubData);
        *updated = true;
    }

    return true;
}

HashNumber
CacheIRStubKey::hash(const CacheIRStubKey::Lookup& l)
{
    HashNumber hash = mozilla::HashBytes(l.code, l.length);
    hash = mozilla::AddToHash(hash, uint32_t(l.kind));
    hash = mozilla::AddToHash(hash, uint32_t(l.engine));
    return hash;
}

bool
CacheIRStubKey::match(const CacheIRStubKey& entry, const CacheIRStubKey::Lookup& l)
{
    if (entry.stubInfo->kind() != l.kind)
        return false;

    if (entry.stubInfo->engine() != l.engine)
        return false;

    if (entry.stubInfo->codeLength() != l.length)
        return false;

    if (!mozilla::PodEqual(entry.stubInfo->code(), l.code, l.length))
        return false;

    return true;
}

CacheIRReader::CacheIRReader(const CacheIRStubInfo* stubInfo)
  : CacheIRReader(stubInfo->code(), stubInfo->code() + stubInfo->codeLength())
{}

CacheIRStubInfo*
CacheIRStubInfo::New(CacheKind kind, ICStubEngine engine, bool makesGCCalls,
                     uint32_t stubDataOffset, const CacheIRWriter& writer)
{
    size_t numStubFields = writer.numStubFields();
    size_t bytesNeeded = sizeof(CacheIRStubInfo) +
                         writer.codeLength() +
                         (numStubFields + 1); // +1 for the GCType::Limit terminator.
    uint8_t* p = js_pod_malloc<uint8_t>(bytesNeeded);
    if (!p)
        return nullptr;

    // Copy the CacheIR code.
    uint8_t* codeStart = p + sizeof(CacheIRStubInfo);
    mozilla::PodCopy(codeStart, writer.codeStart(), writer.codeLength());

    static_assert(sizeof(StubField::Type) == sizeof(uint8_t),
                  "StubField::Type must fit in uint8_t");

    // Copy the stub field types.
    uint8_t* fieldTypes = codeStart + writer.codeLength();
    for (size_t i = 0; i < numStubFields; i++)
        fieldTypes[i] = uint8_t(writer.stubFieldType(i));
    fieldTypes[numStubFields] = uint8_t(StubField::Type::Limit);

    return new(p) CacheIRStubInfo(kind, engine, makesGCCalls, stubDataOffset, codeStart,
                                  writer.codeLength(), fieldTypes);
}

bool
OperandLocation::operator==(const OperandLocation& other) const
{
    if (kind_ != other.kind_)
        return false;

    switch (kind()) {
      case Uninitialized:
        return true;
      case PayloadReg:
        return payloadReg() == other.payloadReg() && payloadType() == other.payloadType();
      case ValueReg:
        return valueReg() == other.valueReg();
      case PayloadStack:
        return payloadStack() == other.payloadStack() && payloadType() == other.payloadType();
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
  : output_(compiler.outputUnchecked_.ref()),
    alloc_(compiler.allocator)
{
    if (output_.hasValue())
        alloc_.allocateFixedValueRegister(compiler.masm, output_.valueReg());
    else if (!output_.typedReg().isFloat())
        alloc_.allocateFixedRegister(compiler.masm, output_.typedReg().gpr());
}

AutoOutputRegister::~AutoOutputRegister()
{
    if (output_.hasValue())
        alloc_.releaseValueRegister(output_.valueReg());
    else if (!output_.typedReg().isFloat())
        alloc_.releaseRegister(output_.typedReg().gpr());
}

bool
FailurePath::canShareFailurePath(const FailurePath& other) const
{
    if (stackPushed_ != other.stackPushed_)
        return false;

    if (spilledRegs_.length() != other.spilledRegs_.length())
        return false;

    for (size_t i = 0; i < spilledRegs_.length(); i++) {
        if (spilledRegs_[i] != other.spilledRegs_[i])
            return false;
    }

    MOZ_ASSERT(inputs_.length() == other.inputs_.length());

    for (size_t i = 0; i < inputs_.length(); i++) {
        if (inputs_[i] != other.inputs_[i])
            return false;
    }
    return true;
}

bool
CacheIRCompiler::addFailurePath(FailurePath** failure)
{
    FailurePath newFailure;
    for (size_t i = 0; i < writer_.numInputOperands(); i++) {
        if (!newFailure.appendInput(allocator.operandLocation(i)))
            return false;
    }
    if (!newFailure.setSpilledRegs(allocator.spilledRegs()))
        return false;
    newFailure.setStackPushed(allocator.stackPushed());

    // Reuse the previous failure path if the current one is the same, to
    // avoid emitting duplicate code.
    if (failurePaths.length() > 0 && failurePaths.back().canShareFailurePath(newFailure)) {
        *failure = &failurePaths.back();
        return true;
    }

    if (!failurePaths.append(Move(newFailure)))
        return false;

    *failure = &failurePaths.back();
    return true;
}

bool
CacheIRCompiler::emitFailurePath(size_t index)
{
    FailurePath& failure = failurePaths[index];

    allocator.setStackPushed(failure.stackPushed());

    for (size_t i = 0; i < writer_.numInputOperands(); i++)
        allocator.setOperandLocation(i, failure.input(i));

    if (!allocator.setSpilledRegs(failure.spilledRegs()))
        return false;

    masm.bind(failure.label());
    allocator.restoreInputState(masm);
    return true;
}

bool
CacheIRCompiler::emitGuardIsNumber()
{
    ValOperandId inputId = reader.valOperandId();
    JSValueType knownType = allocator.knownType(inputId);

    // Doubles and ints are numbers!
    if (knownType == JSVAL_TYPE_DOUBLE || knownType == JSVAL_TYPE_INT32)
        return true;

    ValueOperand input = allocator.useValueRegister(masm, inputId);
    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    masm.branchTestNumber(Assembler::NotEqual, input, failure->label());
    return true;
}

bool
CacheIRCompiler::emitGuardIsObject()
{
    ValOperandId inputId = reader.valOperandId();
    if (allocator.knownType(inputId) == JSVAL_TYPE_OBJECT)
        return true;

    ValueOperand input = allocator.useValueRegister(masm, inputId);
    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;
    masm.branchTestObject(Assembler::NotEqual, input, failure->label());
    return true;
}

bool
CacheIRCompiler::emitGuardIsNullOrUndefined()
{
    ValOperandId inputId = reader.valOperandId();
    JSValueType knownType = allocator.knownType(inputId);
    if (knownType == JSVAL_TYPE_UNDEFINED || knownType == JSVAL_TYPE_NULL)
        return true;

    ValueOperand input = allocator.useValueRegister(masm, inputId);
    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    Label success;
    masm.branchTestNull(Assembler::Equal, input, &success);
    masm.branchTestUndefined(Assembler::NotEqual, input, failure->label());

    masm.bind(&success);
    return true;
}

bool
CacheIRCompiler::emitGuardIsObjectOrNull()
{
    ValOperandId inputId = reader.valOperandId();
    JSValueType knownType = allocator.knownType(inputId);
    if (knownType == JSVAL_TYPE_OBJECT || knownType == JSVAL_TYPE_NULL)
        return true;

    ValueOperand input = allocator.useValueRegister(masm, inputId);
    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    Label done;
    masm.branchTestObject(Assembler::Equal, input, &done);
    masm.branchTestNull(Assembler::NotEqual, input, failure->label());
    masm.bind(&done);
    return true;
}

bool
CacheIRCompiler::emitGuardIsString()
{
    ValOperandId inputId = reader.valOperandId();
    if (allocator.knownType(inputId) == JSVAL_TYPE_STRING)
        return true;

    ValueOperand input = allocator.useValueRegister(masm, inputId);
    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;
    masm.branchTestString(Assembler::NotEqual, input, failure->label());
    return true;
}

bool
CacheIRCompiler::emitGuardIsSymbol()
{
    ValOperandId inputId = reader.valOperandId();
    if (allocator.knownType(inputId) == JSVAL_TYPE_SYMBOL)
        return true;

    ValueOperand input = allocator.useValueRegister(masm, inputId);
    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;
    masm.branchTestSymbol(Assembler::NotEqual, input, failure->label());
    return true;
}

bool
CacheIRCompiler::emitGuardIsInt32Index()
{
    ValOperandId inputId = reader.valOperandId();
    Register output = allocator.defineRegister(masm, reader.int32OperandId());

    if (allocator.knownType(inputId) == JSVAL_TYPE_INT32) {
        Register input = allocator.useRegister(masm, Int32OperandId(inputId.id()));
        masm.move32(input, output);
        return true;
    }

    ValueOperand input = allocator.useValueRegister(masm, inputId);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    Label notInt32, done;
    masm.branchTestInt32(Assembler::NotEqual, input, &notInt32);
    masm.unboxInt32(input, output);
    masm.jump(&done);

    masm.bind(&notInt32);

    if (cx_->runtime()->jitSupportsFloatingPoint) {
        masm.branchTestDouble(Assembler::NotEqual, input, failure->label());

        // If we're compiling a Baseline IC, FloatReg0 is always available.
        Label failurePopReg;
        if (mode_ != Mode::Baseline)
            masm.push(FloatReg0);

        masm.unboxDouble(input, FloatReg0);
        // ToPropertyKey(-0.0) is "0", so we can truncate -0.0 to 0 here.
        masm.convertDoubleToInt32(FloatReg0, output,
                                  (mode_ == Mode::Baseline) ? failure->label() : &failurePopReg,
                                  false);
        if (mode_ != Mode::Baseline) {
            masm.pop(FloatReg0);
            masm.jump(&done);

            masm.bind(&failurePopReg);
            masm.pop(FloatReg0);
            masm.jump(failure->label());
        }
    } else {
        masm.jump(failure->label());
    }

    masm.bind(&done);
    return true;
}

bool
CacheIRCompiler::emitGuardType()
{
    ValOperandId inputId = reader.valOperandId();
    JSValueType type = reader.valueType();

    if (allocator.knownType(inputId) == type)
        return true;

    ValueOperand input = allocator.useValueRegister(masm, inputId);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    switch (type) {
      case JSVAL_TYPE_STRING:
        masm.branchTestString(Assembler::NotEqual, input, failure->label());
        break;
      case JSVAL_TYPE_SYMBOL:
        masm.branchTestSymbol(Assembler::NotEqual, input, failure->label());
        break;
      case JSVAL_TYPE_INT32:
        masm.branchTestInt32(Assembler::NotEqual, input, failure->label());
        break;
      case JSVAL_TYPE_DOUBLE:
        masm.branchTestDouble(Assembler::NotEqual, input, failure->label());
        break;
      case JSVAL_TYPE_BOOLEAN:
        masm.branchTestBoolean(Assembler::NotEqual, input, failure->label());
        break;
      case JSVAL_TYPE_UNDEFINED:
        masm.branchTestUndefined(Assembler::NotEqual, input, failure->label());
        break;
      case JSVAL_TYPE_NULL:
        masm.branchTestNull(Assembler::NotEqual, input, failure->label());
        break;
      default:
        MOZ_CRASH("Unexpected type");
    }

    return true;
}

bool
CacheIRCompiler::emitGuardClass()
{
    ObjOperandId objId = reader.objOperandId();
    Register obj = allocator.useRegister(masm, objId);
    AutoScratchRegister scratch(allocator, masm);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    const Class* clasp = nullptr;
    switch (reader.guardClassKind()) {
      case GuardClassKind::Array:
        clasp = &ArrayObject::class_;
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
        masm.branchTestObjClass(Assembler::NotEqual, obj, clasp, scratch, obj, failure->label());
    } else {
        masm.branchTestObjClassNoSpectreMitigations(Assembler::NotEqual, obj, clasp, scratch,
                                                    failure->label());
    }

    return true;
}

bool
CacheIRCompiler::emitGuardIsNativeFunction()
{
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    JSNative nativeFunc = reinterpret_cast<JSNative>(reader.pointer());
    AutoScratchRegister scratch(allocator, masm);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    // Ensure obj is a function.
    const Class* clasp = &JSFunction::class_;
    masm.branchTestObjClass(Assembler::NotEqual, obj, clasp, scratch, obj, failure->label());

    // Ensure function native matches.
    masm.branchPtr(Assembler::NotEqual, Address(obj, JSFunction::offsetOfNativeOrEnv()),
                   ImmPtr(nativeFunc), failure->label());
    return true;
}

bool
CacheIRCompiler::emitGuardIsNativeObject()
{
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    AutoScratchRegister scratch(allocator, masm);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    masm.branchIfNonNativeObj(obj, scratch, failure->label());
    return true;
}

bool
CacheIRCompiler::emitGuardIsProxy()
{
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    AutoScratchRegister scratch(allocator, masm);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    masm.branchTestObjectIsProxy(false, obj, scratch, failure->label());
    return true;
}

bool
CacheIRCompiler::emitGuardNotDOMProxy()
{
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    AutoScratchRegister scratch(allocator, masm);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    masm.branchTestProxyHandlerFamily(Assembler::Equal, obj, scratch,
                                      GetDOMProxyHandlerFamily(), failure->label());
    return true;
}

bool
CacheIRCompiler::emitGuardSpecificInt32Immediate()
{
    Register reg = allocator.useRegister(masm, reader.int32OperandId());
    int32_t ival = reader.int32Immediate();
    Assembler::Condition cond = (Assembler::Condition) reader.readByte();

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    masm.branch32(Assembler::InvertCondition(cond), reg, Imm32(ival), failure->label());
    return true;
}

bool
CacheIRCompiler::emitGuardMagicValue()
{
    ValueOperand val = allocator.useValueRegister(masm, reader.valOperandId());
    JSWhyMagic magic = reader.whyMagic();

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    masm.branchTestMagicValue(Assembler::NotEqual, val, magic, failure->label());
    return true;
}

bool
CacheIRCompiler::emitGuardNoUnboxedExpando()
{
    Register obj = allocator.useRegister(masm, reader.objOperandId());

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    Address expandoAddr(obj, UnboxedPlainObject::offsetOfExpando());
    masm.branchPtr(Assembler::NotEqual, expandoAddr, ImmWord(0), failure->label());
    return true;
}

bool
CacheIRCompiler::emitGuardAndLoadUnboxedExpando()
{
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    Register output = allocator.defineRegister(masm, reader.objOperandId());

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    Address expandoAddr(obj, UnboxedPlainObject::offsetOfExpando());
    masm.loadPtr(expandoAddr, output);
    masm.branchTestPtr(Assembler::Zero, output, output, failure->label());
    return true;
}

bool
CacheIRCompiler::emitGuardNoDetachedTypedObjects()
{
    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    // All stubs manipulating typed objects must check the compartment-wide
    // flag indicating whether their underlying storage might be detached, to
    // bail out if needed.
    int32_t* address = &cx_->compartment()->detachedTypedObjects;
    masm.branch32(Assembler::NotEqual, AbsoluteAddress(address), Imm32(0), failure->label());
    return true;
}

bool
CacheIRCompiler::emitGuardNoDenseElements()
{
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    AutoScratchRegister scratch(allocator, masm);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    // Load obj->elements.
    masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), scratch);

    // Make sure there are no dense elements.
    Address initLength(scratch, ObjectElements::offsetOfInitializedLength());
    masm.branch32(Assembler::NotEqual, initLength, Imm32(0), failure->label());
    return true;
}

bool
CacheIRCompiler::emitGuardAndGetIndexFromString()
{
    Register str = allocator.useRegister(masm, reader.stringOperandId());
    Register output = allocator.defineRegister(masm, reader.int32OperandId());

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    Label vmCall, done;
    masm.loadStringIndexValue(str, output, &vmCall);
    masm.jump(&done);

    {
        masm.bind(&vmCall);
        LiveRegisterSet save(GeneralRegisterSet::Volatile(), liveVolatileFloatRegs());
        masm.PushRegsInMask(save);

        masm.setupUnalignedABICall(output);
        masm.passABIArg(str);
        masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, GetIndexFromString));
        masm.mov(ReturnReg, output);

        LiveRegisterSet ignore;
        ignore.add(output);
        masm.PopRegsInMaskIgnore(save, ignore);

        // GetIndexFromString returns a negative value on failure.
        masm.branchTest32(Assembler::Signed, output, output, failure->label());
    }

    masm.bind(&done);
    return true;
}

bool
CacheIRCompiler::emitLoadProto()
{
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    Register reg = allocator.defineRegister(masm, reader.objOperandId());
    masm.loadObjProto(obj, reg);
    return true;
}

bool
CacheIRCompiler::emitLoadEnclosingEnvironment()
{
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    Register reg = allocator.defineRegister(masm, reader.objOperandId());
    masm.extractObject(Address(obj, EnvironmentObject::offsetOfEnclosingEnvironment()), reg);
    return true;
}

bool
CacheIRCompiler::emitLoadWrapperTarget()
{
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    Register reg = allocator.defineRegister(masm, reader.objOperandId());

    masm.loadPtr(Address(obj, ProxyObject::offsetOfReservedSlots()), reg);
    masm.unboxObject(Address(reg, detail::ProxyReservedSlots::offsetOfPrivateSlot()), reg);
    return true;
}

bool
CacheIRCompiler::emitLoadValueTag()
{
    ValueOperand val = allocator.useValueRegister(masm, reader.valOperandId());
    Register res = allocator.defineRegister(masm, reader.valueTagOperandId());

    Register tag = masm.extractTag(val, res);
    if (tag != res)
        masm.mov(tag, res);
    return true;
}

bool
CacheIRCompiler::emitLoadDOMExpandoValue()
{
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    ValueOperand val = allocator.defineValueRegister(masm, reader.valOperandId());

    masm.loadPtr(Address(obj, ProxyObject::offsetOfReservedSlots()), val.scratchReg());
    masm.loadValue(Address(val.scratchReg(),
                           detail::ProxyReservedSlots::offsetOfPrivateSlot()),
                   val);
    return true;
}

bool
CacheIRCompiler::emitLoadDOMExpandoValueIgnoreGeneration()
{
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    ValueOperand output = allocator.defineValueRegister(masm, reader.valOperandId());

    // Determine the expando's Address.
    Register scratch = output.scratchReg();
    masm.loadPtr(Address(obj, ProxyObject::offsetOfReservedSlots()), scratch);
    Address expandoAddr(scratch, detail::ProxyReservedSlots::offsetOfPrivateSlot());

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
    masm.loadValue(Address(scratch, ExpandoAndGeneration::offsetOfExpando()), output);
    return true;
}

bool
CacheIRCompiler::emitLoadUndefinedResult()
{
    AutoOutputRegister output(*this);
    if (output.hasValue())
        masm.moveValue(UndefinedValue(), output.valueReg());
    else
        masm.assumeUnreachable("Should have monitored undefined result");
    return true;
}

static void
EmitStoreBoolean(MacroAssembler& masm, bool b, const AutoOutputRegister& output)
{
    if (output.hasValue()) {
        Value val = BooleanValue(b);
        masm.moveValue(val, output.valueReg());
    } else {
        MOZ_ASSERT(output.type() == JSVAL_TYPE_BOOLEAN);
        masm.movePtr(ImmWord(b), output.typedReg().gpr());
    }
}

bool
CacheIRCompiler::emitLoadBooleanResult()
{
    AutoOutputRegister output(*this);
    bool b = reader.readBool();
    EmitStoreBoolean(masm, b, output);

    return true;
}

static void
EmitStoreResult(MacroAssembler& masm, Register reg, JSValueType type,
                const AutoOutputRegister& output)
{
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

bool
CacheIRCompiler::emitLoadInt32ArrayLengthResult()
{
    AutoOutputRegister output(*this);
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), scratch);
    masm.load32(Address(scratch, ObjectElements::offsetOfLength()), scratch);

    // Guard length fits in an int32.
    masm.branchTest32(Assembler::Signed, scratch, scratch, failure->label());
    EmitStoreResult(masm, scratch, JSVAL_TYPE_INT32, output);
    return true;
}

bool
CacheIRCompiler::emitLoadArgumentsObjectLengthResult()
{
    AutoOutputRegister output(*this);
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    // Get initial length value.
    masm.unboxInt32(Address(obj, ArgumentsObject::getInitialLengthSlotOffset()), scratch);

    // Test if length has been overridden.
    masm.branchTest32(Assembler::NonZero,
                      scratch,
                      Imm32(ArgumentsObject::LENGTH_OVERRIDDEN_BIT),
                      failure->label());

    // Shift out arguments length and return it. No need to type monitor
    // because this stub always returns int32.
    masm.rshiftPtr(Imm32(ArgumentsObject::PACKED_BITS_COUNT), scratch);
    EmitStoreResult(masm, scratch, JSVAL_TYPE_INT32, output);
    return true;
}

bool
CacheIRCompiler::emitLoadFunctionLengthResult()
{
    AutoOutputRegister output(*this);
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    // Get the JSFunction flags.
    masm.load16ZeroExtend(Address(obj, JSFunction::offsetOfFlags()), scratch);

    // Functions with lazy scripts don't store their length.
    // If the length was resolved before the length property might be shadowed.
    masm.branchTest32(Assembler::NonZero,
                      scratch,
                      Imm32(JSFunction::INTERPRETED_LAZY |
                            JSFunction::RESOLVED_LENGTH),
                      failure->label());

    Label boundFunction;
    masm.branchTest32(Assembler::NonZero, scratch, Imm32(JSFunction::BOUND_FUN), &boundFunction);
    Label interpreted;
    masm.branchTest32(Assembler::NonZero, scratch, Imm32(JSFunction::INTERPRETED), &interpreted);

    // Load the length of the native function.
    masm.load16ZeroExtend(Address(obj, JSFunction::offsetOfNargs()), scratch);
    Label done;
    masm.jump(&done);

    masm.bind(&boundFunction);
    // Bound functions might have a non-int32 length.
    Address boundLength(obj, FunctionExtended::offsetOfExtendedSlot(BOUND_FUN_LENGTH_SLOT));
    masm.branchTestInt32(Assembler::NotEqual, boundLength, failure->label());
    masm.unboxInt32(boundLength, scratch);
    masm.jump(&done);

    masm.bind(&interpreted);
    // Load the length from the function's script.
    masm.loadPtr(Address(obj, JSFunction::offsetOfScript()), scratch);
    masm.load16ZeroExtend(Address(scratch, JSScript::offsetOfFunLength()), scratch);

    masm.bind(&done);
    EmitStoreResult(masm, scratch, JSVAL_TYPE_INT32, output);
    return true;
}

bool
CacheIRCompiler::emitLoadStringLengthResult()
{
    AutoOutputRegister output(*this);
    Register str = allocator.useRegister(masm, reader.stringOperandId());
    AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

    masm.loadStringLength(str, scratch);
    EmitStoreResult(masm, scratch, JSVAL_TYPE_INT32, output);
    return true;
}

bool
CacheIRCompiler::emitLoadStringCharResult()
{
    AutoOutputRegister output(*this);
    Register str = allocator.useRegister(masm, reader.stringOperandId());
    Register index = allocator.useRegister(masm, reader.int32OperandId());
    AutoScratchRegisterMaybeOutput scratch1(allocator, masm, output);
    AutoScratchRegister scratch2(allocator, masm);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    // Bounds check, load string char.
    masm.spectreBoundsCheck32(index, Address(str, JSString::offsetOfLength()), scratch1,
                              failure->label());
    masm.loadStringChar(str, index, scratch1, scratch2, failure->label());

    // Load StaticString for this char.
    masm.boundsCheck32PowerOfTwo(scratch1, StaticStrings::UNIT_STATIC_LIMIT, failure->label());
    masm.movePtr(ImmPtr(&cx_->staticStrings().unitStaticTable), scratch2);
    masm.loadPtr(BaseIndex(scratch2, scratch1, ScalePointer), scratch2);

    EmitStoreResult(masm, scratch2, JSVAL_TYPE_STRING, output);
    return true;
}

bool
CacheIRCompiler::emitLoadArgumentsObjectArgResult()
{
    AutoOutputRegister output(*this);
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    Register index = allocator.useRegister(masm, reader.int32OperandId());
    AutoScratchRegister scratch1(allocator, masm);
    AutoScratchRegisterMaybeOutput scratch2(allocator, masm, output);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    // Get initial length value.
    masm.unboxInt32(Address(obj, ArgumentsObject::getInitialLengthSlotOffset()), scratch1);

    // Ensure no overridden length/element.
    masm.branchTest32(Assembler::NonZero,
                      scratch1,
                      Imm32(ArgumentsObject::LENGTH_OVERRIDDEN_BIT |
                            ArgumentsObject::ELEMENT_OVERRIDDEN_BIT),
                      failure->label());

    // Bounds check.
    masm.rshift32(Imm32(ArgumentsObject::PACKED_BITS_COUNT), scratch1);
    masm.spectreBoundsCheck32(index, scratch1, scratch2, failure->label());

    // Load ArgumentsData.
    masm.loadPrivate(Address(obj, ArgumentsObject::getDataSlotOffset()), scratch1);

    // Fail if we have a RareArgumentsData (elements were deleted).
    masm.branchPtr(Assembler::NotEqual,
                   Address(scratch1, offsetof(ArgumentsData, rareData)),
                   ImmWord(0),
                   failure->label());

    // Guard the argument is not a FORWARD_TO_CALL_SLOT MagicValue.
    BaseValueIndex argValue(scratch1, index, ArgumentsData::offsetOfArgs());
    masm.branchTestMagic(Assembler::Equal, argValue, failure->label());
    masm.loadValue(argValue, output.valueReg());
    return true;
}

bool
CacheIRCompiler::emitLoadDenseElementResult()
{
    AutoOutputRegister output(*this);
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    Register index = allocator.useRegister(masm, reader.int32OperandId());
    AutoScratchRegister scratch1(allocator, masm);
    AutoScratchRegisterMaybeOutput scratch2(allocator, masm, output);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

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

bool
CacheIRCompiler::emitGuardIndexIsNonNegative()
{
    Register index = allocator.useRegister(masm, reader.int32OperandId());

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    masm.branch32(Assembler::LessThan, index, Imm32(0), failure->label());
    return true;
}

bool
CacheIRCompiler::emitGuardTagNotEqual()
{
    Register lhs = allocator.useRegister(masm, reader.valueTagOperandId());
    Register rhs = allocator.useRegister(masm, reader.valueTagOperandId());

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    Label done;
    masm.branch32(Assembler::Equal, lhs, rhs, failure->label());

    // If both lhs and rhs are numbers, can't use tag comparison to do inequality comparison
    masm.branchTestNumber(Assembler::NotEqual, lhs, &done);
    masm.branchTestNumber(Assembler::NotEqual, rhs, &done);
    masm.jump(failure->label());

    masm.bind(&done);
    return true;
}

bool
CacheIRCompiler::emitLoadDenseElementHoleResult()
{
    AutoOutputRegister output(*this);
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    Register index = allocator.useRegister(masm, reader.int32OperandId());
    AutoScratchRegister scratch1(allocator, masm);
    AutoScratchRegisterMaybeOutput scratch2(allocator, masm, output);

    if (!output.hasValue()) {
        masm.assumeUnreachable("Should have monitored undefined value after attaching stub");
        return true;
    }

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

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

bool
CacheIRCompiler::emitLoadTypedElementExistsResult()
{
    AutoOutputRegister output(*this);
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    Register index = allocator.useRegister(masm, reader.int32OperandId());
    TypedThingLayout layout = reader.typedThingLayout();
    AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

    Label outOfBounds, done;

    // Bound check.
    LoadTypedThingLength(masm, layout, obj, scratch);
    masm.branch32(Assembler::BelowOrEqual, scratch, index, &outOfBounds);
    EmitStoreBoolean(masm, true, output);
    masm.jump(&done);

    masm.bind(&outOfBounds);
    EmitStoreBoolean(masm, false, output);

    masm.bind(&done);
    return true;
}

bool
CacheIRCompiler::emitLoadDenseElementExistsResult()
{
    AutoOutputRegister output(*this);
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    Register index = allocator.useRegister(masm, reader.int32OperandId());
    AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

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

bool
CacheIRCompiler::emitLoadDenseElementHoleExistsResult()
{
    AutoOutputRegister output(*this);
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    Register index = allocator.useRegister(masm, reader.int32OperandId());
    AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

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

bool
CacheIRCompiler::emitArrayJoinResult()
{
    ObjOperandId objId = reader.objOperandId();

    AutoOutputRegister output(*this);
    Register obj = allocator.useRegister(masm, objId);
    AutoScratchRegister scratch(allocator, masm);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    // Load obj->elements in scratch.
    masm.loadPtr(Address(obj, NativeObject::offsetOfElements()), scratch);
    Address lengthAddr(scratch, ObjectElements::offsetOfLength());

    // If array length is 0, return empty string.
    Label finished;

    {
        Label arrayNotEmpty;
        masm.branch32(Assembler::NotEqual, lengthAddr, Imm32(0), &arrayNotEmpty);
        masm.movePtr(ImmGCPtr(cx_->names().empty), scratch);
        masm.tagValue(JSVAL_TYPE_STRING, scratch, output.valueReg());
        masm.jump(&finished);
        masm.bind(&arrayNotEmpty);
    }

    // Otherwise, handle array length 1 case.
    masm.branch32(Assembler::NotEqual, lengthAddr, Imm32(1), failure->label());

    // But only if initializedLength is also 1.
    Address initLength(scratch, ObjectElements::offsetOfInitializedLength());
    masm.branch32(Assembler::NotEqual, initLength, Imm32(1), failure->label());

    // And only if elem0 is a string.
    Address elementAddr(scratch, 0);
    masm.branchTestString(Assembler::NotEqual, elementAddr, failure->label());

    // Store the value.
    masm.loadValue(elementAddr, output.valueReg());

    masm.bind(&finished);

    return true;
}

bool
CacheIRCompiler::emitLoadTypedElementResult()
{
    AutoOutputRegister output(*this);
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    Register index = allocator.useRegister(masm, reader.int32OperandId());
    TypedThingLayout layout = reader.typedThingLayout();
    Scalar::Type type = reader.scalarType();

    AutoScratchRegister scratch1(allocator, masm);
    AutoScratchRegisterMaybeOutput scratch2(allocator, masm, output);

    if (!output.hasValue()) {
        if (type == Scalar::Float32 || type == Scalar::Float64) {
            if (output.type() != JSVAL_TYPE_DOUBLE) {
                masm.assumeUnreachable("Should have monitored double after attaching stub");
                return true;
            }
        } else {
            if (output.type() != JSVAL_TYPE_INT32 && output.type() != JSVAL_TYPE_DOUBLE) {
                masm.assumeUnreachable("Should have monitored int32 after attaching stub");
                return true;
            }
        }
    }

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    // Bounds check.
    LoadTypedThingLength(masm, layout, obj, scratch1);
    masm.spectreBoundsCheck32(index, scratch1, scratch2, failure->label());

    // Load the elements vector.
    LoadTypedThingData(masm, layout, obj, scratch1);

    // Load the value.
    BaseIndex source(scratch1, index, ScaleFromElemWidth(Scalar::byteSize(type)));
    if (output.hasValue()) {
        masm.loadFromTypedArray(type, source, output.valueReg(), *allowDoubleResult_, scratch1,
                                failure->label());
    } else {
        bool needGpr = (type == Scalar::Int8 || type == Scalar::Uint8 ||
                        type == Scalar::Int16 || type == Scalar::Uint16 ||
                        type == Scalar::Uint8Clamped || type == Scalar::Int32);
        if (needGpr && output.type() == JSVAL_TYPE_DOUBLE) {
            // Load the element as integer, then convert it to double.
            masm.loadFromTypedArray(type, source, AnyRegister(scratch1), scratch1,
                                    failure->label());
            masm.convertInt32ToDouble(source, output.typedReg().fpu());
        } else {
            masm.loadFromTypedArray(type, source, output.typedReg(), scratch1, failure->label());
        }
    }
    return true;
}

void
CacheIRCompiler::emitLoadTypedObjectResultShared(const Address& fieldAddr, Register scratch,
                                                 uint32_t typeDescr,
                                                 const AutoOutputRegister& output)
{
    MOZ_ASSERT(output.hasValue());

    if (SimpleTypeDescrKeyIsScalar(typeDescr)) {
        Scalar::Type type = ScalarTypeFromSimpleTypeDescrKey(typeDescr);
        masm.loadFromTypedArray(type, fieldAddr, output.valueReg(),
                                /* allowDouble = */ true, scratch, nullptr);
    } else {
        ReferenceTypeDescr::Type type = ReferenceTypeFromSimpleTypeDescrKey(typeDescr);
        switch (type) {
          case ReferenceTypeDescr::TYPE_ANY:
            masm.loadValue(fieldAddr, output.valueReg());
            break;

          case ReferenceTypeDescr::TYPE_OBJECT: {
            Label notNull, done;
            masm.loadPtr(fieldAddr, scratch);
            masm.branchTestPtr(Assembler::NonZero, scratch, scratch, &notNull);
            masm.moveValue(NullValue(), output.valueReg());
            masm.jump(&done);
            masm.bind(&notNull);
            masm.tagValue(JSVAL_TYPE_OBJECT, scratch, output.valueReg());
            masm.bind(&done);
            break;
          }

          case ReferenceTypeDescr::TYPE_STRING:
            masm.loadPtr(fieldAddr, scratch);
            masm.tagValue(JSVAL_TYPE_STRING, scratch, output.valueReg());
            break;

          default:
            MOZ_CRASH("Invalid ReferenceTypeDescr");
        }
    }
}

bool
CacheIRCompiler::emitLoadObjectResult()
{
    AutoOutputRegister output(*this);
    Register obj = allocator.useRegister(masm, reader.objOperandId());

    if (output.hasValue())
        masm.tagValue(JSVAL_TYPE_OBJECT, obj, output.valueReg());
    else
        masm.mov(obj, output.typedReg().gpr());

    return true;
}

bool
CacheIRCompiler::emitLoadTypeOfObjectResult()
{
    AutoOutputRegister output(*this);
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

    Label slowCheck, isObject, isCallable, isUndefined, done;
    masm.typeOfObject(obj, scratch, &slowCheck, &isObject, &isCallable, &isUndefined);

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
        LiveRegisterSet save(GeneralRegisterSet::Volatile(), liveVolatileFloatRegs());
        masm.PushRegsInMask(save);

        masm.setupUnalignedABICall(scratch);
        masm.passABIArg(obj);
        masm.movePtr(ImmPtr(cx_->runtime()), scratch);
        masm.passABIArg(scratch);
        masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, TypeOfObject));
        masm.mov(ReturnReg, scratch);

        LiveRegisterSet ignore;
        ignore.add(scratch);
        masm.PopRegsInMaskIgnore(save, ignore);

        masm.tagValue(JSVAL_TYPE_STRING, scratch, output.valueReg());
    }

    masm.bind(&done);
    return true;
}

bool
CacheIRCompiler::emitLoadInt32TruthyResult()
{
    AutoOutputRegister output(*this);
    ValueOperand val = allocator.useValueRegister(masm, reader.valOperandId());

    Label ifFalse, done;
    masm.branchTestInt32Truthy(false, val, &ifFalse);
    masm.moveValue(BooleanValue(true), output.valueReg());
    masm.jump(&done);

    masm.bind(&ifFalse);
    masm.moveValue(BooleanValue(false), output.valueReg());

    masm.bind(&done);
    return true;
}

bool
CacheIRCompiler::emitLoadStringTruthyResult()
{
    AutoOutputRegister output(*this);
    Register str = allocator.useRegister(masm, reader.stringOperandId());

    Label ifFalse, done;
    masm.branch32(Assembler::Equal, Address(str, JSString::offsetOfLength()), Imm32(0), &ifFalse);
    masm.moveValue(BooleanValue(true), output.valueReg());
    masm.jump(&done);

    masm.bind(&ifFalse);
    masm.moveValue(BooleanValue(false), output.valueReg());

    masm.bind(&done);
    return true;
}

bool
CacheIRCompiler::emitLoadDoubleTruthyResult()
{
    AutoOutputRegister output(*this);
    ValueOperand val = allocator.useValueRegister(masm, reader.valOperandId());

    Label ifFalse, done, failurePopReg;

     // If we're compiling a Baseline IC, FloatReg0 is always available.
    if (mode_ != Mode::Baseline)
        masm.push(FloatReg0);

    masm.unboxDouble(val, FloatReg0);

    masm.branchTestDoubleTruthy(false, FloatReg0, &ifFalse);
    masm.moveValue(BooleanValue(true), output.valueReg());
    masm.jump(&done);

    masm.bind(&ifFalse);
    masm.moveValue(BooleanValue(false), output.valueReg());

    if (mode_ != Mode::Baseline)
        masm.pop(FloatReg0);
    masm.bind(&done);
    return true;
}

bool
CacheIRCompiler::emitLoadObjectTruthyResult()
{
    AutoOutputRegister output(*this);
    Register obj = allocator.useRegister(masm, reader.objOperandId());
    AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);


    Label emulatesUndefined, slowPath, done;
    masm.branchIfObjectEmulatesUndefined(obj, scratch, &slowPath, &emulatesUndefined);
    masm.moveValue(BooleanValue(true), output.valueReg());
    masm.jump(&done);

    masm.bind(&emulatesUndefined);
    masm.moveValue(BooleanValue(false), output.valueReg());
    masm.jump(&done);

    masm.bind(&slowPath);
    masm.setupUnalignedABICall(scratch);
    masm.passABIArg(obj);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, js::EmulatesUndefined));
    masm.convertBoolToInt32(ReturnReg, ReturnReg);
    masm.xor32(Imm32(1), ReturnReg);
    masm.tagValue(JSVAL_TYPE_BOOLEAN, ReturnReg, output.valueReg());

    masm.bind(&done);
    return true;
}

bool
CacheIRCompiler::emitCompareStringResult()
{
    AutoOutputRegister output(*this);

    Register left = allocator.useRegister(masm, reader.stringOperandId());
    Register right = allocator.useRegister(masm, reader.stringOperandId());
    JSOp op = reader.jsop();

    AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    masm.compareStrings(op, left, right, scratch, failure->label());
    masm.tagValue(JSVAL_TYPE_BOOLEAN, scratch, output.valueReg());
    return true;
}

bool
CacheIRCompiler::emitComparePointerResultShared(bool symbol)
{
    AutoOutputRegister output(*this);

    Register left = symbol ? allocator.useRegister(masm, reader.symbolOperandId())
                           : allocator.useRegister(masm, reader.objOperandId());
    Register right = symbol ? allocator.useRegister(masm, reader.symbolOperandId())
                            : allocator.useRegister(masm, reader.objOperandId());
    JSOp op = reader.jsop();

    AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

    Label ifTrue, done;
    masm.branchPtr(JSOpToCondition(op, /* signed = */true), left, right, &ifTrue);

    masm.moveValue(BooleanValue(false), output.valueReg());
    masm.jump(&done);

    masm.bind(&ifTrue);
    masm.moveValue(BooleanValue(true), output.valueReg());
    masm.bind(&done);
    return true;
}


bool
CacheIRCompiler::emitCompareObjectResult()
{
    return emitComparePointerResultShared(false);
}

bool
CacheIRCompiler::emitCompareSymbolResult()
{
    return emitComparePointerResultShared(true);
}

bool
CacheIRCompiler::emitCallPrintString()
{
    const char* str = reinterpret_cast<char*>(reader.pointer());
    masm.printf(str);
    return true;
}

bool
CacheIRCompiler::emitBreakpoint()
{
    masm.breakpoint();
    return true;
}

void
CacheIRCompiler::emitStoreTypedObjectReferenceProp(ValueOperand val, ReferenceTypeDescr::Type type,
                                                   const Address& dest, Register scratch)
{
    // Callers will post-barrier this store.

    switch (type) {
      case ReferenceTypeDescr::TYPE_ANY:
        EmitPreBarrier(masm, dest, MIRType::Value);
        masm.storeValue(val, dest);
        break;

      case ReferenceTypeDescr::TYPE_OBJECT: {
        EmitPreBarrier(masm, dest, MIRType::Object);
        Label isNull, done;
        masm.branchTestObject(Assembler::NotEqual, val, &isNull);
        masm.unboxObject(val, scratch);
        masm.storePtr(scratch, dest);
        masm.jump(&done);
        masm.bind(&isNull);
        masm.storePtr(ImmWord(0), dest);
        masm.bind(&done);
        break;
      }

      case ReferenceTypeDescr::TYPE_STRING:
        EmitPreBarrier(masm, dest, MIRType::String);
        masm.unboxString(val, scratch);
        masm.storePtr(scratch, dest);
        break;
    }
}

void
CacheIRCompiler::emitRegisterEnumerator(Register enumeratorsList, Register iter, Register scratch)
{
    // iter->next = list
    masm.storePtr(enumeratorsList, Address(iter, NativeIterator::offsetOfNext()));

    // iter->prev = list->prev
    masm.loadPtr(Address(enumeratorsList, NativeIterator::offsetOfPrev()), scratch);
    masm.storePtr(scratch, Address(iter, NativeIterator::offsetOfPrev()));

    // list->prev->next = iter
    masm.storePtr(iter, Address(scratch, NativeIterator::offsetOfNext()));

    // list->prev = ni
    masm.storePtr(iter, Address(enumeratorsList, NativeIterator::offsetOfPrev()));
}

void
CacheIRCompiler::emitPostBarrierShared(Register obj, const ConstantOrRegister& val,
                                       Register scratch, Register maybeIndex)
{
    if (!cx_->nursery().exists())
        return;

    if (val.constant()) {
        MOZ_ASSERT_IF(val.value().isGCThing(), !IsInsideNursery(val.value().toGCThing()));
        return;
    }

    TypedOrValueRegister reg = val.reg();
    if (reg.hasTyped()) {
        if (reg.type() != MIRType::Object && reg.type() != MIRType::String)
            return;
    }

    Label skipBarrier;
    if (reg.hasValue()) {
        masm.branchValueIsNurseryCell(Assembler::NotEqual, reg.valueReg(), scratch, &skipBarrier);
    } else {
        masm.branchPtrInNurseryChunk(Assembler::NotEqual, reg.typedReg().gpr(), scratch,
                                     &skipBarrier);
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
        masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*,
                                             (PostWriteElementBarrier<IndexInBounds::Yes>)));
    } else {
        masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, PostWriteBarrier));
    }
    masm.PopRegsInMask(save);

    masm.bind(&skipBarrier);
}

bool
CacheIRCompiler::emitWrapResult()
{
    AutoOutputRegister output(*this);
    AutoScratchRegister scratch(allocator, masm);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    Label done;
    // We only have to wrap objects, because we are in the same zone.
    masm.branchTestObject(Assembler::NotEqual, output.valueReg(), &done);

    Register obj = output.valueReg().scratchReg();
    masm.unboxObject(output.valueReg(), obj);

    LiveRegisterSet save(GeneralRegisterSet::Volatile(), liveVolatileFloatRegs());
    masm.PushRegsInMask(save);

    masm.setupUnalignedABICall(scratch);
    masm.loadJSContext(scratch);
    masm.passABIArg(scratch);
    masm.passABIArg(obj);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, WrapObjectPure));
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

bool
CacheIRCompiler::emitMegamorphicLoadSlotByValueResult()
{
    AutoOutputRegister output(*this);

    Register obj = allocator.useRegister(masm, reader.objOperandId());
    ValueOperand idVal = allocator.useValueRegister(masm, reader.valOperandId());
    bool handleMissing = reader.readBool();

    AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    // The object must be Native.
    masm.branchIfNonNativeObj(obj, scratch, failure->label());

    // idVal will be in vp[0], result will be stored in vp[1].
    masm.reserveStack(sizeof(Value));
    masm.Push(idVal);
    masm.moveStackPtrTo(idVal.scratchReg());

    LiveRegisterSet volatileRegs(GeneralRegisterSet::Volatile(), liveVolatileFloatRegs());
    volatileRegs.takeUnchecked(scratch);
    volatileRegs.takeUnchecked(idVal);
    masm.PushRegsInMask(volatileRegs);

    masm.setupUnalignedABICall(scratch);
    masm.loadJSContext(scratch);
    masm.passABIArg(scratch);
    masm.passABIArg(obj);
    masm.passABIArg(idVal.scratchReg());
    if (handleMissing)
        masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, (GetNativeDataPropertyByValue<true>)));
    else
        masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, (GetNativeDataPropertyByValue<false>)));
    masm.mov(ReturnReg, scratch);
    masm.PopRegsInMask(volatileRegs);

    masm.Pop(idVal);

    Label ok;
    uint32_t framePushed = masm.framePushed();
    masm.branchIfTrueBool(scratch, &ok);
    masm.adjustStack(sizeof(Value));
    masm.jump(failure->label());

    masm.bind(&ok);
    if (JitOptions.spectreJitToCxxCalls)
        masm.speculationBarrier();
    masm.setFramePushed(framePushed);
    masm.loadTypedOrValue(Address(masm.getStackPointer(), 0), output);
    masm.adjustStack(sizeof(Value));
    return true;
}

bool
CacheIRCompiler::emitMegamorphicHasPropResult()
{
    AutoOutputRegister output(*this);

    Register obj = allocator.useRegister(masm, reader.objOperandId());
    ValueOperand idVal = allocator.useValueRegister(masm, reader.valOperandId());
    bool hasOwn = reader.readBool();

    AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    // idVal will be in vp[0], result will be stored in vp[1].
    masm.reserveStack(sizeof(Value));
    masm.Push(idVal);
    masm.moveStackPtrTo(idVal.scratchReg());

    LiveRegisterSet volatileRegs(GeneralRegisterSet::Volatile(), liveVolatileFloatRegs());
    volatileRegs.takeUnchecked(scratch);
    volatileRegs.takeUnchecked(idVal);
    masm.PushRegsInMask(volatileRegs);

    masm.setupUnalignedABICall(scratch);
    masm.loadJSContext(scratch);
    masm.passABIArg(scratch);
    masm.passABIArg(obj);
    masm.passABIArg(idVal.scratchReg());
    if (hasOwn)
        masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, HasNativeDataProperty<true>));
    else
        masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, HasNativeDataProperty<false>));
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

bool
CacheIRCompiler::emitCallObjectHasSparseElementResult()
{
    AutoOutputRegister output(*this);

    Register obj = allocator.useRegister(masm, reader.objOperandId());
    Register index = allocator.useRegister(masm, reader.int32OperandId());

    AutoScratchRegisterMaybeOutput scratch1(allocator, masm, output);
    AutoScratchRegister scratch2(allocator, masm);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    masm.reserveStack(sizeof(Value));
    masm.moveStackPtrTo(scratch2.get());

    LiveRegisterSet volatileRegs(GeneralRegisterSet::Volatile(), liveVolatileFloatRegs());
    volatileRegs.takeUnchecked(scratch1);
    volatileRegs.takeUnchecked(index);
    masm.PushRegsInMask(volatileRegs);

    masm.setupUnalignedABICall(scratch1);
    masm.loadJSContext(scratch1);
    masm.passABIArg(scratch1);
    masm.passABIArg(obj);
    masm.passABIArg(index);
    masm.passABIArg(scratch2);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, HasNativeElement));
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

bool
CacheIRCompiler::emitLoadInstanceOfObjectResult()
{
    AutoOutputRegister output(*this);
    ValueOperand lhs = allocator.useValueRegister(masm, reader.valOperandId());
    Register proto = allocator.useRegister(masm, reader.objOperandId());

    AutoScratchRegisterMaybeOutput scratch(allocator, masm, output);

    FailurePath* failure;
    if (!addFailurePath(&failure))
        return false;

    Label returnFalse, returnTrue, done;
    masm.branchTestObject(Assembler::NotEqual, lhs, &returnFalse);

    // LHS is an object. Load its proto.
    masm.unboxObject(lhs, scratch);
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
    //fallthrough
    masm.bind(&done);
    return true;
}
