/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/x86-shared/CodeGenerator-x86-shared.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/MathAlgorithms.h"

#include "jsmath.h"

#include "jit/JitCompartment.h"
#include "jit/JitFrames.h"
#include "jit/Linker.h"
#include "jit/RangeAnalysis.h"
#include "vm/TraceLogging.h"

#include "jit/MacroAssembler-inl.h"
#include "jit/shared/CodeGenerator-shared-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::Abs;
using mozilla::BitwiseCast;
using mozilla::DebugOnly;
using mozilla::FloatingPoint;
using mozilla::FloorLog2;
using mozilla::NegativeInfinity;
using mozilla::SpecificNaN;

using JS::GenericNaN;

namespace js {
namespace jit {

CodeGeneratorX86Shared::CodeGeneratorX86Shared(MIRGenerator* gen, LIRGraph* graph, MacroAssembler* masm)
  : CodeGeneratorShared(gen, graph, masm)
{
}

#ifdef JS_PUNBOX64
Operand
CodeGeneratorX86Shared::ToOperandOrRegister64(const LInt64Allocation input)
{
    return ToOperand(input.value());
}
#else
Register64
CodeGeneratorX86Shared::ToOperandOrRegister64(const LInt64Allocation input)
{
    return ToRegister64(input);
}
#endif

void
OutOfLineBailout::accept(CodeGeneratorX86Shared* codegen)
{
    codegen->visitOutOfLineBailout(this);
}

void
CodeGeneratorX86Shared::emitBranch(Assembler::Condition cond, MBasicBlock* mirTrue,
                                   MBasicBlock* mirFalse, Assembler::NaNCond ifNaN)
{
    if (ifNaN == Assembler::NaN_IsFalse)
        jumpToBlock(mirFalse, Assembler::Parity);
    else if (ifNaN == Assembler::NaN_IsTrue)
        jumpToBlock(mirTrue, Assembler::Parity);

    if (isNextBlock(mirFalse->lir())) {
        jumpToBlock(mirTrue, cond);
    } else {
        jumpToBlock(mirFalse, Assembler::InvertCondition(cond));
        jumpToBlock(mirTrue);
    }
}

void
CodeGeneratorX86Shared::visitDouble(LDouble* ins)
{
    const LDefinition* out = ins->getDef(0);
    masm.loadConstantDouble(ins->getDouble(), ToFloatRegister(out));
}

void
CodeGeneratorX86Shared::visitFloat32(LFloat32* ins)
{
    const LDefinition* out = ins->getDef(0);
    masm.loadConstantFloat32(ins->getFloat(), ToFloatRegister(out));
}

void
CodeGeneratorX86Shared::visitTestIAndBranch(LTestIAndBranch* test)
{
    Register input = ToRegister(test->input());
    masm.test32(input, input);
    emitBranch(Assembler::NonZero, test->ifTrue(), test->ifFalse());
}

void
CodeGeneratorX86Shared::visitTestDAndBranch(LTestDAndBranch* test)
{
    const LAllocation* opd = test->input();

    // vucomisd flags:
    //             Z  P  C
    //            ---------
    //      NaN    1  1  1
    //        >    0  0  0
    //        <    0  0  1
    //        =    1  0  0
    //
    // NaN is falsey, so comparing against 0 and then using the Z flag is
    // enough to determine which branch to take.
    ScratchDoubleScope scratch(masm);
    masm.zeroDouble(scratch);
    masm.vucomisd(scratch, ToFloatRegister(opd));
    emitBranch(Assembler::NotEqual, test->ifTrue(), test->ifFalse());
}

void
CodeGeneratorX86Shared::visitTestFAndBranch(LTestFAndBranch* test)
{
    const LAllocation* opd = test->input();
    // vucomiss flags are the same as doubles; see comment above
    {
        ScratchFloat32Scope scratch(masm);
        masm.zeroFloat32(scratch);
        masm.vucomiss(scratch, ToFloatRegister(opd));
    }
    emitBranch(Assembler::NotEqual, test->ifTrue(), test->ifFalse());
}

void
CodeGeneratorX86Shared::visitBitAndAndBranch(LBitAndAndBranch* baab)
{
    if (baab->right()->isConstant())
        masm.test32(ToRegister(baab->left()), Imm32(ToInt32(baab->right())));
    else
        masm.test32(ToRegister(baab->left()), ToRegister(baab->right()));
    emitBranch(baab->cond(), baab->ifTrue(), baab->ifFalse());
}

void
CodeGeneratorX86Shared::emitCompare(MCompare::CompareType type, const LAllocation* left, const LAllocation* right)
{
#ifdef JS_CODEGEN_X64
    if (type == MCompare::Compare_Object || type == MCompare::Compare_Symbol) {
        masm.cmpPtr(ToRegister(left), ToOperand(right));
        return;
    }
#endif

    if (right->isConstant())
        masm.cmp32(ToRegister(left), Imm32(ToInt32(right)));
    else
        masm.cmp32(ToRegister(left), ToOperand(right));
}

void
CodeGeneratorX86Shared::visitCompare(LCompare* comp)
{
    MCompare* mir = comp->mir();
    emitCompare(mir->compareType(), comp->left(), comp->right());
    masm.emitSet(JSOpToCondition(mir->compareType(), comp->jsop()), ToRegister(comp->output()));
}

void
CodeGeneratorX86Shared::visitCompareAndBranch(LCompareAndBranch* comp)
{
    MCompare* mir = comp->cmpMir();
    emitCompare(mir->compareType(), comp->left(), comp->right());
    Assembler::Condition cond = JSOpToCondition(mir->compareType(), comp->jsop());
    emitBranch(cond, comp->ifTrue(), comp->ifFalse());
}

void
CodeGeneratorX86Shared::visitCompareD(LCompareD* comp)
{
    FloatRegister lhs = ToFloatRegister(comp->left());
    FloatRegister rhs = ToFloatRegister(comp->right());

    Assembler::DoubleCondition cond = JSOpToDoubleCondition(comp->mir()->jsop());

    Assembler::NaNCond nanCond = Assembler::NaNCondFromDoubleCondition(cond);
    if (comp->mir()->operandsAreNeverNaN())
        nanCond = Assembler::NaN_HandledByCond;

    masm.compareDouble(cond, lhs, rhs);
    masm.emitSet(Assembler::ConditionFromDoubleCondition(cond), ToRegister(comp->output()), nanCond);
}

void
CodeGeneratorX86Shared::visitCompareF(LCompareF* comp)
{
    FloatRegister lhs = ToFloatRegister(comp->left());
    FloatRegister rhs = ToFloatRegister(comp->right());

    Assembler::DoubleCondition cond = JSOpToDoubleCondition(comp->mir()->jsop());

    Assembler::NaNCond nanCond = Assembler::NaNCondFromDoubleCondition(cond);
    if (comp->mir()->operandsAreNeverNaN())
        nanCond = Assembler::NaN_HandledByCond;

    masm.compareFloat(cond, lhs, rhs);
    masm.emitSet(Assembler::ConditionFromDoubleCondition(cond), ToRegister(comp->output()), nanCond);
}

void
CodeGeneratorX86Shared::visitNotI(LNotI* ins)
{
    masm.cmp32(ToRegister(ins->input()), Imm32(0));
    masm.emitSet(Assembler::Equal, ToRegister(ins->output()));
}

void
CodeGeneratorX86Shared::visitNotD(LNotD* ins)
{
    FloatRegister opd = ToFloatRegister(ins->input());

    // Not returns true if the input is a NaN. We don't have to worry about
    // it if we know the input is never NaN though.
    Assembler::NaNCond nanCond = Assembler::NaN_IsTrue;
    if (ins->mir()->operandIsNeverNaN())
        nanCond = Assembler::NaN_HandledByCond;

    ScratchDoubleScope scratch(masm);
    masm.zeroDouble(scratch);
    masm.compareDouble(Assembler::DoubleEqualOrUnordered, opd, scratch);
    masm.emitSet(Assembler::Equal, ToRegister(ins->output()), nanCond);
}

void
CodeGeneratorX86Shared::visitNotF(LNotF* ins)
{
    FloatRegister opd = ToFloatRegister(ins->input());

    // Not returns true if the input is a NaN. We don't have to worry about
    // it if we know the input is never NaN though.
    Assembler::NaNCond nanCond = Assembler::NaN_IsTrue;
    if (ins->mir()->operandIsNeverNaN())
        nanCond = Assembler::NaN_HandledByCond;

    ScratchFloat32Scope scratch(masm);
    masm.zeroFloat32(scratch);
    masm.compareFloat(Assembler::DoubleEqualOrUnordered, opd, scratch);
    masm.emitSet(Assembler::Equal, ToRegister(ins->output()), nanCond);
}

void
CodeGeneratorX86Shared::visitCompareDAndBranch(LCompareDAndBranch* comp)
{
    FloatRegister lhs = ToFloatRegister(comp->left());
    FloatRegister rhs = ToFloatRegister(comp->right());

    Assembler::DoubleCondition cond = JSOpToDoubleCondition(comp->cmpMir()->jsop());

    Assembler::NaNCond nanCond = Assembler::NaNCondFromDoubleCondition(cond);
    if (comp->cmpMir()->operandsAreNeverNaN())
        nanCond = Assembler::NaN_HandledByCond;

    masm.compareDouble(cond, lhs, rhs);
    emitBranch(Assembler::ConditionFromDoubleCondition(cond), comp->ifTrue(), comp->ifFalse(), nanCond);
}

void
CodeGeneratorX86Shared::visitCompareFAndBranch(LCompareFAndBranch* comp)
{
    FloatRegister lhs = ToFloatRegister(comp->left());
    FloatRegister rhs = ToFloatRegister(comp->right());

    Assembler::DoubleCondition cond = JSOpToDoubleCondition(comp->cmpMir()->jsop());

    Assembler::NaNCond nanCond = Assembler::NaNCondFromDoubleCondition(cond);
    if (comp->cmpMir()->operandsAreNeverNaN())
        nanCond = Assembler::NaN_HandledByCond;

    masm.compareFloat(cond, lhs, rhs);
    emitBranch(Assembler::ConditionFromDoubleCondition(cond), comp->ifTrue(), comp->ifFalse(), nanCond);
}

void
CodeGeneratorX86Shared::visitWasmStackArg(LWasmStackArg* ins)
{
    const MWasmStackArg* mir = ins->mir();
    Address dst(StackPointer, mir->spOffset());
    if (ins->arg()->isConstant()) {
        masm.storePtr(ImmWord(ToInt32(ins->arg())), dst);
    } else if (ins->arg()->isGeneralReg()) {
        masm.storePtr(ToRegister(ins->arg()), dst);
    } else {
        switch (mir->input()->type()) {
          case MIRType::Double:
            masm.storeDouble(ToFloatRegister(ins->arg()), dst);
            return;
          case MIRType::Float32:
            masm.storeFloat32(ToFloatRegister(ins->arg()), dst);
            return;
          // StackPointer is SIMD-aligned and ABIArgGenerator guarantees
          // stack offsets are SIMD-aligned.
          case MIRType::Int32x4:
          case MIRType::Bool32x4:
            masm.storeAlignedSimd128Int(ToFloatRegister(ins->arg()), dst);
            return;
          case MIRType::Float32x4:
            masm.storeAlignedSimd128Float(ToFloatRegister(ins->arg()), dst);
            return;
          default: break;
        }
        MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE("unexpected mir type in WasmStackArg");
    }
}

void
CodeGeneratorX86Shared::visitWasmStackArgI64(LWasmStackArgI64* ins)
{
    const MWasmStackArg* mir = ins->mir();
    Address dst(StackPointer, mir->spOffset());
    if (IsConstant(ins->arg()))
        masm.store64(Imm64(ToInt64(ins->arg())), dst);
    else
        masm.store64(ToRegister64(ins->arg()), dst);
}

void
CodeGeneratorX86Shared::visitWasmSelect(LWasmSelect* ins)
{
    MIRType mirType = ins->mir()->type();

    Register cond = ToRegister(ins->condExpr());
    Operand falseExpr = ToOperand(ins->falseExpr());

    masm.test32(cond, cond);

    if (mirType == MIRType::Int32) {
        Register out = ToRegister(ins->output());
        MOZ_ASSERT(ToRegister(ins->trueExpr()) == out, "true expr input is reused for output");
        masm.cmovzl(falseExpr, out);
        return;
    }

    FloatRegister out = ToFloatRegister(ins->output());
    MOZ_ASSERT(ToFloatRegister(ins->trueExpr()) == out, "true expr input is reused for output");

    Label done;
    masm.j(Assembler::NonZero, &done);

    if (mirType == MIRType::Float32) {
        if (falseExpr.kind() == Operand::FPREG)
            masm.moveFloat32(ToFloatRegister(ins->falseExpr()), out);
        else
            masm.loadFloat32(falseExpr, out);
    } else if (mirType == MIRType::Double) {
        if (falseExpr.kind() == Operand::FPREG)
            masm.moveDouble(ToFloatRegister(ins->falseExpr()), out);
        else
            masm.loadDouble(falseExpr, out);
    } else {
        MOZ_CRASH("unhandled type in visitWasmSelect!");
    }

    masm.bind(&done);
}

void
CodeGeneratorX86Shared::visitWasmReinterpret(LWasmReinterpret* lir)
{
    MOZ_ASSERT(gen->compilingWasm());
    MWasmReinterpret* ins = lir->mir();

    MIRType to = ins->type();
#ifdef DEBUG
    MIRType from = ins->input()->type();
#endif

    switch (to) {
      case MIRType::Int32:
        MOZ_ASSERT(from == MIRType::Float32);
        masm.vmovd(ToFloatRegister(lir->input()), ToRegister(lir->output()));
        break;
      case MIRType::Float32:
        MOZ_ASSERT(from == MIRType::Int32);
        masm.vmovd(ToRegister(lir->input()), ToFloatRegister(lir->output()));
        break;
      case MIRType::Double:
      case MIRType::Int64:
        MOZ_CRASH("not handled by this LIR opcode");
      default:
        MOZ_CRASH("unexpected WasmReinterpret");
    }
}

void
CodeGeneratorX86Shared::visitOutOfLineLoadTypedArrayOutOfBounds(OutOfLineLoadTypedArrayOutOfBounds* ool)
{
    switch (ool->viewType()) {
      case Scalar::Int64:
      case Scalar::Float32x4:
      case Scalar::Int8x16:
      case Scalar::Int16x8:
      case Scalar::Int32x4:
      case Scalar::MaxTypedArrayViewType:
        MOZ_CRASH("unexpected array type");
      case Scalar::Float32:
        masm.loadConstantFloat32(float(GenericNaN()), ool->dest().fpu());
        break;
      case Scalar::Float64:
        masm.loadConstantDouble(GenericNaN(), ool->dest().fpu());
        break;
      case Scalar::Int8:
      case Scalar::Uint8:
      case Scalar::Int16:
      case Scalar::Uint16:
      case Scalar::Int32:
      case Scalar::Uint32:
      case Scalar::Uint8Clamped:
        Register destReg = ool->dest().gpr();
        masm.mov(ImmWord(0), destReg);
        break;
    }
    masm.jmp(ool->rejoin());
}

void
CodeGeneratorX86Shared::visitWasmAddOffset(LWasmAddOffset* lir)
{
    MWasmAddOffset* mir = lir->mir();
    Register base = ToRegister(lir->base());
    Register out = ToRegister(lir->output());

    if (base != out)
        masm.move32(base, out);
    masm.add32(Imm32(mir->offset()), out);

    masm.j(Assembler::CarrySet, oldTrap(mir, wasm::Trap::OutOfBounds));
}

void
CodeGeneratorX86Shared::visitWasmTruncateToInt32(LWasmTruncateToInt32* lir)
{
    FloatRegister input = ToFloatRegister(lir->input());
    Register output = ToRegister(lir->output());

    MWasmTruncateToInt32* mir = lir->mir();
    MIRType inputType = mir->input()->type();

    MOZ_ASSERT(inputType == MIRType::Double || inputType == MIRType::Float32);

    auto* ool = new (alloc()) OutOfLineWasmTruncateCheck(mir, input, output);
    addOutOfLineCode(ool, mir);

    Label* oolEntry = ool->entry();
    if (mir->isUnsigned()) {
        if (inputType == MIRType::Double)
            masm.wasmTruncateDoubleToUInt32(input, output, mir->isSaturating(), oolEntry);
        else if (inputType == MIRType::Float32)
            masm.wasmTruncateFloat32ToUInt32(input, output, mir->isSaturating(), oolEntry);
        else
            MOZ_CRASH("unexpected type");
        if (mir->isSaturating())
            masm.bind(ool->rejoin());
        return;
    }

    if (inputType == MIRType::Double)
        masm.wasmTruncateDoubleToInt32(input, output, mir->isSaturating(), oolEntry);
    else if (inputType == MIRType::Float32)
        masm.wasmTruncateFloat32ToInt32(input, output, mir->isSaturating(), oolEntry);
    else
        MOZ_CRASH("unexpected type");

    masm.bind(ool->rejoin());
}

bool
CodeGeneratorX86Shared::generateOutOfLineCode()
{
    if (!CodeGeneratorShared::generateOutOfLineCode())
        return false;

    if (deoptLabel_.used()) {
        // All non-table-based bailouts will go here.
        masm.bind(&deoptLabel_);

        // Push the frame size, so the handler can recover the IonScript.
        masm.push(Imm32(frameSize()));

        TrampolinePtr handler = gen->jitRuntime()->getGenericBailoutHandler();
        masm.jump(handler);
    }

    return !masm.oom();
}

class BailoutJump {
    Assembler::Condition cond_;

  public:
    explicit BailoutJump(Assembler::Condition cond) : cond_(cond)
    { }
#ifdef JS_CODEGEN_X86
    void operator()(MacroAssembler& masm, uint8_t* code) const {
        masm.j(cond_, ImmPtr(code), Relocation::HARDCODED);
    }
#endif
    void operator()(MacroAssembler& masm, Label* label) const {
        masm.j(cond_, label);
    }
};

class BailoutLabel {
    Label* label_;

  public:
    explicit BailoutLabel(Label* label) : label_(label)
    { }
#ifdef JS_CODEGEN_X86
    void operator()(MacroAssembler& masm, uint8_t* code) const {
        masm.retarget(label_, ImmPtr(code), Relocation::HARDCODED);
    }
#endif
    void operator()(MacroAssembler& masm, Label* label) const {
        masm.retarget(label_, label);
    }
};

template <typename T> void
CodeGeneratorX86Shared::bailout(const T& binder, LSnapshot* snapshot)
{
    encode(snapshot);

    // Though the assembler doesn't track all frame pushes, at least make sure
    // the known value makes sense. We can't use bailout tables if the stack
    // isn't properly aligned to the static frame size.
    MOZ_ASSERT_IF(frameClass_ != FrameSizeClass::None() && deoptTable_,
                  frameClass_.frameSize() == masm.framePushed());

#ifdef JS_CODEGEN_X86
    // On x64, bailout tables are pointless, because 16 extra bytes are
    // reserved per external jump, whereas it takes only 10 bytes to encode a
    // a non-table based bailout.
    if (assignBailoutId(snapshot)) {
        binder(masm, deoptTable_->value + snapshot->bailoutId() * BAILOUT_TABLE_ENTRY_SIZE);
        return;
    }
#endif

    // We could not use a jump table, either because all bailout IDs were
    // reserved, or a jump table is not optimal for this frame size or
    // platform. Whatever, we will generate a lazy bailout.
    //
    // All bailout code is associated with the bytecodeSite of the block we are
    // bailing out from.
    InlineScriptTree* tree = snapshot->mir()->block()->trackedTree();
    OutOfLineBailout* ool = new(alloc()) OutOfLineBailout(snapshot);
    addOutOfLineCode(ool, new(alloc()) BytecodeSite(tree, tree->script()->code()));

    binder(masm, ool->entry());
}

void
CodeGeneratorX86Shared::bailoutIf(Assembler::Condition condition, LSnapshot* snapshot)
{
    bailout(BailoutJump(condition), snapshot);
}

void
CodeGeneratorX86Shared::bailoutIf(Assembler::DoubleCondition condition, LSnapshot* snapshot)
{
    MOZ_ASSERT(Assembler::NaNCondFromDoubleCondition(condition) == Assembler::NaN_HandledByCond);
    bailoutIf(Assembler::ConditionFromDoubleCondition(condition), snapshot);
}

void
CodeGeneratorX86Shared::bailoutFrom(Label* label, LSnapshot* snapshot)
{
    MOZ_ASSERT_IF(!masm.oom(), label->used() && !label->bound());
    bailout(BailoutLabel(label), snapshot);
}

void
CodeGeneratorX86Shared::bailout(LSnapshot* snapshot)
{
    Label label;
    masm.jump(&label);
    bailoutFrom(&label, snapshot);
}

void
CodeGeneratorX86Shared::visitOutOfLineBailout(OutOfLineBailout* ool)
{
    masm.push(Imm32(ool->snapshot()->snapshotOffset()));
    masm.jmp(&deoptLabel_);
}

void
CodeGeneratorX86Shared::visitMinMaxD(LMinMaxD* ins)
{
    FloatRegister first = ToFloatRegister(ins->first());
    FloatRegister second = ToFloatRegister(ins->second());
#ifdef DEBUG
    FloatRegister output = ToFloatRegister(ins->output());
    MOZ_ASSERT(first == output);
#endif

    bool handleNaN = !ins->mir()->range() || ins->mir()->range()->canBeNaN();

    if (ins->mir()->isMax())
        masm.maxDouble(second, first, handleNaN);
    else
        masm.minDouble(second, first, handleNaN);
}

void
CodeGeneratorX86Shared::visitMinMaxF(LMinMaxF* ins)
{
    FloatRegister first = ToFloatRegister(ins->first());
    FloatRegister second = ToFloatRegister(ins->second());
#ifdef DEBUG
    FloatRegister output = ToFloatRegister(ins->output());
    MOZ_ASSERT(first == output);
#endif

    bool handleNaN = !ins->mir()->range() || ins->mir()->range()->canBeNaN();

    if (ins->mir()->isMax())
        masm.maxFloat32(second, first, handleNaN);
    else
        masm.minFloat32(second, first, handleNaN);
}

void
CodeGeneratorX86Shared::visitAbsD(LAbsD* ins)
{
    FloatRegister input = ToFloatRegister(ins->input());
    MOZ_ASSERT(input == ToFloatRegister(ins->output()));
    // Load a value which is all ones except for the sign bit.
    ScratchDoubleScope scratch(masm);
    masm.loadConstantDouble(SpecificNaN<double>(0, FloatingPoint<double>::kSignificandBits), scratch);
    masm.vandpd(scratch, input, input);
}

void
CodeGeneratorX86Shared::visitAbsF(LAbsF* ins)
{
    FloatRegister input = ToFloatRegister(ins->input());
    MOZ_ASSERT(input == ToFloatRegister(ins->output()));
    // Same trick as visitAbsD above.
    ScratchFloat32Scope scratch(masm);
    masm.loadConstantFloat32(SpecificNaN<float>(0, FloatingPoint<float>::kSignificandBits), scratch);
    masm.vandps(scratch, input, input);
}

void
CodeGeneratorX86Shared::visitClzI(LClzI* ins)
{
    Register input = ToRegister(ins->input());
    Register output = ToRegister(ins->output());
    bool knownNotZero = ins->mir()->operandIsNeverZero();

    masm.clz32(input, output, knownNotZero);
}

void
CodeGeneratorX86Shared::visitCtzI(LCtzI* ins)
{
    Register input = ToRegister(ins->input());
    Register output = ToRegister(ins->output());
    bool knownNotZero = ins->mir()->operandIsNeverZero();

    masm.ctz32(input, output, knownNotZero);
}

void
CodeGeneratorX86Shared::visitPopcntI(LPopcntI* ins)
{
    Register input = ToRegister(ins->input());
    Register output = ToRegister(ins->output());
    Register temp = ins->temp()->isBogusTemp() ? InvalidReg : ToRegister(ins->temp());

    masm.popcnt32(input, output, temp);
}

void
CodeGeneratorX86Shared::visitSqrtD(LSqrtD* ins)
{
    FloatRegister input = ToFloatRegister(ins->input());
    FloatRegister output = ToFloatRegister(ins->output());
    masm.vsqrtsd(input, output, output);
}

void
CodeGeneratorX86Shared::visitSqrtF(LSqrtF* ins)
{
    FloatRegister input = ToFloatRegister(ins->input());
    FloatRegister output = ToFloatRegister(ins->output());
    masm.vsqrtss(input, output, output);
}

void
CodeGeneratorX86Shared::visitPowHalfD(LPowHalfD* ins)
{
    FloatRegister input = ToFloatRegister(ins->input());
    FloatRegister output = ToFloatRegister(ins->output());

    ScratchDoubleScope scratch(masm);

    Label done, sqrt;

    if (!ins->mir()->operandIsNeverNegativeInfinity()) {
        // Branch if not -Infinity.
        masm.loadConstantDouble(NegativeInfinity<double>(), scratch);

        Assembler::DoubleCondition cond = Assembler::DoubleNotEqualOrUnordered;
        if (ins->mir()->operandIsNeverNaN())
            cond = Assembler::DoubleNotEqual;
        masm.branchDouble(cond, input, scratch, &sqrt);

        // Math.pow(-Infinity, 0.5) == Infinity.
        masm.zeroDouble(output);
        masm.subDouble(scratch, output);
        masm.jump(&done);

        masm.bind(&sqrt);
    }

    if (!ins->mir()->operandIsNeverNegativeZero()) {
        // Math.pow(-0, 0.5) == 0 == Math.pow(0, 0.5). Adding 0 converts any -0 to 0.
        masm.zeroDouble(scratch);
        masm.addDouble(input, scratch);
        masm.vsqrtsd(scratch, output, output);
    } else {
        masm.vsqrtsd(input, output, output);
    }

    masm.bind(&done);
}

class OutOfLineUndoALUOperation : public OutOfLineCodeBase<CodeGeneratorX86Shared>
{
    LInstruction* ins_;

  public:
    explicit OutOfLineUndoALUOperation(LInstruction* ins)
        : ins_(ins)
    { }

    virtual void accept(CodeGeneratorX86Shared* codegen) override {
        codegen->visitOutOfLineUndoALUOperation(this);
    }
    LInstruction* ins() const {
        return ins_;
    }
};

void
CodeGeneratorX86Shared::visitAddI(LAddI* ins)
{
    if (ins->rhs()->isConstant())
        masm.addl(Imm32(ToInt32(ins->rhs())), ToOperand(ins->lhs()));
    else
        masm.addl(ToOperand(ins->rhs()), ToRegister(ins->lhs()));

    if (ins->snapshot()) {
        if (ins->recoversInput()) {
            OutOfLineUndoALUOperation* ool = new(alloc()) OutOfLineUndoALUOperation(ins);
            addOutOfLineCode(ool, ins->mir());
            masm.j(Assembler::Overflow, ool->entry());
        } else {
            bailoutIf(Assembler::Overflow, ins->snapshot());
        }
    }
}

void
CodeGeneratorX86Shared::visitAddI64(LAddI64* lir)
{
    const LInt64Allocation lhs = lir->getInt64Operand(LAddI64::Lhs);
    const LInt64Allocation rhs = lir->getInt64Operand(LAddI64::Rhs);

    MOZ_ASSERT(ToOutRegister64(lir) == ToRegister64(lhs));

    if (IsConstant(rhs)) {
        masm.add64(Imm64(ToInt64(rhs)), ToRegister64(lhs));
        return;
    }

    masm.add64(ToOperandOrRegister64(rhs), ToRegister64(lhs));
}

void
CodeGeneratorX86Shared::visitSubI(LSubI* ins)
{
    if (ins->rhs()->isConstant())
        masm.subl(Imm32(ToInt32(ins->rhs())), ToOperand(ins->lhs()));
    else
        masm.subl(ToOperand(ins->rhs()), ToRegister(ins->lhs()));

    if (ins->snapshot()) {
        if (ins->recoversInput()) {
            OutOfLineUndoALUOperation* ool = new(alloc()) OutOfLineUndoALUOperation(ins);
            addOutOfLineCode(ool, ins->mir());
            masm.j(Assembler::Overflow, ool->entry());
        } else {
            bailoutIf(Assembler::Overflow, ins->snapshot());
        }
    }
}

void
CodeGeneratorX86Shared::visitSubI64(LSubI64* lir)
{
    const LInt64Allocation lhs = lir->getInt64Operand(LSubI64::Lhs);
    const LInt64Allocation rhs = lir->getInt64Operand(LSubI64::Rhs);

    MOZ_ASSERT(ToOutRegister64(lir) == ToRegister64(lhs));

    if (IsConstant(rhs)) {
        masm.sub64(Imm64(ToInt64(rhs)), ToRegister64(lhs));
        return;
    }

    masm.sub64(ToOperandOrRegister64(rhs), ToRegister64(lhs));
}

void
CodeGeneratorX86Shared::visitOutOfLineUndoALUOperation(OutOfLineUndoALUOperation* ool)
{
    LInstruction* ins = ool->ins();
    Register reg = ToRegister(ins->getDef(0));

    DebugOnly<LAllocation*> lhs = ins->getOperand(0);
    LAllocation* rhs = ins->getOperand(1);

    MOZ_ASSERT(reg == ToRegister(lhs));
    MOZ_ASSERT_IF(rhs->isGeneralReg(), reg != ToRegister(rhs));

    // Undo the effect of the ALU operation, which was performed on the output
    // register and overflowed. Writing to the output register clobbered an
    // input reg, and the original value of the input needs to be recovered
    // to satisfy the constraint imposed by any RECOVERED_INPUT operands to
    // the bailout snapshot.

    if (rhs->isConstant()) {
        Imm32 constant(ToInt32(rhs));
        if (ins->isAddI())
            masm.subl(constant, reg);
        else
            masm.addl(constant, reg);
    } else {
        if (ins->isAddI())
            masm.subl(ToOperand(rhs), reg);
        else
            masm.addl(ToOperand(rhs), reg);
    }

    bailout(ool->ins()->snapshot());
}

class MulNegativeZeroCheck : public OutOfLineCodeBase<CodeGeneratorX86Shared>
{
    LMulI* ins_;

  public:
    explicit MulNegativeZeroCheck(LMulI* ins)
      : ins_(ins)
    { }

    virtual void accept(CodeGeneratorX86Shared* codegen) override {
        codegen->visitMulNegativeZeroCheck(this);
    }
    LMulI* ins() const {
        return ins_;
    }
};

void
CodeGeneratorX86Shared::visitMulI(LMulI* ins)
{
    const LAllocation* lhs = ins->lhs();
    const LAllocation* rhs = ins->rhs();
    MMul* mul = ins->mir();
    MOZ_ASSERT_IF(mul->mode() == MMul::Integer, !mul->canBeNegativeZero() && !mul->canOverflow());

    if (rhs->isConstant()) {
        // Bailout on -0.0
        int32_t constant = ToInt32(rhs);
        if (mul->canBeNegativeZero() && constant <= 0) {
            Assembler::Condition bailoutCond = (constant == 0) ? Assembler::Signed : Assembler::Equal;
            masm.test32(ToRegister(lhs), ToRegister(lhs));
            bailoutIf(bailoutCond, ins->snapshot());
        }

        switch (constant) {
          case -1:
            masm.negl(ToOperand(lhs));
            break;
          case 0:
            masm.xorl(ToOperand(lhs), ToRegister(lhs));
            return; // escape overflow check;
          case 1:
            // nop
            return; // escape overflow check;
          case 2:
            masm.addl(ToOperand(lhs), ToRegister(lhs));
            break;
          default:
            if (!mul->canOverflow() && constant > 0) {
                // Use shift if cannot overflow and constant is power of 2
                int32_t shift = FloorLog2(constant);
                if ((1 << shift) == constant) {
                    masm.shll(Imm32(shift), ToRegister(lhs));
                    return;
                }
            }
            masm.imull(Imm32(ToInt32(rhs)), ToRegister(lhs));
        }

        // Bailout on overflow
        if (mul->canOverflow())
            bailoutIf(Assembler::Overflow, ins->snapshot());
    } else {
        masm.imull(ToOperand(rhs), ToRegister(lhs));

        // Bailout on overflow
        if (mul->canOverflow())
            bailoutIf(Assembler::Overflow, ins->snapshot());

        if (mul->canBeNegativeZero()) {
            // Jump to an OOL path if the result is 0.
            MulNegativeZeroCheck* ool = new(alloc()) MulNegativeZeroCheck(ins);
            addOutOfLineCode(ool, mul);

            masm.test32(ToRegister(lhs), ToRegister(lhs));
            masm.j(Assembler::Zero, ool->entry());
            masm.bind(ool->rejoin());
        }
    }
}

void
CodeGeneratorX86Shared::visitMulI64(LMulI64* lir)
{
    const LInt64Allocation lhs = lir->getInt64Operand(LMulI64::Lhs);
    const LInt64Allocation rhs = lir->getInt64Operand(LMulI64::Rhs);

    MOZ_ASSERT(ToRegister64(lhs) == ToOutRegister64(lir));

    if (IsConstant(rhs)) {
        int64_t constant = ToInt64(rhs);
        switch (constant) {
          case -1:
            masm.neg64(ToRegister64(lhs));
            return;
          case 0:
            masm.xor64(ToRegister64(lhs), ToRegister64(lhs));
            return;
          case 1:
            // nop
            return;
          case 2:
            masm.add64(ToRegister64(lhs), ToRegister64(lhs));
            return;
          default:
            if (constant > 0) {
                // Use shift if constant is power of 2.
                int32_t shift = mozilla::FloorLog2(constant);
                if (int64_t(1) << shift == constant) {
                    masm.lshift64(Imm32(shift), ToRegister64(lhs));
                    return;
                }
            }
            Register temp = ToTempRegisterOrInvalid(lir->temp());
            masm.mul64(Imm64(constant), ToRegister64(lhs), temp);
        }
    } else {
        Register temp = ToTempRegisterOrInvalid(lir->temp());
        masm.mul64(ToOperandOrRegister64(rhs), ToRegister64(lhs), temp);
    }
}

class ReturnZero : public OutOfLineCodeBase<CodeGeneratorX86Shared>
{
    Register reg_;

  public:
    explicit ReturnZero(Register reg)
      : reg_(reg)
    { }

    virtual void accept(CodeGeneratorX86Shared* codegen) override {
        codegen->visitReturnZero(this);
    }
    Register reg() const {
        return reg_;
    }
};

void
CodeGeneratorX86Shared::visitReturnZero(ReturnZero* ool)
{
    masm.mov(ImmWord(0), ool->reg());
    masm.jmp(ool->rejoin());
}

void
CodeGeneratorX86Shared::visitUDivOrMod(LUDivOrMod* ins)
{
    Register lhs = ToRegister(ins->lhs());
    Register rhs = ToRegister(ins->rhs());
    Register output = ToRegister(ins->output());

    MOZ_ASSERT_IF(lhs != rhs, rhs != eax);
    MOZ_ASSERT(rhs != edx);
    MOZ_ASSERT_IF(output == eax, ToRegister(ins->remainder()) == edx);

    ReturnZero* ool = nullptr;

    // Put the lhs in eax.
    if (lhs != eax)
        masm.mov(lhs, eax);

    // Prevent divide by zero.
    if (ins->canBeDivideByZero()) {
        masm.test32(rhs, rhs);
        if (ins->mir()->isTruncated()) {
            if (ins->trapOnError()) {
                Label nonZero;
                masm.j(Assembler::NonZero, &nonZero);
                masm.wasmTrap(wasm::Trap::IntegerDivideByZero, ins->bytecodeOffset());
                masm.bind(&nonZero);
            } else {
                ool = new(alloc()) ReturnZero(output);
                masm.j(Assembler::Zero, ool->entry());
            }
        } else {
            bailoutIf(Assembler::Zero, ins->snapshot());
        }
    }

    // Zero extend the lhs into edx to make (edx:eax), since udiv is 64-bit.
    masm.mov(ImmWord(0), edx);
    masm.udiv(rhs);

    // If the remainder is > 0, bailout since this must be a double.
    if (ins->mir()->isDiv() && !ins->mir()->toDiv()->canTruncateRemainder()) {
        Register remainder = ToRegister(ins->remainder());
        masm.test32(remainder, remainder);
        bailoutIf(Assembler::NonZero, ins->snapshot());
    }

    // Unsigned div or mod can return a value that's not a signed int32.
    // If our users aren't expecting that, bail.
    if (!ins->mir()->isTruncated()) {
        masm.test32(output, output);
        bailoutIf(Assembler::Signed, ins->snapshot());
    }

    if (ool) {
        addOutOfLineCode(ool, ins->mir());
        masm.bind(ool->rejoin());
    }
}

void
CodeGeneratorX86Shared::visitUDivOrModConstant(LUDivOrModConstant *ins) {
    Register lhs = ToRegister(ins->numerator());
    Register output = ToRegister(ins->output());
    uint32_t d = ins->denominator();

    // This emits the division answer into edx or the modulus answer into eax.
    MOZ_ASSERT(output == eax || output == edx);
    MOZ_ASSERT(lhs != eax && lhs != edx);
    bool isDiv = (output == edx);

    if (d == 0) {
        if (ins->mir()->isTruncated()) {
            if (ins->trapOnError())
                masm.wasmTrap(wasm::Trap::IntegerDivideByZero, ins->bytecodeOffset());
            else
                masm.xorl(output, output);
        } else {
            bailout(ins->snapshot());
        }
        return;
    }

    // The denominator isn't a power of 2 (see LDivPowTwoI and LModPowTwoI).
    MOZ_ASSERT((d & (d - 1)) != 0);

    ReciprocalMulConstants rmc = computeDivisionConstants(d, /* maxLog = */ 32);

    // We first compute (M * n) >> 32, where M = rmc.multiplier.
    masm.movl(Imm32(rmc.multiplier), eax);
    masm.umull(lhs);
    if (rmc.multiplier > UINT32_MAX) {
        // M >= 2^32 and shift == 0 is impossible, as d >= 2 implies that
        // ((M * n) >> (32 + shift)) >= n > floor(n/d) whenever n >= d, contradicting
        // the proof of correctness in computeDivisionConstants.
        MOZ_ASSERT(rmc.shiftAmount > 0);
        MOZ_ASSERT(rmc.multiplier < (int64_t(1) << 33));

        // We actually computed edx = ((uint32_t(M) * n) >> 32) instead. Since
        // (M * n) >> (32 + shift) is the same as (edx + n) >> shift, we can
        // correct for the overflow. This case is a bit trickier than the signed
        // case, though, as the (edx + n) addition itself can overflow; however,
        // note that (edx + n) >> shift == (((n - edx) >> 1) + edx) >> (shift - 1),
        // which is overflow-free. See Hacker's Delight, section 10-8 for details.

        // Compute (n - edx) >> 1 into eax.
        masm.movl(lhs, eax);
        masm.subl(edx, eax);
        masm.shrl(Imm32(1), eax);

        // Finish the computation.
        masm.addl(eax, edx);
        masm.shrl(Imm32(rmc.shiftAmount - 1), edx);
    } else {
        masm.shrl(Imm32(rmc.shiftAmount), edx);
    }

    // We now have the truncated division value in edx. If we're
    // computing a modulus or checking whether the division resulted
    // in an integer, we need to multiply the obtained value by d and
    // finish the computation/check.
    if (!isDiv) {
        masm.imull(Imm32(d), edx, edx);
        masm.movl(lhs, eax);
        masm.subl(edx, eax);

        // The final result of the modulus op, just computed above by the
        // sub instruction, can be a number in the range [2^31, 2^32). If
        // this is the case and the modulus is not truncated, we must bail
        // out.
        if (!ins->mir()->isTruncated())
            bailoutIf(Assembler::Signed, ins->snapshot());
    } else if (!ins->mir()->isTruncated()) {
        masm.imull(Imm32(d), edx, eax);
        masm.cmpl(lhs, eax);
        bailoutIf(Assembler::NotEqual, ins->snapshot());
    }
}

void
CodeGeneratorX86Shared::visitMulNegativeZeroCheck(MulNegativeZeroCheck* ool)
{
    LMulI* ins = ool->ins();
    Register result = ToRegister(ins->output());
    Operand lhsCopy = ToOperand(ins->lhsCopy());
    Operand rhs = ToOperand(ins->rhs());
    MOZ_ASSERT_IF(lhsCopy.kind() == Operand::REG, lhsCopy.reg() != result.code());

    // Result is -0 if lhs or rhs is negative.
    masm.movl(lhsCopy, result);
    masm.orl(rhs, result);
    bailoutIf(Assembler::Signed, ins->snapshot());

    masm.mov(ImmWord(0), result);
    masm.jmp(ool->rejoin());
}

void
CodeGeneratorX86Shared::visitDivPowTwoI(LDivPowTwoI* ins)
{
    Register lhs = ToRegister(ins->numerator());
    DebugOnly<Register> output = ToRegister(ins->output());

    int32_t shift = ins->shift();
    bool negativeDivisor = ins->negativeDivisor();
    MDiv* mir = ins->mir();

    // We use defineReuseInput so these should always be the same, which is
    // convenient since all of our instructions here are two-address.
    MOZ_ASSERT(lhs == output);

    if (!mir->isTruncated() && negativeDivisor) {
        // 0 divided by a negative number must return a double.
        masm.test32(lhs, lhs);
        bailoutIf(Assembler::Zero, ins->snapshot());
    }

    if (shift) {
        if (!mir->isTruncated()) {
            // If the remainder is != 0, bailout since this must be a double.
            masm.test32(lhs, Imm32(UINT32_MAX >> (32 - shift)));
            bailoutIf(Assembler::NonZero, ins->snapshot());
        }

        if (mir->isUnsigned()) {
            masm.shrl(Imm32(shift), lhs);
        } else {
            // Adjust the value so that shifting produces a correctly
            // rounded result when the numerator is negative. See 10-1
            // "Signed Division by a Known Power of 2" in Henry
            // S. Warren, Jr.'s Hacker's Delight.
            if (mir->canBeNegativeDividend()) {
                Register lhsCopy = ToRegister(ins->numeratorCopy());
                MOZ_ASSERT(lhsCopy != lhs);
                if (shift > 1)
                    masm.sarl(Imm32(31), lhs);
                masm.shrl(Imm32(32 - shift), lhs);
                masm.addl(lhsCopy, lhs);
            }
            masm.sarl(Imm32(shift), lhs);

            if (negativeDivisor)
                masm.negl(lhs);
        }
        return;
    }

    if (negativeDivisor) {
        // INT32_MIN / -1 overflows.
        masm.negl(lhs);
        if (!mir->isTruncated()) {
            bailoutIf(Assembler::Overflow, ins->snapshot());
        } else if (mir->trapOnError()) {
            Label ok;
            masm.j(Assembler::NoOverflow, &ok);
            masm.wasmTrap(wasm::Trap::IntegerOverflow, mir->bytecodeOffset());
            masm.bind(&ok);
        }
    } else if (mir->isUnsigned() && !mir->isTruncated()) {
        // Unsigned division by 1 can overflow if output is not
        // truncated.
        masm.test32(lhs, lhs);
        bailoutIf(Assembler::Signed, ins->snapshot());
    }
}

void
CodeGeneratorX86Shared::visitDivOrModConstantI(LDivOrModConstantI* ins) {
    Register lhs = ToRegister(ins->numerator());
    Register output = ToRegister(ins->output());
    int32_t d = ins->denominator();

    // This emits the division answer into edx or the modulus answer into eax.
    MOZ_ASSERT(output == eax || output == edx);
    MOZ_ASSERT(lhs != eax && lhs != edx);
    bool isDiv = (output == edx);

    // The absolute value of the denominator isn't a power of 2 (see LDivPowTwoI
    // and LModPowTwoI).
    MOZ_ASSERT((Abs(d) & (Abs(d) - 1)) != 0);

    // We will first divide by Abs(d), and negate the answer if d is negative.
    // If desired, this can be avoided by generalizing computeDivisionConstants.
    ReciprocalMulConstants rmc = computeDivisionConstants(Abs(d), /* maxLog = */ 31);

    // We first compute (M * n) >> 32, where M = rmc.multiplier.
    masm.movl(Imm32(rmc.multiplier), eax);
    masm.imull(lhs);
    if (rmc.multiplier > INT32_MAX) {
        MOZ_ASSERT(rmc.multiplier < (int64_t(1) << 32));

        // We actually computed edx = ((int32_t(M) * n) >> 32) instead. Since
        // (M * n) >> 32 is the same as (edx + n), we can correct for the overflow.
        // (edx + n) can't overflow, as n and edx have opposite signs because int32_t(M)
        // is negative.
        masm.addl(lhs, edx);
    }
    // (M * n) >> (32 + shift) is the truncated division answer if n is non-negative,
    // as proved in the comments of computeDivisionConstants. We must add 1 later if n is
    // negative to get the right answer in all cases.
    masm.sarl(Imm32(rmc.shiftAmount), edx);

    // We'll subtract -1 instead of adding 1, because (n < 0 ? -1 : 0) can be
    // computed with just a sign-extending shift of 31 bits.
    if (ins->canBeNegativeDividend()) {
        masm.movl(lhs, eax);
        masm.sarl(Imm32(31), eax);
        masm.subl(eax, edx);
    }

    // After this, edx contains the correct truncated division result.
    if (d < 0)
        masm.negl(edx);

    if (!isDiv) {
        masm.imull(Imm32(-d), edx, eax);
        masm.addl(lhs, eax);
    }

    if (!ins->mir()->isTruncated()) {
        if (isDiv) {
            // This is a division op. Multiply the obtained value by d to check if
            // the correct answer is an integer. This cannot overflow, since |d| > 1.
            masm.imull(Imm32(d), edx, eax);
            masm.cmp32(lhs, eax);
            bailoutIf(Assembler::NotEqual, ins->snapshot());

            // If lhs is zero and the divisor is negative, the answer should have
            // been -0.
            if (d < 0) {
                masm.test32(lhs, lhs);
                bailoutIf(Assembler::Zero, ins->snapshot());
            }
        } else if (ins->canBeNegativeDividend()) {
            // This is a mod op. If the computed value is zero and lhs
            // is negative, the answer should have been -0.
            Label done;

            masm.cmp32(lhs, Imm32(0));
            masm.j(Assembler::GreaterThanOrEqual, &done);

            masm.test32(eax, eax);
            bailoutIf(Assembler::Zero, ins->snapshot());

            masm.bind(&done);
        }
    }
}

void
CodeGeneratorX86Shared::visitDivI(LDivI* ins)
{
    Register remainder = ToRegister(ins->remainder());
    Register lhs = ToRegister(ins->lhs());
    Register rhs = ToRegister(ins->rhs());
    Register output = ToRegister(ins->output());

    MDiv* mir = ins->mir();

    MOZ_ASSERT_IF(lhs != rhs, rhs != eax);
    MOZ_ASSERT(rhs != edx);
    MOZ_ASSERT(remainder == edx);
    MOZ_ASSERT(output == eax);

    Label done;
    ReturnZero* ool = nullptr;

    // Put the lhs in eax, for either the negative overflow case or the regular
    // divide case.
    if (lhs != eax)
        masm.mov(lhs, eax);

    // Handle divide by zero.
    if (mir->canBeDivideByZero()) {
        masm.test32(rhs, rhs);
        if (mir->trapOnError()) {
            Label nonZero;
            masm.j(Assembler::NonZero, &nonZero);
            masm.wasmTrap(wasm::Trap::IntegerDivideByZero, mir->bytecodeOffset());
            masm.bind(&nonZero);
        } else if (mir->canTruncateInfinities()) {
            // Truncated division by zero is zero (Infinity|0 == 0)
            if (!ool)
                ool = new(alloc()) ReturnZero(output);
            masm.j(Assembler::Zero, ool->entry());
        } else {
            MOZ_ASSERT(mir->fallible());
            bailoutIf(Assembler::Zero, ins->snapshot());
        }
    }

    // Handle an integer overflow exception from -2147483648 / -1.
    if (mir->canBeNegativeOverflow()) {
        Label notOverflow;
        masm.cmp32(lhs, Imm32(INT32_MIN));
        masm.j(Assembler::NotEqual, &notOverflow);
        masm.cmp32(rhs, Imm32(-1));
        if (mir->trapOnError()) {
            masm.j(Assembler::NotEqual, &notOverflow);
            masm.wasmTrap(wasm::Trap::IntegerOverflow, mir->bytecodeOffset());
        } else if (mir->canTruncateOverflow()) {
            // (-INT32_MIN)|0 == INT32_MIN and INT32_MIN is already in the
            // output register (lhs == eax).
            masm.j(Assembler::Equal, &done);
        } else {
            MOZ_ASSERT(mir->fallible());
            bailoutIf(Assembler::Equal, ins->snapshot());
        }
        masm.bind(&notOverflow);
    }

    // Handle negative 0.
    if (!mir->canTruncateNegativeZero() && mir->canBeNegativeZero()) {
        Label nonzero;
        masm.test32(lhs, lhs);
        masm.j(Assembler::NonZero, &nonzero);
        masm.cmp32(rhs, Imm32(0));
        bailoutIf(Assembler::LessThan, ins->snapshot());
        masm.bind(&nonzero);
    }

    // Sign extend the lhs into edx to make (edx:eax), since idiv is 64-bit.
    if (lhs != eax)
        masm.mov(lhs, eax);
    masm.cdq();
    masm.idiv(rhs);

    if (!mir->canTruncateRemainder()) {
        // If the remainder is > 0, bailout since this must be a double.
        masm.test32(remainder, remainder);
        bailoutIf(Assembler::NonZero, ins->snapshot());
    }

    masm.bind(&done);

    if (ool) {
        addOutOfLineCode(ool, mir);
        masm.bind(ool->rejoin());
    }
}

void
CodeGeneratorX86Shared::visitModPowTwoI(LModPowTwoI* ins)
{
    Register lhs = ToRegister(ins->getOperand(0));
    int32_t shift = ins->shift();

    Label negative;

    if (!ins->mir()->isUnsigned() && ins->mir()->canBeNegativeDividend()) {
        // Switch based on sign of the lhs.
        // Positive numbers are just a bitmask
        masm.branchTest32(Assembler::Signed, lhs, lhs, &negative);
    }

    masm.andl(Imm32((uint32_t(1) << shift) - 1), lhs);

    if (!ins->mir()->isUnsigned() && ins->mir()->canBeNegativeDividend()) {
        Label done;
        masm.jump(&done);

        // Negative numbers need a negate, bitmask, negate
        masm.bind(&negative);

        // Unlike in the visitModI case, we are not computing the mod by means of a
        // division. Therefore, the divisor = -1 case isn't problematic (the andl
        // always returns 0, which is what we expect).
        //
        // The negl instruction overflows if lhs == INT32_MIN, but this is also not
        // a problem: shift is at most 31, and so the andl also always returns 0.
        masm.negl(lhs);
        masm.andl(Imm32((uint32_t(1) << shift) - 1), lhs);
        masm.negl(lhs);

        // Since a%b has the same sign as b, and a is negative in this branch,
        // an answer of 0 means the correct result is actually -0. Bail out.
        if (!ins->mir()->isTruncated())
            bailoutIf(Assembler::Zero, ins->snapshot());
        masm.bind(&done);
    }
}

class ModOverflowCheck : public OutOfLineCodeBase<CodeGeneratorX86Shared>
{
    Label done_;
    LModI* ins_;
    Register rhs_;

  public:
    explicit ModOverflowCheck(LModI* ins, Register rhs)
      : ins_(ins), rhs_(rhs)
    { }

    virtual void accept(CodeGeneratorX86Shared* codegen) override {
        codegen->visitModOverflowCheck(this);
    }
    Label* done() {
        return &done_;
    }
    LModI* ins() const {
        return ins_;
    }
    Register rhs() const {
        return rhs_;
    }
};

void
CodeGeneratorX86Shared::visitModOverflowCheck(ModOverflowCheck* ool)
{
    masm.cmp32(ool->rhs(), Imm32(-1));
    if (ool->ins()->mir()->isTruncated()) {
        masm.j(Assembler::NotEqual, ool->rejoin());
        masm.mov(ImmWord(0), edx);
        masm.jmp(ool->done());
    } else {
        bailoutIf(Assembler::Equal, ool->ins()->snapshot());
        masm.jmp(ool->rejoin());
    }
}

void
CodeGeneratorX86Shared::visitModI(LModI* ins)
{
    Register remainder = ToRegister(ins->remainder());
    Register lhs = ToRegister(ins->lhs());
    Register rhs = ToRegister(ins->rhs());

    // Required to use idiv.
    MOZ_ASSERT_IF(lhs != rhs, rhs != eax);
    MOZ_ASSERT(rhs != edx);
    MOZ_ASSERT(remainder == edx);
    MOZ_ASSERT(ToRegister(ins->getTemp(0)) == eax);

    Label done;
    ReturnZero* ool = nullptr;
    ModOverflowCheck* overflow = nullptr;

    // Set up eax in preparation for doing a div.
    if (lhs != eax)
        masm.mov(lhs, eax);

    MMod* mir = ins->mir();

    // Prevent divide by zero.
    if (mir->canBeDivideByZero()) {
        masm.test32(rhs, rhs);
        if (mir->isTruncated()) {
            if (mir->trapOnError()) {
                Label nonZero;
                masm.j(Assembler::NonZero, &nonZero);
                masm.wasmTrap(wasm::Trap::IntegerDivideByZero, mir->bytecodeOffset());
                masm.bind(&nonZero);
            } else {
                if (!ool)
                    ool = new(alloc()) ReturnZero(edx);
                masm.j(Assembler::Zero, ool->entry());
            }
        } else {
            bailoutIf(Assembler::Zero, ins->snapshot());
        }
    }

    Label negative;

    // Switch based on sign of the lhs.
    if (mir->canBeNegativeDividend())
        masm.branchTest32(Assembler::Signed, lhs, lhs, &negative);

    // If lhs >= 0 then remainder = lhs % rhs. The remainder must be positive.
    {
        // Check if rhs is a power-of-two.
        if (mir->canBePowerOfTwoDivisor()) {
            MOZ_ASSERT(rhs != remainder);

            // Rhs y is a power-of-two if (y & (y-1)) == 0. Note that if
            // y is any negative number other than INT32_MIN, both y and
            // y-1 will have the sign bit set so these are never optimized
            // as powers-of-two. If y is INT32_MIN, y-1 will be INT32_MAX
            // and because lhs >= 0 at this point, lhs & INT32_MAX returns
            // the correct value.
            Label notPowerOfTwo;
            masm.mov(rhs, remainder);
            masm.subl(Imm32(1), remainder);
            masm.branchTest32(Assembler::NonZero, remainder, rhs, &notPowerOfTwo);
            {
                masm.andl(lhs, remainder);
                masm.jmp(&done);
            }
            masm.bind(&notPowerOfTwo);
        }

        // Since lhs >= 0, the sign-extension will be 0
        masm.mov(ImmWord(0), edx);
        masm.idiv(rhs);
    }

    // Otherwise, we have to beware of two special cases:
    if (mir->canBeNegativeDividend()) {
        masm.jump(&done);

        masm.bind(&negative);

        // Prevent an integer overflow exception from -2147483648 % -1
        Label notmin;
        masm.cmp32(lhs, Imm32(INT32_MIN));
        overflow = new(alloc()) ModOverflowCheck(ins, rhs);
        masm.j(Assembler::Equal, overflow->entry());
        masm.bind(overflow->rejoin());
        masm.cdq();
        masm.idiv(rhs);

        if (!mir->isTruncated()) {
            // A remainder of 0 means that the rval must be -0, which is a double.
            masm.test32(remainder, remainder);
            bailoutIf(Assembler::Zero, ins->snapshot());
        }
    }

    masm.bind(&done);

    if (overflow) {
        addOutOfLineCode(overflow, mir);
        masm.bind(overflow->done());
    }

    if (ool) {
        addOutOfLineCode(ool, mir);
        masm.bind(ool->rejoin());
    }
}

void
CodeGeneratorX86Shared::visitBitNotI(LBitNotI* ins)
{
    const LAllocation* input = ins->getOperand(0);
    MOZ_ASSERT(!input->isConstant());

    masm.notl(ToOperand(input));
}

void
CodeGeneratorX86Shared::visitBitOpI(LBitOpI* ins)
{
    const LAllocation* lhs = ins->getOperand(0);
    const LAllocation* rhs = ins->getOperand(1);

    switch (ins->bitop()) {
        case JSOP_BITOR:
            if (rhs->isConstant())
                masm.orl(Imm32(ToInt32(rhs)), ToOperand(lhs));
            else
                masm.orl(ToOperand(rhs), ToRegister(lhs));
            break;
        case JSOP_BITXOR:
            if (rhs->isConstant())
                masm.xorl(Imm32(ToInt32(rhs)), ToOperand(lhs));
            else
                masm.xorl(ToOperand(rhs), ToRegister(lhs));
            break;
        case JSOP_BITAND:
            if (rhs->isConstant())
                masm.andl(Imm32(ToInt32(rhs)), ToOperand(lhs));
            else
                masm.andl(ToOperand(rhs), ToRegister(lhs));
            break;
        default:
            MOZ_CRASH("unexpected binary opcode");
    }
}

void
CodeGeneratorX86Shared::visitBitOpI64(LBitOpI64* lir)
{
    const LInt64Allocation lhs = lir->getInt64Operand(LBitOpI64::Lhs);
    const LInt64Allocation rhs = lir->getInt64Operand(LBitOpI64::Rhs);

    MOZ_ASSERT(ToOutRegister64(lir) == ToRegister64(lhs));

    switch (lir->bitop()) {
      case JSOP_BITOR:
        if (IsConstant(rhs))
            masm.or64(Imm64(ToInt64(rhs)), ToRegister64(lhs));
        else
            masm.or64(ToOperandOrRegister64(rhs), ToRegister64(lhs));
        break;
      case JSOP_BITXOR:
        if (IsConstant(rhs))
            masm.xor64(Imm64(ToInt64(rhs)), ToRegister64(lhs));
        else
            masm.xor64(ToOperandOrRegister64(rhs), ToRegister64(lhs));
        break;
      case JSOP_BITAND:
        if (IsConstant(rhs))
            masm.and64(Imm64(ToInt64(rhs)), ToRegister64(lhs));
        else
            masm.and64(ToOperandOrRegister64(rhs), ToRegister64(lhs));
        break;
      default:
        MOZ_CRASH("unexpected binary opcode");
    }
}

void
CodeGeneratorX86Shared::visitShiftI(LShiftI* ins)
{
    Register lhs = ToRegister(ins->lhs());
    const LAllocation* rhs = ins->rhs();

    if (rhs->isConstant()) {
        int32_t shift = ToInt32(rhs) & 0x1F;
        switch (ins->bitop()) {
          case JSOP_LSH:
            if (shift)
                masm.shll(Imm32(shift), lhs);
            break;
          case JSOP_RSH:
            if (shift)
                masm.sarl(Imm32(shift), lhs);
            break;
          case JSOP_URSH:
            if (shift) {
                masm.shrl(Imm32(shift), lhs);
            } else if (ins->mir()->toUrsh()->fallible()) {
                // x >>> 0 can overflow.
                masm.test32(lhs, lhs);
                bailoutIf(Assembler::Signed, ins->snapshot());
            }
            break;
          default:
            MOZ_CRASH("Unexpected shift op");
        }
    } else {
        MOZ_ASSERT(ToRegister(rhs) == ecx);
        switch (ins->bitop()) {
          case JSOP_LSH:
            masm.shll_cl(lhs);
            break;
          case JSOP_RSH:
            masm.sarl_cl(lhs);
            break;
          case JSOP_URSH:
            masm.shrl_cl(lhs);
            if (ins->mir()->toUrsh()->fallible()) {
                // x >>> 0 can overflow.
                masm.test32(lhs, lhs);
                bailoutIf(Assembler::Signed, ins->snapshot());
            }
            break;
          default:
            MOZ_CRASH("Unexpected shift op");
        }
    }
}

void
CodeGeneratorX86Shared::visitShiftI64(LShiftI64* lir)
{
    const LInt64Allocation lhs = lir->getInt64Operand(LShiftI64::Lhs);
    LAllocation* rhs = lir->getOperand(LShiftI64::Rhs);

    MOZ_ASSERT(ToOutRegister64(lir) == ToRegister64(lhs));

    if (rhs->isConstant()) {
        int32_t shift = int32_t(rhs->toConstant()->toInt64() & 0x3F);
        switch (lir->bitop()) {
          case JSOP_LSH:
            if (shift)
                masm.lshift64(Imm32(shift), ToRegister64(lhs));
            break;
          case JSOP_RSH:
            if (shift)
                masm.rshift64Arithmetic(Imm32(shift), ToRegister64(lhs));
            break;
          case JSOP_URSH:
            if (shift)
                masm.rshift64(Imm32(shift), ToRegister64(lhs));
            break;
          default:
            MOZ_CRASH("Unexpected shift op");
        }
        return;
    }

    MOZ_ASSERT(ToRegister(rhs) == ecx);
    switch (lir->bitop()) {
      case JSOP_LSH:
        masm.lshift64(ecx, ToRegister64(lhs));
        break;
      case JSOP_RSH:
        masm.rshift64Arithmetic(ecx, ToRegister64(lhs));
        break;
      case JSOP_URSH:
        masm.rshift64(ecx, ToRegister64(lhs));
        break;
      default:
        MOZ_CRASH("Unexpected shift op");
    }
}

void
CodeGeneratorX86Shared::visitUrshD(LUrshD* ins)
{
    Register lhs = ToRegister(ins->lhs());
    MOZ_ASSERT(ToRegister(ins->temp()) == lhs);

    const LAllocation* rhs = ins->rhs();
    FloatRegister out = ToFloatRegister(ins->output());

    if (rhs->isConstant()) {
        int32_t shift = ToInt32(rhs) & 0x1F;
        if (shift)
            masm.shrl(Imm32(shift), lhs);
    } else {
        MOZ_ASSERT(ToRegister(rhs) == ecx);
        masm.shrl_cl(lhs);
    }

    masm.convertUInt32ToDouble(lhs, out);
}

Operand
CodeGeneratorX86Shared::ToOperand(const LAllocation& a)
{
    if (a.isGeneralReg())
        return Operand(a.toGeneralReg()->reg());
    if (a.isFloatReg())
        return Operand(a.toFloatReg()->reg());
    return Operand(masm.getStackPointer(), ToStackOffset(&a));
}

Operand
CodeGeneratorX86Shared::ToOperand(const LAllocation* a)
{
    return ToOperand(*a);
}

Operand
CodeGeneratorX86Shared::ToOperand(const LDefinition* def)
{
    return ToOperand(def->output());
}

MoveOperand
CodeGeneratorX86Shared::toMoveOperand(LAllocation a) const
{
    if (a.isGeneralReg())
        return MoveOperand(ToRegister(a));
    if (a.isFloatReg())
        return MoveOperand(ToFloatRegister(a));
    return MoveOperand(StackPointer, ToStackOffset(a));
}

class OutOfLineTableSwitch : public OutOfLineCodeBase<CodeGeneratorX86Shared>
{
    MTableSwitch* mir_;
    CodeLabel jumpLabel_;

    void accept(CodeGeneratorX86Shared* codegen) override {
        codegen->visitOutOfLineTableSwitch(this);
    }

  public:
    explicit OutOfLineTableSwitch(MTableSwitch* mir)
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
CodeGeneratorX86Shared::visitOutOfLineTableSwitch(OutOfLineTableSwitch* ool)
{
    MTableSwitch* mir = ool->mir();

    masm.haltingAlign(sizeof(void*));
    masm.bind(ool->jumpLabel());
    masm.addCodeLabel(*ool->jumpLabel());

    for (size_t i = 0; i < mir->numCases(); i++) {
        LBlock* caseblock = skipTrivialBlocks(mir->getCase(i))->lir();
        Label* caseheader = caseblock->label();
        uint32_t caseoffset = caseheader->offset();

        // The entries of the jump table need to be absolute addresses and thus
        // must be patched after codegen is finished.
        CodeLabel cl;
        masm.writeCodePointer(&cl);
        cl.target()->bind(caseoffset);
        masm.addCodeLabel(cl);
    }
}

void
CodeGeneratorX86Shared::emitTableSwitchDispatch(MTableSwitch* mir, Register index, Register base)
{
    Label* defaultcase = skipTrivialBlocks(mir->getDefault())->lir()->label();

    // Lower value with low value
    if (mir->low() != 0)
        masm.subl(Imm32(mir->low()), index);

    // Jump to default case if input is out of range
    int32_t cases = mir->numCases();
    masm.cmp32(index, Imm32(cases));
    masm.j(AssemblerX86Shared::AboveOrEqual, defaultcase);

    // To fill in the CodeLabels for the case entries, we need to first
    // generate the case entries (we don't yet know their offsets in the
    // instruction stream).
    OutOfLineTableSwitch* ool = new(alloc()) OutOfLineTableSwitch(mir);
    addOutOfLineCode(ool, mir);

    // Compute the position where a pointer to the right case stands.
    masm.mov(ool->jumpLabel(), base);
    BaseIndex pointer(base, index, ScalePointer);

    // Jump to the right case
    masm.branchToComputedAddress(pointer);
}

void
CodeGeneratorX86Shared::visitMathD(LMathD* math)
{
    FloatRegister lhs = ToFloatRegister(math->lhs());
    Operand rhs = ToOperand(math->rhs());
    FloatRegister output = ToFloatRegister(math->output());

    switch (math->jsop()) {
      case JSOP_ADD:
        masm.vaddsd(rhs, lhs, output);
        break;
      case JSOP_SUB:
        masm.vsubsd(rhs, lhs, output);
        break;
      case JSOP_MUL:
        masm.vmulsd(rhs, lhs, output);
        break;
      case JSOP_DIV:
        masm.vdivsd(rhs, lhs, output);
        break;
      default:
        MOZ_CRASH("unexpected opcode");
    }
}

void
CodeGeneratorX86Shared::visitMathF(LMathF* math)
{
    FloatRegister lhs = ToFloatRegister(math->lhs());
    Operand rhs = ToOperand(math->rhs());
    FloatRegister output = ToFloatRegister(math->output());

    switch (math->jsop()) {
      case JSOP_ADD:
        masm.vaddss(rhs, lhs, output);
        break;
      case JSOP_SUB:
        masm.vsubss(rhs, lhs, output);
        break;
      case JSOP_MUL:
        masm.vmulss(rhs, lhs, output);
        break;
      case JSOP_DIV:
        masm.vdivss(rhs, lhs, output);
        break;
      default:
        MOZ_CRASH("unexpected opcode");
    }
}

void
CodeGeneratorX86Shared::visitFloor(LFloor* lir)
{
    FloatRegister input = ToFloatRegister(lir->input());
    Register output = ToRegister(lir->output());

    Label bailout;

    if (AssemblerX86Shared::HasSSE41()) {
        // Bail on negative-zero.
        masm.branchNegativeZero(input, output, &bailout);
        bailoutFrom(&bailout, lir->snapshot());

        // Round toward -Infinity.
        {
            ScratchDoubleScope scratch(masm);
            masm.vroundsd(X86Encoding::RoundDown, input, scratch, scratch);
            bailoutCvttsd2si(scratch, output, lir->snapshot());
        }
    } else {
        Label negative, end;

        // Branch to a slow path for negative inputs. Doesn't catch NaN or -0.
        {
            ScratchDoubleScope scratch(masm);
            masm.zeroDouble(scratch);
            masm.branchDouble(Assembler::DoubleLessThan, input, scratch, &negative);
        }

        // Bail on negative-zero.
        masm.branchNegativeZero(input, output, &bailout);
        bailoutFrom(&bailout, lir->snapshot());

        // Input is non-negative, so truncation correctly rounds.
        bailoutCvttsd2si(input, output, lir->snapshot());

        masm.jump(&end);

        // Input is negative, but isn't -0.
        // Negative values go on a comparatively expensive path, since no
        // native rounding mode matches JS semantics. Still better than callVM.
        masm.bind(&negative);
        {
            // Truncate and round toward zero.
            // This is off-by-one for everything but integer-valued inputs.
            bailoutCvttsd2si(input, output, lir->snapshot());

            // Test whether the input double was integer-valued.
            {
                ScratchDoubleScope scratch(masm);
                masm.convertInt32ToDouble(output, scratch);
                masm.branchDouble(Assembler::DoubleEqualOrUnordered, input, scratch, &end);
            }

            // Input is not integer-valued, so we rounded off-by-one in the
            // wrong direction. Correct by subtraction.
            masm.subl(Imm32(1), output);
            // Cannot overflow: output was already checked against INT_MIN.
        }

        masm.bind(&end);
    }
}

void
CodeGeneratorX86Shared::visitFloorF(LFloorF* lir)
{
    FloatRegister input = ToFloatRegister(lir->input());
    Register output = ToRegister(lir->output());

    Label bailout;

    if (AssemblerX86Shared::HasSSE41()) {
        // Bail on negative-zero.
        masm.branchNegativeZeroFloat32(input, output, &bailout);
        bailoutFrom(&bailout, lir->snapshot());

        // Round toward -Infinity.
        {
            ScratchFloat32Scope scratch(masm);
            masm.vroundss(X86Encoding::RoundDown, input, scratch, scratch);
            bailoutCvttss2si(scratch, output, lir->snapshot());
        }
    } else {
        Label negative, end;

        // Branch to a slow path for negative inputs. Doesn't catch NaN or -0.
        {
            ScratchFloat32Scope scratch(masm);
            masm.zeroFloat32(scratch);
            masm.branchFloat(Assembler::DoubleLessThan, input, scratch, &negative);
        }

        // Bail on negative-zero.
        masm.branchNegativeZeroFloat32(input, output, &bailout);
        bailoutFrom(&bailout, lir->snapshot());

        // Input is non-negative, so truncation correctly rounds.
        bailoutCvttss2si(input, output, lir->snapshot());

        masm.jump(&end);

        // Input is negative, but isn't -0.
        // Negative values go on a comparatively expensive path, since no
        // native rounding mode matches JS semantics. Still better than callVM.
        masm.bind(&negative);
        {
            // Truncate and round toward zero.
            // This is off-by-one for everything but integer-valued inputs.
            bailoutCvttss2si(input, output, lir->snapshot());

            // Test whether the input double was integer-valued.
            {
                ScratchFloat32Scope scratch(masm);
                masm.convertInt32ToFloat32(output, scratch);
                masm.branchFloat(Assembler::DoubleEqualOrUnordered, input, scratch, &end);
            }

            // Input is not integer-valued, so we rounded off-by-one in the
            // wrong direction. Correct by subtraction.
            masm.subl(Imm32(1), output);
            // Cannot overflow: output was already checked against INT_MIN.
        }

        masm.bind(&end);
    }
}

void
CodeGeneratorX86Shared::visitCeil(LCeil* lir)
{
    FloatRegister input = ToFloatRegister(lir->input());
    ScratchDoubleScope scratch(masm);
    Register output = ToRegister(lir->output());

    Label bailout, lessThanMinusOne;

    // Bail on ]-1; -0] range
    masm.loadConstantDouble(-1, scratch);
    masm.branchDouble(Assembler::DoubleLessThanOrEqualOrUnordered, input,
                      scratch, &lessThanMinusOne);

    // Test for remaining values with the sign bit set, i.e. ]-1; -0]
    masm.vmovmskpd(input, output);
    masm.branchTest32(Assembler::NonZero, output, Imm32(1), &bailout);
    bailoutFrom(&bailout, lir->snapshot());

    if (AssemblerX86Shared::HasSSE41()) {
        // x <= -1 or x > -0
        masm.bind(&lessThanMinusOne);
        // Round toward +Infinity.
        masm.vroundsd(X86Encoding::RoundUp, input, scratch, scratch);
        bailoutCvttsd2si(scratch, output, lir->snapshot());
        return;
    }

    // No SSE4.1
    Label end;

    // x >= 0 and x is not -0.0, we can truncate (resp. truncate and add 1) for
    // integer (resp. non-integer) values.
    // Will also work for values >= INT_MAX + 1, as the truncate
    // operation will return INT_MIN and there'll be a bailout.
    bailoutCvttsd2si(input, output, lir->snapshot());
    masm.convertInt32ToDouble(output, scratch);
    masm.branchDouble(Assembler::DoubleEqualOrUnordered, input, scratch, &end);

    // Input is not integer-valued, add 1 to obtain the ceiling value
    masm.addl(Imm32(1), output);
    // if input > INT_MAX, output == INT_MAX so adding 1 will overflow.
    bailoutIf(Assembler::Overflow, lir->snapshot());
    masm.jump(&end);

    // x <= -1, truncation is the way to go.
    masm.bind(&lessThanMinusOne);
    bailoutCvttsd2si(input, output, lir->snapshot());

    masm.bind(&end);
}

void
CodeGeneratorX86Shared::visitCeilF(LCeilF* lir)
{
    FloatRegister input = ToFloatRegister(lir->input());
    ScratchFloat32Scope scratch(masm);
    Register output = ToRegister(lir->output());

    Label bailout, lessThanMinusOne;

    // Bail on ]-1; -0] range
    masm.loadConstantFloat32(-1.f, scratch);
    masm.branchFloat(Assembler::DoubleLessThanOrEqualOrUnordered, input,
                     scratch, &lessThanMinusOne);

    // Test for remaining values with the sign bit set, i.e. ]-1; -0]
    masm.vmovmskps(input, output);
    masm.branchTest32(Assembler::NonZero, output, Imm32(1), &bailout);
    bailoutFrom(&bailout, lir->snapshot());

    if (AssemblerX86Shared::HasSSE41()) {
        // x <= -1 or x > -0
        masm.bind(&lessThanMinusOne);
        // Round toward +Infinity.
        masm.vroundss(X86Encoding::RoundUp, input, scratch, scratch);
        bailoutCvttss2si(scratch, output, lir->snapshot());
        return;
    }

    // No SSE4.1
    Label end;

    // x >= 0 and x is not -0.0, we can truncate (resp. truncate and add 1) for
    // integer (resp. non-integer) values.
    // Will also work for values >= INT_MAX + 1, as the truncate
    // operation will return INT_MIN and there'll be a bailout.
    bailoutCvttss2si(input, output, lir->snapshot());
    masm.convertInt32ToFloat32(output, scratch);
    masm.branchFloat(Assembler::DoubleEqualOrUnordered, input, scratch, &end);

    // Input is not integer-valued, add 1 to obtain the ceiling value
    masm.addl(Imm32(1), output);
    // if input > INT_MAX, output == INT_MAX so adding 1 will overflow.
    bailoutIf(Assembler::Overflow, lir->snapshot());
    masm.jump(&end);

    // x <= -1, truncation is the way to go.
    masm.bind(&lessThanMinusOne);
    bailoutCvttss2si(input, output, lir->snapshot());

    masm.bind(&end);
}

void
CodeGeneratorX86Shared::visitRound(LRound* lir)
{
    FloatRegister input = ToFloatRegister(lir->input());
    FloatRegister temp = ToFloatRegister(lir->temp());
    ScratchDoubleScope scratch(masm);
    Register output = ToRegister(lir->output());

    Label negativeOrZero, negative, end, bailout;

    // Branch to a slow path for non-positive inputs. Doesn't catch NaN.
    masm.zeroDouble(scratch);
    masm.loadConstantDouble(GetBiggestNumberLessThan(0.5), temp);
    masm.branchDouble(Assembler::DoubleLessThanOrEqual, input, scratch, &negativeOrZero);

    // Input is positive. Add the biggest double less than 0.5 and
    // truncate, rounding down (because if the input is the biggest double less
    // than 0.5, adding 0.5 would undesirably round up to 1). Note that we have
    // to add the input to the temp register because we're not allowed to
    // modify the input register.
    masm.addDouble(input, temp);
    bailoutCvttsd2si(temp, output, lir->snapshot());

    masm.jump(&end);

    // Input is negative, +0 or -0.
    masm.bind(&negativeOrZero);
    // Branch on negative input.
    masm.j(Assembler::NotEqual, &negative);

    // Bail on negative-zero.
    masm.branchNegativeZero(input, output, &bailout, /* maybeNonZero = */ false);
    bailoutFrom(&bailout, lir->snapshot());

    // Input is +0
    masm.xor32(output, output);
    masm.jump(&end);

    // Input is negative.
    masm.bind(&negative);

    // Inputs in ]-0.5; 0] need to be added 0.5, other negative inputs need to
    // be added the biggest double less than 0.5.
    Label loadJoin;
    masm.loadConstantDouble(-0.5, scratch);
    masm.branchDouble(Assembler::DoubleLessThan, input, scratch, &loadJoin);
    masm.loadConstantDouble(0.5, temp);
    masm.bind(&loadJoin);

    if (AssemblerX86Shared::HasSSE41()) {
        // Add 0.5 and round toward -Infinity. The result is stored in the temp
        // register (currently contains 0.5).
        masm.addDouble(input, temp);
        masm.vroundsd(X86Encoding::RoundDown, temp, scratch, scratch);

        // Truncate.
        bailoutCvttsd2si(scratch, output, lir->snapshot());

        // If the result is positive zero, then the actual result is -0. Bail.
        // Otherwise, the truncation will have produced the correct negative integer.
        masm.test32(output, output);
        bailoutIf(Assembler::Zero, lir->snapshot());
    } else {
        masm.addDouble(input, temp);

        // Round toward -Infinity without the benefit of ROUNDSD.
        {
            // If input + 0.5 >= 0, input is a negative number >= -0.5 and the result is -0.
            masm.compareDouble(Assembler::DoubleGreaterThanOrEqual, temp, scratch);
            bailoutIf(Assembler::DoubleGreaterThanOrEqual, lir->snapshot());

            // Truncate and round toward zero.
            // This is off-by-one for everything but integer-valued inputs.
            bailoutCvttsd2si(temp, output, lir->snapshot());

            // Test whether the truncated double was integer-valued.
            masm.convertInt32ToDouble(output, scratch);
            masm.branchDouble(Assembler::DoubleEqualOrUnordered, temp, scratch, &end);

            // Input is not integer-valued, so we rounded off-by-one in the
            // wrong direction. Correct by subtraction.
            masm.subl(Imm32(1), output);
            // Cannot overflow: output was already checked against INT_MIN.
        }
    }

    masm.bind(&end);
}

void
CodeGeneratorX86Shared::visitRoundF(LRoundF* lir)
{
    FloatRegister input = ToFloatRegister(lir->input());
    FloatRegister temp = ToFloatRegister(lir->temp());
    ScratchFloat32Scope scratch(masm);
    Register output = ToRegister(lir->output());

    Label negativeOrZero, negative, end, bailout;

    // Branch to a slow path for non-positive inputs. Doesn't catch NaN.
    masm.zeroFloat32(scratch);
    masm.loadConstantFloat32(GetBiggestNumberLessThan(0.5f), temp);
    masm.branchFloat(Assembler::DoubleLessThanOrEqual, input, scratch, &negativeOrZero);

    // Input is non-negative. Add the biggest float less than 0.5 and truncate,
    // rounding down (because if the input is the biggest float less than 0.5,
    // adding 0.5 would undesirably round up to 1). Note that we have to add
    // the input to the temp register because we're not allowed to modify the
    // input register.
    masm.addFloat32(input, temp);

    bailoutCvttss2si(temp, output, lir->snapshot());

    masm.jump(&end);

    // Input is negative, +0 or -0.
    masm.bind(&negativeOrZero);
    // Branch on negative input.
    masm.j(Assembler::NotEqual, &negative);

    // Bail on negative-zero.
    masm.branchNegativeZeroFloat32(input, output, &bailout);
    bailoutFrom(&bailout, lir->snapshot());

    // Input is +0.
    masm.xor32(output, output);
    masm.jump(&end);

    // Input is negative.
    masm.bind(&negative);

    // Inputs in ]-0.5; 0] need to be added 0.5, other negative inputs need to
    // be added the biggest double less than 0.5.
    Label loadJoin;
    masm.loadConstantFloat32(-0.5f, scratch);
    masm.branchFloat(Assembler::DoubleLessThan, input, scratch, &loadJoin);
    masm.loadConstantFloat32(0.5f, temp);
    masm.bind(&loadJoin);

    if (AssemblerX86Shared::HasSSE41()) {
        // Add 0.5 and round toward -Infinity. The result is stored in the temp
        // register (currently contains 0.5).
        masm.addFloat32(input, temp);
        masm.vroundss(X86Encoding::RoundDown, temp, scratch, scratch);

        // Truncate.
        bailoutCvttss2si(scratch, output, lir->snapshot());

        // If the result is positive zero, then the actual result is -0. Bail.
        // Otherwise, the truncation will have produced the correct negative integer.
        masm.test32(output, output);
        bailoutIf(Assembler::Zero, lir->snapshot());
    } else {
        masm.addFloat32(input, temp);
        // Round toward -Infinity without the benefit of ROUNDSS.
        {
            // If input + 0.5 >= 0, input is a negative number >= -0.5 and the result is -0.
            masm.compareFloat(Assembler::DoubleGreaterThanOrEqual, temp, scratch);
            bailoutIf(Assembler::DoubleGreaterThanOrEqual, lir->snapshot());

            // Truncate and round toward zero.
            // This is off-by-one for everything but integer-valued inputs.
            bailoutCvttss2si(temp, output, lir->snapshot());

            // Test whether the truncated double was integer-valued.
            masm.convertInt32ToFloat32(output, scratch);
            masm.branchFloat(Assembler::DoubleEqualOrUnordered, temp, scratch, &end);

            // Input is not integer-valued, so we rounded off-by-one in the
            // wrong direction. Correct by subtraction.
            masm.subl(Imm32(1), output);
            // Cannot overflow: output was already checked against INT_MIN.
        }
    }

    masm.bind(&end);
}

void
CodeGeneratorX86Shared::visitNearbyInt(LNearbyInt* lir)
{
    FloatRegister input = ToFloatRegister(lir->input());
    FloatRegister output = ToFloatRegister(lir->output());

    RoundingMode roundingMode = lir->mir()->roundingMode();
    masm.vroundsd(Assembler::ToX86RoundingMode(roundingMode), input, output, output);
}

void
CodeGeneratorX86Shared::visitNearbyIntF(LNearbyIntF* lir)
{
    FloatRegister input = ToFloatRegister(lir->input());
    FloatRegister output = ToFloatRegister(lir->output());

    RoundingMode roundingMode = lir->mir()->roundingMode();
    masm.vroundss(Assembler::ToX86RoundingMode(roundingMode), input, output, output);
}

void
CodeGeneratorX86Shared::visitEffectiveAddress(LEffectiveAddress* ins)
{
    const MEffectiveAddress* mir = ins->mir();
    Register base = ToRegister(ins->base());
    Register index = ToRegister(ins->index());
    Register output = ToRegister(ins->output());
    masm.leal(Operand(base, index, mir->scale(), mir->displacement()), output);
}

void
CodeGeneratorX86Shared::generateInvalidateEpilogue()
{
    // Ensure that there is enough space in the buffer for the OsiPoint
    // patching to occur. Otherwise, we could overwrite the invalidation
    // epilogue.
    for (size_t i = 0; i < sizeof(void*); i += Assembler::NopSize())
        masm.nop();

    masm.bind(&invalidate_);

    // Push the Ion script onto the stack (when we determine what that pointer is).
    invalidateEpilogueData_ = masm.pushWithPatch(ImmWord(uintptr_t(-1)));

    TrampolinePtr thunk = gen->jitRuntime()->getInvalidationThunk();
    masm.call(thunk);

    // We should never reach this point in JIT code -- the invalidation thunk should
    // pop the invalidated JS frame and return directly to its caller.
    masm.assumeUnreachable("Should have returned directly to its caller instead of here.");
}

void
CodeGeneratorX86Shared::visitNegI(LNegI* ins)
{
    Register input = ToRegister(ins->input());
    MOZ_ASSERT(input == ToRegister(ins->output()));

    masm.neg32(input);
}

void
CodeGeneratorX86Shared::visitNegD(LNegD* ins)
{
    FloatRegister input = ToFloatRegister(ins->input());
    MOZ_ASSERT(input == ToFloatRegister(ins->output()));

    masm.negateDouble(input);
}

void
CodeGeneratorX86Shared::visitNegF(LNegF* ins)
{
    FloatRegister input = ToFloatRegister(ins->input());
    MOZ_ASSERT(input == ToFloatRegister(ins->output()));

    masm.negateFloat(input);
}

void
CodeGeneratorX86Shared::visitSimd128Int(LSimd128Int* ins)
{
    const LDefinition* out = ins->getDef(0);
    masm.loadConstantSimd128Int(ins->getValue(), ToFloatRegister(out));
}

void
CodeGeneratorX86Shared::visitSimd128Float(LSimd128Float* ins)
{
    const LDefinition* out = ins->getDef(0);
    masm.loadConstantSimd128Float(ins->getValue(), ToFloatRegister(out));
}

void
CodeGeneratorX86Shared::visitInt32x4ToFloat32x4(LInt32x4ToFloat32x4* ins)
{
    FloatRegister in = ToFloatRegister(ins->input());
    FloatRegister out = ToFloatRegister(ins->output());
    masm.convertInt32x4ToFloat32x4(in, out);
}

void
CodeGeneratorX86Shared::visitFloat32x4ToInt32x4(LFloat32x4ToInt32x4* ins)
{
    FloatRegister in = ToFloatRegister(ins->input());
    FloatRegister out = ToFloatRegister(ins->output());
    Register temp = ToRegister(ins->temp());

    masm.convertFloat32x4ToInt32x4(in, out);

    auto* ool = new(alloc()) OutOfLineSimdFloatToIntCheck(temp, in, ins,
                                                          ins->mir()->bytecodeOffset());
    addOutOfLineCode(ool, ins->mir());

    static const SimdConstant InvalidResult = SimdConstant::SplatX4(int32_t(-2147483648));

    ScratchSimd128Scope scratch(masm);
    masm.loadConstantSimd128Int(InvalidResult, scratch);
    masm.packedEqualInt32x4(Operand(out), scratch);
    // TODO (bug 1156228): If we have SSE4.1, we can use PTEST here instead of
    // the two following instructions.
    masm.vmovmskps(scratch, temp);
    masm.cmp32(temp, Imm32(0));
    masm.j(Assembler::NotEqual, ool->entry());

    masm.bind(ool->rejoin());
}

void
CodeGeneratorX86Shared::visitOutOfLineSimdFloatToIntCheck(OutOfLineSimdFloatToIntCheck *ool)
{
    static const SimdConstant Int32MaxX4 = SimdConstant::SplatX4(2147483647.f);
    static const SimdConstant Int32MinX4 = SimdConstant::SplatX4(-2147483648.f);

    Label onConversionError;

    FloatRegister input = ool->input();
    Register temp = ool->temp();

    ScratchSimd128Scope scratch(masm);
    masm.loadConstantSimd128Float(Int32MinX4, scratch);
    masm.vcmpleps(Operand(input), scratch, scratch);
    masm.vmovmskps(scratch, temp);
    masm.cmp32(temp, Imm32(15));
    masm.j(Assembler::NotEqual, &onConversionError);

    masm.loadConstantSimd128Float(Int32MaxX4, scratch);
    masm.vcmpleps(Operand(input), scratch, scratch);
    masm.vmovmskps(scratch, temp);
    masm.cmp32(temp, Imm32(0));
    masm.j(Assembler::NotEqual, &onConversionError);

    masm.jump(ool->rejoin());

    masm.bind(&onConversionError);
    if (gen->compilingWasm())
        masm.wasmTrap(wasm::Trap::ImpreciseSimdConversion, ool->bytecodeOffset());
    else
        bailout(ool->ins()->snapshot());
}

// Convert Float32x4 to Uint32x4.
//
// If any input lane value is out of range or NaN, bail out.
void
CodeGeneratorX86Shared::visitFloat32x4ToUint32x4(LFloat32x4ToUint32x4* ins)
{
    const MSimdConvert* mir = ins->mir();
    FloatRegister in = ToFloatRegister(ins->input());
    FloatRegister out = ToFloatRegister(ins->output());
    Register temp = ToRegister(ins->tempR());
    FloatRegister tempF = ToFloatRegister(ins->tempF());

    // Classify lane values into 4 disjoint classes:
    //
    //   N-lanes:             in <= -1.0
    //   A-lanes:      -1.0 < in <= 0x0.ffffffp31
    //   B-lanes: 0x1.0p31 <= in <= 0x0.ffffffp32
    //   V-lanes: 0x1.0p32 <= in, or isnan(in)
    //
    // We need to bail out to throw a RangeError if we see any N-lanes or
    // V-lanes.
    //
    // For A-lanes and B-lanes, we make two float -> int32 conversions:
    //
    //   A = cvttps2dq(in)
    //   B = cvttps2dq(in - 0x1.0p31f)
    //
    // Note that the subtraction for the B computation is exact for B-lanes.
    // There is no rounding, so B is the low 31 bits of the correctly converted
    // result.
    //
    // The cvttps2dq instruction produces 0x80000000 when the input is NaN or
    // out of range for a signed int32_t. This conveniently provides the missing
    // high bit for B, so the desired result is A for A-lanes and A|B for
    // B-lanes.

    ScratchSimd128Scope scratch(masm);

    // TODO: If the majority of lanes are A-lanes, it could be faster to compute
    // A first, use vmovmskps to check for any non-A-lanes and handle them in
    // ool code. OTOH, we we're wrong about the lane distribution, that would be
    // slower.

    // Compute B in |scratch|.
    static const float Adjust = 0x80000000; // 0x1.0p31f for the benefit of MSVC.
    static const SimdConstant Bias = SimdConstant::SplatX4(-Adjust);
    masm.loadConstantSimd128Float(Bias, scratch);
    masm.packedAddFloat32(Operand(in), scratch);
    masm.convertFloat32x4ToInt32x4(scratch, scratch);

    // Compute A in |out|. This is the last time we use |in| and the first time
    // we use |out|, so we can tolerate if they are the same register.
    masm.convertFloat32x4ToInt32x4(in, out);

    // We can identify A-lanes by the sign bits in A: Any A-lanes will be
    // positive in A, and N, B, and V-lanes will be 0x80000000 in A. Compute a
    // mask of non-A-lanes into |tempF|.
    masm.zeroSimd128Float(tempF);
    masm.packedGreaterThanInt32x4(Operand(out), tempF);

    // Clear the A-lanes in B.
    masm.bitwiseAndSimd128(Operand(tempF), scratch);

    // Compute the final result: A for A-lanes, A|B for B-lanes.
    masm.bitwiseOrSimd128(Operand(scratch), out);

    // We still need to filter out the V-lanes. They would show up as 0x80000000
    // in both A and B. Since we cleared the valid A-lanes in B, the V-lanes are
    // the remaining negative lanes in B.
    masm.vmovmskps(scratch, temp);
    masm.cmp32(temp, Imm32(0));

    if (gen->compilingWasm()) {
        Label ok;
        masm.j(Assembler::Equal, &ok);
        masm.wasmTrap(wasm::Trap::ImpreciseSimdConversion, mir->bytecodeOffset());
        masm.bind(&ok);
    } else {
        bailoutIf(Assembler::NotEqual, ins->snapshot());
    }
}

void
CodeGeneratorX86Shared::visitSimdValueInt32x4(LSimdValueInt32x4* ins)
{
    MOZ_ASSERT(ins->mir()->type() == MIRType::Int32x4 || ins->mir()->type() == MIRType::Bool32x4);

    FloatRegister output = ToFloatRegister(ins->output());
    if (AssemblerX86Shared::HasSSE41()) {
        masm.vmovd(ToRegister(ins->getOperand(0)), output);
        for (size_t i = 1; i < 4; ++i) {
            Register r = ToRegister(ins->getOperand(i));
            masm.vpinsrd(i, r, output, output);
        }
        return;
    }

    masm.reserveStack(Simd128DataSize);
    for (size_t i = 0; i < 4; ++i) {
        Register r = ToRegister(ins->getOperand(i));
        masm.store32(r, Address(StackPointer, i * sizeof(int32_t)));
    }
    masm.loadAlignedSimd128Int(Address(StackPointer, 0), output);
    masm.freeStack(Simd128DataSize);
}

void
CodeGeneratorX86Shared::visitSimdValueFloat32x4(LSimdValueFloat32x4* ins)
{
    MOZ_ASSERT(ins->mir()->type() == MIRType::Float32x4);

    FloatRegister r0 = ToFloatRegister(ins->getOperand(0));
    FloatRegister r1 = ToFloatRegister(ins->getOperand(1));
    FloatRegister r2 = ToFloatRegister(ins->getOperand(2));
    FloatRegister r3 = ToFloatRegister(ins->getOperand(3));
    FloatRegister tmp = ToFloatRegister(ins->getTemp(0));
    FloatRegister output = ToFloatRegister(ins->output());

    FloatRegister r0Copy = masm.reusedInputFloat32x4(r0, output);
    FloatRegister r1Copy = masm.reusedInputFloat32x4(r1, tmp);

    masm.vunpcklps(r3, r1Copy, tmp);
    masm.vunpcklps(r2, r0Copy, output);
    masm.vunpcklps(tmp, output, output);
}

void
CodeGeneratorX86Shared::visitSimdSplatX16(LSimdSplatX16* ins)
{
    MOZ_ASSERT(SimdTypeToLength(ins->mir()->type()) == 16);
    Register input = ToRegister(ins->getOperand(0));
    FloatRegister output = ToFloatRegister(ins->output());
    masm.vmovd(input, output);
    if (AssemblerX86Shared::HasSSSE3()) {
        masm.zeroSimd128Int(ScratchSimd128Reg);
        masm.vpshufb(ScratchSimd128Reg, output, output);
    } else {
        // Use two shifts to duplicate the low 8 bits into the low 16 bits.
        masm.vpsllw(Imm32(8), output, output);
        masm.vmovdqa(output, ScratchSimd128Reg);
        masm.vpsrlw(Imm32(8), ScratchSimd128Reg, ScratchSimd128Reg);
        masm.vpor(ScratchSimd128Reg, output, output);
        // Then do an X8 splat.
        masm.vpshuflw(0, output, output);
        masm.vpshufd(0, output, output);
    }
}

void
CodeGeneratorX86Shared::visitSimdSplatX8(LSimdSplatX8* ins)
{
    MOZ_ASSERT(SimdTypeToLength(ins->mir()->type()) == 8);
    Register input = ToRegister(ins->getOperand(0));
    FloatRegister output = ToFloatRegister(ins->output());
    masm.vmovd(input, output);
    masm.vpshuflw(0, output, output);
    masm.vpshufd(0, output, output);
}

void
CodeGeneratorX86Shared::visitSimdSplatX4(LSimdSplatX4* ins)
{
    FloatRegister output = ToFloatRegister(ins->output());

    MSimdSplat* mir = ins->mir();
    MOZ_ASSERT(IsSimdType(mir->type()));
    JS_STATIC_ASSERT(sizeof(float) == sizeof(int32_t));

    if (mir->type() == MIRType::Float32x4) {
        FloatRegister r = ToFloatRegister(ins->getOperand(0));
        FloatRegister rCopy = masm.reusedInputFloat32x4(r, output);
        masm.vshufps(0, rCopy, rCopy, output);
    } else {
        Register r = ToRegister(ins->getOperand(0));
        masm.vmovd(r, output);
        masm.vpshufd(0, output, output);
    }
}

void
CodeGeneratorX86Shared::visitSimdReinterpretCast(LSimdReinterpretCast* ins)
{
    FloatRegister input = ToFloatRegister(ins->input());
    FloatRegister output = ToFloatRegister(ins->output());

    if (input.aliases(output))
        return;

    if (IsIntegerSimdType(ins->mir()->type()))
        masm.vmovdqa(input, output);
    else
        masm.vmovaps(input, output);
}

// Extract an integer lane from the 32x4 vector register |input| and place it in
// |output|.
void
CodeGeneratorX86Shared::emitSimdExtractLane32x4(FloatRegister input, Register output, unsigned lane)
{
    if (lane == 0) {
        // The value we want to extract is in the low double-word
        masm.moveLowInt32(input, output);
    } else if (AssemblerX86Shared::HasSSE41()) {
        masm.vpextrd(lane, input, output);
    } else {
        uint32_t mask = MacroAssembler::ComputeShuffleMask(lane);
        masm.shuffleInt32(mask, input, ScratchSimd128Reg);
        masm.moveLowInt32(ScratchSimd128Reg, output);
    }
}

// Extract an integer lane from the 16x8 vector register |input|, sign- or
// zero-extend to 32 bits and place the result in |output|.
void
CodeGeneratorX86Shared::emitSimdExtractLane16x8(FloatRegister input, Register output,
                                                unsigned lane, SimdSign signedness)
{
    // Unlike pextrd and pextrb, this is available in SSE2.
    masm.vpextrw(lane, input, output);

    if (signedness == SimdSign::Signed)
        masm.movswl(output, output);
}

// Extract an integer lane from the 8x16 vector register |input|, sign- or
// zero-extend to 32 bits and place the result in |output|.
void
CodeGeneratorX86Shared::emitSimdExtractLane8x16(FloatRegister input, Register output,
                                                unsigned lane, SimdSign signedness)
{
    if (AssemblerX86Shared::HasSSE41()) {
        masm.vpextrb(lane, input, output);
        // vpextrb clears the high bits, so no further extension required.
        if (signedness == SimdSign::Unsigned)
            signedness = SimdSign::NotApplicable;
    } else {
        // Extract the relevant 16 bits containing our lane, then shift the
        // right 8 bits into place.
        emitSimdExtractLane16x8(input, output, lane / 2, SimdSign::Unsigned);
        if (lane % 2) {
            masm.shrl(Imm32(8), output);
            // The shrl handles the zero-extension. Don't repeat it.
            if (signedness == SimdSign::Unsigned)
                signedness = SimdSign::NotApplicable;
        }
    }

    // We have the right low 8 bits in |output|, but we may need to fix the high
    // bits. Note that this requires |output| to be one of the %eax-%edx
    // registers.
    switch (signedness) {
      case SimdSign::Signed:
        masm.movsbl(output, output);
        break;
      case SimdSign::Unsigned:
        masm.movzbl(output, output);
        break;
      case SimdSign::NotApplicable:
        // No adjustment needed.
        break;
    }
}

void
CodeGeneratorX86Shared::visitSimdExtractElementB(LSimdExtractElementB* ins)
{
    FloatRegister input = ToFloatRegister(ins->input());
    Register output = ToRegister(ins->output());
    MSimdExtractElement* mir = ins->mir();
    unsigned length = SimdTypeToLength(mir->specialization());

    switch (length) {
      case 4:
        emitSimdExtractLane32x4(input, output, mir->lane());
        break;
      case 8:
        // Get a lane, don't bother fixing the high bits since we'll mask below.
        emitSimdExtractLane16x8(input, output, mir->lane(), SimdSign::NotApplicable);
        break;
      case 16:
        emitSimdExtractLane8x16(input, output, mir->lane(), SimdSign::NotApplicable);
        break;
      default:
        MOZ_CRASH("Unhandled SIMD length");
    }

    // We need to generate a 0/1 value. We have 0/-1 and possibly dirty high bits.
    masm.and32(Imm32(1), output);
}

void
CodeGeneratorX86Shared::visitSimdExtractElementI(LSimdExtractElementI* ins)
{
    FloatRegister input = ToFloatRegister(ins->input());
    Register output = ToRegister(ins->output());
    MSimdExtractElement* mir = ins->mir();
    unsigned length = SimdTypeToLength(mir->specialization());

    switch (length) {
      case 4:
        emitSimdExtractLane32x4(input, output, mir->lane());
        break;
      case 8:
        emitSimdExtractLane16x8(input, output, mir->lane(), mir->signedness());
        break;
      case 16:
        emitSimdExtractLane8x16(input, output, mir->lane(), mir->signedness());
        break;
      default:
        MOZ_CRASH("Unhandled SIMD length");
    }
}

void
CodeGeneratorX86Shared::visitSimdExtractElementU2D(LSimdExtractElementU2D* ins)
{
    FloatRegister input = ToFloatRegister(ins->input());
    FloatRegister output = ToFloatRegister(ins->output());
    Register temp = ToRegister(ins->temp());
    MSimdExtractElement* mir = ins->mir();
    MOZ_ASSERT(mir->specialization() == MIRType::Int32x4);
    emitSimdExtractLane32x4(input, temp, mir->lane());
    masm.convertUInt32ToDouble(temp, output);
}

void
CodeGeneratorX86Shared::visitSimdExtractElementF(LSimdExtractElementF* ins)
{
    FloatRegister input = ToFloatRegister(ins->input());
    FloatRegister output = ToFloatRegister(ins->output());

    unsigned lane = ins->mir()->lane();
    if (lane == 0) {
        // The value we want to extract is in the low double-word
        if (input != output)
            masm.moveFloat32(input, output);
    } else if (lane == 2) {
        masm.moveHighPairToLowPairFloat32(input, output);
    } else {
        uint32_t mask = MacroAssembler::ComputeShuffleMask(lane);
        masm.shuffleFloat32(mask, input, output);
    }
    // NaNs contained within SIMD values are not enforced to be canonical, so
    // when we extract an element into a "regular" scalar JS value, we have to
    // canonicalize. In wasm code, we can skip this, as wasm only has to
    // canonicalize NaNs at FFI boundaries.
    if (!gen->compilingWasm())
        masm.canonicalizeFloat(output);
}

void
CodeGeneratorX86Shared::visitSimdInsertElementI(LSimdInsertElementI* ins)
{
    FloatRegister vector = ToFloatRegister(ins->vector());
    Register value = ToRegister(ins->value());
    FloatRegister output = ToFloatRegister(ins->output());
    MOZ_ASSERT(vector == output); // defineReuseInput(0)

    unsigned lane = ins->lane();
    unsigned length = ins->length();

    if (length == 8) {
        // Available in SSE 2.
        masm.vpinsrw(lane, value, vector, output);
        return;
    }

    // Note that, contrarily to float32x4, we cannot use vmovd if the inserted
    // value goes into the first component, as vmovd clears out the higher lanes
    // of the output.
    if (AssemblerX86Shared::HasSSE41()) {
        // TODO: Teach Lowering that we don't need defineReuseInput if we have AVX.
        switch (length) {
          case 4:
            masm.vpinsrd(lane, value, vector, output);
            return;
          case 16:
            masm.vpinsrb(lane, value, vector, output);
            return;
        }
    }

    masm.reserveStack(Simd128DataSize);
    masm.storeAlignedSimd128Int(vector, Address(StackPointer, 0));
    switch (length) {
      case 4:
        masm.store32(value, Address(StackPointer, lane * sizeof(int32_t)));
        break;
      case 16:
        // Note that this requires `value` to be in one the registers where the
        // low 8 bits are addressible (%eax - %edx on x86, all of them on x86-64).
        masm.store8(value, Address(StackPointer, lane * sizeof(int8_t)));
        break;
      default:
        MOZ_CRASH("Unsupported SIMD length");
    }
    masm.loadAlignedSimd128Int(Address(StackPointer, 0), output);
    masm.freeStack(Simd128DataSize);
}

void
CodeGeneratorX86Shared::visitSimdInsertElementF(LSimdInsertElementF* ins)
{
    FloatRegister vector = ToFloatRegister(ins->vector());
    FloatRegister value = ToFloatRegister(ins->value());
    FloatRegister output = ToFloatRegister(ins->output());
    MOZ_ASSERT(vector == output); // defineReuseInput(0)

    if (ins->lane() == 0) {
        // As both operands are registers, vmovss doesn't modify the upper bits
        // of the destination operand.
        if (value != output)
            masm.vmovss(value, vector, output);
        return;
    }

    if (AssemblerX86Shared::HasSSE41()) {
        // The input value is in the low float32 of the 'value' FloatRegister.
        masm.vinsertps(masm.vinsertpsMask(0, ins->lane()), value, output, output);
        return;
    }

    unsigned component = unsigned(ins->lane());
    masm.reserveStack(Simd128DataSize);
    masm.storeAlignedSimd128Float(vector, Address(StackPointer, 0));
    masm.storeFloat32(value, Address(StackPointer, component * sizeof(int32_t)));
    masm.loadAlignedSimd128Float(Address(StackPointer, 0), output);
    masm.freeStack(Simd128DataSize);
}

void
CodeGeneratorX86Shared::visitSimdAllTrue(LSimdAllTrue* ins)
{
    FloatRegister input = ToFloatRegister(ins->input());
    Register output = ToRegister(ins->output());

    // We know that the input lanes are boolean, so they are either 0 or -1.
    // The all-true vector has all 128 bits set, no matter the lane geometry.
    masm.vpmovmskb(input, output);
    masm.cmp32(output, Imm32(0xffff));
    masm.emitSet(Assembler::Zero, output);
}

void
CodeGeneratorX86Shared::visitSimdAnyTrue(LSimdAnyTrue* ins)
{
    FloatRegister input = ToFloatRegister(ins->input());
    Register output = ToRegister(ins->output());

    masm.vpmovmskb(input, output);
    masm.cmp32(output, Imm32(0x0));
    masm.emitSet(Assembler::NonZero, output);
}

template <class T, class Reg> void
CodeGeneratorX86Shared::visitSimdGeneralShuffle(LSimdGeneralShuffleBase* ins, Reg tempRegister)
{
    MSimdGeneralShuffle* mir = ins->mir();
    unsigned numVectors = mir->numVectors();

    Register laneTemp = ToRegister(ins->temp());

    // This won't generate fast code, but it's fine because we expect users
    // to have used constant indices (and thus MSimdGeneralShuffle to be fold
    // into MSimdSwizzle/MSimdShuffle, which are fast).

    // We need stack space for the numVectors inputs and for the output vector.
    unsigned stackSpace = Simd128DataSize * (numVectors + 1);
    masm.reserveStack(stackSpace);

    for (unsigned i = 0; i < numVectors; i++) {
        masm.storeAlignedVector<T>(ToFloatRegister(ins->vector(i)),
                                   Address(StackPointer, Simd128DataSize * (1 + i)));
    }

    Label bail;
    const Scale laneScale = ScaleFromElemWidth(sizeof(T));

    for (size_t i = 0; i < mir->numLanes(); i++) {
        Operand lane = ToOperand(ins->lane(i));

        masm.cmp32(lane, Imm32(numVectors * mir->numLanes() - 1));
        masm.j(Assembler::Above, &bail);

        if (lane.kind() == Operand::REG) {
            masm.loadScalar<T>(Operand(StackPointer, ToRegister(ins->lane(i)), laneScale, Simd128DataSize),
                               tempRegister);
        } else {
            masm.load32(lane, laneTemp);
            masm.loadScalar<T>(Operand(StackPointer, laneTemp, laneScale, Simd128DataSize), tempRegister);
        }

        masm.storeScalar<T>(tempRegister, Address(StackPointer, i * sizeof(T)));
    }

    FloatRegister output = ToFloatRegister(ins->output());
    masm.loadAlignedVector<T>(Address(StackPointer, 0), output);

    Label join;
    masm.jump(&join);

    {
        masm.bind(&bail);
        masm.freeStack(stackSpace);
        bailout(ins->snapshot());
    }

    masm.bind(&join);
    masm.setFramePushed(masm.framePushed() + stackSpace);
    masm.freeStack(stackSpace);
}

void
CodeGeneratorX86Shared::visitSimdGeneralShuffleI(LSimdGeneralShuffleI* ins)
{
    switch (ins->mir()->type()) {
      case MIRType::Int8x16:
        return visitSimdGeneralShuffle<int8_t, Register>(ins, ToRegister(ins->temp()));
      case MIRType::Int16x8:
        return visitSimdGeneralShuffle<int16_t, Register>(ins, ToRegister(ins->temp()));
      case MIRType::Int32x4:
        return visitSimdGeneralShuffle<int32_t, Register>(ins, ToRegister(ins->temp()));
      default:
        MOZ_CRASH("unsupported type for general shuffle");
    }
}
void
CodeGeneratorX86Shared::visitSimdGeneralShuffleF(LSimdGeneralShuffleF* ins)
{
    ScratchFloat32Scope scratch(masm);
    visitSimdGeneralShuffle<float, FloatRegister>(ins, scratch);
}

void
CodeGeneratorX86Shared::visitSimdSwizzleI(LSimdSwizzleI* ins)
{
    FloatRegister input = ToFloatRegister(ins->input());
    FloatRegister output = ToFloatRegister(ins->output());
    const unsigned numLanes = ins->numLanes();

    switch (numLanes) {
        case 4: {
            uint32_t x = ins->lane(0);
            uint32_t y = ins->lane(1);
            uint32_t z = ins->lane(2);
            uint32_t w = ins->lane(3);

            uint32_t mask = MacroAssembler::ComputeShuffleMask(x, y, z, w);
            masm.shuffleInt32(mask, input, output);
            return;
        }
    }

    // In the general case, use pshufb if it is available. Convert to a
    // byte-wise swizzle.
    const unsigned bytesPerLane = 16 / numLanes;
    int8_t bLane[16];
    for (unsigned i = 0; i < numLanes; i++) {
        for (unsigned b = 0; b < bytesPerLane; b++) {
            bLane[i * bytesPerLane + b] = ins->lane(i) * bytesPerLane + b;
        }
    }

    if (AssemblerX86Shared::HasSSSE3()) {
        ScratchSimd128Scope scratch(masm);
        masm.loadConstantSimd128Int(SimdConstant::CreateX16(bLane), scratch);
        FloatRegister inputCopy = masm.reusedInputInt32x4(input, output);
        masm.vpshufb(scratch, inputCopy, output);
        return;
    }

    // Worst-case fallback for pre-SSSE3 machines. Bounce through memory.
    Register temp = ToRegister(ins->getTemp(0));
    masm.reserveStack(2 * Simd128DataSize);
    masm.storeAlignedSimd128Int(input, Address(StackPointer, Simd128DataSize));
    for (unsigned i = 0; i < 16; i++) {
        masm.load8ZeroExtend(Address(StackPointer, Simd128DataSize + bLane[i]), temp);
        masm.store8(temp, Address(StackPointer, i));
    }
    masm.loadAlignedSimd128Int(Address(StackPointer, 0), output);
    masm.freeStack(2 * Simd128DataSize);
}

void
CodeGeneratorX86Shared::visitSimdSwizzleF(LSimdSwizzleF* ins)
{
    FloatRegister input = ToFloatRegister(ins->input());
    FloatRegister output = ToFloatRegister(ins->output());
    MOZ_ASSERT(ins->numLanes() == 4);

    uint32_t x = ins->lane(0);
    uint32_t y = ins->lane(1);
    uint32_t z = ins->lane(2);
    uint32_t w = ins->lane(3);

    if (AssemblerX86Shared::HasSSE3()) {
        if (ins->lanesMatch(0, 0, 2, 2)) {
            masm.vmovsldup(input, output);
            return;
        }
        if (ins->lanesMatch(1, 1, 3, 3)) {
            masm.vmovshdup(input, output);
            return;
        }
    }

    // TODO Here and below, arch specific lowering could identify this pattern
    // and use defineReuseInput to avoid this move (bug 1084404)
    if (ins->lanesMatch(2, 3, 2, 3)) {
        FloatRegister inputCopy = masm.reusedInputFloat32x4(input, output);
        masm.vmovhlps(input, inputCopy, output);
        return;
    }

    if (ins->lanesMatch(0, 1, 0, 1)) {
        if (AssemblerX86Shared::HasSSE3() && !AssemblerX86Shared::HasAVX()) {
            masm.vmovddup(input, output);
            return;
        }
        FloatRegister inputCopy = masm.reusedInputFloat32x4(input, output);
        masm.vmovlhps(input, inputCopy, output);
        return;
    }

    if (ins->lanesMatch(0, 0, 1, 1)) {
        FloatRegister inputCopy = masm.reusedInputFloat32x4(input, output);
        masm.vunpcklps(input, inputCopy, output);
        return;
    }

    if (ins->lanesMatch(2, 2, 3, 3)) {
        FloatRegister inputCopy = masm.reusedInputFloat32x4(input, output);
        masm.vunpckhps(input, inputCopy, output);
        return;
    }

    uint32_t mask = MacroAssembler::ComputeShuffleMask(x, y, z, w);
    masm.shuffleFloat32(mask, input, output);
}

void
CodeGeneratorX86Shared::visitSimdShuffle(LSimdShuffle* ins)
{
    FloatRegister lhs = ToFloatRegister(ins->lhs());
    FloatRegister rhs = ToFloatRegister(ins->rhs());
    FloatRegister output = ToFloatRegister(ins->output());
    const unsigned numLanes = ins->numLanes();
    const unsigned bytesPerLane = 16 / numLanes;

    // Convert the shuffle to a byte-wise shuffle.
    uint8_t bLane[16];
    for (unsigned i = 0; i < numLanes; i++) {
        for (unsigned b = 0; b < bytesPerLane; b++) {
            bLane[i * bytesPerLane + b] = ins->lane(i) * bytesPerLane + b;
        }
    }

    // Use pshufb if it is available.
    if (AssemblerX86Shared::HasSSSE3()) {
        FloatRegister scratch1 = ToFloatRegister(ins->temp());
        ScratchSimd128Scope scratch2(masm);

        // Use pshufb instructions to gather the lanes from each source vector.
        // A negative index creates a zero lane, so the two vectors can be combined.

        // Set scratch2 = lanes from lhs.
        int8_t idx[16];
        for (unsigned i = 0; i < 16; i++)
            idx[i] = bLane[i] < 16 ? bLane[i] : -1;
        masm.loadConstantSimd128Int(SimdConstant::CreateX16(idx), scratch1);
        FloatRegister lhsCopy = masm.reusedInputInt32x4(lhs, scratch2);
        masm.vpshufb(scratch1, lhsCopy, scratch2);

        // Set output = lanes from rhs.
        for (unsigned i = 0; i < 16; i++)
            idx[i] = bLane[i] >= 16 ? bLane[i] - 16 : -1;
        masm.loadConstantSimd128Int(SimdConstant::CreateX16(idx), scratch1);
        FloatRegister rhsCopy = masm.reusedInputInt32x4(rhs, output);
        masm.vpshufb(scratch1, rhsCopy, output);

        // Combine.
        masm.vpor(scratch2, output, output);
        return;
    }

    // Worst-case fallback for pre-SSE3 machines. Bounce through memory.
    Register temp = ToRegister(ins->getTemp(0));
    masm.reserveStack(3 * Simd128DataSize);
    masm.storeAlignedSimd128Int(lhs, Address(StackPointer, Simd128DataSize));
    masm.storeAlignedSimd128Int(rhs, Address(StackPointer, 2 * Simd128DataSize));
    for (unsigned i = 0; i < 16; i++) {
        masm.load8ZeroExtend(Address(StackPointer, Simd128DataSize + bLane[i]), temp);
        masm.store8(temp, Address(StackPointer, i));
    }
    masm.loadAlignedSimd128Int(Address(StackPointer, 0), output);
    masm.freeStack(3 * Simd128DataSize);
}

void
CodeGeneratorX86Shared::visitSimdShuffleX4(LSimdShuffleX4* ins)
{
    FloatRegister lhs = ToFloatRegister(ins->lhs());
    Operand rhs = ToOperand(ins->rhs());
    FloatRegister out = ToFloatRegister(ins->output());

    uint32_t x = ins->lane(0);
    uint32_t y = ins->lane(1);
    uint32_t z = ins->lane(2);
    uint32_t w = ins->lane(3);

    // Check that lanes come from LHS in majority:
    unsigned numLanesFromLHS = (x < 4) + (y < 4) + (z < 4) + (w < 4);
    MOZ_ASSERT(numLanesFromLHS >= 2);

    // When reading this method, remember that vshufps takes the two first
    // inputs of the destination operand (right operand) and the two last
    // inputs of the source operand (left operand).
    //
    // Legend for explanations:
    // - L: LHS
    // - R: RHS
    // - T: temporary

    uint32_t mask;

    // If all lanes came from a single vector, we should have constructed a
    // MSimdSwizzle instead.
    MOZ_ASSERT(numLanesFromLHS < 4);

    // If all values stay in their lane, this is a blend.
    if (AssemblerX86Shared::HasSSE41()) {
        if (x % 4 == 0 && y % 4 == 1 && z % 4 == 2 && w % 4 == 3) {
            masm.vblendps(masm.blendpsMask(x >= 4, y >= 4, z >= 4, w >= 4), rhs, lhs, out);
            return;
        }
    }

    // One element of the second, all other elements of the first
    if (numLanesFromLHS == 3) {
        unsigned firstMask = -1, secondMask = -1;

        // register-register vmovss preserves the high lanes.
        if (ins->lanesMatch(4, 1, 2, 3) && rhs.kind() == Operand::FPREG) {
            masm.vmovss(FloatRegister::FromCode(rhs.fpu()), lhs, out);
            return;
        }

        // SSE4.1 vinsertps can handle any single element.
        unsigned numLanesUnchanged = (x == 0) + (y == 1) + (z == 2) + (w == 3);
        if (AssemblerX86Shared::HasSSE41() && numLanesUnchanged == 3) {
            unsigned srcLane;
            unsigned dstLane;
            if (x >= 4) {
                srcLane = x - 4;
                dstLane = 0;
            } else if (y >= 4) {
                srcLane = y - 4;
                dstLane = 1;
            } else if (z >= 4) {
                srcLane = z - 4;
                dstLane = 2;
            } else {
                MOZ_ASSERT(w >= 4);
                srcLane = w - 4;
                dstLane = 3;
            }
            masm.vinsertps(masm.vinsertpsMask(srcLane, dstLane), rhs, lhs, out);
            return;
        }

        FloatRegister rhsCopy = ToFloatRegister(ins->temp());

        if (x < 4 && y < 4) {
            if (w >= 4) {
                w %= 4;
                // T = (Rw Rw Lz Lz) = vshufps(firstMask, lhs, rhs, rhsCopy)
                firstMask = MacroAssembler::ComputeShuffleMask(w, w, z, z);
                // (Lx Ly Lz Rw) = (Lx Ly Tz Tx) = vshufps(secondMask, T, lhs, out)
                secondMask = MacroAssembler::ComputeShuffleMask(x, y, 2, 0);
            } else {
                MOZ_ASSERT(z >= 4);
                z %= 4;
                // T = (Rz Rz Lw Lw) = vshufps(firstMask, lhs, rhs, rhsCopy)
                firstMask = MacroAssembler::ComputeShuffleMask(z, z, w, w);
                // (Lx Ly Rz Lw) = (Lx Ly Tx Tz) = vshufps(secondMask, T, lhs, out)
                secondMask = MacroAssembler::ComputeShuffleMask(x, y, 0, 2);
            }

            masm.vshufps(firstMask, lhs, rhsCopy, rhsCopy);
            masm.vshufps(secondMask, rhsCopy, lhs, out);
            return;
        }

        MOZ_ASSERT(z < 4 && w < 4);

        if (y >= 4) {
            y %= 4;
            // T = (Ry Ry Lx Lx) = vshufps(firstMask, lhs, rhs, rhsCopy)
            firstMask = MacroAssembler::ComputeShuffleMask(y, y, x, x);
            // (Lx Ry Lz Lw) = (Tz Tx Lz Lw) = vshufps(secondMask, lhs, T, out)
            secondMask = MacroAssembler::ComputeShuffleMask(2, 0, z, w);
        } else {
            MOZ_ASSERT(x >= 4);
            x %= 4;
            // T = (Rx Rx Ly Ly) = vshufps(firstMask, lhs, rhs, rhsCopy)
            firstMask = MacroAssembler::ComputeShuffleMask(x, x, y, y);
            // (Rx Ly Lz Lw) = (Tx Tz Lz Lw) = vshufps(secondMask, lhs, T, out)
            secondMask = MacroAssembler::ComputeShuffleMask(0, 2, z, w);
        }

        masm.vshufps(firstMask, lhs, rhsCopy, rhsCopy);
        if (AssemblerX86Shared::HasAVX()) {
            masm.vshufps(secondMask, lhs, rhsCopy, out);
        } else {
            masm.vshufps(secondMask, lhs, rhsCopy, rhsCopy);
            masm.moveSimd128Float(rhsCopy, out);
        }
        return;
    }

    // Two elements from one vector, two other elements from the other
    MOZ_ASSERT(numLanesFromLHS == 2);

    // TODO Here and below, symmetric case would be more handy to avoid a move,
    // but can't be reached because operands would get swapped (bug 1084404).
    if (ins->lanesMatch(2, 3, 6, 7)) {
        ScratchSimd128Scope scratch(masm);
        if (AssemblerX86Shared::HasAVX()) {
            FloatRegister rhsCopy = masm.reusedInputAlignedFloat32x4(rhs, scratch);
            masm.vmovhlps(lhs, rhsCopy, out);
        } else {
            masm.loadAlignedSimd128Float(rhs, scratch);
            masm.vmovhlps(lhs, scratch, scratch);
            masm.moveSimd128Float(scratch, out);
        }
        return;
    }

    if (ins->lanesMatch(0, 1, 4, 5)) {
        FloatRegister rhsCopy;
        ScratchSimd128Scope scratch(masm);
        if (rhs.kind() == Operand::FPREG) {
            // No need to make an actual copy, since the operand is already
            // in a register, and it won't be clobbered by the vmovlhps.
            rhsCopy = FloatRegister::FromCode(rhs.fpu());
        } else {
            masm.loadAlignedSimd128Float(rhs, scratch);
            rhsCopy = scratch;
        }
        masm.vmovlhps(rhsCopy, lhs, out);
        return;
    }

    if (ins->lanesMatch(0, 4, 1, 5)) {
        masm.vunpcklps(rhs, lhs, out);
        return;
    }

    // TODO swapped case would be better (bug 1084404)
    if (ins->lanesMatch(4, 0, 5, 1)) {
        ScratchSimd128Scope scratch(masm);
        if (AssemblerX86Shared::HasAVX()) {
            FloatRegister rhsCopy = masm.reusedInputAlignedFloat32x4(rhs, scratch);
            masm.vunpcklps(lhs, rhsCopy, out);
        } else {
            masm.loadAlignedSimd128Float(rhs, scratch);
            masm.vunpcklps(lhs, scratch, scratch);
            masm.moveSimd128Float(scratch, out);
        }
        return;
    }

    if (ins->lanesMatch(2, 6, 3, 7)) {
        masm.vunpckhps(rhs, lhs, out);
        return;
    }

    // TODO swapped case would be better (bug 1084404)
    if (ins->lanesMatch(6, 2, 7, 3)) {
        ScratchSimd128Scope scratch(masm);
        if (AssemblerX86Shared::HasAVX()) {
            FloatRegister rhsCopy = masm.reusedInputAlignedFloat32x4(rhs, scratch);
            masm.vunpckhps(lhs, rhsCopy, out);
        } else {
            masm.loadAlignedSimd128Float(rhs, scratch);
            masm.vunpckhps(lhs, scratch, scratch);
            masm.moveSimd128Float(scratch, out);
        }
        return;
    }

    // In one vshufps
    if (x < 4 && y < 4) {
        mask = MacroAssembler::ComputeShuffleMask(x, y, z % 4, w % 4);
        masm.vshufps(mask, rhs, lhs, out);
        return;
    }

    // At creation, we should have explicitly swapped in this case.
    MOZ_ASSERT(!(z >= 4 && w >= 4));

    // In two vshufps, for the most generic case:
    uint32_t firstMask[4], secondMask[4];
    unsigned i = 0, j = 2, k = 0;

#define COMPUTE_MASK(lane)       \
    if (lane >= 4) {             \
        firstMask[j] = lane % 4; \
        secondMask[k++] = j++;   \
    } else {                     \
        firstMask[i] = lane;     \
        secondMask[k++] = i++;   \
    }

    COMPUTE_MASK(x)
    COMPUTE_MASK(y)
    COMPUTE_MASK(z)
    COMPUTE_MASK(w)
#undef COMPUTE_MASK

    MOZ_ASSERT(i == 2 && j == 4 && k == 4);

    mask = MacroAssembler::ComputeShuffleMask(firstMask[0], firstMask[1],
                                              firstMask[2], firstMask[3]);
    masm.vshufps(mask, rhs, lhs, lhs);

    mask = MacroAssembler::ComputeShuffleMask(secondMask[0], secondMask[1],
                                              secondMask[2], secondMask[3]);
    masm.vshufps(mask, lhs, lhs, lhs);
}

void
CodeGeneratorX86Shared::visitSimdBinaryCompIx16(LSimdBinaryCompIx16* ins)
{
    static const SimdConstant allOnes = SimdConstant::SplatX16(-1);

    FloatRegister lhs = ToFloatRegister(ins->lhs());
    Operand rhs = ToOperand(ins->rhs());
    FloatRegister output = ToFloatRegister(ins->output());
    MOZ_ASSERT_IF(!Assembler::HasAVX(), output == lhs);

    ScratchSimd128Scope scratch(masm);

    MSimdBinaryComp::Operation op = ins->operation();
    switch (op) {
      case MSimdBinaryComp::greaterThan:
        masm.vpcmpgtb(rhs, lhs, output);
        return;
      case MSimdBinaryComp::equal:
        masm.vpcmpeqb(rhs, lhs, output);
        return;
      case MSimdBinaryComp::lessThan:
        // src := rhs
        if (rhs.kind() == Operand::FPREG)
            masm.moveSimd128Int(ToFloatRegister(ins->rhs()), scratch);
        else
            masm.loadAlignedSimd128Int(rhs, scratch);

        // src := src > lhs (i.e. lhs < rhs)
        // Improve by doing custom lowering (rhs is tied to the output register)
        masm.vpcmpgtb(ToOperand(ins->lhs()), scratch, scratch);
        masm.moveSimd128Int(scratch, output);
        return;
      case MSimdBinaryComp::notEqual:
        // Ideally for notEqual, greaterThanOrEqual, and lessThanOrEqual, we
        // should invert the comparison by, e.g. swapping the arms of a select
        // if that's what it's used in.
        masm.loadConstantSimd128Int(allOnes, scratch);
        masm.vpcmpeqb(rhs, lhs, output);
        masm.bitwiseXorSimd128(Operand(scratch), output);
        return;
      case MSimdBinaryComp::greaterThanOrEqual:
        // src := rhs
        if (rhs.kind() == Operand::FPREG)
            masm.moveSimd128Int(ToFloatRegister(ins->rhs()), scratch);
        else
            masm.loadAlignedSimd128Int(rhs, scratch);
        masm.vpcmpgtb(ToOperand(ins->lhs()), scratch, scratch);
        masm.loadConstantSimd128Int(allOnes, output);
        masm.bitwiseXorSimd128(Operand(scratch), output);
        return;
      case MSimdBinaryComp::lessThanOrEqual:
        // lhs <= rhs is equivalent to !(rhs < lhs), which we compute here.
        masm.loadConstantSimd128Int(allOnes, scratch);
        masm.vpcmpgtb(rhs, lhs, output);
        masm.bitwiseXorSimd128(Operand(scratch), output);
        return;
    }
    MOZ_CRASH("unexpected SIMD op");
}

void
CodeGeneratorX86Shared::visitSimdBinaryCompIx8(LSimdBinaryCompIx8* ins)
{
    static const SimdConstant allOnes = SimdConstant::SplatX8(-1);

    FloatRegister lhs = ToFloatRegister(ins->lhs());
    Operand rhs = ToOperand(ins->rhs());
    FloatRegister output = ToFloatRegister(ins->output());
    MOZ_ASSERT_IF(!Assembler::HasAVX(), output == lhs);

    ScratchSimd128Scope scratch(masm);

    MSimdBinaryComp::Operation op = ins->operation();
    switch (op) {
      case MSimdBinaryComp::greaterThan:
        masm.vpcmpgtw(rhs, lhs, output);
        return;
      case MSimdBinaryComp::equal:
        masm.vpcmpeqw(rhs, lhs, output);
        return;
      case MSimdBinaryComp::lessThan:
        // src := rhs
        if (rhs.kind() == Operand::FPREG)
            masm.moveSimd128Int(ToFloatRegister(ins->rhs()), scratch);
        else
            masm.loadAlignedSimd128Int(rhs, scratch);

        // src := src > lhs (i.e. lhs < rhs)
        // Improve by doing custom lowering (rhs is tied to the output register)
        masm.vpcmpgtw(ToOperand(ins->lhs()), scratch, scratch);
        masm.moveSimd128Int(scratch, output);
        return;
      case MSimdBinaryComp::notEqual:
        // Ideally for notEqual, greaterThanOrEqual, and lessThanOrEqual, we
        // should invert the comparison by, e.g. swapping the arms of a select
        // if that's what it's used in.
        masm.loadConstantSimd128Int(allOnes, scratch);
        masm.vpcmpeqw(rhs, lhs, output);
        masm.bitwiseXorSimd128(Operand(scratch), output);
        return;
      case MSimdBinaryComp::greaterThanOrEqual:
        // src := rhs
        if (rhs.kind() == Operand::FPREG)
            masm.moveSimd128Int(ToFloatRegister(ins->rhs()), scratch);
        else
            masm.loadAlignedSimd128Int(rhs, scratch);
        masm.vpcmpgtw(ToOperand(ins->lhs()), scratch, scratch);
        masm.loadConstantSimd128Int(allOnes, output);
        masm.bitwiseXorSimd128(Operand(scratch), output);
        return;
      case MSimdBinaryComp::lessThanOrEqual:
        // lhs <= rhs is equivalent to !(rhs < lhs), which we compute here.
        masm.loadConstantSimd128Int(allOnes, scratch);
        masm.vpcmpgtw(rhs, lhs, output);
        masm.bitwiseXorSimd128(Operand(scratch), output);
        return;
    }
    MOZ_CRASH("unexpected SIMD op");
}

void
CodeGeneratorX86Shared::visitSimdBinaryCompIx4(LSimdBinaryCompIx4* ins)
{
    static const SimdConstant allOnes = SimdConstant::SplatX4(-1);

    FloatRegister lhs = ToFloatRegister(ins->lhs());
    Operand rhs = ToOperand(ins->rhs());
    MOZ_ASSERT(ToFloatRegister(ins->output()) == lhs);

    ScratchSimd128Scope scratch(masm);

    MSimdBinaryComp::Operation op = ins->operation();
    switch (op) {
      case MSimdBinaryComp::greaterThan:
        masm.packedGreaterThanInt32x4(rhs, lhs);
        return;
      case MSimdBinaryComp::equal:
        masm.packedEqualInt32x4(rhs, lhs);
        return;
      case MSimdBinaryComp::lessThan:
        // src := rhs
        if (rhs.kind() == Operand::FPREG)
            masm.moveSimd128Int(ToFloatRegister(ins->rhs()), scratch);
        else
            masm.loadAlignedSimd128Int(rhs, scratch);

        // src := src > lhs (i.e. lhs < rhs)
        // Improve by doing custom lowering (rhs is tied to the output register)
        masm.packedGreaterThanInt32x4(ToOperand(ins->lhs()), scratch);
        masm.moveSimd128Int(scratch, lhs);
        return;
      case MSimdBinaryComp::notEqual:
        // Ideally for notEqual, greaterThanOrEqual, and lessThanOrEqual, we
        // should invert the comparison by, e.g. swapping the arms of a select
        // if that's what it's used in.
        masm.loadConstantSimd128Int(allOnes, scratch);
        masm.packedEqualInt32x4(rhs, lhs);
        masm.bitwiseXorSimd128(Operand(scratch), lhs);
        return;
      case MSimdBinaryComp::greaterThanOrEqual:
        // src := rhs
        if (rhs.kind() == Operand::FPREG)
            masm.moveSimd128Int(ToFloatRegister(ins->rhs()), scratch);
        else
            masm.loadAlignedSimd128Int(rhs, scratch);
        masm.packedGreaterThanInt32x4(ToOperand(ins->lhs()), scratch);
        masm.loadConstantSimd128Int(allOnes, lhs);
        masm.bitwiseXorSimd128(Operand(scratch), lhs);
        return;
      case MSimdBinaryComp::lessThanOrEqual:
        // lhs <= rhs is equivalent to !(rhs < lhs), which we compute here.
        masm.loadConstantSimd128Int(allOnes, scratch);
        masm.packedGreaterThanInt32x4(rhs, lhs);
        masm.bitwiseXorSimd128(Operand(scratch), lhs);
        return;
    }
    MOZ_CRASH("unexpected SIMD op");
}

void
CodeGeneratorX86Shared::visitSimdBinaryCompFx4(LSimdBinaryCompFx4* ins)
{
    FloatRegister lhs = ToFloatRegister(ins->lhs());
    Operand rhs = ToOperand(ins->rhs());
    FloatRegister output = ToFloatRegister(ins->output());

    MSimdBinaryComp::Operation op = ins->operation();
    switch (op) {
      case MSimdBinaryComp::equal:
        masm.vcmpeqps(rhs, lhs, output);
        return;
      case MSimdBinaryComp::lessThan:
        masm.vcmpltps(rhs, lhs, output);
        return;
      case MSimdBinaryComp::lessThanOrEqual:
        masm.vcmpleps(rhs, lhs, output);
        return;
      case MSimdBinaryComp::notEqual:
        masm.vcmpneqps(rhs, lhs, output);
        return;
      case MSimdBinaryComp::greaterThanOrEqual:
      case MSimdBinaryComp::greaterThan:
        // We reverse these before register allocation so that we don't have to
        // copy into and out of temporaries after codegen.
        MOZ_CRASH("lowering should have reversed this");
    }
    MOZ_CRASH("unexpected SIMD op");
}

void
CodeGeneratorX86Shared::visitSimdBinaryArithIx16(LSimdBinaryArithIx16* ins)
{
    FloatRegister lhs = ToFloatRegister(ins->lhs());
    Operand rhs = ToOperand(ins->rhs());
    FloatRegister output = ToFloatRegister(ins->output());

    MSimdBinaryArith::Operation op = ins->operation();
    switch (op) {
      case MSimdBinaryArith::Op_add:
        masm.vpaddb(rhs, lhs, output);
        return;
      case MSimdBinaryArith::Op_sub:
        masm.vpsubb(rhs, lhs, output);
        return;
      case MSimdBinaryArith::Op_mul:
        // 8x16 mul is a valid operation, but not supported in SSE or AVX.
        // The operation is synthesized from 16x8 multiplies by
        // MSimdBinaryArith::AddLegalized().
        break;
      case MSimdBinaryArith::Op_div:
      case MSimdBinaryArith::Op_max:
      case MSimdBinaryArith::Op_min:
      case MSimdBinaryArith::Op_minNum:
      case MSimdBinaryArith::Op_maxNum:
        break;
    }
    MOZ_CRASH("unexpected SIMD op");
}

void
CodeGeneratorX86Shared::visitSimdBinaryArithIx8(LSimdBinaryArithIx8* ins)
{
    FloatRegister lhs = ToFloatRegister(ins->lhs());
    Operand rhs = ToOperand(ins->rhs());
    FloatRegister output = ToFloatRegister(ins->output());

    MSimdBinaryArith::Operation op = ins->operation();
    switch (op) {
      case MSimdBinaryArith::Op_add:
        masm.vpaddw(rhs, lhs, output);
        return;
      case MSimdBinaryArith::Op_sub:
        masm.vpsubw(rhs, lhs, output);
        return;
      case MSimdBinaryArith::Op_mul:
        masm.vpmullw(rhs, lhs, output);
        return;
      case MSimdBinaryArith::Op_div:
      case MSimdBinaryArith::Op_max:
      case MSimdBinaryArith::Op_min:
      case MSimdBinaryArith::Op_minNum:
      case MSimdBinaryArith::Op_maxNum:
        break;
    }
    MOZ_CRASH("unexpected SIMD op");
}

void
CodeGeneratorX86Shared::visitSimdBinaryArithIx4(LSimdBinaryArithIx4* ins)
{
    FloatRegister lhs = ToFloatRegister(ins->lhs());
    Operand rhs = ToOperand(ins->rhs());
    FloatRegister output = ToFloatRegister(ins->output());

    ScratchSimd128Scope scratch(masm);

    MSimdBinaryArith::Operation op = ins->operation();
    switch (op) {
      case MSimdBinaryArith::Op_add:
        masm.vpaddd(rhs, lhs, output);
        return;
      case MSimdBinaryArith::Op_sub:
        masm.vpsubd(rhs, lhs, output);
        return;
      case MSimdBinaryArith::Op_mul: {
        if (AssemblerX86Shared::HasSSE41()) {
            masm.vpmulld(rhs, lhs, output);
            return;
        }

        masm.loadAlignedSimd128Int(rhs, scratch);
        masm.vpmuludq(lhs, scratch, scratch);
        // scratch contains (Rx, _, Rz, _) where R is the resulting vector.

        FloatRegister temp = ToFloatRegister(ins->temp());
        masm.vpshufd(MacroAssembler::ComputeShuffleMask(1, 1, 3, 3), lhs, lhs);
        masm.vpshufd(MacroAssembler::ComputeShuffleMask(1, 1, 3, 3), rhs, temp);
        masm.vpmuludq(temp, lhs, lhs);
        // lhs contains (Ry, _, Rw, _) where R is the resulting vector.

        masm.vshufps(MacroAssembler::ComputeShuffleMask(0, 2, 0, 2), scratch, lhs, lhs);
        // lhs contains (Ry, Rw, Rx, Rz)
        masm.vshufps(MacroAssembler::ComputeShuffleMask(2, 0, 3, 1), lhs, lhs, lhs);
        return;
      }
      case MSimdBinaryArith::Op_div:
        // x86 doesn't have SIMD i32 div.
        break;
      case MSimdBinaryArith::Op_max:
        // we can do max with a single instruction only if we have SSE4.1
        // using the PMAXSD instruction.
        break;
      case MSimdBinaryArith::Op_min:
        // we can do max with a single instruction only if we have SSE4.1
        // using the PMINSD instruction.
        break;
      case MSimdBinaryArith::Op_minNum:
      case MSimdBinaryArith::Op_maxNum:
        break;
    }
    MOZ_CRASH("unexpected SIMD op");
}

void
CodeGeneratorX86Shared::visitSimdBinaryArithFx4(LSimdBinaryArithFx4* ins)
{
    FloatRegister lhs = ToFloatRegister(ins->lhs());
    Operand rhs = ToOperand(ins->rhs());
    FloatRegister output = ToFloatRegister(ins->output());

    ScratchSimd128Scope scratch(masm);

    MSimdBinaryArith::Operation op = ins->operation();
    switch (op) {
      case MSimdBinaryArith::Op_add:
        masm.vaddps(rhs, lhs, output);
        return;
      case MSimdBinaryArith::Op_sub:
        masm.vsubps(rhs, lhs, output);
        return;
      case MSimdBinaryArith::Op_mul:
        masm.vmulps(rhs, lhs, output);
        return;
      case MSimdBinaryArith::Op_div:
        masm.vdivps(rhs, lhs, output);
        return;
      case MSimdBinaryArith::Op_max: {
        FloatRegister lhsCopy = masm.reusedInputFloat32x4(lhs, scratch);
        masm.vcmpunordps(rhs, lhsCopy, scratch);

        FloatRegister tmp = ToFloatRegister(ins->temp());
        FloatRegister rhsCopy = masm.reusedInputAlignedFloat32x4(rhs, tmp);
        masm.vmaxps(Operand(lhs), rhsCopy, tmp);
        masm.vmaxps(rhs, lhs, output);

        masm.vandps(tmp, output, output);
        masm.vorps(scratch, output, output); // or in the all-ones NaNs
        return;
      }
      case MSimdBinaryArith::Op_min: {
        FloatRegister rhsCopy = masm.reusedInputAlignedFloat32x4(rhs, scratch);
        masm.vminps(Operand(lhs), rhsCopy, scratch);
        masm.vminps(rhs, lhs, output);
        masm.vorps(scratch, output, output); // NaN or'd with arbitrary bits is NaN
        return;
      }
      case MSimdBinaryArith::Op_minNum: {
        FloatRegister tmp = ToFloatRegister(ins->temp());
        masm.loadConstantSimd128Int(SimdConstant::SplatX4(int32_t(0x80000000)), tmp);

        FloatRegister mask = scratch;
        FloatRegister tmpCopy = masm.reusedInputFloat32x4(tmp, scratch);
        masm.vpcmpeqd(Operand(lhs), tmpCopy, mask);
        masm.vandps(tmp, mask, mask);

        FloatRegister lhsCopy = masm.reusedInputFloat32x4(lhs, tmp);
        masm.vminps(rhs, lhsCopy, tmp);
        masm.vorps(mask, tmp, tmp);

        FloatRegister rhsCopy = masm.reusedInputAlignedFloat32x4(rhs, mask);
        masm.vcmpneqps(rhs, rhsCopy, mask);

        if (AssemblerX86Shared::HasAVX()) {
            masm.vblendvps(mask, lhs, tmp, output);
        } else {
            // Emulate vblendvps.
            // With SSE.4.1 we could use blendvps, however it's awkward since
            // it requires the mask to be in xmm0.
            if (lhs != output)
                masm.moveSimd128Float(lhs, output);
            masm.vandps(Operand(mask), output, output);
            masm.vandnps(Operand(tmp), mask, mask);
            masm.vorps(Operand(mask), output, output);
        }
        return;
      }
      case MSimdBinaryArith::Op_maxNum: {
        FloatRegister mask = scratch;
        masm.loadConstantSimd128Int(SimdConstant::SplatX4(0), mask);
        masm.vpcmpeqd(Operand(lhs), mask, mask);

        FloatRegister tmp = ToFloatRegister(ins->temp());
        masm.loadConstantSimd128Int(SimdConstant::SplatX4(int32_t(0x80000000)), tmp);
        masm.vandps(tmp, mask, mask);

        FloatRegister lhsCopy = masm.reusedInputFloat32x4(lhs, tmp);
        masm.vmaxps(rhs, lhsCopy, tmp);
        masm.vandnps(Operand(tmp), mask, mask);

        // Ensure tmp always contains the temporary result
        mask = tmp;
        tmp = scratch;

        FloatRegister rhsCopy = masm.reusedInputAlignedFloat32x4(rhs, mask);
        masm.vcmpneqps(rhs, rhsCopy, mask);

        if (AssemblerX86Shared::HasAVX()) {
            masm.vblendvps(mask, lhs, tmp, output);
        } else {
            // Emulate vblendvps.
            // With SSE.4.1 we could use blendvps, however it's awkward since
            // it requires the mask to be in xmm0.
            if (lhs != output)
                masm.moveSimd128Float(lhs, output);
            masm.vandps(Operand(mask), output, output);
            masm.vandnps(Operand(tmp), mask, mask);
            masm.vorps(Operand(mask), output, output);
        }
        return;
      }
    }
    MOZ_CRASH("unexpected SIMD op");
}

void
CodeGeneratorX86Shared::visitSimdBinarySaturating(LSimdBinarySaturating* ins)
{
    FloatRegister lhs = ToFloatRegister(ins->lhs());
    Operand rhs = ToOperand(ins->rhs());
    FloatRegister output = ToFloatRegister(ins->output());

    SimdSign sign = ins->signedness();
    MOZ_ASSERT(sign != SimdSign::NotApplicable);

    switch (ins->type()) {
      case MIRType::Int8x16:
        switch (ins->operation()) {
          case MSimdBinarySaturating::add:
            if (sign == SimdSign::Signed)
                masm.vpaddsb(rhs, lhs, output);
            else
                masm.vpaddusb(rhs, lhs, output);
            return;
          case MSimdBinarySaturating::sub:
            if (sign == SimdSign::Signed)
                masm.vpsubsb(rhs, lhs, output);
            else
                masm.vpsubusb(rhs, lhs, output);
            return;
        }
        break;

      case MIRType::Int16x8:
        switch (ins->operation()) {
          case MSimdBinarySaturating::add:
            if (sign == SimdSign::Signed)
                masm.vpaddsw(rhs, lhs, output);
            else
                masm.vpaddusw(rhs, lhs, output);
            return;
          case MSimdBinarySaturating::sub:
            if (sign == SimdSign::Signed)
                masm.vpsubsw(rhs, lhs, output);
            else
                masm.vpsubusw(rhs, lhs, output);
            return;
        }
        break;

      default:
        break;
    }
    MOZ_CRASH("unsupported type for SIMD saturating arithmetic");
}

void
CodeGeneratorX86Shared::visitSimdUnaryArithIx16(LSimdUnaryArithIx16* ins)
{
    Operand in = ToOperand(ins->input());
    FloatRegister out = ToFloatRegister(ins->output());

    static const SimdConstant allOnes = SimdConstant::SplatX16(-1);

    switch (ins->operation()) {
      case MSimdUnaryArith::neg:
        masm.zeroSimd128Int(out);
        masm.packedSubInt8(in, out);
        return;
      case MSimdUnaryArith::not_:
        masm.loadConstantSimd128Int(allOnes, out);
        masm.bitwiseXorSimd128(in, out);
        return;
      case MSimdUnaryArith::abs:
      case MSimdUnaryArith::reciprocalApproximation:
      case MSimdUnaryArith::reciprocalSqrtApproximation:
      case MSimdUnaryArith::sqrt:
        break;
    }
    MOZ_CRASH("unexpected SIMD op");
}

void
CodeGeneratorX86Shared::visitSimdUnaryArithIx8(LSimdUnaryArithIx8* ins)
{
    Operand in = ToOperand(ins->input());
    FloatRegister out = ToFloatRegister(ins->output());

    static const SimdConstant allOnes = SimdConstant::SplatX8(-1);

    switch (ins->operation()) {
      case MSimdUnaryArith::neg:
        masm.zeroSimd128Int(out);
        masm.packedSubInt16(in, out);
        return;
      case MSimdUnaryArith::not_:
        masm.loadConstantSimd128Int(allOnes, out);
        masm.bitwiseXorSimd128(in, out);
        return;
      case MSimdUnaryArith::abs:
      case MSimdUnaryArith::reciprocalApproximation:
      case MSimdUnaryArith::reciprocalSqrtApproximation:
      case MSimdUnaryArith::sqrt:
        break;
    }
    MOZ_CRASH("unexpected SIMD op");
}

void
CodeGeneratorX86Shared::visitSimdUnaryArithIx4(LSimdUnaryArithIx4* ins)
{
    Operand in = ToOperand(ins->input());
    FloatRegister out = ToFloatRegister(ins->output());

    static const SimdConstant allOnes = SimdConstant::SplatX4(-1);

    switch (ins->operation()) {
      case MSimdUnaryArith::neg:
        masm.zeroSimd128Int(out);
        masm.packedSubInt32(in, out);
        return;
      case MSimdUnaryArith::not_:
        masm.loadConstantSimd128Int(allOnes, out);
        masm.bitwiseXorSimd128(in, out);
        return;
      case MSimdUnaryArith::abs:
      case MSimdUnaryArith::reciprocalApproximation:
      case MSimdUnaryArith::reciprocalSqrtApproximation:
      case MSimdUnaryArith::sqrt:
        break;
    }
    MOZ_CRASH("unexpected SIMD op");
}

void
CodeGeneratorX86Shared::visitSimdUnaryArithFx4(LSimdUnaryArithFx4* ins)
{
    Operand in = ToOperand(ins->input());
    FloatRegister out = ToFloatRegister(ins->output());

    // All ones but the sign bit
    float signMask = SpecificNaN<float>(0, FloatingPoint<float>::kSignificandBits);
    static const SimdConstant signMasks = SimdConstant::SplatX4(signMask);

    // All ones including the sign bit
    float ones = SpecificNaN<float>(1, FloatingPoint<float>::kSignificandBits);
    static const SimdConstant allOnes = SimdConstant::SplatX4(ones);

    // All zeros but the sign bit
    static const SimdConstant minusZero = SimdConstant::SplatX4(-0.f);

    switch (ins->operation()) {
      case MSimdUnaryArith::abs:
        masm.loadConstantSimd128Float(signMasks, out);
        masm.bitwiseAndSimd128(in, out);
        return;
      case MSimdUnaryArith::neg:
        masm.loadConstantSimd128Float(minusZero, out);
        masm.bitwiseXorSimd128(in, out);
        return;
      case MSimdUnaryArith::not_:
        masm.loadConstantSimd128Float(allOnes, out);
        masm.bitwiseXorSimd128(in, out);
        return;
      case MSimdUnaryArith::reciprocalApproximation:
        masm.packedRcpApproximationFloat32x4(in, out);
        return;
      case MSimdUnaryArith::reciprocalSqrtApproximation:
        masm.packedRcpSqrtApproximationFloat32x4(in, out);
        return;
      case MSimdUnaryArith::sqrt:
        masm.packedSqrtFloat32x4(in, out);
        return;
    }
    MOZ_CRASH("unexpected SIMD op");
}

void
CodeGeneratorX86Shared::visitSimdBinaryBitwise(LSimdBinaryBitwise* ins)
{
    FloatRegister lhs = ToFloatRegister(ins->lhs());
    Operand rhs = ToOperand(ins->rhs());
    FloatRegister output = ToFloatRegister(ins->output());

    MSimdBinaryBitwise::Operation op = ins->operation();
    switch (op) {
      case MSimdBinaryBitwise::and_:
        if (ins->type() == MIRType::Float32x4)
            masm.vandps(rhs, lhs, output);
        else
            masm.vpand(rhs, lhs, output);
        return;
      case MSimdBinaryBitwise::or_:
        if (ins->type() == MIRType::Float32x4)
            masm.vorps(rhs, lhs, output);
        else
            masm.vpor(rhs, lhs, output);
        return;
      case MSimdBinaryBitwise::xor_:
        if (ins->type() == MIRType::Float32x4)
            masm.vxorps(rhs, lhs, output);
        else
            masm.vpxor(rhs, lhs, output);
        return;
    }
    MOZ_CRASH("unexpected SIMD bitwise op");
}

void
CodeGeneratorX86Shared::visitSimdShift(LSimdShift* ins)
{
    FloatRegister out = ToFloatRegister(ins->output());
    MOZ_ASSERT(ToFloatRegister(ins->vector()) == out); // defineReuseInput(0);

    // The shift amount is masked to the number of bits in a lane.
    uint32_t shiftmask = (128u / SimdTypeToLength(ins->type())) - 1;

    // Note that SSE doesn't have instructions for shifting 8x16 vectors.
    // These shifts are synthesized by the MSimdShift::AddLegalized() function.
    const LAllocation* val = ins->value();
    if (val->isConstant()) {
        MOZ_ASSERT(ins->temp()->isBogusTemp());
        Imm32 count(uint32_t(ToInt32(val)) & shiftmask);
        switch (ins->type()) {
          case MIRType::Int16x8:
            switch (ins->operation()) {
              case MSimdShift::lsh:
                masm.packedLeftShiftByScalarInt16x8(count, out);
                return;
              case MSimdShift::rsh:
                masm.packedRightShiftByScalarInt16x8(count, out);
                return;
              case MSimdShift::ursh:
                masm.packedUnsignedRightShiftByScalarInt16x8(count, out);
                return;
            }
            break;
          case MIRType::Int32x4:
            switch (ins->operation()) {
              case MSimdShift::lsh:
                masm.packedLeftShiftByScalarInt32x4(count, out);
                return;
              case MSimdShift::rsh:
                masm.packedRightShiftByScalarInt32x4(count, out);
                return;
              case MSimdShift::ursh:
                masm.packedUnsignedRightShiftByScalarInt32x4(count, out);
                return;
            }
            break;
          default:
            MOZ_CRASH("unsupported type for SIMD shifts");
        }
        MOZ_CRASH("unexpected SIMD bitwise op");
    }

    // Truncate val to 5 bits. We should have a temp register for that.
    MOZ_ASSERT(val->isRegister());
    Register count = ToRegister(ins->temp());
    masm.mov(ToRegister(val), count);
    masm.andl(Imm32(shiftmask), count);
    ScratchFloat32Scope scratch(masm);
    masm.vmovd(count, scratch);

    switch (ins->type()) {
      case MIRType::Int16x8:
        switch (ins->operation()) {
          case MSimdShift::lsh:
            masm.packedLeftShiftByScalarInt16x8(scratch, out);
            return;
          case MSimdShift::rsh:
            masm.packedRightShiftByScalarInt16x8(scratch, out);
            return;
          case MSimdShift::ursh:
            masm.packedUnsignedRightShiftByScalarInt16x8(scratch, out);
            return;
        }
        break;
      case MIRType::Int32x4:
        switch (ins->operation()) {
          case MSimdShift::lsh:
            masm.packedLeftShiftByScalarInt32x4(scratch, out);
            return;
          case MSimdShift::rsh:
            masm.packedRightShiftByScalarInt32x4(scratch, out);
            return;
          case MSimdShift::ursh:
            masm.packedUnsignedRightShiftByScalarInt32x4(scratch, out);
            return;
        }
        break;
      default:
        MOZ_CRASH("unsupported type for SIMD shifts");
    }
    MOZ_CRASH("unexpected SIMD bitwise op");
}

void
CodeGeneratorX86Shared::visitSimdSelect(LSimdSelect* ins)
{
    FloatRegister mask = ToFloatRegister(ins->mask());
    FloatRegister onTrue = ToFloatRegister(ins->lhs());
    FloatRegister onFalse = ToFloatRegister(ins->rhs());
    FloatRegister output = ToFloatRegister(ins->output());
    FloatRegister temp = ToFloatRegister(ins->temp());

    if (onTrue != output)
        masm.vmovaps(onTrue, output);
    if (mask != temp)
        masm.vmovaps(mask, temp);

    MSimdSelect* mir = ins->mir();
    unsigned lanes = SimdTypeToLength(mir->type());

    if (AssemblerX86Shared::HasAVX() && lanes == 4) {
        // TBD: Use vpblendvb for lanes > 4, HasAVX.
        masm.vblendvps(mask, onTrue, onFalse, output);
        return;
    }

    // SSE4.1 has plain blendvps which can do this, but it is awkward
    // to use because it requires the mask to be in xmm0.

    masm.bitwiseAndSimd128(Operand(temp), output);
    masm.bitwiseAndNotSimd128(Operand(onFalse), temp);
    masm.bitwiseOrSimd128(Operand(temp), output);
}

void
CodeGeneratorX86Shared::visitCompareExchangeTypedArrayElement(LCompareExchangeTypedArrayElement* lir)
{
    Register elements = ToRegister(lir->elements());
    AnyRegister output = ToAnyRegister(lir->output());
    Register temp = lir->temp()->isBogusTemp() ? InvalidReg : ToRegister(lir->temp());

    Register oldval = ToRegister(lir->oldval());
    Register newval = ToRegister(lir->newval());

    Scalar::Type arrayType = lir->mir()->arrayType();
    int width = Scalar::byteSize(arrayType);

    if (lir->index()->isConstant()) {
        Address dest(elements, ToInt32(lir->index()) * width);
        masm.compareExchangeJS(arrayType, Synchronization::Full(), dest, oldval, newval, temp, output);
    } else {
        BaseIndex dest(elements, ToRegister(lir->index()), ScaleFromElemWidth(width));
        masm.compareExchangeJS(arrayType, Synchronization::Full(), dest, oldval, newval, temp, output);
    }
}

void
CodeGeneratorX86Shared::visitAtomicExchangeTypedArrayElement(LAtomicExchangeTypedArrayElement* lir)
{
    Register elements = ToRegister(lir->elements());
    AnyRegister output = ToAnyRegister(lir->output());
    Register temp = lir->temp()->isBogusTemp() ? InvalidReg : ToRegister(lir->temp());

    Register value = ToRegister(lir->value());

    Scalar::Type arrayType = lir->mir()->arrayType();
    int width = Scalar::byteSize(arrayType);

    if (lir->index()->isConstant()) {
        Address dest(elements, ToInt32(lir->index()) * width);
        masm.atomicExchangeJS(arrayType, Synchronization::Full(), dest, value, temp, output);
    } else {
        BaseIndex dest(elements, ToRegister(lir->index()), ScaleFromElemWidth(width));
        masm.atomicExchangeJS(arrayType, Synchronization::Full(), dest, value, temp, output);
    }
}

template <typename T>
static inline void
AtomicBinopToTypedArray(MacroAssembler& masm, AtomicOp op, Scalar::Type arrayType,
                        const LAllocation* value, const T& mem, Register temp1,
                        Register temp2, AnyRegister output)
{
    if (value->isConstant()) {
        masm.atomicFetchOpJS(arrayType, Synchronization::Full(), op, Imm32(ToInt32(value)), mem,
                             temp1, temp2, output);
    } else {
        masm.atomicFetchOpJS(arrayType, Synchronization::Full(), op, ToRegister(value), mem, temp1,
                             temp2, output);
    }
}

void
CodeGeneratorX86Shared::visitAtomicTypedArrayElementBinop(LAtomicTypedArrayElementBinop* lir)
{
    MOZ_ASSERT(lir->mir()->hasUses());

    AnyRegister output = ToAnyRegister(lir->output());
    Register elements = ToRegister(lir->elements());
    Register temp1 = lir->temp1()->isBogusTemp() ? InvalidReg : ToRegister(lir->temp1());
    Register temp2 = lir->temp2()->isBogusTemp() ? InvalidReg : ToRegister(lir->temp2());
    const LAllocation* value = lir->value();

    Scalar::Type arrayType = lir->mir()->arrayType();
    int width = Scalar::byteSize(arrayType);

    if (lir->index()->isConstant()) {
        Address mem(elements, ToInt32(lir->index()) * width);
        AtomicBinopToTypedArray(masm, lir->mir()->operation(), arrayType, value, mem, temp1, temp2, output);
    } else {
        BaseIndex mem(elements, ToRegister(lir->index()), ScaleFromElemWidth(width));
        AtomicBinopToTypedArray(masm, lir->mir()->operation(), arrayType, value, mem, temp1, temp2, output);
    }
}

template <typename T>
static inline void
AtomicBinopToTypedArray(MacroAssembler& masm, Scalar::Type arrayType, AtomicOp op,
                        const LAllocation* value, const T& mem)
{
    if (value->isConstant()) {
        masm.atomicEffectOpJS(arrayType, Synchronization::Full(), op, Imm32(ToInt32(value)), mem,
                              InvalidReg);
    } else {
        masm.atomicEffectOpJS(arrayType, Synchronization::Full(), op, ToRegister(value), mem,
                              InvalidReg);
    }
}

void
CodeGeneratorX86Shared::visitAtomicTypedArrayElementBinopForEffect(LAtomicTypedArrayElementBinopForEffect* lir)
{
    MOZ_ASSERT(!lir->mir()->hasUses());

    Register elements = ToRegister(lir->elements());
    const LAllocation* value = lir->value();
    Scalar::Type arrayType = lir->mir()->arrayType();
    int width = Scalar::byteSize(arrayType);

    if (lir->index()->isConstant()) {
        Address mem(elements, ToInt32(lir->index()) * width);
        AtomicBinopToTypedArray(masm, arrayType, lir->mir()->operation(), value, mem);
    } else {
        BaseIndex mem(elements, ToRegister(lir->index()), ScaleFromElemWidth(width));
        AtomicBinopToTypedArray(masm, arrayType, lir->mir()->operation(), value, mem);
    }
}

void
CodeGeneratorX86Shared::visitMemoryBarrier(LMemoryBarrier* ins)
{
    if (ins->type() & MembarStoreLoad)
        masm.storeLoadFence();
}

void
CodeGeneratorX86Shared::setReturnDoubleRegs(LiveRegisterSet* regs)
{
    MOZ_ASSERT(ReturnFloat32Reg.encoding() == X86Encoding::xmm0);
    MOZ_ASSERT(ReturnDoubleReg.encoding() == X86Encoding::xmm0);
    MOZ_ASSERT(ReturnSimd128Reg.encoding() == X86Encoding::xmm0);
    regs->add(ReturnFloat32Reg);
    regs->add(ReturnDoubleReg);
    regs->add(ReturnSimd128Reg);
}

void
CodeGeneratorX86Shared::visitOutOfLineWasmTruncateCheck(OutOfLineWasmTruncateCheck* ool)
{
    FloatRegister input = ool->input();
    Register output = ool->output();
    Register64 output64 = ool->output64();
    MIRType fromType = ool->fromType();
    MIRType toType = ool->toType();
    Label* oolRejoin = ool->rejoin();
    TruncFlags flags = ool->flags();
    wasm::BytecodeOffset off = ool->bytecodeOffset();

    if (fromType == MIRType::Float32) {
        if (toType == MIRType::Int32)
            masm.oolWasmTruncateCheckF32ToI32(input, output, flags, off, oolRejoin);
        else if (toType == MIRType::Int64)
            masm.oolWasmTruncateCheckF32ToI64(input, output64, flags, off, oolRejoin);
        else
            MOZ_CRASH("unexpected type");
    } else if (fromType == MIRType::Double) {
        if (toType == MIRType::Int32)
            masm.oolWasmTruncateCheckF64ToI32(input, output, flags, off, oolRejoin);
        else if (toType == MIRType::Int64)
            masm.oolWasmTruncateCheckF64ToI64(input, output64, flags, off, oolRejoin);
        else
            MOZ_CRASH("unexpected type");
    } else {
        MOZ_CRASH("unexpected type");
    }
}

void
CodeGeneratorX86Shared::canonicalizeIfDeterministic(Scalar::Type type, const LAllocation* value)
{
#ifdef JS_MORE_DETERMINISTIC
    switch (type) {
      case Scalar::Float32: {
        FloatRegister in = ToFloatRegister(value);
        masm.canonicalizeFloatIfDeterministic(in);
        break;
      }
      case Scalar::Float64: {
        FloatRegister in = ToFloatRegister(value);
        masm.canonicalizeDoubleIfDeterministic(in);
        break;
      }
      case Scalar::Float32x4: {
        FloatRegister in = ToFloatRegister(value);
        MOZ_ASSERT(in.isSimd128());
        FloatRegister scratch = in != xmm0.asSimd128() ? xmm0 : xmm1;
        masm.push(scratch);
        masm.canonicalizeFloat32x4(in, scratch);
        masm.pop(scratch);
        break;
      }
      default: {
        // Other types don't need canonicalization.
        break;
      }
    }
#endif // JS_MORE_DETERMINISTIC
}

void
CodeGeneratorX86Shared::visitCopySignF(LCopySignF* lir)
{
    FloatRegister lhs = ToFloatRegister(lir->getOperand(0));
    FloatRegister rhs = ToFloatRegister(lir->getOperand(1));

    FloatRegister out = ToFloatRegister(lir->output());

    if (lhs == rhs) {
        if (lhs != out)
            masm.moveFloat32(lhs, out);
        return;
    }

    ScratchFloat32Scope scratch(masm);

    float clearSignMask = BitwiseCast<float>(INT32_MAX);
    masm.loadConstantFloat32(clearSignMask, scratch);
    masm.vandps(scratch, lhs, out);

    float keepSignMask = BitwiseCast<float>(INT32_MIN);
    masm.loadConstantFloat32(keepSignMask, scratch);
    masm.vandps(rhs, scratch, scratch);

    masm.vorps(scratch, out, out);
}

void
CodeGeneratorX86Shared::visitCopySignD(LCopySignD* lir)
{
    FloatRegister lhs = ToFloatRegister(lir->getOperand(0));
    FloatRegister rhs = ToFloatRegister(lir->getOperand(1));

    FloatRegister out = ToFloatRegister(lir->output());

    if (lhs == rhs) {
        if (lhs != out)
            masm.moveDouble(lhs, out);
        return;
    }

    ScratchDoubleScope scratch(masm);

    double clearSignMask = BitwiseCast<double>(INT64_MAX);
    masm.loadConstantDouble(clearSignMask, scratch);
    masm.vandpd(scratch, lhs, out);

    double keepSignMask = BitwiseCast<double>(INT64_MIN);
    masm.loadConstantDouble(keepSignMask, scratch);
    masm.vandpd(rhs, scratch, scratch);

    masm.vorpd(scratch, out, out);
}

void
CodeGeneratorX86Shared::visitRotateI64(LRotateI64* lir)
{
    MRotate* mir = lir->mir();
    LAllocation* count = lir->count();

    Register64 input = ToRegister64(lir->input());
    Register64 output = ToOutRegister64(lir);
    Register temp = ToTempRegisterOrInvalid(lir->temp());

    MOZ_ASSERT(input == output);

    if (count->isConstant()) {
        int32_t c = int32_t(count->toConstant()->toInt64() & 0x3F);
        if (!c)
            return;
        if (mir->isLeftRotate())
            masm.rotateLeft64(Imm32(c), input, output, temp);
        else
            masm.rotateRight64(Imm32(c), input, output, temp);
    } else {
        if (mir->isLeftRotate())
            masm.rotateLeft64(ToRegister(count), input, output, temp);
        else
            masm.rotateRight64(ToRegister(count), input, output, temp);
    }
}

void
CodeGeneratorX86Shared::visitPopcntI64(LPopcntI64* lir)
{
    Register64 input = ToRegister64(lir->getInt64Operand(0));
    Register64 output = ToOutRegister64(lir);
    Register temp = InvalidReg;
    if (!AssemblerX86Shared::HasPOPCNT())
        temp = ToRegister(lir->getTemp(0));

    masm.popcnt64(input, output, temp);
}

} // namespace jit
} // namespace js
