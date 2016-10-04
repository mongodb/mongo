/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/mips-shared/Lowering-mips-shared.h"

#include "mozilla/MathAlgorithms.h"

#include "jit/MIR.h"

#include "jit/shared/Lowering-shared-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::FloorLog2;

LAllocation
LIRGeneratorMIPSShared::useByteOpRegister(MDefinition* mir)
{
    return useRegister(mir);
}

LAllocation
LIRGeneratorMIPSShared::useByteOpRegisterOrNonDoubleConstant(MDefinition* mir)
{
    return useRegisterOrNonDoubleConstant(mir);
}

LDefinition
LIRGeneratorMIPSShared::tempByteOpRegister()
{
    return temp();
}

// x = !y
void
LIRGeneratorMIPSShared::lowerForALU(LInstructionHelper<1, 1, 0>* ins,
                                    MDefinition* mir, MDefinition* input)
{
    ins->setOperand(0, useRegister(input));
    define(ins, mir, LDefinition(LDefinition::TypeFrom(mir->type()), LDefinition::REGISTER));
}

// z = x+y
void
LIRGeneratorMIPSShared::lowerForALU(LInstructionHelper<1, 2, 0>* ins, MDefinition* mir,
                                    MDefinition* lhs, MDefinition* rhs)
{
    ins->setOperand(0, useRegister(lhs));
    ins->setOperand(1, useRegisterOrConstant(rhs));
    define(ins, mir, LDefinition(LDefinition::TypeFrom(mir->type()), LDefinition::REGISTER));
}

void
LIRGeneratorMIPSShared::lowerForFPU(LInstructionHelper<1, 1, 0>* ins, MDefinition* mir,
                                    MDefinition* input)
{
    ins->setOperand(0, useRegister(input));
    define(ins, mir, LDefinition(LDefinition::TypeFrom(mir->type()), LDefinition::REGISTER));
}

template<size_t Temps>
void
LIRGeneratorMIPSShared::lowerForFPU(LInstructionHelper<1, 2, Temps>* ins, MDefinition* mir,
                                    MDefinition* lhs, MDefinition* rhs)
{
    ins->setOperand(0, useRegister(lhs));
    ins->setOperand(1, useRegister(rhs));
    define(ins, mir, LDefinition(LDefinition::TypeFrom(mir->type()), LDefinition::REGISTER));
}

template void LIRGeneratorMIPSShared::lowerForFPU(LInstructionHelper<1, 2, 0>* ins, MDefinition* mir,
                                                  MDefinition* lhs, MDefinition* rhs);
template void LIRGeneratorMIPSShared::lowerForFPU(LInstructionHelper<1, 2, 1>* ins, MDefinition* mir,
                                                  MDefinition* lhs, MDefinition* rhs);

void
LIRGeneratorMIPSShared::lowerForBitAndAndBranch(LBitAndAndBranch* baab, MInstruction* mir,
                                                MDefinition* lhs, MDefinition* rhs)
{
    baab->setOperand(0, useRegisterAtStart(lhs));
    baab->setOperand(1, useRegisterOrConstantAtStart(rhs));
    add(baab, mir);
}

void
LIRGeneratorMIPSShared::lowerForShift(LInstructionHelper<1, 2, 0>* ins, MDefinition* mir,
                                      MDefinition* lhs, MDefinition* rhs)
{
    ins->setOperand(0, useRegister(lhs));
    ins->setOperand(1, useRegisterOrConstant(rhs));
    define(ins, mir);
}

void
LIRGeneratorMIPSShared::lowerDivI(MDiv* div)
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
            LDivPowTwoI* lir = new(alloc()) LDivPowTwoI(useRegister(div->lhs()), shift, temp());
            if (div->fallible())
                assignSnapshot(lir, Bailout_DoubleOutput);
            define(lir, div);
            return;
        }
    }

    LDivI* lir = new(alloc()) LDivI(useRegister(div->lhs()), useRegister(div->rhs()), temp());
    if (div->fallible())
        assignSnapshot(lir, Bailout_DoubleOutput);
    define(lir, div);
}

void
LIRGeneratorMIPSShared::lowerMulI(MMul* mul, MDefinition* lhs, MDefinition* rhs)
{
    LMulI* lir = new(alloc()) LMulI;
    if (mul->fallible())
        assignSnapshot(lir, Bailout_DoubleOutput);

    lowerForALU(lir, mul, lhs, rhs);
}

void
LIRGeneratorMIPSShared::lowerModI(MMod* mod)
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
        } else if (shift < 31 && (1 << (shift + 1)) - 1 == rhs) {
            LModMaskI* lir = new(alloc()) LModMaskI(useRegister(mod->lhs()),
                                                    temp(LDefinition::GENERAL),
                                                    temp(LDefinition::GENERAL),
                                                    shift + 1);
            if (mod->fallible())
                assignSnapshot(lir, Bailout_DoubleOutput);
            define(lir, mod);
            return;
        }
    }
    LModI* lir = new(alloc()) LModI(useRegister(mod->lhs()), useRegister(mod->rhs()),
                           temp(LDefinition::GENERAL));

    if (mod->fallible())
        assignSnapshot(lir, Bailout_DoubleOutput);
    define(lir, mod);
}

void
LIRGeneratorMIPSShared::visitPowHalf(MPowHalf* ins)
{
    MDefinition* input = ins->input();
    MOZ_ASSERT(input->type() == MIRType_Double);
    LPowHalfD* lir = new(alloc()) LPowHalfD(useRegisterAtStart(input));
    defineReuseInput(lir, ins, 0);
}

LTableSwitch*
LIRGeneratorMIPSShared::newLTableSwitch(const LAllocation& in, const LDefinition& inputCopy,
                                        MTableSwitch* tableswitch)
{
    return new(alloc()) LTableSwitch(in, inputCopy, temp(), tableswitch);
}

LTableSwitchV*
LIRGeneratorMIPSShared::newLTableSwitchV(MTableSwitch* tableswitch)
{
    return new(alloc()) LTableSwitchV(temp(), tempDouble(), temp(), tableswitch);
}

void
LIRGeneratorMIPSShared::visitGuardShape(MGuardShape* ins)
{
    MOZ_ASSERT(ins->obj()->type() == MIRType_Object);

    LDefinition tempObj = temp(LDefinition::OBJECT);
    LGuardShape* guard = new(alloc()) LGuardShape(useRegister(ins->obj()), tempObj);
    assignSnapshot(guard, ins->bailoutKind());
    add(guard, ins);
    redefine(ins, ins->obj());
}

void
LIRGeneratorMIPSShared::visitGuardObjectGroup(MGuardObjectGroup* ins)
{
    MOZ_ASSERT(ins->obj()->type() == MIRType_Object);

    LDefinition tempObj = temp(LDefinition::OBJECT);
    LGuardObjectGroup* guard = new(alloc()) LGuardObjectGroup(useRegister(ins->obj()), tempObj);
    assignSnapshot(guard, ins->bailoutKind());
    add(guard, ins);
    redefine(ins, ins->obj());
}

void
LIRGeneratorMIPSShared::lowerUrshD(MUrsh* mir)
{
    MDefinition* lhs = mir->lhs();
    MDefinition* rhs = mir->rhs();

    MOZ_ASSERT(lhs->type() == MIRType_Int32);
    MOZ_ASSERT(rhs->type() == MIRType_Int32);

    LUrshD* lir = new(alloc()) LUrshD(useRegister(lhs), useRegisterOrConstant(rhs), temp());
    define(lir, mir);
}

void
LIRGeneratorMIPSShared::visitAsmJSNeg(MAsmJSNeg* ins)
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
LIRGeneratorMIPSShared::lowerUDiv(MDiv* div)
{
    MDefinition* lhs = div->getOperand(0);
    MDefinition* rhs = div->getOperand(1);

    LUDivOrMod* lir = new(alloc()) LUDivOrMod;
    lir->setOperand(0, useRegister(lhs));
    lir->setOperand(1, useRegister(rhs));
    if (div->fallible())
        assignSnapshot(lir, Bailout_DoubleOutput);

    define(lir, div);
}

void
LIRGeneratorMIPSShared::lowerUMod(MMod* mod)
{
    MDefinition* lhs = mod->getOperand(0);
    MDefinition* rhs = mod->getOperand(1);

    LUDivOrMod* lir = new(alloc()) LUDivOrMod;
    lir->setOperand(0, useRegister(lhs));
    lir->setOperand(1, useRegister(rhs));
    if (mod->fallible())
        assignSnapshot(lir, Bailout_DoubleOutput);

    define(lir, mod);
}

void
LIRGeneratorMIPSShared::visitAsmJSUnsignedToDouble(MAsmJSUnsignedToDouble* ins)
{
    MOZ_ASSERT(ins->input()->type() == MIRType_Int32);
    LAsmJSUInt32ToDouble* lir = new(alloc()) LAsmJSUInt32ToDouble(useRegisterAtStart(ins->input()));
    define(lir, ins);
}

void
LIRGeneratorMIPSShared::visitAsmJSUnsignedToFloat32(MAsmJSUnsignedToFloat32* ins)
{
    MOZ_ASSERT(ins->input()->type() == MIRType_Int32);
    LAsmJSUInt32ToFloat32* lir = new(alloc()) LAsmJSUInt32ToFloat32(useRegisterAtStart(ins->input()));
    define(lir, ins);
}

void
LIRGeneratorMIPSShared::visitAsmJSLoadHeap(MAsmJSLoadHeap* ins)
{
    MDefinition* ptr = ins->ptr();
    MOZ_ASSERT(ptr->type() == MIRType_Int32);
    LAllocation ptrAlloc;

    // For MIPS it is best to keep the 'ptr' in a register if a bounds check
    // is needed.
    if (ptr->isConstantValue() && !ins->needsBoundsCheck()) {
        // A bounds check is only skipped for a positive index.
        MOZ_ASSERT(ptr->constantValue().toInt32() >= 0);
        ptrAlloc = LAllocation(ptr->constantVp());
    } else
        ptrAlloc = useRegisterAtStart(ptr);

    define(new(alloc()) LAsmJSLoadHeap(ptrAlloc), ins);
}

void
LIRGeneratorMIPSShared::visitAsmJSStoreHeap(MAsmJSStoreHeap* ins)
{
    MDefinition* ptr = ins->ptr();
    MOZ_ASSERT(ptr->type() == MIRType_Int32);
    LAllocation ptrAlloc;

    if (ptr->isConstantValue() && !ins->needsBoundsCheck()) {
        MOZ_ASSERT(ptr->constantValue().toInt32() >= 0);
        ptrAlloc = LAllocation(ptr->constantVp());
    } else
        ptrAlloc = useRegisterAtStart(ptr);

    add(new(alloc()) LAsmJSStoreHeap(ptrAlloc, useRegisterAtStart(ins->value())), ins);
}

void
LIRGeneratorMIPSShared::visitAsmJSLoadFuncPtr(MAsmJSLoadFuncPtr* ins)
{
    define(new(alloc()) LAsmJSLoadFuncPtr(useRegister(ins->index())), ins);
}

void
LIRGeneratorMIPSShared::visitSubstr(MSubstr* ins)
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
LIRGeneratorMIPSShared::visitStoreTypedArrayElementStatic(MStoreTypedArrayElementStatic* ins)
{
    MOZ_CRASH("NYI");
}

void
LIRGeneratorMIPSShared::visitSimdBinaryArith(MSimdBinaryArith* ins)
{
    MOZ_CRASH("NYI");
}

void
LIRGeneratorMIPSShared::visitSimdSelect(MSimdSelect* ins)
{
    MOZ_CRASH("NYI");
}

void
LIRGeneratorMIPSShared::visitSimdSplatX4(MSimdSplatX4* ins)
{
    MOZ_CRASH("NYI");
}

void
LIRGeneratorMIPSShared::visitSimdValueX4(MSimdValueX4* ins)
{
    MOZ_CRASH("NYI");
}

void
LIRGeneratorMIPSShared::visitCompareExchangeTypedArrayElement(MCompareExchangeTypedArrayElement* ins)
{
    MOZ_ASSERT(ins->arrayType() != Scalar::Float32);
    MOZ_ASSERT(ins->arrayType() != Scalar::Float64);

    MOZ_ASSERT(ins->elements()->type() == MIRType_Elements);
    MOZ_ASSERT(ins->index()->type() == MIRType_Int32);

    const LUse elements = useRegister(ins->elements());
    const LAllocation index = useRegisterOrConstant(ins->index());

    // If the target is a floating register then we need a temp at the
    // CodeGenerator level for creating the result.

    const LAllocation newval = useRegister(ins->newval());
    const LAllocation oldval = useRegister(ins->oldval());
    LDefinition uint32Temp = LDefinition::BogusTemp();
    if (ins->arrayType() == Scalar::Uint32 && IsFloatingPointType(ins->type()))
        uint32Temp = temp();

    LCompareExchangeTypedArrayElement* lir =
        new(alloc()) LCompareExchangeTypedArrayElement(elements, index, oldval, newval, uint32Temp,
                                                       /* valueTemp= */ temp(), /* offsetTemp= */ temp(),
                                                       /* maskTemp= */ temp());

    define(lir, ins);
}

void
LIRGeneratorMIPSShared::visitAtomicExchangeTypedArrayElement(MAtomicExchangeTypedArrayElement* ins)
{
    MOZ_ASSERT(ins->arrayType() <= Scalar::Uint32);

    MOZ_ASSERT(ins->elements()->type() == MIRType_Elements);
    MOZ_ASSERT(ins->index()->type() == MIRType_Int32);

    const LUse elements = useRegister(ins->elements());
    const LAllocation index = useRegisterOrConstant(ins->index());

    // If the target is a floating register then we need a temp at the
    // CodeGenerator level for creating the result.

    const LAllocation value = useRegister(ins->value());
    LDefinition uint32Temp = LDefinition::BogusTemp();
    if (ins->arrayType() == Scalar::Uint32) {
        MOZ_ASSERT(ins->type() == MIRType_Double);
        uint32Temp = temp();
    }

    LAtomicExchangeTypedArrayElement* lir =
        new(alloc()) LAtomicExchangeTypedArrayElement(elements, index, value, uint32Temp,
                                                      /* valueTemp= */ temp(), /* offsetTemp= */ temp(),
                                                      /* maskTemp= */ temp());

    define(lir, ins);
}

void
LIRGeneratorMIPSShared::visitAsmJSCompareExchangeHeap(MAsmJSCompareExchangeHeap* ins)
{
    MOZ_ASSERT(ins->accessType() < Scalar::Float32);

    MDefinition* ptr = ins->ptr();
    MOZ_ASSERT(ptr->type() == MIRType_Int32);

    LAsmJSCompareExchangeHeap* lir =
        new(alloc()) LAsmJSCompareExchangeHeap(useRegister(ptr),
                                               useRegister(ins->oldValue()),
                                               useRegister(ins->newValue()),
                                               /* valueTemp= */ temp(),
                                               /* offsetTemp= */ temp(),
                                               /* maskTemp= */ temp());

    define(lir, ins);
}

void
LIRGeneratorMIPSShared::visitAsmJSAtomicExchangeHeap(MAsmJSAtomicExchangeHeap* ins)
{
    MOZ_ASSERT(ins->ptr()->type() == MIRType_Int32);

    const LAllocation ptr = useRegister(ins->ptr());
    const LAllocation value = useRegister(ins->value());

    // The output may not be used but will be clobbered regardless,
    // so ignore the case where we're not using the value and just
    // use the output register as a temp.

    LAsmJSAtomicExchangeHeap* lir =
        new(alloc()) LAsmJSAtomicExchangeHeap(ptr, value,
                                              /* valueTemp= */ temp(),
                                              /* offsetTemp= */ temp(),
                                              /* maskTemp= */ temp());
    define(lir, ins);
}

void
LIRGeneratorMIPSShared::visitAsmJSAtomicBinopHeap(MAsmJSAtomicBinopHeap* ins)
{
    MOZ_ASSERT(ins->accessType() < Scalar::Float32);

    MDefinition* ptr = ins->ptr();
    MOZ_ASSERT(ptr->type() == MIRType_Int32);

    if (!ins->hasUses()) {
        LAsmJSAtomicBinopHeapForEffect* lir =
            new(alloc()) LAsmJSAtomicBinopHeapForEffect(useRegister(ptr),
                                                        useRegister(ins->value()),
                                                        /* flagTemp= */ temp(),
                                                        /* valueTemp= */ temp(),
                                                        /* offsetTemp= */ temp(),
                                                        /* maskTemp= */ temp());
        add(lir, ins);
        return;
    }

    LAsmJSAtomicBinopHeap* lir =
        new(alloc()) LAsmJSAtomicBinopHeap(useRegister(ptr),
                                           useRegister(ins->value()),
                                           /* temp= */ LDefinition::BogusTemp(),
                                           /* flagTemp= */ temp(),
                                           /* valueTemp= */ temp(),
                                           /* offsetTemp= */ temp(),
                                           /* maskTemp= */ temp());

    define(lir, ins);
}

void
LIRGeneratorMIPSShared::visitAtomicTypedArrayElementBinop(MAtomicTypedArrayElementBinop* ins)
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
                                                                /* flagTemp= */ temp(),
                                                                /* valueTemp= */ temp(),
                                                                /* offsetTemp= */ temp(),
                                                                /* maskTemp= */ temp());
        add(lir, ins);
        return;
    }

    // For a Uint32Array with a known double result we need a temp for
    // the intermediate output.

    LDefinition flagTemp = temp();
    LDefinition outTemp = LDefinition::BogusTemp();

    if (ins->arrayType() == Scalar::Uint32 && IsFloatingPointType(ins->type()))
        outTemp = temp();

    // On mips, map flagTemp to temp1 and outTemp to temp2, at least for now.

    LAtomicTypedArrayElementBinop* lir =
        new(alloc()) LAtomicTypedArrayElementBinop(elements, index, value, flagTemp, outTemp,
                                                   /* valueTemp= */ temp(), /* offsetTemp= */ temp(),
                                                   /* maskTemp= */ temp());
    define(lir, ins);
}
