/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/MathAlgorithms.h"

#include "jit/arm/Assembler-arm.h"
#include "jit/Lowering.h"
#include "jit/MIR.h"

#include "jit/shared/Lowering-shared-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::FloorLog2;

void
LIRGeneratorARM::useBoxFixed(LInstruction* lir, size_t n, MDefinition* mir, Register reg1,
                             Register reg2, bool useAtStart)
{
    MOZ_ASSERT(mir->type() == MIRType_Value);
    MOZ_ASSERT(reg1 != reg2);

    ensureDefined(mir);
    lir->setOperand(n, LUse(reg1, mir->virtualRegister(), useAtStart));
    lir->setOperand(n + 1, LUse(reg2, VirtualRegisterOfPayload(mir), useAtStart));
}

LAllocation
LIRGeneratorARM::useByteOpRegister(MDefinition* mir)
{
    return useRegister(mir);
}

LAllocation
LIRGeneratorARM::useByteOpRegisterOrNonDoubleConstant(MDefinition* mir)
{
    return useRegisterOrNonDoubleConstant(mir);
}

LDefinition
LIRGeneratorARM::tempByteOpRegister()
{
    return temp();
}

void
LIRGeneratorARM::visitBox(MBox* box)
{
    MDefinition* inner = box->getOperand(0);

    // If the box wrapped a double, it needs a new register.
    if (IsFloatingPointType(inner->type())) {
        defineBox(new(alloc()) LBoxFloatingPoint(useRegisterAtStart(inner), tempCopy(inner, 0),
                                                 inner->type()), box);
        return;
    }

    if (box->canEmitAtUses()) {
        emitAtUses(box);
        return;
    }

    if (inner->isConstant()) {
        defineBox(new(alloc()) LValue(inner->toConstant()->value()), box);
        return;
    }

    LBox* lir = new(alloc()) LBox(use(inner), inner->type());

    // Otherwise, we should not define a new register for the payload portion
    // of the output, so bypass defineBox().
    uint32_t vreg = getVirtualRegister();

    // Note that because we're using BogusTemp(), we do not change the type of
    // the definition. We also do not define the first output as "TYPE",
    // because it has no corresponding payload at (vreg + 1). Also note that
    // although we copy the input's original type for the payload half of the
    // definition, this is only for clarity. BogusTemp() definitions are
    // ignored.
    lir->setDef(0, LDefinition(vreg, LDefinition::GENERAL));
    lir->setDef(1, LDefinition::BogusTemp());
    box->setVirtualRegister(vreg);
    add(lir);
}

void
LIRGeneratorARM::visitUnbox(MUnbox* unbox)
{
    MDefinition* inner = unbox->getOperand(0);

    if (inner->type() == MIRType_ObjectOrNull) {
        LUnboxObjectOrNull* lir = new(alloc()) LUnboxObjectOrNull(useRegisterAtStart(inner));
        if (unbox->fallible())
            assignSnapshot(lir, unbox->bailoutKind());
        defineReuseInput(lir, unbox, 0);
        return;
    }

    // An unbox on arm reads in a type tag (either in memory or a register) and
    // a payload. Unlike most instructions consuming a box, we ask for the type
    // second, so that the result can re-use the first input.
    MOZ_ASSERT(inner->type() == MIRType_Value);

    ensureDefined(inner);

    if (IsFloatingPointType(unbox->type())) {
        LUnboxFloatingPoint* lir = new(alloc()) LUnboxFloatingPoint(unbox->type());
        if (unbox->fallible())
            assignSnapshot(lir, unbox->bailoutKind());
        useBox(lir, LUnboxFloatingPoint::Input, inner);
        define(lir, unbox);
        return;
    }

    // Swap the order we use the box pieces so we can re-use the payload register.
    LUnbox* lir = new(alloc()) LUnbox;
    lir->setOperand(0, usePayloadInRegisterAtStart(inner));
    lir->setOperand(1, useType(inner, LUse::REGISTER));

    if (unbox->fallible())
        assignSnapshot(lir, unbox->bailoutKind());

    // Types and payloads form two separate intervals. If the type becomes dead
    // before the payload, it could be used as a Value without the type being
    // recoverable. Unbox's purpose is to eagerly kill the definition of a type
    // tag, so keeping both alive (for the purpose of gcmaps) is unappealing.
    // Instead, we create a new virtual register.
    defineReuseInput(lir, unbox, 0);
}

void
LIRGeneratorARM::visitReturn(MReturn* ret)
{
    MDefinition* opd = ret->getOperand(0);
    MOZ_ASSERT(opd->type() == MIRType_Value);

    LReturn* ins = new(alloc()) LReturn;
    ins->setOperand(0, LUse(JSReturnReg_Type));
    ins->setOperand(1, LUse(JSReturnReg_Data));
    fillBoxUses(ins, 0, opd);
    add(ins);
}

// x = !y
void
LIRGeneratorARM::lowerForALU(LInstructionHelper<1, 1, 0>* ins, MDefinition* mir, MDefinition* input)
{
    ins->setOperand(0, ins->snapshot() ? useRegister(input) : useRegisterAtStart(input));
    define(ins, mir, LDefinition(LDefinition::TypeFrom(mir->type()), LDefinition::REGISTER));
}

// z = x+y
void
LIRGeneratorARM::lowerForALU(LInstructionHelper<1, 2, 0>* ins, MDefinition* mir, MDefinition* lhs, MDefinition* rhs)
{
    // Some operations depend on checking inputs after writing the result, e.g.
    // MulI, but only for bail out paths so useAtStart when no bailouts.
    ins->setOperand(0, ins->snapshot() ? useRegister(lhs) : useRegisterAtStart(lhs));
    ins->setOperand(1, ins->snapshot() ? useRegisterOrConstant(rhs) :
                                         useRegisterOrConstantAtStart(rhs));
    define(ins, mir, LDefinition(LDefinition::TypeFrom(mir->type()), LDefinition::REGISTER));
}

void
LIRGeneratorARM::lowerForFPU(LInstructionHelper<1, 1, 0>* ins, MDefinition* mir, MDefinition* input)
{
    ins->setOperand(0, useRegisterAtStart(input));
    define(ins, mir, LDefinition(LDefinition::TypeFrom(mir->type()), LDefinition::REGISTER));
}

template<size_t Temps>
void
LIRGeneratorARM::lowerForFPU(LInstructionHelper<1, 2, Temps>* ins, MDefinition* mir, MDefinition* lhs, MDefinition* rhs)
{
    ins->setOperand(0, useRegisterAtStart(lhs));
    ins->setOperand(1, useRegisterAtStart(rhs));
    define(ins, mir, LDefinition(LDefinition::TypeFrom(mir->type()), LDefinition::REGISTER));
}

template void LIRGeneratorARM::lowerForFPU(LInstructionHelper<1, 2, 0>* ins, MDefinition* mir,
                                           MDefinition* lhs, MDefinition* rhs);
template void LIRGeneratorARM::lowerForFPU(LInstructionHelper<1, 2, 1>* ins, MDefinition* mir,
                                           MDefinition* lhs, MDefinition* rhs);

void
LIRGeneratorARM::lowerForBitAndAndBranch(LBitAndAndBranch* baab, MInstruction* mir,
                                         MDefinition* lhs, MDefinition* rhs)
{
    baab->setOperand(0, useRegisterAtStart(lhs));
    baab->setOperand(1, useRegisterOrConstantAtStart(rhs));
    add(baab, mir);
}

void
LIRGeneratorARM::defineUntypedPhi(MPhi* phi, size_t lirIndex)
{
    LPhi* type = current->getPhi(lirIndex + VREG_TYPE_OFFSET);
    LPhi* payload = current->getPhi(lirIndex + VREG_DATA_OFFSET);

    uint32_t typeVreg = getVirtualRegister();
    phi->setVirtualRegister(typeVreg);

    uint32_t payloadVreg = getVirtualRegister();
    MOZ_ASSERT(typeVreg + 1 == payloadVreg);

    type->setDef(0, LDefinition(typeVreg, LDefinition::TYPE));
    payload->setDef(0, LDefinition(payloadVreg, LDefinition::PAYLOAD));
    annotate(type);
    annotate(payload);
}

void
LIRGeneratorARM::lowerUntypedPhiInput(MPhi* phi, uint32_t inputPosition, LBlock* block, size_t lirIndex)
{
    // oh god, what is this code?
    MDefinition* operand = phi->getOperand(inputPosition);
    LPhi* type = block->getPhi(lirIndex + VREG_TYPE_OFFSET);
    LPhi* payload = block->getPhi(lirIndex + VREG_DATA_OFFSET);
    type->setOperand(inputPosition, LUse(operand->virtualRegister() + VREG_TYPE_OFFSET, LUse::ANY));
    payload->setOperand(inputPosition, LUse(VirtualRegisterOfPayload(operand), LUse::ANY));
}

void
LIRGeneratorARM::lowerForShift(LInstructionHelper<1, 2, 0>* ins, MDefinition* mir, MDefinition* lhs, MDefinition* rhs)
{
    ins->setOperand(0, useRegister(lhs));
    ins->setOperand(1, useRegisterOrConstant(rhs));
    define(ins, mir);
}

void
LIRGeneratorARM::lowerDivI(MDiv* div)
{
    if (div->isUnsigned()) {
        lowerUDiv(div);
        return;
    }

    // Division instructions are slow. Division by constant denominators can be
    // rewritten to use other instructions.
    if (div->rhs()->isConstant()) {
        int32_t rhs = div->rhs()->toConstant()->value().toInt32();
        // Check for division by a positive power of two, which is an easy and
        // important case to optimize. Note that other optimizations are also
        // possible; division by negative powers of two can be optimized in a
        // similar manner as positive powers of two, and division by other
        // constants can be optimized by a reciprocal multiplication technique.
        int32_t shift = FloorLog2(rhs);
        if (rhs > 0 && 1 << shift == rhs) {
            LDivPowTwoI* lir = new(alloc()) LDivPowTwoI(useRegisterAtStart(div->lhs()), shift);
            if (div->fallible())
                assignSnapshot(lir, Bailout_DoubleOutput);
            define(lir, div);
            return;
        }
    }

    if (HasIDIV()) {
        LDivI* lir = new(alloc()) LDivI(useRegister(div->lhs()), useRegister(div->rhs()), temp());
        if (div->fallible())
            assignSnapshot(lir, Bailout_DoubleOutput);
        define(lir, div);
        return;
    }

    LSoftDivI* lir = new(alloc()) LSoftDivI(useFixedAtStart(div->lhs(), r0), useFixedAtStart(div->rhs(), r1),
                                            tempFixed(r1), tempFixed(r2), tempFixed(r3));
    if (div->fallible())
        assignSnapshot(lir, Bailout_DoubleOutput);
    defineFixed(lir, div, LAllocation(AnyRegister(r0)));
}

void
LIRGeneratorARM::lowerMulI(MMul* mul, MDefinition* lhs, MDefinition* rhs)
{
    LMulI* lir = new(alloc()) LMulI;
    if (mul->fallible())
        assignSnapshot(lir, Bailout_DoubleOutput);
    lowerForALU(lir, mul, lhs, rhs);
}

void
LIRGeneratorARM::lowerModI(MMod* mod)
{
    if (mod->isUnsigned()) {
        lowerUMod(mod);
        return;
    }

    if (mod->rhs()->isConstant()) {
        int32_t rhs = mod->rhs()->toConstant()->value().toInt32();
        int32_t shift = FloorLog2(rhs);
        if (rhs > 0 && 1 << shift == rhs) {
            LModPowTwoI* lir = new(alloc()) LModPowTwoI(useRegister(mod->lhs()), shift);
            if (mod->fallible())
                assignSnapshot(lir, Bailout_DoubleOutput);
            define(lir, mod);
            return;
        }
        if (shift < 31 && (1 << (shift+1)) - 1 == rhs) {
            LModMaskI* lir = new(alloc()) LModMaskI(useRegister(mod->lhs()), temp(), temp(), shift+1);
            if (mod->fallible())
                assignSnapshot(lir, Bailout_DoubleOutput);
            define(lir, mod);
            return;
        }
    }

    if (HasIDIV()) {
        LModI* lir = new(alloc()) LModI(useRegister(mod->lhs()), useRegister(mod->rhs()), temp());
        if (mod->fallible())
            assignSnapshot(lir, Bailout_DoubleOutput);
        define(lir, mod);
        return;
    }

    LSoftModI* lir = new(alloc()) LSoftModI(useFixedAtStart(mod->lhs(), r0), useFixedAtStart(mod->rhs(), r1),
                                            tempFixed(r0), tempFixed(r2), tempFixed(r3),
                                            temp(LDefinition::GENERAL));
    if (mod->fallible())
        assignSnapshot(lir, Bailout_DoubleOutput);
    defineFixed(lir, mod, LAllocation(AnyRegister(r1)));
}

void
LIRGeneratorARM::visitPowHalf(MPowHalf* ins)
{
    MDefinition* input = ins->input();
    MOZ_ASSERT(input->type() == MIRType_Double);
    LPowHalfD* lir = new(alloc()) LPowHalfD(useRegisterAtStart(input));
    defineReuseInput(lir, ins, 0);
}

LTableSwitch*
LIRGeneratorARM::newLTableSwitch(const LAllocation& in, const LDefinition& inputCopy,
                                       MTableSwitch* tableswitch)
{
    return new(alloc()) LTableSwitch(in, inputCopy, tableswitch);
}

LTableSwitchV*
LIRGeneratorARM::newLTableSwitchV(MTableSwitch* tableswitch)
{
    return new(alloc()) LTableSwitchV(temp(), tempDouble(), tableswitch);
}

void
LIRGeneratorARM::visitGuardShape(MGuardShape* ins)
{
    MOZ_ASSERT(ins->obj()->type() == MIRType_Object);

    LDefinition tempObj = temp(LDefinition::OBJECT);
    LGuardShape* guard = new(alloc()) LGuardShape(useRegister(ins->obj()), tempObj);
    assignSnapshot(guard, ins->bailoutKind());
    add(guard, ins);
    redefine(ins, ins->obj());
}

void
LIRGeneratorARM::visitGuardObjectGroup(MGuardObjectGroup* ins)
{
    MOZ_ASSERT(ins->obj()->type() == MIRType_Object);

    LDefinition tempObj = temp(LDefinition::OBJECT);
    LGuardObjectGroup* guard = new(alloc()) LGuardObjectGroup(useRegister(ins->obj()), tempObj);
    assignSnapshot(guard, ins->bailoutKind());
    add(guard, ins);
    redefine(ins, ins->obj());
}

void
LIRGeneratorARM::lowerUrshD(MUrsh* mir)
{
    MDefinition* lhs = mir->lhs();
    MDefinition* rhs = mir->rhs();

    MOZ_ASSERT(lhs->type() == MIRType_Int32);
    MOZ_ASSERT(rhs->type() == MIRType_Int32);

    LUrshD* lir = new(alloc()) LUrshD(useRegister(lhs), useRegisterOrConstant(rhs), temp());
    define(lir, mir);
}

void
LIRGeneratorARM::visitAsmJSNeg(MAsmJSNeg* ins)
{
    if (ins->type() == MIRType_Int32) {
        define(new(alloc()) LNegI(useRegisterAtStart(ins->input())), ins);
    } else if (ins->type() == MIRType_Float32) {
        define(new(alloc()) LNegF(useRegisterAtStart(ins->input())), ins);
    } else {
        MOZ_ASSERT(ins->type() == MIRType_Double);
        define(new(alloc()) LNegD(useRegisterAtStart(ins->input())), ins);
    }
}

void
LIRGeneratorARM::lowerUDiv(MDiv* div)
{
    MDefinition* lhs = div->getOperand(0);
    MDefinition* rhs = div->getOperand(1);

    if (HasIDIV()) {
        LUDiv* lir = new(alloc()) LUDiv;
        lir->setOperand(0, useRegister(lhs));
        lir->setOperand(1, useRegister(rhs));
        if (div->fallible())
            assignSnapshot(lir, Bailout_DoubleOutput);
        define(lir, div);
    } else {
        LSoftUDivOrMod* lir = new(alloc()) LSoftUDivOrMod(useFixedAtStart(lhs, r0), useFixedAtStart(rhs, r1),
                                                          tempFixed(r1), tempFixed(r2), tempFixed(r3));
        if (div->fallible())
            assignSnapshot(lir, Bailout_DoubleOutput);
        defineFixed(lir, div, LAllocation(AnyRegister(r0)));
    }
}

void
LIRGeneratorARM::lowerUMod(MMod* mod)
{
    MDefinition* lhs = mod->getOperand(0);
    MDefinition* rhs = mod->getOperand(1);

    if (HasIDIV()) {
        LUMod* lir = new(alloc()) LUMod;
        lir->setOperand(0, useRegister(lhs));
        lir->setOperand(1, useRegister(rhs));
        if (mod->fallible())
            assignSnapshot(lir, Bailout_DoubleOutput);
        define(lir, mod);
    } else {
        LSoftUDivOrMod* lir = new(alloc()) LSoftUDivOrMod(useFixedAtStart(lhs, r0), useFixedAtStart(rhs, r1),
                                                          tempFixed(r0), tempFixed(r2), tempFixed(r3));
        if (mod->fallible())
            assignSnapshot(lir, Bailout_DoubleOutput);
        defineFixed(lir, mod, LAllocation(AnyRegister(r1)));
    }
}

void
LIRGeneratorARM::visitAsmJSUnsignedToDouble(MAsmJSUnsignedToDouble* ins)
{
    MOZ_ASSERT(ins->input()->type() == MIRType_Int32);
    LAsmJSUInt32ToDouble* lir = new(alloc()) LAsmJSUInt32ToDouble(useRegisterAtStart(ins->input()));
    define(lir, ins);
}

void
LIRGeneratorARM::visitAsmJSUnsignedToFloat32(MAsmJSUnsignedToFloat32* ins)
{
    MOZ_ASSERT(ins->input()->type() == MIRType_Int32);
    LAsmJSUInt32ToFloat32* lir = new(alloc()) LAsmJSUInt32ToFloat32(useRegisterAtStart(ins->input()));
    define(lir, ins);
}

void
LIRGeneratorARM::visitAsmJSLoadHeap(MAsmJSLoadHeap* ins)
{
    MDefinition* ptr = ins->ptr();
    MOZ_ASSERT(ptr->type() == MIRType_Int32);
    LAllocation ptrAlloc;

    // For the ARM it is best to keep the 'ptr' in a register if a bounds check is needed.
    if (ptr->isConstantValue() && !ins->needsBoundsCheck()) {
        // A bounds check is only skipped for a positive index.
        MOZ_ASSERT(ptr->constantValue().toInt32() >= 0);
        ptrAlloc = LAllocation(ptr->constantVp());
    } else {
        ptrAlloc = useRegisterAtStart(ptr);
    }

    define(new(alloc()) LAsmJSLoadHeap(ptrAlloc), ins);
}

void
LIRGeneratorARM::visitAsmJSStoreHeap(MAsmJSStoreHeap* ins)
{
    MDefinition* ptr = ins->ptr();
    MOZ_ASSERT(ptr->type() == MIRType_Int32);
    LAllocation ptrAlloc;

    if (ptr->isConstantValue() && !ins->needsBoundsCheck()) {
        MOZ_ASSERT(ptr->constantValue().toInt32() >= 0);
        ptrAlloc = LAllocation(ptr->constantVp());
    } else {
        ptrAlloc = useRegisterAtStart(ptr);
    }

    add(new(alloc()) LAsmJSStoreHeap(ptrAlloc, useRegisterAtStart(ins->value())), ins);
}

void
LIRGeneratorARM::visitAsmJSLoadFuncPtr(MAsmJSLoadFuncPtr* ins)
{
    define(new(alloc()) LAsmJSLoadFuncPtr(useRegister(ins->index()), temp()), ins);
}

void
LIRGeneratorARM::lowerTruncateDToInt32(MTruncateToInt32* ins)
{
    MDefinition* opd = ins->input();
    MOZ_ASSERT(opd->type() == MIRType_Double);

    define(new(alloc()) LTruncateDToInt32(useRegister(opd), LDefinition::BogusTemp()), ins);
}

void
LIRGeneratorARM::lowerTruncateFToInt32(MTruncateToInt32* ins)
{
    MDefinition* opd = ins->input();
    MOZ_ASSERT(opd->type() == MIRType_Float32);

    define(new(alloc()) LTruncateFToInt32(useRegister(opd), LDefinition::BogusTemp()), ins);
}

void
LIRGeneratorARM::visitStoreTypedArrayElementStatic(MStoreTypedArrayElementStatic* ins)
{
    MOZ_CRASH("NYI");
}

void
LIRGeneratorARM::visitSimdBinaryArith(MSimdBinaryArith* ins)
{
    MOZ_CRASH("NYI");
}

void
LIRGeneratorARM::visitSimdSelect(MSimdSelect* ins)
{
    MOZ_CRASH("NYI");
}

void
LIRGeneratorARM::visitSimdSplatX4(MSimdSplatX4* ins)
{
    MOZ_CRASH("NYI");
}

void
LIRGeneratorARM::visitSimdValueX4(MSimdValueX4* ins)
{
    MOZ_CRASH("NYI");
}

void
LIRGeneratorARM::visitAtomicExchangeTypedArrayElement(MAtomicExchangeTypedArrayElement* ins)
{
    MOZ_ASSERT(HasLDSTREXBHD());
    MOZ_ASSERT(ins->arrayType() <= Scalar::Uint32);

    MOZ_ASSERT(ins->elements()->type() == MIRType_Elements);
    MOZ_ASSERT(ins->index()->type() == MIRType_Int32);

    const LUse elements = useRegister(ins->elements());
    const LAllocation index = useRegisterOrConstant(ins->index());

    // If the target is a floating register then we need a temp at the
    // CodeGenerator level for creating the result.

    const LAllocation value = useRegister(ins->value());
    LDefinition tempDef = LDefinition::BogusTemp();
    if (ins->arrayType() == Scalar::Uint32) {
        MOZ_ASSERT(ins->type() == MIRType_Double);
        tempDef = temp();
    }

    LAtomicExchangeTypedArrayElement* lir =
        new(alloc()) LAtomicExchangeTypedArrayElement(elements, index, value, tempDef);

    define(lir, ins);
}

void
LIRGeneratorARM::visitAtomicTypedArrayElementBinop(MAtomicTypedArrayElementBinop* ins)
{
    MOZ_ASSERT(ins->arrayType() != Scalar::Uint8Clamped);
    MOZ_ASSERT(ins->arrayType() != Scalar::Float32);
    MOZ_ASSERT(ins->arrayType() != Scalar::Float64);

    MOZ_ASSERT(ins->elements()->type() == MIRType_Elements);
    MOZ_ASSERT(ins->index()->type() == MIRType_Int32);

    const LUse elements = useRegister(ins->elements());
    const LAllocation index = useRegisterOrConstant(ins->index());
    const LAllocation value = useRegister(ins->value());

    if (!ins->hasUses()) {
        LAtomicTypedArrayElementBinopForEffect* lir =
            new(alloc()) LAtomicTypedArrayElementBinopForEffect(elements, index, value,
                                                                /* flagTemp= */ temp());
        add(lir, ins);
        return;
    }

    // For a Uint32Array with a known double result we need a temp for
    // the intermediate output.
    //
    // Optimization opportunity (bug 1077317): We can do better by
    // allowing 'value' to remain as an imm32 if it is small enough to
    // fit in an instruction.

    LDefinition flagTemp = temp();
    LDefinition outTemp = LDefinition::BogusTemp();

    if (ins->arrayType() == Scalar::Uint32 && IsFloatingPointType(ins->type()))
        outTemp = temp();

    // On arm, map flagTemp to temp1 and outTemp to temp2, at least for now.

    LAtomicTypedArrayElementBinop* lir =
        new(alloc()) LAtomicTypedArrayElementBinop(elements, index, value, flagTemp, outTemp);
    define(lir, ins);
}

void
LIRGeneratorARM::visitCompareExchangeTypedArrayElement(MCompareExchangeTypedArrayElement* ins)
{
    MOZ_ASSERT(ins->arrayType() != Scalar::Float32);
    MOZ_ASSERT(ins->arrayType() != Scalar::Float64);

    MOZ_ASSERT(ins->elements()->type() == MIRType_Elements);
    MOZ_ASSERT(ins->index()->type() == MIRType_Int32);

    const LUse elements = useRegister(ins->elements());
    const LAllocation index = useRegisterOrConstant(ins->index());

    // If the target is a floating register then we need a temp at the
    // CodeGenerator level for creating the result.
    //
    // Optimization opportunity (bug 1077317): We could do better by
    // allowing oldval to remain an immediate, if it is small enough
    // to fit in an instruction.

    const LAllocation newval = useRegister(ins->newval());
    const LAllocation oldval = useRegister(ins->oldval());
    LDefinition tempDef = LDefinition::BogusTemp();
    if (ins->arrayType() == Scalar::Uint32 && IsFloatingPointType(ins->type()))
        tempDef = temp();

    LCompareExchangeTypedArrayElement* lir =
        new(alloc()) LCompareExchangeTypedArrayElement(elements, index, oldval, newval, tempDef);

    define(lir, ins);
}

void
LIRGeneratorARM::visitAsmJSCompareExchangeHeap(MAsmJSCompareExchangeHeap* ins)
{
    MOZ_ASSERT(ins->accessType() < Scalar::Float32);

    MDefinition* ptr = ins->ptr();
    MOZ_ASSERT(ptr->type() == MIRType_Int32);

    if (byteSize(ins->accessType()) != 4 && !HasLDSTREXBHD()) {
        LAsmJSCompareExchangeCallout* lir =
            new(alloc()) LAsmJSCompareExchangeCallout(useRegisterAtStart(ptr),
                                                      useRegisterAtStart(ins->oldValue()),
                                                      useRegisterAtStart(ins->newValue()));
        defineReturn(lir, ins);
        return;
    }

    LAsmJSCompareExchangeHeap* lir =
        new(alloc()) LAsmJSCompareExchangeHeap(useRegister(ptr),
                                               useRegister(ins->oldValue()),
                                               useRegister(ins->newValue()));

    define(lir, ins);
}

void
LIRGeneratorARM::visitAsmJSAtomicExchangeHeap(MAsmJSAtomicExchangeHeap* ins)
{
    MOZ_ASSERT(ins->ptr()->type() == MIRType_Int32);
    MOZ_ASSERT(ins->accessType() < Scalar::Float32);

    const LAllocation ptr = useRegisterAtStart(ins->ptr());
    const LAllocation value = useRegisterAtStart(ins->value());

    if (byteSize(ins->accessType()) < 4 && !HasLDSTREXBHD()) {
        // Call out on ARMv6.
        defineReturn(new(alloc()) LAsmJSAtomicExchangeCallout(ptr, value), ins);
        return;
    }

    define(new(alloc()) LAsmJSAtomicExchangeHeap(ptr, value), ins);
}

void
LIRGeneratorARM::visitAsmJSAtomicBinopHeap(MAsmJSAtomicBinopHeap* ins)
{
    MOZ_ASSERT(ins->accessType() < Scalar::Float32);

    MDefinition* ptr = ins->ptr();
    MOZ_ASSERT(ptr->type() == MIRType_Int32);

    if (byteSize(ins->accessType()) != 4 && !HasLDSTREXBHD()) {
        LAsmJSAtomicBinopCallout* lir =
            new(alloc()) LAsmJSAtomicBinopCallout(useRegisterAtStart(ptr),
                                                  useRegisterAtStart(ins->value()));
        defineReturn(lir, ins);
        return;
    }

    if (!ins->hasUses()) {
        LAsmJSAtomicBinopHeapForEffect* lir =
            new(alloc()) LAsmJSAtomicBinopHeapForEffect(useRegister(ptr),
                                                        useRegister(ins->value()),
                                                        /* flagTemp= */ temp());
        add(lir, ins);
        return;
    }

    LAsmJSAtomicBinopHeap* lir =
        new(alloc()) LAsmJSAtomicBinopHeap(useRegister(ptr),
                                           useRegister(ins->value()),
                                           /* temp = */ LDefinition::BogusTemp(),
                                           /* flagTemp= */ temp());
    define(lir, ins);
}

void
LIRGeneratorARM::visitSubstr(MSubstr* ins)
{
    LSubstr* lir = new (alloc()) LSubstr(useRegister(ins->string()),
                                         useRegister(ins->begin()),
                                         useRegister(ins->length()),
                                         temp(),
                                         temp(),
                                         tempByteOpRegister());
    define(lir, ins);
    assignSafepoint(lir, ins);
}

void
LIRGeneratorARM::visitRandom(MRandom* ins)
{
    LRandom *lir = new(alloc()) LRandom(temp(),
                                        temp(),
                                        temp(),
                                        temp(),
                                        temp());
    defineFixed(lir, ins, LFloatReg(ReturnDoubleReg));
}
