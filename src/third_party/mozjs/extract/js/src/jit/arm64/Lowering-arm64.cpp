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

LBoxAllocation
LIRGeneratorARM64::useBoxFixed(MDefinition* mir, Register reg1, Register, bool useAtStart)
{
    MOZ_CRASH("useBoxFixed");
}

LAllocation
LIRGeneratorARM64::useByteOpRegister(MDefinition* mir)
{
    MOZ_CRASH("useByteOpRegister");
}

LAllocation
LIRGeneratorARM64::useByteOpRegisterAtStart(MDefinition* mir)
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
LIRGeneratorARM64::lowerForALUInt64(LInstructionHelper<INT64_PIECES, 2 * INT64_PIECES, 0>* ins,
                                    MDefinition* mir, MDefinition* lhs, MDefinition* rhs)
{
    MOZ_CRASH("NYI");
}

void
LIRGeneratorARM64::lowerForMulInt64(LMulI64* ins, MMul* mir, MDefinition* lhs, MDefinition* rhs)
{
    MOZ_CRASH("NYI");
}

template<size_t Temps>
void
LIRGeneratorARM64::lowerForShiftInt64(LInstructionHelper<INT64_PIECES, INT64_PIECES + 1, Temps>* ins,
                                      MDefinition* mir, MDefinition* lhs, MDefinition* rhs)
{
    MOZ_CRASH("NYI");
}

template void LIRGeneratorARM64::lowerForShiftInt64(
    LInstructionHelper<INT64_PIECES, INT64_PIECES+1, 0>* ins, MDefinition* mir,
    MDefinition* lhs, MDefinition* rhs);
template void LIRGeneratorARM64::lowerForShiftInt64(
    LInstructionHelper<INT64_PIECES, INT64_PIECES+1, 1>* ins, MDefinition* mir,
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
LIRGeneratorARM64::lowerDivI64(MDiv* div)
{
    MOZ_CRASH("NYI");
}

void
LIRGeneratorARM64::lowerModI64(MMod* mod)
{
    MOZ_CRASH("NYI");
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
LIRGeneratorARM64::lowerUrshD(MUrsh* mir)
{
    MOZ_CRASH("lowerUrshD");
}

void
LIRGeneratorARM64::visitWasmNeg(MWasmNeg* ins)
{
    MOZ_CRASH("visitWasmNeg");
}

void
LIRGeneratorARM64::visitWasmSelect(MWasmSelect* ins)
{
    MOZ_CRASH("visitWasmSelect");
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
LIRGeneratorARM64::visitWasmUnsignedToDouble(MWasmUnsignedToDouble* ins)
{
    MOZ_CRASH("visitWasmUnsignedToDouble");
}

void
LIRGeneratorARM64::visitWasmUnsignedToFloat32(MWasmUnsignedToFloat32* ins)
{
    MOZ_CRASH("visitWasmUnsignedToFloat32");
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
LIRGeneratorARM64::visitWasmCompareExchangeHeap(MWasmCompareExchangeHeap* ins)
{
    MOZ_CRASH("visitWasmCompareExchangeHeap");
}

void
LIRGeneratorARM64::visitWasmAtomicExchangeHeap(MWasmAtomicExchangeHeap* ins)
{
    MOZ_CRASH("visitWasmAtomicExchangeHeap");
}

void
LIRGeneratorARM64::visitWasmAtomicBinopHeap(MWasmAtomicBinopHeap* ins)
{
    MOZ_CRASH("visitWasmAtomicBinopHeap");
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

void
LIRGeneratorARM64::visitWasmTruncateToInt64(MWasmTruncateToInt64* ins)
{
    MOZ_CRASH("NY");
}

void
LIRGeneratorARM64::visitWasmLoad(MWasmLoad* ins)
{
    MOZ_CRASH("NY");
}

void
LIRGeneratorARM64::visitWasmStore(MWasmStore* ins)
{
    MOZ_CRASH("NY");
}

void
LIRGeneratorARM64::visitInt64ToFloatingPoint(MInt64ToFloatingPoint* ins)
{
    MOZ_CRASH("NY");
}

void
LIRGeneratorARM64::visitCopySign(MCopySign* ins)
{
    MOZ_CRASH("NY");
}

void
LIRGeneratorARM64::visitExtendInt32ToInt64(MExtendInt32ToInt64* ins)
{
    MOZ_CRASH("NYI");
}

void
LIRGeneratorARM64::visitSignExtendInt64(MSignExtendInt64* ins)
{
    MOZ_CRASH("NYI");
}
