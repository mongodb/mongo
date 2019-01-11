/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x64_CodeGenerator_x64_h
#define jit_x64_CodeGenerator_x64_h

#include "jit/x86-shared/CodeGenerator-x86-shared.h"

namespace js {
namespace jit {

class CodeGeneratorX64 : public CodeGeneratorX86Shared
{
    CodeGeneratorX64* thisFromCtor() {
        return this;
    }

  protected:
    Operand ToOperand64(const LInt64Allocation& a);
    ValueOperand ToValue(LInstruction* ins, size_t pos);
    ValueOperand ToTempValue(LInstruction* ins, size_t pos);

    void storeUnboxedValue(const LAllocation* value, MIRType valueType,
                           Operand dest, MIRType slotType);

    void wasmStore(const wasm::MemoryAccessDesc& access, const LAllocation* value, Operand dstAddr);
    template <typename T> void emitWasmLoad(T* ins);
    template <typename T> void emitWasmStore(T* ins);

  public:
    CodeGeneratorX64(MIRGenerator* gen, LIRGraph* graph, MacroAssembler* masm);

  public:
    void visitValue(LValue* value);
    void visitBox(LBox* box);
    void visitUnbox(LUnbox* unbox);
    void visitCompareB(LCompareB* lir);
    void visitCompareBAndBranch(LCompareBAndBranch* lir);
    void visitCompareBitwise(LCompareBitwise* lir);
    void visitCompareBitwiseAndBranch(LCompareBitwiseAndBranch* lir);
    void visitCompareI64(LCompareI64* lir);
    void visitCompareI64AndBranch(LCompareI64AndBranch* lir);
    void visitDivOrModI64(LDivOrModI64* lir);
    void visitUDivOrModI64(LUDivOrModI64* lir);
    void visitNotI64(LNotI64* lir);
    void visitClzI64(LClzI64* lir);
    void visitCtzI64(LCtzI64* lir);
    void visitTruncateDToInt32(LTruncateDToInt32* ins);
    void visitTruncateFToInt32(LTruncateFToInt32* ins);
    void visitWrapInt64ToInt32(LWrapInt64ToInt32* lir);
    void visitExtendInt32ToInt64(LExtendInt32ToInt64* lir);
    void visitSignExtendInt64(LSignExtendInt64* ins);
    void visitWasmTruncateToInt64(LWasmTruncateToInt64* lir);
    void visitInt64ToFloatingPoint(LInt64ToFloatingPoint* lir);
    void visitWasmLoad(LWasmLoad* ins);
    void visitWasmLoadI64(LWasmLoadI64* ins);
    void visitWasmStore(LWasmStore* ins);
    void visitWasmStoreI64(LWasmStoreI64* ins);
    void visitWasmSelectI64(LWasmSelectI64* ins);
    void visitAsmJSLoadHeap(LAsmJSLoadHeap* ins);
    void visitAsmJSStoreHeap(LAsmJSStoreHeap* ins);
    void visitWasmCompareExchangeHeap(LWasmCompareExchangeHeap* ins);
    void visitWasmAtomicExchangeHeap(LWasmAtomicExchangeHeap* ins);
    void visitWasmAtomicBinopHeap(LWasmAtomicBinopHeap* ins);
    void visitWasmAtomicBinopHeapForEffect(LWasmAtomicBinopHeapForEffect* ins);
    void visitWasmUint32ToDouble(LWasmUint32ToDouble* lir);
    void visitWasmUint32ToFloat32(LWasmUint32ToFloat32* lir);
    void visitWasmReinterpretFromI64(LWasmReinterpretFromI64* lir);
    void visitWasmReinterpretToI64(LWasmReinterpretToI64* lir);
    void visitTestI64AndBranch(LTestI64AndBranch* lir);
};

typedef CodeGeneratorX64 CodeGeneratorSpecific;

} // namespace jit
} // namespace js

#endif /* jit_x64_CodeGenerator_x64_h */
