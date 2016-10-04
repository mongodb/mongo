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

    void emitTableSwitchDispatch(MTableSwitch* mir, Register index, Register base);

  public:
    virtual void visitCompareB(LCompareB* lir);
    virtual void visitCompareBAndBranch(LCompareBAndBranch* lir);
    virtual void visitCompareBitwise(LCompareBitwise* lir);
    virtual void visitCompareBitwiseAndBranch(LCompareBitwiseAndBranch* lir);

    // Out of line visitors.
    void visitOutOfLineBailout(OutOfLineBailout* ool);
    void visitOutOfLineTableSwitch(OutOfLineTableSwitch* ool);
  protected:
    ValueOperand ToValue(LInstruction* ins, size_t pos);
    ValueOperand ToOutValue(LInstruction* ins);
    ValueOperand ToTempValue(LInstruction* ins, size_t pos);

    // Functions for LTestVAndBranch.
    Register splitTagForTest(const ValueOperand& value);

  public:
    CodeGeneratorMIPS(MIRGenerator* gen, LIRGraph* graph, MacroAssembler* masm)
      : CodeGeneratorMIPSShared(gen, graph, masm)
    { }

  public:
    void visitBox(LBox* box);
    void visitBoxFloatingPoint(LBoxFloatingPoint* box);
    void visitUnbox(LUnbox* unbox);
    void setReturnDoubleRegs(LiveRegisterSet* regs);
};

typedef CodeGeneratorMIPS CodeGeneratorSpecific;

} // namespace jit
} // namespace js

#endif /* jit_mips32_CodeGenerator_mips32_h */
