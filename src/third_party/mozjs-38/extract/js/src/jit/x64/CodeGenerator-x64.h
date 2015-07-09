/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x64_CodeGenerator_x64_h
#define jit_x64_CodeGenerator_x64_h

#include "jit/shared/CodeGenerator-x86-shared.h"

namespace js {
namespace jit {

class CodeGeneratorX64 : public CodeGeneratorX86Shared
{
    CodeGeneratorX64* thisFromCtor() {
        return this;
    }

  protected:
    ValueOperand ToValue(LInstruction* ins, size_t pos);
    ValueOperand ToOutValue(LInstruction* ins);
    ValueOperand ToTempValue(LInstruction* ins, size_t pos);

    void storeUnboxedValue(const LAllocation* value, MIRType valueType,
                           Operand dest, MIRType slotType);
    void memoryBarrier(MemoryBarrierBits barrier);

    void loadSimd(Scalar::Type type, unsigned numElems, const Operand& srcAddr, FloatRegister out);
    void emitSimdLoad(LAsmJSLoadHeap* ins);
    void storeSimd(Scalar::Type type, unsigned numElems, FloatRegister in, const Operand& dstAddr);
    void emitSimdStore(LAsmJSStoreHeap* ins);
  public:
    CodeGeneratorX64(MIRGenerator* gen, LIRGraph* graph, MacroAssembler* masm);

  public:
    void visitValue(LValue* value);
    void visitBox(LBox* box);
    void visitUnbox(LUnbox* unbox);
    void visitCompareB(LCompareB* lir);
    void visitCompareBAndBranch(LCompareBAndBranch* lir);
    void visitCompareV(LCompareV* lir);
    void visitCompareVAndBranch(LCompareVAndBranch* lir);
    void visitTruncateDToInt32(LTruncateDToInt32* ins);
    void visitTruncateFToInt32(LTruncateFToInt32* ins);
    void visitLoadTypedArrayElementStatic(LLoadTypedArrayElementStatic* ins);
    void visitStoreTypedArrayElementStatic(LStoreTypedArrayElementStatic* ins);
    void visitAsmJSCall(LAsmJSCall* ins);
    void visitAsmJSLoadHeap(LAsmJSLoadHeap* ins);
    void visitAsmJSStoreHeap(LAsmJSStoreHeap* ins);
    void visitAsmJSCompareExchangeHeap(LAsmJSCompareExchangeHeap* ins);
    void visitAsmJSAtomicBinopHeap(LAsmJSAtomicBinopHeap* ins);
    void visitAsmJSLoadGlobalVar(LAsmJSLoadGlobalVar* ins);
    void visitAsmJSStoreGlobalVar(LAsmJSStoreGlobalVar* ins);
    void visitAsmJSLoadFuncPtr(LAsmJSLoadFuncPtr* ins);
    void visitAsmJSLoadFFIFunc(LAsmJSLoadFFIFunc* ins);
    void visitAsmJSUInt32ToDouble(LAsmJSUInt32ToDouble* lir);
    void visitAsmJSUInt32ToFloat32(LAsmJSUInt32ToFloat32* lir);
};

typedef CodeGeneratorX64 CodeGeneratorSpecific;

} // namespace jit
} // namespace js

#endif /* jit_x64_CodeGenerator_x64_h */
