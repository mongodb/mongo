/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x86_Lowering_x86_h
#define jit_x86_Lowering_x86_h

#include "jit/x86-shared/Lowering-x86-shared.h"

namespace js {
namespace jit {

class LIRGeneratorX86 : public LIRGeneratorX86Shared
{
  public:
    LIRGeneratorX86(MIRGenerator* gen, MIRGraph& graph, LIRGraph& lirGraph)
      : LIRGeneratorX86Shared(gen, graph, lirGraph)
    { }

  protected:
    // Adds a box input to an instruction, setting operand |n| to the type and
    // |n+1| to the payload.
    void useBoxFixed(LInstruction* lir, size_t n, MDefinition* mir, Register reg1, Register reg2,
                     bool useAtStart = false);

    // It's a trap! On x86, the 1-byte store can only use one of
    // {al,bl,cl,dl,ah,bh,ch,dh}. That means if the register allocator
    // gives us one of {edi,esi,ebp,esp}, we're out of luck. (The formatter
    // will assert on us.) Ideally, we'd just ask the register allocator to
    // give us one of {al,bl,cl,dl}. For now, just useFixed(al).
    LAllocation useByteOpRegister(MDefinition* mir);
    LAllocation useByteOpRegisterOrNonDoubleConstant(MDefinition* mir);
    LDefinition tempByteOpRegister();

    inline LDefinition tempToUnbox() {
        return LDefinition::BogusTemp();
    }

    bool needTempForPostBarrier() { return true; }

    void lowerUntypedPhiInput(MPhi* phi, uint32_t inputPosition, LBlock* block, size_t lirIndex);
    void defineUntypedPhi(MPhi* phi, size_t lirIndex);

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
    void lowerPhi(MPhi* phi);

    static bool allowTypedElementHoleCheck() {
        return true;
    }

    static bool allowStaticTypedArrayAccesses() {
        return true;
    }
};

typedef LIRGeneratorX86 LIRGeneratorSpecific;

} // namespace jit
} // namespace js

#endif /* jit_x86_Lowering_x86_h */
