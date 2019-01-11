/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_mips32_CodeGenerator_mips32_h
#define jit_mips32_CodeGenerator_mips32_h

#include "jit/mips-shared/CodeGenerator-mips-shared.h"

namespace js {
namespace jit {

class CodeGeneratorMIPS : public CodeGeneratorMIPSShared
{
  protected:
    void testNullEmitBranch(Assembler::Condition cond, const ValueOperand& value,
                            MBasicBlock* ifTrue, MBasicBlock* ifFalse)
    {
        emitBranch(value.typeReg(), (Imm32)ImmType(JSVAL_TYPE_NULL), cond, ifTrue, ifFalse);
    }
    void testUndefinedEmitBranch(Assembler::Condition cond, const ValueOperand& value,
                                 MBasicBlock* ifTrue, MBasicBlock* ifFalse)
    {
        emitBranch(value.typeReg(), (Imm32)ImmType(JSVAL_TYPE_UNDEFINED), cond, ifTrue, ifFalse);
    }
    void testObjectEmitBranch(Assembler::Condition cond, const ValueOperand& value,
                              MBasicBlock* ifTrue, MBasicBlock* ifFalse)
    {
        emitBranch(value.typeReg(), (Imm32)ImmType(JSVAL_TYPE_OBJECT), cond, ifTrue, ifFalse);
    }

    template <typename T>
    void emitWasmLoadI64(T* ins);
    template <typename T>
    void emitWasmStoreI64(T* ins);

  public:
    void visitCompareB(LCompareB* lir);
    void visitCompareBAndBranch(LCompareBAndBranch* lir);
    void visitCompareBitwise(LCompareBitwise* lir);
    void visitCompareBitwiseAndBranch(LCompareBitwiseAndBranch* lir);
    void visitCompareI64(LCompareI64* lir);
    void visitCompareI64AndBranch(LCompareI64AndBranch* lir);
    void visitDivOrModI64(LDivOrModI64* lir);
    void visitUDivOrModI64(LUDivOrModI64* lir);
    void visitWasmLoadI64(LWasmLoadI64* ins);
    void visitWasmUnalignedLoadI64(LWasmUnalignedLoadI64* lir);
    void visitWasmStoreI64(LWasmStoreI64* ins);
    void visitWasmUnalignedStoreI64(LWasmUnalignedStoreI64* ins);
    void visitWasmSelectI64(LWasmSelectI64* lir);
    void visitWasmReinterpretFromI64(LWasmReinterpretFromI64* lir);
    void visitWasmReinterpretToI64(LWasmReinterpretToI64* lir);
    void visitExtendInt32ToInt64(LExtendInt32ToInt64* lir);
    void visitWrapInt64ToInt32(LWrapInt64ToInt32* lir);
    void visitSignExtendInt64(LSignExtendInt64* ins);
    void visitClzI64(LClzI64* ins);
    void visitCtzI64(LCtzI64* ins);
    void visitNotI64(LNotI64* ins);
    void visitWasmTruncateToInt64(LWasmTruncateToInt64* ins);
    void visitInt64ToFloatingPoint(LInt64ToFloatingPoint* lir);
    void visitTestI64AndBranch(LTestI64AndBranch* lir);

    // Out of line visitors.
    void visitOutOfLineBailout(OutOfLineBailout* ool);
  protected:
    ValueOperand ToValue(LInstruction* ins, size_t pos);
    ValueOperand ToTempValue(LInstruction* ins, size_t pos);

    // Functions for LTestVAndBranch.
    void splitTagForTest(const ValueOperand& value, ScratchTagScope& tag);

  public:
    CodeGeneratorMIPS(MIRGenerator* gen, LIRGraph* graph, MacroAssembler* masm)
      : CodeGeneratorMIPSShared(gen, graph, masm)
    { }

  public:
    void visitBox(LBox* box);
    void visitBoxFloatingPoint(LBoxFloatingPoint* box);
    void visitUnbox(LUnbox* unbox);
    void setReturnDoubleRegs(LiveRegisterSet* regs);
    void visitWasmAtomicLoadI64(LWasmAtomicLoadI64* lir);
    void visitWasmAtomicStoreI64(LWasmAtomicStoreI64* lir);
};

typedef CodeGeneratorMIPS CodeGeneratorSpecific;

} // namespace jit
} // namespace js

#endif /* jit_mips32_CodeGenerator_mips32_h */
