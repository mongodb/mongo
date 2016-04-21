/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/MathAlgorithms.h"

#include "jit/arm64/Assembler-arm64.h"
#include "jit/Lowering.h"
#include "jit/MIR.h"

#include "jit/shared/Lowering-shared-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::FloorLog2;

void
LIRGeneratorARM64::useBoxFixed(LInstruction* lir, size_t n, MDefinition* mir, Register reg1, Register,                               bool useAtStart)
{
    MOZ_CRASH("useBoxFixed");
}

LAllocation
LIRGeneratorARM64::useByteOpRegister(MDefinition* mir)
{
    MOZ_CRASH("useByteOpRegister");
}

LAllocation
LIRGeneratorARM64::useByteOpRegisterOrNonDoubleConstant(MDefinition* mir)
{
    MOZ_CRASH("useByteOpRegisterOrNonDoubleConstant");
}

void
LIRGeneratorARM64::visitBox(MBox* box)
{
    MOZ_CRASH("visitBox");
}

void
LIRGeneratorARM64::visitUnbox(MUnbox* unbox)
{
    MOZ_CRASH("visitUnbox");
}

void
LIRGeneratorARM64::visitReturn(MReturn* ret)
{
    MOZ_CRASH("visitReturn");
}

// x = !y
void
LIRGeneratorARM64::lowerForALU(LInstructionHelper<1, 1, 0>* ins, MDefinition* mir, MDefinition* input)
{
    MOZ_CRASH("lowerForALU");
}

// z = x+y
void
LIRGeneratorARM64::lowerForALU(LInstructionHelper<1, 2, 0>* ins, MDefinition* mir,
                               MDefinition* lhs, MDefinition* rhs)
{
    MOZ_CRASH("lowerForALU");
}

void
LIRGeneratorARM64::lowerForFPU(LInstructionHelper<1, 1, 0>* ins, MDefinition* mir, MDefinition* input)
{
    MOZ_CRASH("lowerForFPU");
}

template <size_t Temps>
void
LIRGeneratorARM64::lowerForFPU(LInstructionHelper<1, 2, Temps>* ins, MDefinition* mir,
                               MDefinition* lhs, MDefinition* rhs)
{
    MOZ_CRASH("lowerForFPU");
}

template void LIRGeneratorARM64::lowerForFPU(LInstructionHelper<1, 2, 0>* ins, MDefinition* mir,
                                             MDefinition* lhs, MDefinition* rhs);
template void LIRGeneratorARM64::lowerForFPU(LInstructionHelper<1, 2, 1>* ins, MDefinition* mir,
                                             MDefinition* lhs, MDefinition* rhs);

void
LIRGeneratorARM64::lowerForBitAndAndBranch(LBitAndAndBranch* baab, MInstruction* mir,
                                         MDefinition* lhs, MDefinition* rhs)
{
    MOZ_CRASH("lowerForBitAndAndBranch");
}

void
LIRGeneratorARM64::defineUntypedPhi(MPhi* phi, size_t lirIndex)
{
    MOZ_CRASH("defineUntypedPhi");
}

void
LIRGeneratorARM64::lowerUntypedPhiInput(MPhi* phi, uint32_t inputPosition,
                                        LBlock* block, size_t lirIndex)
{
    MOZ_CRASH("lowerUntypedPhiInput");
}

void
LIRGeneratorARM64::lowerForShift(LInstructionHelper<1, 2, 0>* ins,
                                 MDefinition* mir, MDefinition* lhs, MDefinition* rhs)
{
    MOZ_CRASH("lowerForShift");
}

void
LIRGeneratorARM64::lowerDivI(MDiv* div)
{
    MOZ_CRASH("lowerDivI");
}

void
LIRGeneratorARM64::lowerMulI(MMul* mul, MDefinition* lhs, MDefinition* rhs)
{
    MOZ_CRASH("lowerMulI");
}

void
LIRGeneratorARM64::lowerModI(MMod* mod)
{
    MOZ_CRASH("lowerModI");
}

void
LIRGeneratorARM64::visitPowHalf(MPowHalf* ins)
{
    MOZ_CRASH("visitPowHalf");
}

LTableSwitch*
LIRGeneratorARM64::newLTableSwitch(const LAllocation& in, const LDefinition& inputCopy,
                                       MTableSwitch* tableswitch)
{
    MOZ_CRASH("newLTableSwitch");
}

LTableSwitchV*
LIRGeneratorARM64::newLTableSwitchV(MTableSwitch* tableswitch)
{
    MOZ_CRASH("newLTableSwitchV");
}

void
LIRGeneratorARM64::visitGuardShape(MGuardShape* ins)
{
    MOZ_CRASH("visitGuardShape");
}

void
LIRGeneratorARM64::visitGuardObjectGroup(MGuardObjectGroup* ins)
{
    MOZ_CRASH("visitGuardObjectGroup");
}

void
LIRGeneratorARM64::lowerUrshD(MUrsh* mir)
{
    MOZ_CRASH("lowerUrshD");
}

void
LIRGeneratorARM64::visitAsmJSNeg(MAsmJSNeg* ins)
{
    MOZ_CRASH("visitAsmJSNeg");
}

void
LIRGeneratorARM64::lowerUDiv(MDiv* div)
{
    MOZ_CRASH("lowerUDiv");
}

void
LIRGeneratorARM64::lowerUMod(MMod* mod)
{
    MOZ_CRASH("lowerUMod");
}

void
LIRGeneratorARM64::visitAsmJSUnsignedToDouble(MAsmJSUnsignedToDouble* ins)
{
    MOZ_CRASH("visitAsmJSUnsignedToDouble");
}

void
LIRGeneratorARM64::visitAsmJSUnsignedToFloat32(MAsmJSUnsignedToFloat32* ins)
{
    MOZ_CRASH("visitAsmJSUnsignedToFloat32");
}

void
LIRGeneratorARM64::visitAsmJSLoadHeap(MAsmJSLoadHeap* ins)
{
    MOZ_CRASH("visitAsmJSLoadHeap");
}

void
LIRGeneratorARM64::visitAsmJSStoreHeap(MAsmJSStoreHeap* ins)
{
    MOZ_CRASH("visitAsmJSStoreHeap");
}

void
LIRGeneratorARM64::visitAsmJSLoadFuncPtr(MAsmJSLoadFuncPtr* ins)
{
    MOZ_CRASH("visitAsmJSLoadFuncPtr");
}

void
LIRGeneratorARM64::visitAsmJSCompareExchangeHeap(MAsmJSCompareExchangeHeap* ins)
{
    MOZ_CRASH("visitAsmJSCompareExchangeHeap");
}

void
LIRGeneratorARM64::visitAsmJSAtomicExchangeHeap(MAsmJSAtomicExchangeHeap* ins)
{
    MOZ_CRASH("visitAsmJSAtomicExchangeHeap");
}

void
LIRGeneratorARM64::visitAsmJSAtomicBinopHeap(MAsmJSAtomicBinopHeap* ins)
{
    MOZ_CRASH("visitAsmJSAtomicBinopHeap");
}

void
LIRGeneratorARM64::lowerTruncateDToInt32(MTruncateToInt32* ins)
{
    MOZ_CRASH("lowerTruncateDToInt32");
}

void
LIRGeneratorARM64::lowerTruncateFToInt32(MTruncateToInt32* ins)
{
    MOZ_CRASH("lowerTruncateFToInt32");
}

void
LIRGeneratorARM64::visitStoreTypedArrayElementStatic(MStoreTypedArrayElementStatic* ins)
{
    MOZ_CRASH("NYI");
}

void
LIRGeneratorARM64::visitSimdBinaryArith(MSimdBinaryArith* ins)
{
    MOZ_CRASH("NYI");
}

void
LIRGeneratorARM64::visitSimdSelect(MSimdSelect* ins)
{
    MOZ_CRASH("NYI");
}

void
LIRGeneratorARM64::visitSimdSplatX4(MSimdSplatX4* ins)
{
    MOZ_CRASH("NYI");
}

void
LIRGeneratorARM64::visitSimdValueX4(MSimdValueX4* ins)
{
    MOZ_CRASH("NYI");
}

void
LIRGeneratorARM64::visitAtomicTypedArrayElementBinop(MAtomicTypedArrayElementBinop* ins)
{
    MOZ_CRASH("NYI");
}

void
LIRGeneratorARM64::visitCompareExchangeTypedArrayElement(MCompareExchangeTypedArrayElement* ins)
{
    MOZ_CRASH("NYI");
}

void
LIRGeneratorARM64::visitAtomicExchangeTypedArrayElement(MAtomicExchangeTypedArrayElement* ins)
{
    MOZ_CRASH("NYI");
}

void
LIRGeneratorARM64::visitSubstr(MSubstr* ins)
{
    MOZ_CRASH("visitSubstr");
}

void
LIRGeneratorARM64::visitRandom(MRandom* ins)
{
    LRandom *lir = new(alloc()) LRandom(temp(),
                                        temp(),
                                        temp());
    defineFixed(lir, ins, LFloatReg(ReturnDoubleReg));
}
