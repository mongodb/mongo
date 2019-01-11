/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/mips32/Lowering-mips32.h"

#include "jit/mips32/Assembler-mips32.h"

#include "jit/MIR.h"

#include "jit/shared/Lowering-shared-inl.h"

using namespace js;
using namespace js::jit;

LBoxAllocation
LIRGeneratorMIPS::useBoxFixed(MDefinition* mir, Register reg1, Register reg2, bool useAtStart)
{
    MOZ_ASSERT(mir->type() == MIRType::Value);
    MOZ_ASSERT(reg1 != reg2);

    ensureDefined(mir);
    return LBoxAllocation(LUse(reg1, mir->virtualRegister(), useAtStart),
                          LUse(reg2, VirtualRegisterOfPayload(mir), useAtStart));
}

void
LIRGeneratorMIPS::visitBox(MBox* box)
{
    MDefinition* inner = box->getOperand(0);

    // If the box wrapped a double, it needs a new register.
    if (IsFloatingPointType(inner->type())) {
        defineBox(new(alloc()) LBoxFloatingPoint(useRegisterAtStart(inner),
                                                 tempCopy(inner, 0), inner->type()), box);
        return;
    }

    if (box->canEmitAtUses()) {
        emitAtUses(box);
        return;
    }

    if (inner->isConstant()) {
        defineBox(new(alloc()) LValue(inner->toConstant()->toJSValue()), box);
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
LIRGeneratorMIPS::visitUnbox(MUnbox* unbox)
{
    MDefinition* inner = unbox->getOperand(0);

    if (inner->type() == MIRType::ObjectOrNull) {
        LUnboxObjectOrNull* lir = new(alloc()) LUnboxObjectOrNull(useRegisterAtStart(inner));
        if (unbox->fallible())
            assignSnapshot(lir, unbox->bailoutKind());
        defineReuseInput(lir, unbox, 0);
        return;
    }

    // An unbox on mips reads in a type tag (either in memory or a register) and
    // a payload. Unlike most instructions consuming a box, we ask for the type
    // second, so that the result can re-use the first input.
    MOZ_ASSERT(inner->type() == MIRType::Value);

    ensureDefined(inner);

    if (IsFloatingPointType(unbox->type())) {
        LUnboxFloatingPoint* lir = new(alloc()) LUnboxFloatingPoint(useBox(inner), unbox->type());
        if (unbox->fallible())
            assignSnapshot(lir, unbox->bailoutKind());
        define(lir, unbox);
        return;
    }

    // Swap the order we use the box pieces so we can re-use the payload
    // register.
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
LIRGeneratorMIPS::visitReturn(MReturn* ret)
{
    MDefinition* opd = ret->getOperand(0);
    MOZ_ASSERT(opd->type() == MIRType::Value);

    LReturn* ins = new(alloc()) LReturn;
    ins->setOperand(0, LUse(JSReturnReg_Type));
    ins->setOperand(1, LUse(JSReturnReg_Data));
    fillBoxUses(ins, 0, opd);
    add(ins);
}

void
LIRGeneratorMIPS::defineUntypedPhi(MPhi* phi, size_t lirIndex)
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
LIRGeneratorMIPS::lowerUntypedPhiInput(MPhi* phi, uint32_t inputPosition,
                                       LBlock* block, size_t lirIndex)
{
    MDefinition* operand = phi->getOperand(inputPosition);
    LPhi* type = block->getPhi(lirIndex + VREG_TYPE_OFFSET);
    LPhi* payload = block->getPhi(lirIndex + VREG_DATA_OFFSET);
    type->setOperand(inputPosition, LUse(operand->virtualRegister() + VREG_TYPE_OFFSET,
                                         LUse::ANY));
    payload->setOperand(inputPosition, LUse(VirtualRegisterOfPayload(operand), LUse::ANY));
}

void
LIRGeneratorMIPS::defineInt64Phi(MPhi* phi, size_t lirIndex)
{
    LPhi* low = current->getPhi(lirIndex + INT64LOW_INDEX);
    LPhi* high = current->getPhi(lirIndex + INT64HIGH_INDEX);

    uint32_t lowVreg = getVirtualRegister();

    phi->setVirtualRegister(lowVreg);

    uint32_t highVreg = getVirtualRegister();
    MOZ_ASSERT(lowVreg + INT64HIGH_INDEX == highVreg + INT64LOW_INDEX);

    low->setDef(0, LDefinition(lowVreg, LDefinition::INT32));
    high->setDef(0, LDefinition(highVreg, LDefinition::INT32));
    annotate(high);
    annotate(low);
}

void
LIRGeneratorMIPS::lowerInt64PhiInput(MPhi* phi, uint32_t inputPosition,
                                     LBlock* block, size_t lirIndex)
{
    MDefinition* operand = phi->getOperand(inputPosition);
    LPhi* low = block->getPhi(lirIndex + INT64LOW_INDEX);
    LPhi* high = block->getPhi(lirIndex + INT64HIGH_INDEX);
    low->setOperand(inputPosition, LUse(operand->virtualRegister() + INT64LOW_INDEX, LUse::ANY));
    high->setOperand(inputPosition, LUse(operand->virtualRegister() + INT64HIGH_INDEX, LUse::ANY));
}

void
LIRGeneratorMIPS::lowerTruncateDToInt32(MTruncateToInt32* ins)
{
    MDefinition* opd = ins->input();
    MOZ_ASSERT(opd->type() == MIRType::Double);

    define(new(alloc()) LTruncateDToInt32(useRegister(opd), LDefinition::BogusTemp()), ins);
}

void
LIRGeneratorMIPS::lowerTruncateFToInt32(MTruncateToInt32* ins)
{
    MDefinition* opd = ins->input();
    MOZ_ASSERT(opd->type() == MIRType::Float32);

    define(new(alloc()) LTruncateFToInt32(useRegister(opd), LDefinition::BogusTemp()), ins);
}

void
LIRGeneratorMIPS::lowerDivI64(MDiv* div)
{
    if (div->isUnsigned()) {
        lowerUDivI64(div);
        return;
    }

    LDivOrModI64* lir = new(alloc()) LDivOrModI64(useInt64RegisterAtStart(div->lhs()),
                                                  useInt64RegisterAtStart(div->rhs()));

    defineReturn(lir, div);
}

void
LIRGeneratorMIPS::lowerModI64(MMod* mod)
{
    if (mod->isUnsigned()) {
        lowerUModI64(mod);
        return;
    }

    LDivOrModI64* lir = new(alloc()) LDivOrModI64(useInt64RegisterAtStart(mod->lhs()),
                                                  useInt64RegisterAtStart(mod->rhs()));

    defineReturn(lir, mod);
}

void
LIRGeneratorMIPS::lowerUDivI64(MDiv* div)
{
    LUDivOrModI64* lir = new(alloc()) LUDivOrModI64(useInt64RegisterAtStart(div->lhs()),
                                                    useInt64RegisterAtStart(div->rhs()));
    defineReturn(lir, div);
}

void
LIRGeneratorMIPS::lowerUModI64(MMod* mod)
{
    LUDivOrModI64* lir = new(alloc()) LUDivOrModI64(useInt64RegisterAtStart(mod->lhs()),
                                                    useInt64RegisterAtStart(mod->rhs()));
    defineReturn(lir, mod);
}

void
LIRGeneratorMIPS::visitRandom(MRandom* ins)
{
    LRandom *lir = new(alloc()) LRandom(temp(),
                                        temp(),
                                        temp(),
                                        temp(),
                                        temp());
    defineFixed(lir, ins, LFloatReg(ReturnDoubleReg));
}


void
LIRGeneratorMIPS::visitWasmTruncateToInt64(MWasmTruncateToInt64* ins)
{
    MDefinition* opd = ins->input();
    MOZ_ASSERT(opd->type() == MIRType::Double || opd->type() == MIRType::Float32);

    defineReturn(new(alloc()) LWasmTruncateToInt64(useRegisterAtStart(opd)), ins);
}

void
LIRGeneratorMIPS::visitInt64ToFloatingPoint(MInt64ToFloatingPoint* ins)
{
    MDefinition* opd = ins->input();
    MOZ_ASSERT(opd->type() == MIRType::Int64);
    MOZ_ASSERT(IsFloatingPointType(ins->type()));

    defineReturn(new(alloc()) LInt64ToFloatingPoint(useInt64RegisterAtStart(opd)), ins);
}
