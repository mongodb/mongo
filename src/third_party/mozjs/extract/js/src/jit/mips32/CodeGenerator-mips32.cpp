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

ValueOperand
CodeGeneratorMIPS::ToValue(LInstruction* ins, size_t pos)
{
    Register typeReg = ToRegister(ins->getOperand(pos + TYPE_INDEX));
    Register payloadReg = ToRegister(ins->getOperand(pos + PAYLOAD_INDEX));
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
    const AnyRegister in = ToAnyRegister(box->getOperand(0));
    const ValueOperand out = ToOutValue(box);

    masm.moveValue(TypedOrValueRegister(box->type(), in), out);
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

void
CodeGeneratorMIPS::splitTagForTest(const ValueOperand& value, ScratchTagScope& tag)
{
    MOZ_ASSERT(value.typeReg() == tag);
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
CodeGeneratorMIPS::visitCompareI64(LCompareI64* lir)
{
    MCompare* mir = lir->mir();
    MOZ_ASSERT(mir->compareType() == MCompare::Compare_Int64 ||
               mir->compareType() == MCompare::Compare_UInt64);

    const LInt64Allocation lhs = lir->getInt64Operand(LCompareI64::Lhs);
    const LInt64Allocation rhs = lir->getInt64Operand(LCompareI64::Rhs);
    Register64 lhsRegs = ToRegister64(lhs);
    Register output = ToRegister(lir->output());

    bool isSigned = mir->compareType() == MCompare::Compare_Int64;
    Assembler::Condition condition = JSOpToCondition(lir->jsop(), isSigned);

    if (IsConstant(rhs)) {
        Imm64 imm = Imm64(ToInt64(rhs));
        masm.cmp64Set(condition, lhsRegs, imm, output);
    } else {
        Register64 rhsRegs = ToRegister64(rhs);
        masm.cmp64Set(condition, lhsRegs, rhsRegs, output);
    }
}

void
CodeGeneratorMIPS::visitCompareI64AndBranch(LCompareI64AndBranch* lir)
{
    MCompare* mir = lir->cmpMir();
    MOZ_ASSERT(mir->compareType() == MCompare::Compare_Int64 ||
               mir->compareType() == MCompare::Compare_UInt64);

    const LInt64Allocation lhs = lir->getInt64Operand(LCompareI64::Lhs);
    const LInt64Allocation rhs = lir->getInt64Operand(LCompareI64::Rhs);
    Register64 lhsRegs = ToRegister64(lhs);

    bool isSigned = mir->compareType() == MCompare::Compare_Int64;
    Assembler::Condition condition = JSOpToCondition(lir->jsop(), isSigned);

    Label* trueLabel = getJumpLabelForBranch(lir->ifTrue());
    Label* falseLabel = getJumpLabelForBranch(lir->ifFalse());

    if (isNextBlock(lir->ifFalse()->lir())) {
        falseLabel = nullptr;
    } else if (isNextBlock(lir->ifTrue()->lir())) {
        condition = Assembler::InvertCondition(condition);
        trueLabel = falseLabel;
        falseLabel = nullptr;
    }

    if (IsConstant(rhs)) {
        Imm64 imm = Imm64(ToInt64(rhs));
        masm.branch64(condition, lhsRegs, imm, trueLabel, falseLabel);
    } else {
        Register64 rhsRegs = ToRegister64(rhs);
        masm.branch64(condition, lhsRegs, rhsRegs, trueLabel, falseLabel);
    }
}

void
CodeGeneratorMIPS::visitDivOrModI64(LDivOrModI64* lir)
{
    Register64 lhs = ToRegister64(lir->getInt64Operand(LDivOrModI64::Lhs));
    Register64 rhs = ToRegister64(lir->getInt64Operand(LDivOrModI64::Rhs));
    Register64 output = ToOutRegister64(lir);

    MOZ_ASSERT(output == ReturnReg64);

    Label done;

    // Handle divide by zero.
    if (lir->canBeDivideByZero()) {
        Label nonZero;
        masm.branchTest64(Assembler::NonZero, rhs, rhs, InvalidReg, &nonZero);
        masm.wasmTrap(wasm::Trap::IntegerDivideByZero, lir->bytecodeOffset());
        masm.bind(&nonZero);
    }

    // Handle an integer overflow exception from INT64_MIN / -1.
    if (lir->canBeNegativeOverflow()) {
        Label notOverflow;
        masm.branch64(Assembler::NotEqual, lhs, Imm64(INT64_MIN), &notOverflow);
        masm.branch64(Assembler::NotEqual, rhs, Imm64(-1), &notOverflow);
        if (lir->mir()->isMod())
            masm.xor64(output, output);
        else
            masm.wasmTrap(wasm::Trap::IntegerOverflow, lir->bytecodeOffset());
        masm.jump(&done);
        masm.bind(&notOverflow);
    }

    masm.setupWasmABICall();
    masm.passABIArg(lhs.high);
    masm.passABIArg(lhs.low);
    masm.passABIArg(rhs.high);
    masm.passABIArg(rhs.low);

    MOZ_ASSERT(gen->compilingWasm());
    if (lir->mir()->isMod())
        masm.callWithABI(lir->bytecodeOffset(), wasm::SymbolicAddress::ModI64);
    else
        masm.callWithABI(lir->bytecodeOffset(), wasm::SymbolicAddress::DivI64);
    MOZ_ASSERT(ReturnReg64 == output);

    masm.bind(&done);
}

void
CodeGeneratorMIPS::visitUDivOrModI64(LUDivOrModI64* lir)
{
    Register64 lhs = ToRegister64(lir->getInt64Operand(LDivOrModI64::Lhs));
    Register64 rhs = ToRegister64(lir->getInt64Operand(LDivOrModI64::Rhs));

    MOZ_ASSERT(ToOutRegister64(lir) == ReturnReg64);

    // Prevent divide by zero.
    if (lir->canBeDivideByZero()) {
        Label nonZero;
        masm.branchTest64(Assembler::NonZero, rhs, rhs, InvalidReg, &nonZero);
        masm.wasmTrap(wasm::Trap::IntegerDivideByZero, lir->bytecodeOffset());
        masm.bind(&nonZero);
    }

    masm.setupWasmABICall();
    masm.passABIArg(lhs.high);
    masm.passABIArg(lhs.low);
    masm.passABIArg(rhs.high);
    masm.passABIArg(rhs.low);

    MOZ_ASSERT(gen->compilingWasm());
    if (lir->mir()->isMod())
        masm.callWithABI(lir->bytecodeOffset(), wasm::SymbolicAddress::UModI64);
    else
        masm.callWithABI(lir->bytecodeOffset(), wasm::SymbolicAddress::UDivI64);
}

template <typename T>
void
CodeGeneratorMIPS::emitWasmLoadI64(T* lir)
{
    const MWasmLoad* mir = lir->mir();

    Register ptrScratch = InvalidReg;
    if(!lir->ptrCopy()->isBogusTemp()){
        ptrScratch = ToRegister(lir->ptrCopy());
    }

    if (IsUnaligned(mir->access())) {
        masm.wasmUnalignedLoadI64(mir->access(), HeapReg, ToRegister(lir->ptr()),
                                  ptrScratch, ToOutRegister64(lir), ToRegister(lir->getTemp(1)));
    } else {
        masm.wasmLoadI64(mir->access(), HeapReg, ToRegister(lir->ptr()), ptrScratch,
                         ToOutRegister64(lir));
    }
}

void
CodeGeneratorMIPS::visitWasmLoadI64(LWasmLoadI64* lir)
{
    emitWasmLoadI64(lir);
}

void
CodeGeneratorMIPS::visitWasmUnalignedLoadI64(LWasmUnalignedLoadI64* lir)
{
    emitWasmLoadI64(lir);
}

template <typename T>
void
CodeGeneratorMIPS::emitWasmStoreI64(T* lir)
{
    const MWasmStore* mir = lir->mir();

    Register ptrScratch = InvalidReg;
    if(!lir->ptrCopy()->isBogusTemp()){
        ptrScratch = ToRegister(lir->ptrCopy());
    }

    if (IsUnaligned(mir->access())) {
        masm.wasmUnalignedStoreI64(mir->access(), ToRegister64(lir->value()), HeapReg,
                                   ToRegister(lir->ptr()), ptrScratch, ToRegister(lir->getTemp(1)));
    } else {
        masm.wasmStoreI64(mir->access(), ToRegister64(lir->value()), HeapReg,
                          ToRegister(lir->ptr()), ptrScratch);
    }
}

void
CodeGeneratorMIPS::visitWasmStoreI64(LWasmStoreI64* lir)
{
    emitWasmStoreI64(lir);
}

void
CodeGeneratorMIPS::visitWasmUnalignedStoreI64(LWasmUnalignedStoreI64* lir)
{
    emitWasmStoreI64(lir);
}

void
CodeGeneratorMIPS::visitWasmSelectI64(LWasmSelectI64* lir)
{
    MOZ_ASSERT(lir->mir()->type() == MIRType::Int64);
    Register cond = ToRegister(lir->condExpr());
    const LInt64Allocation trueExpr = lir->trueExpr();
    const LInt64Allocation falseExpr = lir->falseExpr();

    Register64 output = ToOutRegister64(lir);

    masm.move64(ToRegister64(trueExpr), output);

    if (falseExpr.low().isRegister()) {
        masm.as_movz(output.low, ToRegister(falseExpr.low()), cond);
        masm.as_movz(output.high, ToRegister(falseExpr.high()), cond);
    } else {
        Label done;
        masm.ma_b(cond, cond, &done, Assembler::NonZero, ShortJump);
        masm.loadPtr(ToAddress(falseExpr.low()), output.low);
        masm.loadPtr(ToAddress(falseExpr.high()), output.high);
        masm.bind(&done);
    }
}

void
CodeGeneratorMIPS::visitWasmReinterpretFromI64(LWasmReinterpretFromI64* lir)
{
    MOZ_ASSERT(lir->mir()->type() == MIRType::Double);
    MOZ_ASSERT(lir->mir()->input()->type() == MIRType::Int64);
    Register64 input = ToRegister64(lir->getInt64Operand(0));
    FloatRegister output = ToFloatRegister(lir->output());

    masm.moveToDoubleLo(input.low, output);
    masm.moveToDoubleHi(input.high, output);
}

void
CodeGeneratorMIPS::visitWasmReinterpretToI64(LWasmReinterpretToI64* lir)
{
    MOZ_ASSERT(lir->mir()->type() == MIRType::Int64);
    MOZ_ASSERT(lir->mir()->input()->type() == MIRType::Double);
    FloatRegister input = ToFloatRegister(lir->getOperand(0));
    Register64 output = ToOutRegister64(lir);

    masm.moveFromDoubleLo(input, output.low);
    masm.moveFromDoubleHi(input, output.high);
}

void
CodeGeneratorMIPS::visitExtendInt32ToInt64(LExtendInt32ToInt64* lir)
{
    Register input = ToRegister(lir->input());
    Register64 output = ToOutRegister64(lir);

    if (input != output.low)
        masm.move32(input, output.low);
    if (lir->mir()->isUnsigned())
        masm.move32(Imm32(0), output.high);
    else
        masm.ma_sra(output.high, output.low, Imm32(31));
}

void
CodeGeneratorMIPS::visitWrapInt64ToInt32(LWrapInt64ToInt32* lir)
{
    const LInt64Allocation& input = lir->getInt64Operand(0);
    Register output = ToRegister(lir->output());

    if (lir->mir()->bottomHalf())
        masm.move32(ToRegister(input.low()), output);
    else
        masm.move32(ToRegister(input.high()), output);
}

void
CodeGeneratorMIPS::visitSignExtendInt64(LSignExtendInt64* lir)
{
    Register64 input = ToRegister64(lir->getInt64Operand(0));
    Register64 output = ToOutRegister64(lir);
    switch (lir->mode()) {
      case MSignExtendInt64::Byte:
        masm.move8SignExtend(input.low, output.low);
        break;
      case MSignExtendInt64::Half:
        masm.move16SignExtend(input.low, output.low);
        break;
      case MSignExtendInt64::Word:
        masm.move32(input.low, output.low);
        break;
    }
    masm.ma_sra(output.high, output.low, Imm32(31));
}

void
CodeGeneratorMIPS::visitClzI64(LClzI64* lir)
{
    Register64 input = ToRegister64(lir->getInt64Operand(0));
    Register64 output = ToOutRegister64(lir);
    masm.clz64(input, output.low);
    masm.move32(Imm32(0), output.high);
}

void
CodeGeneratorMIPS::visitCtzI64(LCtzI64* lir)
{
    Register64 input = ToRegister64(lir->getInt64Operand(0));
    Register64 output = ToOutRegister64(lir);
    masm.ctz64(input, output.low);
    masm.move32(Imm32(0), output.high);
}

void
CodeGeneratorMIPS::visitNotI64(LNotI64* lir)
{
    Register64 input = ToRegister64(lir->getInt64Operand(0));
    Register output = ToRegister(lir->output());

    masm.as_or(output, input.low, input.high);
    masm.cmp32Set(Assembler::Equal, output, Imm32(0), output);
}

void
CodeGeneratorMIPS::visitWasmTruncateToInt64(LWasmTruncateToInt64* lir)
{
    FloatRegister input = ToFloatRegister(lir->input());
    FloatRegister arg = input;
    Register64 output = ToOutRegister64(lir);
    MWasmTruncateToInt64* mir = lir->mir();
    MIRType fromType = mir->input()->type();

    auto* ool = new(alloc()) OutOfLineWasmTruncateCheck(mir, input, Register64::Invalid());
    addOutOfLineCode(ool, mir);

    if (fromType == MIRType::Float32) {
        arg = ScratchDoubleReg;
        masm.convertFloat32ToDouble(input, arg);
    }

    if (!lir->mir()->isSaturating()) {
        masm.Push(input);

        masm.setupWasmABICall();
        masm.passABIArg(arg, MoveOp::DOUBLE);

        if (lir->mir()->isUnsigned())
            masm.callWithABI(mir->bytecodeOffset(), wasm::SymbolicAddress::TruncateDoubleToUint64);
        else
            masm.callWithABI(mir->bytecodeOffset(), wasm::SymbolicAddress::TruncateDoubleToInt64);

        masm.Pop(input);

        masm.ma_xor(ScratchRegister, output.high, Imm32(0x80000000));
        masm.ma_or(ScratchRegister, output.low);
        masm.ma_b(ScratchRegister, Imm32(0), ool->entry(), Assembler::Equal);

        masm.bind(ool->rejoin());
    } else {
        masm.setupWasmABICall();
        masm.passABIArg(arg, MoveOp::DOUBLE);
        if (lir->mir()->isUnsigned())
            masm.callWithABI(mir->bytecodeOffset(), wasm::SymbolicAddress::SaturatingTruncateDoubleToUint64);
        else
            masm.callWithABI(mir->bytecodeOffset(), wasm::SymbolicAddress::SaturatingTruncateDoubleToInt64);
    }

    MOZ_ASSERT(ReturnReg64 == output);
}

void
CodeGeneratorMIPS::visitInt64ToFloatingPoint(LInt64ToFloatingPoint* lir)
{
    Register64 input = ToRegister64(lir->getInt64Operand(0));
    mozilla::DebugOnly<FloatRegister> output = ToFloatRegister(lir->output());

    MInt64ToFloatingPoint* mir = lir->mir();
    MIRType toType = mir->type();

    masm.setupWasmABICall();
    masm.passABIArg(input.high);
    masm.passABIArg(input.low);

    if (lir->mir()->isUnsigned())
        if (toType == MIRType::Double)
            masm.callWithABI(mir->bytecodeOffset(), wasm::SymbolicAddress::Uint64ToDouble, MoveOp::DOUBLE);
        else
            masm.callWithABI(mir->bytecodeOffset(), wasm::SymbolicAddress::Uint64ToFloat32, MoveOp::FLOAT32);
    else
        if (toType == MIRType::Double)
            masm.callWithABI(mir->bytecodeOffset(), wasm::SymbolicAddress::Int64ToDouble, MoveOp::DOUBLE);
        else
            masm.callWithABI(mir->bytecodeOffset(), wasm::SymbolicAddress::Int64ToFloat32, MoveOp::FLOAT32);

    MOZ_ASSERT_IF(toType == MIRType::Double, *(&output) == ReturnDoubleReg);
    MOZ_ASSERT_IF(toType == MIRType::Float32, *(&output) == ReturnFloat32Reg);
}

void
CodeGeneratorMIPS::visitTestI64AndBranch(LTestI64AndBranch* lir)
{
    Register64 input = ToRegister64(lir->getInt64Operand(0));

    branchToBlock(input.high, Imm32(0), lir->ifTrue(), Assembler::NonZero);
    emitBranch(input.low, Imm32(0), Assembler::NonZero, lir->ifTrue(), lir->ifFalse());
}

void
CodeGeneratorMIPS::setReturnDoubleRegs(LiveRegisterSet* regs)
{
    MOZ_ASSERT(ReturnFloat32Reg.code_ == ReturnDoubleReg.code_);
    regs->add(ReturnFloat32Reg);
    regs->add(ReturnDoubleReg);
}

void
CodeGeneratorMIPS::visitWasmAtomicLoadI64(LWasmAtomicLoadI64* lir)
{
    Register ptr = ToRegister(lir->ptr());
    Register64 output = ToOutRegister64(lir);
    uint32_t offset = lir->mir()->access().offset();

    BaseIndex addr(HeapReg, ptr, TimesOne, offset);

    masm.atomicLoad64(Synchronization::Full(), addr, Register64::Invalid(), output);
}

void
CodeGeneratorMIPS::visitWasmAtomicStoreI64(LWasmAtomicStoreI64* lir)
{
    Register ptr = ToRegister(lir->ptr());
    Register64 value = ToRegister64(lir->value());
    Register tmp = ToRegister(lir->tmp());
    uint32_t offset = lir->mir()->access().offset();

    BaseIndex addr(HeapReg, ptr, TimesOne, offset);

    masm.atomicStore64(addr, tmp, value);
}
