/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_shared_Lowering_shared_inl_h
#define jit_shared_Lowering_shared_inl_h

#include "jit/shared/Lowering-shared.h"

#include "jit/MIR.h"
#include "jit/MIRGenerator.h"

namespace js {
namespace jit {

void
LIRGeneratorShared::emitAtUses(MInstruction* mir)
{
    MOZ_ASSERT(mir->canEmitAtUses());
    mir->setEmittedAtUses();
    mir->setVirtualRegister(0);
}

LUse
LIRGeneratorShared::use(MDefinition* mir, LUse policy)
{
    // It is illegal to call use() on an instruction with two defs.
#if BOX_PIECES > 1
    MOZ_ASSERT(mir->type() != MIRType_Value);
#endif
    ensureDefined(mir);
    policy.setVirtualRegister(mir->virtualRegister());
    return policy;
}

template <size_t X, size_t Y> void
LIRGeneratorShared::define(LInstructionHelper<1, X, Y>* lir, MDefinition* mir, const LDefinition& def)
{
    // Call instructions should use defineReturn.
    MOZ_ASSERT(!lir->isCall());

    uint32_t vreg = getVirtualRegister();

    // Assign the definition and a virtual register. Then, propagate this
    // virtual register to the MIR, so we can map MIR to LIR during lowering.
    lir->setDef(0, def);
    lir->getDef(0)->setVirtualRegister(vreg);
    lir->setMir(mir);
    mir->setVirtualRegister(vreg);
    add(lir);
}

template <size_t X, size_t Y> void
LIRGeneratorShared::define(LInstructionHelper<1, X, Y>* lir, MDefinition* mir, LDefinition::Policy policy)
{
    LDefinition::Type type = LDefinition::TypeFrom(mir->type());
    define(lir, mir, LDefinition(type, policy));
}

template <size_t X, size_t Y> void
LIRGeneratorShared::defineFixed(LInstructionHelper<1, X, Y>* lir, MDefinition* mir, const LAllocation& output)
{
    LDefinition::Type type = LDefinition::TypeFrom(mir->type());

    LDefinition def(type, LDefinition::FIXED);
    def.setOutput(output);

    // Add an LNop to avoid regalloc problems if the next op uses this value
    // with a fixed or at-start policy.
    define(lir, mir, def);

    if (gen->optimizationInfo().registerAllocator() == RegisterAllocator_LSRA)
        add(new(alloc()) LNop);
}

template <size_t Ops, size_t Temps> void
LIRGeneratorShared::defineReuseInput(LInstructionHelper<1, Ops, Temps>* lir, MDefinition* mir, uint32_t operand)
{
    // The input should be used at the start of the instruction, to avoid moves.
    MOZ_ASSERT(lir->getOperand(operand)->toUse()->usedAtStart());

    LDefinition::Type type = LDefinition::TypeFrom(mir->type());

    LDefinition def(type, LDefinition::MUST_REUSE_INPUT);
    def.setReusedInput(operand);

    define(lir, mir, def);
}

template <size_t Ops, size_t Temps> void
LIRGeneratorShared::defineBox(LInstructionHelper<BOX_PIECES, Ops, Temps>* lir, MDefinition* mir,
                              LDefinition::Policy policy)
{
    // Call instructions should use defineReturn.
    MOZ_ASSERT(!lir->isCall());

    uint32_t vreg = getVirtualRegister();

#if defined(JS_NUNBOX32)
    lir->setDef(0, LDefinition(vreg + VREG_TYPE_OFFSET, LDefinition::TYPE, policy));
    lir->setDef(1, LDefinition(vreg + VREG_DATA_OFFSET, LDefinition::PAYLOAD, policy));
    getVirtualRegister();
#elif defined(JS_PUNBOX64)
    lir->setDef(0, LDefinition(vreg, LDefinition::BOX, policy));
#endif
    lir->setMir(mir);

    mir->setVirtualRegister(vreg);
    add(lir);
}

void
LIRGeneratorShared::defineReturn(LInstruction* lir, MDefinition* mir)
{
    lir->setMir(mir);

    MOZ_ASSERT(lir->isCall());

    uint32_t vreg = getVirtualRegister();

    switch (mir->type()) {
      case MIRType_Value:
#if defined(JS_NUNBOX32)
        lir->setDef(TYPE_INDEX, LDefinition(vreg + VREG_TYPE_OFFSET, LDefinition::TYPE,
                                            LGeneralReg(JSReturnReg_Type)));
        lir->setDef(PAYLOAD_INDEX, LDefinition(vreg + VREG_DATA_OFFSET, LDefinition::PAYLOAD,
                                               LGeneralReg(JSReturnReg_Data)));
        getVirtualRegister();
#elif defined(JS_PUNBOX64)
        lir->setDef(0, LDefinition(vreg, LDefinition::BOX, LGeneralReg(JSReturnReg)));
#endif
        break;
      case MIRType_Float32:
        lir->setDef(0, LDefinition(vreg, LDefinition::FLOAT32, LFloatReg(ReturnFloat32Reg)));
        break;
      case MIRType_Double:
        lir->setDef(0, LDefinition(vreg, LDefinition::DOUBLE, LFloatReg(ReturnDoubleReg)));
        break;
      case MIRType_Int32x4:
        lir->setDef(0, LDefinition(vreg, LDefinition::INT32X4, LFloatReg(ReturnSimdReg)));
        break;
      case MIRType_Float32x4:
        lir->setDef(0, LDefinition(vreg, LDefinition::FLOAT32X4, LFloatReg(ReturnSimdReg)));
        break;
      default:
        LDefinition::Type type = LDefinition::TypeFrom(mir->type());
        MOZ_ASSERT(type != LDefinition::DOUBLE && type != LDefinition::FLOAT32);
        lir->setDef(0, LDefinition(vreg, type, LGeneralReg(ReturnReg)));
        break;
    }

    mir->setVirtualRegister(vreg);
    add(lir);

    if (gen->optimizationInfo().registerAllocator() == RegisterAllocator_LSRA)
        add(new(alloc()) LNop);
}

// In LIR, we treat booleans and integers as the same low-level type (INTEGER).
// When snapshotting, we recover the actual JS type from MIR. This function
// checks that when making redefinitions, we don't accidentally coerce two
// incompatible types.
static inline bool
IsCompatibleLIRCoercion(MIRType to, MIRType from)
{
    if (to == from)
        return true;
    if ((to == MIRType_Int32 || to == MIRType_Boolean) &&
        (from == MIRType_Int32 || from == MIRType_Boolean)) {
        return true;
    }
    // SIMD types can be coerced with from*Bits operators.
    if (IsSimdType(to) && IsSimdType(from))
        return true;
    return false;
}

void
LIRGeneratorShared::redefine(MDefinition* def, MDefinition* as)
{
    MOZ_ASSERT(IsCompatibleLIRCoercion(def->type(), as->type()));

    // Try to emit MIR marked as emitted-at-uses at, well, uses. For
    // snapshotting reasons we delay the MIRTypes match, or when we are
    // coercing between bool and int32 constants.
    if (as->isEmittedAtUses() &&
        (def->type() == as->type() ||
         (as->isConstant() &&
          (def->type() == MIRType_Int32 || def->type() == MIRType_Boolean) &&
          (as->type() == MIRType_Int32 || as->type() == MIRType_Boolean))))
    {
        MInstruction* replacement;
        if (def->type() != as->type()) {
            Value v = as->toConstant()->value();
            if (as->type() == MIRType_Int32)
                replacement = MConstant::New(alloc(), BooleanValue(v.toInt32()));
            else
                replacement = MConstant::New(alloc(), Int32Value(v.toBoolean()));
            def->block()->insertBefore(def->toInstruction(), replacement);
            emitAtUses(replacement->toInstruction());
        } else {
            replacement = as->toInstruction();
        }
        def->replaceAllUsesWith(replacement);
    } else {
        ensureDefined(as);
        def->setVirtualRegister(as->virtualRegister());
    }
}

void
LIRGeneratorShared::ensureDefined(MDefinition* mir)
{
    if (mir->isEmittedAtUses()) {
        mir->toInstruction()->accept(this);
        MOZ_ASSERT(mir->isLowered());
    }
}

LUse
LIRGeneratorShared::useRegister(MDefinition* mir)
{
    return use(mir, LUse(LUse::REGISTER));
}

LUse
LIRGeneratorShared::useRegisterAtStart(MDefinition* mir)
{
    return use(mir, LUse(LUse::REGISTER, true));
}

LUse
LIRGeneratorShared::use(MDefinition* mir)
{
    return use(mir, LUse(LUse::ANY));
}

LUse
LIRGeneratorShared::useAtStart(MDefinition* mir)
{
    return use(mir, LUse(LUse::ANY, true));
}

LAllocation
LIRGeneratorShared::useOrConstant(MDefinition* mir)
{
    if (mir->isConstant())
        return LAllocation(mir->toConstant()->vp());
    return use(mir);
}

LAllocation
LIRGeneratorShared::useOrConstantAtStart(MDefinition* mir)
{
    if (mir->isConstant())
        return LAllocation(mir->toConstant()->vp());
    return useAtStart(mir);
}

LAllocation
LIRGeneratorShared::useRegisterOrConstant(MDefinition* mir)
{
    if (mir->isConstant())
        return LAllocation(mir->toConstant()->vp());
    return useRegister(mir);
}

LAllocation
LIRGeneratorShared::useRegisterOrConstantAtStart(MDefinition* mir)
{
    if (mir->isConstant())
        return LAllocation(mir->toConstant()->vp());
    return useRegisterAtStart(mir);
}

LAllocation
LIRGeneratorShared::useRegisterOrNonNegativeConstantAtStart(MDefinition* mir)
{
    if (mir->isConstant() && mir->toConstant()->value().toInt32() >= 0)
        return LAllocation(mir->toConstant()->vp());
    return useRegisterAtStart(mir);
}

LAllocation
LIRGeneratorShared::useRegisterOrNonDoubleConstant(MDefinition* mir)
{
    if (mir->isConstant() && mir->type() != MIRType_Double && mir->type() != MIRType_Float32)
        return LAllocation(mir->toConstant()->vp());
    return useRegister(mir);
}

#if defined(JS_CODEGEN_ARM)
LAllocation
LIRGeneratorShared::useAnyOrConstant(MDefinition* mir)
{
    return useRegisterOrConstant(mir);
}
LAllocation
LIRGeneratorShared::useStorable(MDefinition* mir)
{
    return useRegister(mir);
}
LAllocation
LIRGeneratorShared::useStorableAtStart(MDefinition* mir)
{
    return useRegisterAtStart(mir);
}

LAllocation
LIRGeneratorShared::useAny(MDefinition* mir)
{
    return useRegister(mir);
}
#else
LAllocation
LIRGeneratorShared::useAnyOrConstant(MDefinition* mir)
{
    return useOrConstant(mir);
}

LAllocation
LIRGeneratorShared::useAny(MDefinition* mir)
{
    return use(mir);
}
LAllocation
LIRGeneratorShared::useStorable(MDefinition* mir)
{
    return useRegisterOrConstant(mir);
}
LAllocation
LIRGeneratorShared::useStorableAtStart(MDefinition* mir)
{
    return useRegisterOrConstantAtStart(mir);
}

#endif

LAllocation
LIRGeneratorShared::useKeepalive(MDefinition* mir)
{
    return use(mir, LUse(LUse::KEEPALIVE));
}

LAllocation
LIRGeneratorShared::useKeepaliveOrConstant(MDefinition* mir)
{
    if (mir->isConstant())
        return LAllocation(mir->toConstant()->vp());
    return useKeepalive(mir);
}

LUse
LIRGeneratorShared::useFixed(MDefinition* mir, Register reg)
{
    return use(mir, LUse(reg));
}

LUse
LIRGeneratorShared::useFixedAtStart(MDefinition* mir, Register reg)
{
    return use(mir, LUse(reg, true));
}

LUse
LIRGeneratorShared::useFixed(MDefinition* mir, FloatRegister reg)
{
    return use(mir, LUse(reg));
}

LUse
LIRGeneratorShared::useFixed(MDefinition* mir, AnyRegister reg)
{
    return reg.isFloat() ? use(mir, LUse(reg.fpu())) : use(mir, LUse(reg.gpr()));
}

LDefinition
LIRGeneratorShared::temp(LDefinition::Type type, LDefinition::Policy policy)
{
    return LDefinition(getVirtualRegister(), type, policy);
}

LDefinition
LIRGeneratorShared::tempFixed(Register reg)
{
    LDefinition t = temp(LDefinition::GENERAL);
    t.setOutput(LGeneralReg(reg));
    return t;
}

LDefinition
LIRGeneratorShared::tempFloat32()
{
    return temp(LDefinition::FLOAT32);
}

LDefinition
LIRGeneratorShared::tempDouble()
{
    return temp(LDefinition::DOUBLE);
}

LDefinition
LIRGeneratorShared::tempCopy(MDefinition* input, uint32_t reusedInput)
{
    MOZ_ASSERT(input->virtualRegister());
    LDefinition t = temp(LDefinition::TypeFrom(input->type()), LDefinition::MUST_REUSE_INPUT);
    t.setReusedInput(reusedInput);
    return t;
}

template <typename T> void
LIRGeneratorShared::annotate(T* ins)
{
    ins->setId(lirGraph_.getInstructionId());
}

template <typename T> void
LIRGeneratorShared::add(T* ins, MInstruction* mir)
{
    MOZ_ASSERT(!ins->isPhi());
    current->add(ins);
    if (mir) {
        MOZ_ASSERT(current == mir->block()->lir());
        ins->setMir(mir);
    }
    annotate(ins);
}

#ifdef JS_NUNBOX32
// Returns the virtual register of a js::Value-defining instruction. This is
// abstracted because MBox is a special value-returning instruction that
// redefines its input payload if its input is not constant. Therefore, it is
// illegal to request a box's payload by adding VREG_DATA_OFFSET to its raw id.
static inline uint32_t
VirtualRegisterOfPayload(MDefinition* mir)
{
    if (mir->isBox()) {
        MDefinition* inner = mir->toBox()->getOperand(0);
        if (!inner->isConstant() && inner->type() != MIRType_Double && inner->type() != MIRType_Float32)
            return inner->virtualRegister();
    }
    if (mir->isTypeBarrier())
        return VirtualRegisterOfPayload(mir->getOperand(0));
    return mir->virtualRegister() + VREG_DATA_OFFSET;
}

// Note: always call ensureDefined before calling useType/usePayload,
// so that emitted-at-use operands are handled correctly.
LUse
LIRGeneratorShared::useType(MDefinition* mir, LUse::Policy policy)
{
    MOZ_ASSERT(mir->type() == MIRType_Value);

    return LUse(mir->virtualRegister() + VREG_TYPE_OFFSET, policy);
}

LUse
LIRGeneratorShared::usePayload(MDefinition* mir, LUse::Policy policy)
{
    MOZ_ASSERT(mir->type() == MIRType_Value);

    return LUse(VirtualRegisterOfPayload(mir), policy);
}

LUse
LIRGeneratorShared::usePayloadAtStart(MDefinition* mir, LUse::Policy policy)
{
    MOZ_ASSERT(mir->type() == MIRType_Value);

    return LUse(VirtualRegisterOfPayload(mir), policy, true);
}

LUse
LIRGeneratorShared::usePayloadInRegisterAtStart(MDefinition* mir)
{
    return usePayloadAtStart(mir, LUse::REGISTER);
}

void
LIRGeneratorShared::fillBoxUses(LInstruction* lir, size_t n, MDefinition* mir)
{
    ensureDefined(mir);
    lir->getOperand(n)->toUse()->setVirtualRegister(mir->virtualRegister() + VREG_TYPE_OFFSET);
    lir->getOperand(n + 1)->toUse()->setVirtualRegister(VirtualRegisterOfPayload(mir));
}
#endif

LUse
LIRGeneratorShared::useRegisterForTypedLoad(MDefinition* mir, MIRType type)
{
    MOZ_ASSERT(type != MIRType_Value && type != MIRType_None);
    MOZ_ASSERT(mir->type() == MIRType_Object || mir->type() == MIRType_Slots);

#ifdef JS_PUNBOX64
    // On x64, masm.loadUnboxedValue emits slightly less efficient code when
    // the input and output use the same register and we're not loading an
    // int32/bool/double, so we just call useRegister in this case.
    if (type != MIRType_Int32 && type != MIRType_Boolean && type != MIRType_Double)
        return useRegister(mir);
#endif

    return useRegisterAtStart(mir);
}

} // namespace jit
} // namespace js

#endif /* jit_shared_Lowering_shared_inl_h */
