/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x64_Lowering_x64_h
#define jit_x64_Lowering_x64_h

#include "jit/x86-shared/Lowering-x86-shared.h"

namespace js {
namespace jit {

class LIRGeneratorX64 : public LIRGeneratorX86Shared
{
  public:
    LIRGeneratorX64(MIRGenerator* gen, MIRGraph& graph, LIRGraph& lirGraph)
      : LIRGeneratorX86Shared(gen, graph, lirGraph)
    { }

  protected:
    void lowerUntypedPhiInput(MPhi* phi, uint32_t inputPosition, LBlock* block, size_t lirIndex);
    void defineUntypedPhi(MPhi* phi, size_t lirIndex);

    // Adds a use at operand |n| of a value-typed insturction.
    void useBoxFixed(LInstruction* lir, size_t n, MDefinition* mir, Register reg1, Register, bool useAtStart = false);

    // x86 has constraints on what registers can be formatted for 1-byte
    // stores and loads; on x64 all registers are okay.
    LAllocation useByteOpRegister(MDefinition* mir);
    LAllocation useByteOpRegisterOrNonDoubleConstant(MDefinition* mir);
    LDefinition tempByteOpRegister();

    LDefinition tempToUnbox();

    bool needTempForPostBarrier() { return false; }

  public:
    void visitBox(MBox* box);
    void visitUnbox(MUnbox* unbox);
    void visitReturn(MReturn* ret);
    void visitCompareExchangeTypedArrayElement(MCompareExchangeTypedArrayElement* ins);
    void visitAtomicExchangeTypedArrayElement(MAtomicExchangeTypedArrayElement* ins);
    void visitAtomicTypedArrayElementBinop(MAtomicTypedArrayElementBinop* ins);
    void visitAsmJSUnsignedToDouble(MAsmJSUnsignedToDouble* ins);
    void visitAsmJSUnsignedToFloat32(MAsmJSUnsignedToFloat32* ins);
    void visitAsmJSLoadHeap(MAsmJSLoadHeap* ins);
    void visitAsmJSStoreHeap(MAsmJSStoreHeap* ins);
    void visitAsmJSLoadFuncPtr(MAsmJSLoadFuncPtr* ins);
    void visitAsmJSCompareExchangeHeap(MAsmJSCompareExchangeHeap* ins);
    void visitAsmJSAtomicExchangeHeap(MAsmJSAtomicExchangeHeap* ins);
    void visitAsmJSAtomicBinopHeap(MAsmJSAtomicBinopHeap* ins);
    void visitStoreTypedArrayElementStatic(MStoreTypedArrayElementStatic* ins);
    void visitSubstr(MSubstr* ins);
    void visitRandom(MRandom* ins);
};

typedef LIRGeneratorX64 LIRGeneratorSpecific;

} // namespace jit
} // namespace js

#endif /* jit_x64_Lowering_x64_h */
