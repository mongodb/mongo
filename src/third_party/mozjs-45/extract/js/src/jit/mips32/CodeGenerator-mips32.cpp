/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/mips32/CodeGenerator-mips32.h"

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

class js::jit::OutOfLineTableSwitch : public OutOfLineCodeBase<CodeGeneratorMIPS>
{
    MTableSwitch* mir_;
    CodeLabel jumpLabel_;

    void accept(CodeGeneratorMIPS* codegen) {
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
CodeGeneratorMIPS::visitOutOfLineBailout(OutOfLineBailout* ool)
{
    // Push snapshotOffset and make sure stack is aligned.
    masm.subPtr(Imm32(2 * sizeof(void*)), StackPointer);
    masm.storePtr(ImmWord(ool->snapshot()->snapshotOffset()), Address(StackPointer, 0));

    masm.jump(&deoptLabel_);
}

void
CodeGeneratorMIPS::visitOutOfLineTableSwitch(OutOfLineTableSwitch* ool)
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
        // must be patched after codegen is finished.
        CodeLabel cl;
        masm.ma_li(ScratchRegister, cl.patchAt());
        masm.branch(ScratchRegister);
        cl.target()->bind(caseoffset);
        masm.addCodeLabel(cl);
    }
}

void
CodeGeneratorMIPS::emitTableSwitchDispatch(MTableSwitch* mir, Register index,
                                           Register address)
{
    Label* defaultcase = skipTrivialBlocks(mir->getDefault())->lir()->label();

    // Lower value with low value
    if (mir->low() != 0)
        masm.subPtr(Imm32(mir->low()), index);

    // Jump to default case if input is out of range
    int32_t cases = mir->numCases();
    masm.branchPtr(Assembler::AboveOrEqual, index, ImmWord(cases), defaultcase);

    // To fill in the CodeLabels for the case entries, we need to first
    // generate the case entries (we don't yet know their offsets in the
    // instruction stream).
    OutOfLineTableSwitch* ool = new(alloc()) OutOfLineTableSwitch(mir);
    addOutOfLineCode(ool, mir);

    // Compute the position where a pointer to the right case stands.
    masm.ma_li(address, ool->jumpLabel()->patchAt());
    masm.lshiftPtr(Imm32(4), index);
    masm.addPtr(index, address);

    masm.branch(address);
}

static const uint32_t FrameSizes[] = { 128, 256, 512, 1024 };

FrameSizeClass
FrameSizeClass::FromDepth(uint32_t frameDepth)
{
    for (uint32_t i = 0; i < JS_ARRAY_LENGTH(FrameSizes); i++) {
        if (frameDepth < FrameSizes[i])
            return FrameSizeClass(i);
    }

    return FrameSizeClass::None();
}

FrameSizeClass
FrameSizeClass::ClassLimit()
{
    return FrameSizeClass(JS_ARRAY_LENGTH(FrameSizes));
}

uint32_t
FrameSizeClass::frameSize() const
{
    MOZ_ASSERT(class_ != NO_FRAME_SIZE_CLASS_ID);
    MOZ_ASSERT(class_ < JS_ARRAY_LENGTH(FrameSizes));

    return FrameSizes[class_];
}

ValueOperand
CodeGeneratorMIPS::ToValue(LInstruction* ins, size_t pos)
{
    Register typeReg = ToRegister(ins->getOperand(pos + TYPE_INDEX));
    Register payloadReg = ToRegister(ins->getOperand(pos + PAYLOAD_INDEX));
    return ValueOperand(typeReg, payloadReg);
}

ValueOperand
CodeGeneratorMIPS::ToOutValue(LInstruction* ins)
{
    Register typeReg = ToRegister(ins->getDef(TYPE_INDEX));
    Register payloadReg = ToRegister(ins->getDef(PAYLOAD_INDEX));
    return ValueOperand(typeReg, payloadReg);
}

ValueOperand
CodeGeneratorMIPS::ToTempValue(LInstruction* ins, size_t pos)
{
    Register typeReg = ToRegister(ins->getTemp(pos + TYPE_INDEX));
    Register payloadReg = ToRegister(ins->getTemp(pos + PAYLOAD_INDEX));
    return ValueOperand(typeReg, payloadReg);
}

void
CodeGeneratorMIPS::visitBox(LBox* box)
{
    const LDefinition* type = box->getDef(TYPE_INDEX);

    MOZ_ASSERT(!box->getOperand(0)->isConstant());

    // For NUNBOX32, the input operand and the output payload have the same
    // virtual register. All that needs to be written is the type tag for
    // the type definition.
    masm.move32(Imm32(MIRTypeToTag(box->type())), ToRegister(type));
}

void
CodeGeneratorMIPS::visitBoxFloatingPoint(LBoxFloatingPoint* box)
{
    const LDefinition* payload = box->getDef(PAYLOAD_INDEX);
    const LDefinition* type = box->getDef(TYPE_INDEX);
    const LAllocation* in = box->getOperand(0);

    FloatRegister reg = ToFloatRegister(in);
    if (box->type() == MIRType_Float32) {
        masm.convertFloat32ToDouble(reg, ScratchDoubleReg);
        reg = ScratchDoubleReg;
    }
    masm.ma_mv(reg, ValueOperand(ToRegister(type), ToRegister(payload)));
}

void
CodeGeneratorMIPS::visitUnbox(LUnbox* unbox)
{
    // Note that for unbox, the type and payload indexes are switched on the
    // inputs.
    MUnbox* mir = unbox->mir();
    Register type = ToRegister(unbox->type());

    if (mir->fallible()) {
        bailoutCmp32(Assembler::NotEqual, type, Imm32(MIRTypeToTag(mir->type())),
                     unbox->snapshot());
    }
}

Register
CodeGeneratorMIPS::splitTagForTest(const ValueOperand& value)
{
    return value.typeReg();
}

void
CodeGeneratorMIPS::visitCompareB(LCompareB* lir)
{
    MCompare* mir = lir->mir();

    const ValueOperand lhs = ToValue(lir, LCompareB::Lhs);
    const LAllocation* rhs = lir->rhs();
    const Register output = ToRegister(lir->output());

    MOZ_ASSERT(mir->jsop() == JSOP_STRICTEQ || mir->jsop() == JSOP_STRICTNE);
    Assembler::Condition cond = JSOpToCondition(mir->compareType(), mir->jsop());

    Label notBoolean, done;
    masm.branchTestBoolean(Assembler::NotEqual, lhs, &notBoolean);
    {
        if (rhs->isConstant())
            masm.cmp32Set(cond, lhs.payloadReg(), Imm32(rhs->toConstant()->toBoolean()), output);
        else
            masm.cmp32Set(cond, lhs.payloadReg(), ToRegister(rhs), output);
        masm.jump(&done);
    }

    masm.bind(&notBoolean);
    {
        masm.move32(Imm32(mir->jsop() == JSOP_STRICTNE), output);
    }

    masm.bind(&done);
}

void
CodeGeneratorMIPS::visitCompareBAndBranch(LCompareBAndBranch* lir)
{
    MCompare* mir = lir->cmpMir();
    const ValueOperand lhs = ToValue(lir, LCompareBAndBranch::Lhs);
    const LAllocation* rhs = lir->rhs();

    MOZ_ASSERT(mir->jsop() == JSOP_STRICTEQ || mir->jsop() == JSOP_STRICTNE);

    MBasicBlock* mirNotBoolean = (mir->jsop() == JSOP_STRICTEQ) ? lir->ifFalse() : lir->ifTrue();
    branchToBlock(lhs.typeReg(), ImmType(JSVAL_TYPE_BOOLEAN), mirNotBoolean, Assembler::NotEqual);

    Assembler::Condition cond = JSOpToCondition(mir->compareType(), mir->jsop());
    if (rhs->isConstant())
        emitBranch(lhs.payloadReg(), Imm32(rhs->toConstant()->toBoolean()), cond, lir->ifTrue(),
                   lir->ifFalse());
    else
        emitBranch(lhs.payloadReg(), ToRegister(rhs), cond, lir->ifTrue(), lir->ifFalse());
}

void
CodeGeneratorMIPS::visitCompareBitwise(LCompareBitwise* lir)
{
    MCompare* mir = lir->mir();
    Assembler::Condition cond = JSOpToCondition(mir->compareType(), mir->jsop());
    const ValueOperand lhs = ToValue(lir, LCompareBitwise::LhsInput);
    const ValueOperand rhs = ToValue(lir, LCompareBitwise::RhsInput);
    const Register output = ToRegister(lir->output());

    MOZ_ASSERT(IsEqualityOp(mir->jsop()));

    Label notEqual, done;
    masm.ma_b(lhs.typeReg(), rhs.typeReg(), &notEqual, Assembler::NotEqual, ShortJump);
    {
        masm.cmp32Set(cond, lhs.payloadReg(), rhs.payloadReg(), output);
        masm.ma_b(&done, ShortJump);
    }
    masm.bind(&notEqual);
    {
        masm.move32(Imm32(cond == Assembler::NotEqual), output);
    }

    masm.bind(&done);
}

void
CodeGeneratorMIPS::visitCompareBitwiseAndBranch(LCompareBitwiseAndBranch* lir)
{
    MCompare* mir = lir->cmpMir();
    Assembler::Condition cond = JSOpToCondition(mir->compareType(), mir->jsop());
    const ValueOperand lhs = ToValue(lir, LCompareBitwiseAndBranch::LhsInput);
    const ValueOperand rhs = ToValue(lir, LCompareBitwiseAndBranch::RhsInput);

    MOZ_ASSERT(mir->jsop() == JSOP_EQ || mir->jsop() == JSOP_STRICTEQ ||
               mir->jsop() == JSOP_NE || mir->jsop() == JSOP_STRICTNE);

    MBasicBlock* notEqual = (cond == Assembler::Equal) ? lir->ifFalse() : lir->ifTrue();

    branchToBlock(lhs.typeReg(), rhs.typeReg(), notEqual, Assembler::NotEqual);
    emitBranch(lhs.payloadReg(), rhs.payloadReg(), cond, lir->ifTrue(), lir->ifFalse());
}

void
CodeGeneratorMIPS::setReturnDoubleRegs(LiveRegisterSet* regs)
{
    MOZ_ASSERT(ReturnFloat32Reg.code_ == ReturnDoubleReg.code_);
    regs->add(ReturnFloat32Reg);
    regs->add(ReturnDoubleReg.singleOverlay(1));
    regs->add(ReturnDoubleReg);
}
