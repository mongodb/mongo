/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/mips64/Lowering-mips64.h"

#include "jit/mips64/Assembler-mips64.h"

#include "jit/MIR.h"

#include "jit/shared/Lowering-shared-inl.h"

using namespace js;
using namespace js::jit;

void
LIRGeneratorMIPS64::useBoxFixed(LInstruction* lir, size_t n, MDefinition* mir, Register reg1,
                                Register reg2, bool useAtStart)
{
    MOZ_ASSERT(mir->type() == MIRType_Value);

    ensureDefined(mir);
    lir->setOperand(n, LUse(reg1, mir->virtualRegister(), useAtStart));
}

void
LIRGeneratorMIPS64::visitBox(MBox* box)
{
    MDefinition* opd = box->getOperand(0);

    // If the operand is a constant, emit near its uses.
    if (opd->isConstant() && box->canEmitAtUses()) {
        emitAtUses(box);
        return;
    }

    if (opd->isConstant()) {
        define(new(alloc()) LValue(opd->toConstant()->value()), box, LDefinition(LDefinition::BOX));
    } else {
        LBox* ins = new(alloc()) LBox(useRegister(opd), opd->type());
        define(ins, box, LDefinition(LDefinition::BOX));
    }
}

void
LIRGeneratorMIPS64::visitUnbox(MUnbox* unbox)
{
    MDefinition* box = unbox->getOperand(0);

    if (box->type() == MIRType_ObjectOrNull) {
        LUnboxObjectOrNull* lir = new(alloc()) LUnboxObjectOrNull(useRegisterAtStart(box));
        if (unbox->fallible())
            assignSnapshot(lir, unbox->bailoutKind());
        defineReuseInput(lir, unbox, 0);
        return;
    }

    MOZ_ASSERT(box->type() == MIRType_Value);

    LUnbox* lir;
    if (IsFloatingPointType(unbox->type())) {
        lir = new(alloc()) LUnboxFloatingPoint(useRegisterAtStart(box), unbox->type());
    } else if (unbox->fallible()) {
        // If the unbox is fallible, load the Value in a register first to
        // avoid multiple loads.
        lir = new(alloc()) LUnbox(useRegisterAtStart(box));
    } else {
        lir = new(alloc()) LUnbox(useAtStart(box));
    }

    if (unbox->fallible())
        assignSnapshot(lir, unbox->bailoutKind());

    define(lir, unbox);
}

void
LIRGeneratorMIPS64::visitReturn(MReturn* ret)
{
    MDefinition* opd = ret->getOperand(0);
    MOZ_ASSERT(opd->type() == MIRType_Value);

    LReturn* ins = new(alloc()) LReturn;
    ins->setOperand(0, useFixed(opd, JSReturnReg));
    add(ins);
}

void
LIRGeneratorMIPS64::defineUntypedPhi(MPhi* phi, size_t lirIndex)
{
    defineTypedPhi(phi, lirIndex);
}

void
LIRGeneratorMIPS64::lowerUntypedPhiInput(MPhi* phi, uint32_t inputPosition,
                                         LBlock* block, size_t lirIndex)
{
    lowerTypedPhiInput(phi, inputPosition, block, lirIndex);
}

void
LIRGeneratorMIPS64::lowerTruncateDToInt32(MTruncateToInt32* ins)
{
    MDefinition* opd = ins->input();
    MOZ_ASSERT(opd->type() == MIRType_Double);

    define(new(alloc())
           LTruncateDToInt32(useRegister(opd), tempDouble()), ins);
}

void
LIRGeneratorMIPS64::lowerTruncateFToInt32(MTruncateToInt32* ins)
{
    MDefinition* opd = ins->input();
    MOZ_ASSERT(opd->type() == MIRType_Float32);

    define(new(alloc())
           LTruncateFToInt32(useRegister(opd), tempFloat32()), ins);
}

void
LIRGeneratorMIPS64::visitRandom(MRandom* ins)
{
    LRandom *lir = new(alloc()) LRandom(temp(), temp(), temp());
    defineFixed(lir, ins, LFloatReg(ReturnDoubleReg));
}
