/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/mips64/CodeGenerator-mips64.h"

#include "mozilla/MathAlgorithms.h"

#include "jit/CodeGenerator.h"
#include "jit/JitCompartment.h"
#include "jit/JitFrames.h"
#include "jit/MIR.h"
#include "jit/MIRGraph.h"
#include "js/Conversions.h"
#include "vm/Shape.h"
#include "vm/TraceLogging.h"

#include "jit/MacroAssembler-inl.h"
#include "jit/shared/CodeGenerator-shared-inl.h"

using namespace js;
using namespace js::jit;

class js::jit::OutOfLineTableSwitch : public OutOfLineCodeBase<CodeGeneratorMIPS64>
{
    MTableSwitch* mir_;
    CodeLabel jumpLabel_;

    void accept(CodeGeneratorMIPS64* codegen) {
        codegen->visitOutOfLineTableSwitch(this);
    }

  public:
    OutOfLineTableSwitch(MTableSwitch* mir)
      : mir_(mir)
    {}

    MTableSwitch* mir() const {
        return mir_;
    }

    CodeLabel* jumpLabel() {
        return &jumpLabel_;
    }
};

void
CodeGeneratorMIPS64::visitOutOfLineBailout(OutOfLineBailout* ool)
{
    masm.push(ImmWord(ool->snapshot()->snapshotOffset()));

    masm.jump(&deoptLabel_);
}

void
CodeGeneratorMIPS64::visitOutOfLineTableSwitch(OutOfLineTableSwitch* ool)
{
    MTableSwitch* mir = ool->mir();

    masm.haltingAlign(sizeof(void*));
    masm.bind(ool->jumpLabel()->target());
    masm.addCodeLabel(*ool->jumpLabel());

    for (size_t i = 0; i < mir->numCases(); i++) {
        LBlock* caseblock = skipTrivialBlocks(mir->getCase(i))->lir();
        Label* caseheader = caseblock->label();
        uint32_t caseoffset = caseheader->offset();

        // The entries of the jump table need to be absolute addresses and thus
        // must be patched after codegen is finished. Each table entry uses 8
        // instructions (4 for load address, 2 for branch, and 2 padding).
        CodeLabel cl;
        masm.ma_li(ScratchRegister, cl.patchAt());
        masm.branch(ScratchRegister);
        masm.as_nop();
        masm.as_nop();
        cl.target()->bind(caseoffset);
        masm.addCodeLabel(cl);
    }
}

void
CodeGeneratorMIPS64::emitTableSwitchDispatch(MTableSwitch* mir, Register index,
                                             Register address)
{
    Label* defaultcase = skipTrivialBlocks(mir->getDefault())->lir()->label();

    // Lower value with low value
    if (mir->low() != 0)
        masm.subPtr(Imm32(mir->low()), index);

    // Jump to default case if input is out of range
    int32_t cases = mir->numCases();
    masm.branch32(Assembler::AboveOrEqual, index, Imm32(cases), defaultcase);

    // To fill in the CodeLabels for the case entries, we need to first
    // generate the case entries (we don't yet know their offsets in the
    // instruction stream).
    OutOfLineTableSwitch* ool = new(alloc()) OutOfLineTableSwitch(mir);
    addOutOfLineCode(ool, mir);

    // Compute the position where a pointer to the right case stands.
    masm.ma_li(address, ool->jumpLabel()->patchAt());
    // index = size of table entry * index.
    // See CodeGeneratorMIPS64::visitOutOfLineTableSwitch
    masm.lshiftPtr(Imm32(5), index);
    masm.addPtr(index, address);

    masm.branch(address);
}

FrameSizeClass
FrameSizeClass::FromDepth(uint32_t frameDepth)
{
    return FrameSizeClass::None();
}

FrameSizeClass
FrameSizeClass::ClassLimit()
{
    return FrameSizeClass(0);
}

uint32_t
FrameSizeClass::frameSize() const
{
    MOZ_CRASH("MIPS64 does not use frame size classes");
}

ValueOperand
CodeGeneratorMIPS64::ToValue(LInstruction* ins, size_t pos)
{
    return ValueOperand(ToRegister(ins->getOperand(pos)));
}

ValueOperand
CodeGeneratorMIPS64::ToOutValue(LInstruction* ins)
{
    return ValueOperand(ToRegister(ins->getDef(0)));
}

ValueOperand
CodeGeneratorMIPS64::ToTempValue(LInstruction* ins, size_t pos)
{
    return ValueOperand(ToRegister(ins->getTemp(pos)));
}

void
CodeGeneratorMIPS64::visitBox(LBox* box)
{
    const LAllocation* in = box->getOperand(0);
    const LDefinition* result = box->getDef(0);

    if (IsFloatingPointType(box->type())) {
        FloatRegister reg = ToFloatRegister(in);
        if (box->type() == MIRType_Float32) {
            masm.convertFloat32ToDouble(reg, ScratchDoubleReg);
            reg = ScratchDoubleReg;
        }
        masm.moveFromDouble(reg, ToRegister(result));
    } else {
        masm.boxValue(ValueTypeFromMIRType(box->type()), ToRegister(in), ToRegister(result));
    }
}

void
CodeGeneratorMIPS64::visitUnbox(LUnbox* unbox)
{
    MUnbox* mir = unbox->mir();

    if (mir->fallible()) {
        const ValueOperand value = ToValue(unbox, LUnbox::Input);
        masm.splitTag(value, SecondScratchReg);
        bailoutCmp32(Assembler::NotEqual, SecondScratchReg, Imm32(MIRTypeToTag(mir->type())),
                     unbox->snapshot());
    }

    Operand input = ToOperand(unbox->getOperand(LUnbox::Input));
    Register result = ToRegister(unbox->output());
    switch (mir->type()) {
      case MIRType_Int32:
        masm.unboxInt32(input, result);
        break;
      case MIRType_Boolean:
        masm.unboxBoolean(input, result);
        break;
      case MIRType_Object:
        masm.unboxObject(input, result);
        break;
      case MIRType_String:
        masm.unboxString(input, result);
        break;
      case MIRType_Symbol:
        masm.unboxSymbol(input, result);
        break;
      default:
        MOZ_CRASH("Given MIRType cannot be unboxed.");
    }
}

Register
CodeGeneratorMIPS64::splitTagForTest(const ValueOperand& value)
{
    MOZ_ASSERT(value.valueReg() != SecondScratchReg);
    masm.splitTag(value.valueReg(), SecondScratchReg);
    return SecondScratchReg;
}

void
CodeGeneratorMIPS64::visitCompareB(LCompareB* lir)
{
    MCompare* mir = lir->mir();

    const ValueOperand lhs = ToValue(lir, LCompareB::Lhs);
    const LAllocation* rhs = lir->rhs();
    const Register output = ToRegister(lir->output());

    MOZ_ASSERT(mir->jsop() == JSOP_STRICTEQ || mir->jsop() == JSOP_STRICTNE);
    Assembler::Condition cond = JSOpToCondition(mir->compareType(), mir->jsop());

    // Load boxed boolean in ScratchRegister.
    if (rhs->isConstant())
        masm.moveValue(*rhs->toConstant(), ScratchRegister);
    else
        masm.boxValue(JSVAL_TYPE_BOOLEAN, ToRegister(rhs), ScratchRegister);

    // Perform the comparison.
    masm.cmpPtrSet(cond, lhs.valueReg(), ScratchRegister, output);
}

void
CodeGeneratorMIPS64::visitCompareBAndBranch(LCompareBAndBranch* lir)
{
    MCompare* mir = lir->cmpMir();
    const ValueOperand lhs = ToValue(lir, LCompareBAndBranch::Lhs);
    const LAllocation* rhs = lir->rhs();

    MOZ_ASSERT(mir->jsop() == JSOP_STRICTEQ || mir->jsop() == JSOP_STRICTNE);

    // Load boxed boolean in ScratchRegister.
    if (rhs->isConstant())
        masm.moveValue(*rhs->toConstant(), ScratchRegister);
    else
        masm.boxValue(JSVAL_TYPE_BOOLEAN, ToRegister(rhs), ScratchRegister);

    // Perform the comparison.
    Assembler::Condition cond = JSOpToCondition(mir->compareType(), mir->jsop());
    emitBranch(lhs.valueReg(), ScratchRegister, cond, lir->ifTrue(), lir->ifFalse());
}

void
CodeGeneratorMIPS64::visitCompareBitwise(LCompareBitwise* lir)
{
    MCompare* mir = lir->mir();
    Assembler::Condition cond = JSOpToCondition(mir->compareType(), mir->jsop());
    const ValueOperand lhs = ToValue(lir, LCompareBitwise::LhsInput);
    const ValueOperand rhs = ToValue(lir, LCompareBitwise::RhsInput);
    const Register output = ToRegister(lir->output());

    MOZ_ASSERT(IsEqualityOp(mir->jsop()));

    masm.cmpPtrSet(cond, lhs.valueReg(), rhs.valueReg(), output);
}

void
CodeGeneratorMIPS64::visitCompareBitwiseAndBranch(LCompareBitwiseAndBranch* lir)
{
    MCompare* mir = lir->cmpMir();
    Assembler::Condition cond = JSOpToCondition(mir->compareType(), mir->jsop());
    const ValueOperand lhs = ToValue(lir, LCompareBitwiseAndBranch::LhsInput);
    const ValueOperand rhs = ToValue(lir, LCompareBitwiseAndBranch::RhsInput);

    MOZ_ASSERT(mir->jsop() == JSOP_EQ || mir->jsop() == JSOP_STRICTEQ ||
               mir->jsop() == JSOP_NE || mir->jsop() == JSOP_STRICTNE);

    emitBranch(lhs.valueReg(), rhs.valueReg(), cond, lir->ifTrue(), lir->ifFalse());
}

void
CodeGeneratorMIPS64::setReturnDoubleRegs(LiveRegisterSet* regs)
{
    MOZ_ASSERT(ReturnFloat32Reg.reg_ == FloatRegisters::f0);
    MOZ_ASSERT(ReturnDoubleReg.reg_ == FloatRegisters::f0);
    FloatRegister f1 = { FloatRegisters::f1, FloatRegisters::Single };
    regs->add(ReturnFloat32Reg);
    regs->add(f1);
    regs->add(ReturnDoubleReg);
}
