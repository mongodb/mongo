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
    void lowerInt64PhiInput(MPhi* phi, uint32_t inputPosition, LBlock* block, size_t lirIndex);
    void defineInt64Phi(MPhi* phi, size_t lirIndex);

    void lowerForALUInt64(LInstructionHelper<INT64_PIECES, 2 * INT64_PIECES, 0>* ins,
                          MDefinition* mir, MDefinition* lhs, MDefinition* rhs);
    void lowerForMulInt64(LMulI64* ins, MMul* mir, MDefinition* lhs, MDefinition* rhs);

    // Returns a box allocation. reg2 is ignored on 64-bit platforms.
    LBoxAllocation useBoxFixed(MDefinition* mir, Register reg1, Register, bool useAtStart = false);

    // x86 has constraints on what registers can be formatted for 1-byte
    // stores and loads; on x64 all registers are okay.
    LAllocation useByteOpRegister(MDefinition* mir);
    LAllocation useByteOpRegisterAtStart(MDefinition* mir);
    LAllocation useByteOpRegisterOrNonDoubleConstant(MDefinition* mir);
    LDefinition tempByteOpRegister();

    LDefinition tempToUnbox();

    bool needTempForPostBarrier() { return true; }

    void lowerDivI64(MDiv* div);
    void lowerModI64(MMod* mod);
    void lowerUDivI64(MDiv* div);
    void lowerUModI64(MMod* mod);

  public:
    void visitBox(MBox* box) override;
    void visitUnbox(MUnbox* unbox) override;
    void visitReturn(MReturn* ret) override;
    void visitCompareExchangeTypedArrayElement(MCompareExchangeTypedArrayElement* ins) override;
    void visitAtomicExchangeTypedArrayElement(MAtomicExchangeTypedArrayElement* ins) override;
    void visitAtomicTypedArrayElementBinop(MAtomicTypedArrayElementBinop* ins) override;
    void visitWasmUnsignedToDouble(MWasmUnsignedToDouble* ins) override;
    void visitWasmUnsignedToFloat32(MWasmUnsignedToFloat32* ins) override;
    void visitAsmJSLoadHeap(MAsmJSLoadHeap* ins) override;
    void visitAsmJSStoreHeap(MAsmJSStoreHeap* ins) override;
    void visitWasmCompareExchangeHeap(MWasmCompareExchangeHeap* ins) override;
    void visitWasmAtomicExchangeHeap(MWasmAtomicExchangeHeap* ins) override;
    void visitWasmAtomicBinopHeap(MWasmAtomicBinopHeap* ins) override;
    void visitWasmLoad(MWasmLoad* ins) override;
    void visitWasmStore(MWasmStore* ins) override;
    void visitSubstr(MSubstr* ins) override;
    void visitRandom(MRandom* ins) override;
    void visitWasmTruncateToInt64(MWasmTruncateToInt64* ins) override;
    void visitInt64ToFloatingPoint(MInt64ToFloatingPoint* ins) override;
    void visitExtendInt32ToInt64(MExtendInt32ToInt64* ins) override;
    void visitSignExtendInt64(MSignExtendInt64* ins) override;
};

typedef LIRGeneratorX64 LIRGeneratorSpecific;

} // namespace jit
} // namespace js

#endif /* jit_x64_Lowering_x64_h */
