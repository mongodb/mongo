/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x86_CodeGenerator_x86_h
#define jit_x86_CodeGenerator_x86_h

#include "jit/shared/CodeGenerator-x86-shared.h"
#include "jit/x86/Assembler-x86.h"

namespace js {
namespace jit {

class OutOfLineTruncate;
class OutOfLineTruncateFloat32;

class CodeGeneratorX86 : public CodeGeneratorX86Shared
{
  private:
    CodeGeneratorX86* thisFromCtor() {
        return this;
    }

  protected:
    ValueOperand ToValue(LInstruction* ins, size_t pos);
    ValueOperand ToOutValue(LInstruction* ins);
    ValueOperand ToTempValue(LInstruction* ins, size_t pos);

    template<typename T>
    void loadAndNoteViewTypeElement(Scalar::Type vt, const T& srcAddr, const LDefinition* out);
    template<typename T>
    void load(Scalar::Type vt, const T& srcAddr, const LDefinition* out);
    template<typename T>
    void storeAndNoteViewTypeElement(Scalar::Type vt, const LAllocation* value, const T& dstAddr);
    template<typename T>
    void store(Scalar::Type vt, const LAllocation* value, const T& dstAddr);

    template<typename T>
    void loadSimd(Scalar::Type type, unsigned numElems, T srcAddr, FloatRegister out);
    void emitSimdLoad(Scalar::Type type, unsigned numElems, const LAllocation* ptr,
                      FloatRegister out, bool needsBoundsCheck = false, Label* oobLabel = nullptr);

    template<typename T>
    void storeSimd(Scalar::Type type, unsigned numElems, FloatRegister in, T destAddr);
    void emitSimdStore(Scalar::Type type, unsigned numElems, FloatRegister in,
                       const LAllocation* ptr, bool needsBoundsCheck = false,
                       Label* oobLabel = nullptr);

    void memoryBarrier(MemoryBarrierBits barrier);

  public:
    CodeGeneratorX86(MIRGenerator* gen, LIRGraph* graph, MacroAssembler* masm);

  public:
    void visitBox(LBox* box);
    void visitBoxFloatingPoint(LBoxFloatingPoint* box);
    void visitUnbox(LUnbox* unbox);
    void visitValue(LValue* value);
    void visitCompareB(LCompareB* lir);
    void visitCompareBAndBranch(LCompareBAndBranch* lir);
    void visitCompareV(LCompareV* lir);
    void visitCompareVAndBranch(LCompareVAndBranch* lir);
    void visitAsmJSUInt32ToDouble(LAsmJSUInt32ToDouble* lir);
    void visitAsmJSUInt32ToFloat32(LAsmJSUInt32ToFloat32* lir);
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

    void visitOutOfLineTruncate(OutOfLineTruncate* ool);
    void visitOutOfLineTruncateFloat32(OutOfLineTruncateFloat32* ool);
};

typedef CodeGeneratorX86 CodeGeneratorSpecific;

} // namespace jit
} // namespace js

#endif /* jit_x86_CodeGenerator_x86_h */
