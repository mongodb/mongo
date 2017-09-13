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
    const LAllocation* opd = test->input();

    // Test the operand
    masm.test32(ToRegister(opd), ToRegister(opd));
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
    emitBranch(Assembler::NonZero, baab->ifTrue(), baab->ifFalse());
}

void
CodeGeneratorX86Shared::emitCompare(MCompare::CompareType type, const LAllocation* left, const LAllocation* right)
{
#ifdef JS_CODEGEN_X64
    if (type == MCompare::Compare_Object) {
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
CodeGeneratorX86Shared::visitAsmJSPassStackArg(LAsmJSPassStackArg* ins)
{
    const MAsmJSPassStackArg* mir = ins->mir();
    Address dst(StackPointer, mir->spOffset());
    if (ins->arg()->isConstant()) {
        masm.storePtr(ImmWord(ToInt32(ins->arg())), dst);
    } else {
        if (ins->arg()->isGeneralReg()) {
            masm.storePtr(ToRegister(ins->arg()), dst);
        } else {
            switch (mir->input()->type()) {
              case MIRType_Double:
              case MIRType_Float32:
                masm.storeDouble(ToFloatRegister(ins->arg()), dst);
                return;
              // StackPointer is SIMD-aligned and ABIArgGenerator guarantees
              // stack offsets are SIMD-aligned.
              case MIRType_Int32x4:
                masm.storeAlignedInt32x4(ToFloatRegister(ins->arg()), dst);
                return;
              case MIRType_Float32x4:
                masm.storeAlignedFloat32x4(ToFloatRegister(ins->arg()), dst);
                return;
              default: break;
            }
            MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE("unexpected mir type in AsmJSPassStackArg");
        }
    }
}

void
CodeGeneratorX86Shared::visitOutOfLineLoadTypedArrayOutOfBounds(OutOfLineLoadTypedArrayOutOfBounds* ool)
{
    switch (ool->viewType()) {
      case Scalar::Float32x4:
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
CodeGeneratorX86Shared::visitOffsetBoundsCheck(OffsetBoundsCheck* oolCheck)
{
    // The access is heap[ptr + offset]. The inline code checks that
    // ptr < heap.length - offset. We get here when that fails. We need to check
    // for the case where ptr + offset >= 0, in which case the access is still
    // in bounds.
    MOZ_ASSERT(oolCheck->offset() != 0,
               "An access without a constant offset doesn't need a separate OffsetBoundsCheck");
    masm.cmp32(oolCheck->ptrReg(), Imm32(-uint32_t(oolCheck->offset())));
    masm.j(Assembler::Below, oolCheck->outOfBounds());

#ifdef JS_CODEGEN_X64
    // In order to get the offset to wrap properly, we must sign-extend the
    // pointer to 32-bits. We'll zero out the sign extension immediately
    // after the access to restore asm.js invariants.
    masm.movslq(oolCheck->ptrReg(), oolCheck->ptrReg());
#endif

    masm.jmp(oolCheck->rejoin());
}

uint32_t
CodeGeneratorX86Shared::emitAsmJSBoundsCheckBranch(const MAsmJSHeapAccess* access,
                                                   const MInstruction* mir,
                                                   Register ptr, Label* fail)
{
    // Emit a bounds-checking branch for |access|.

    MOZ_ASSERT(gen->needsAsmJSBoundsCheckBranch(access));

    Label* pass = nullptr;

    // If we have a non-zero offset, it's possible that |ptr| itself is out of
    // bounds, while adding the offset computes an in-bounds address. To catch
    // this case, we need a second branch, which we emit out of line since it's
    // unlikely to be needed in normal programs.
    if (access->offset() != 0) {
        OffsetBoundsCheck* oolCheck = new(alloc()) OffsetBoundsCheck(fail, ptr, access->offset());
        fail = oolCheck->entry();
        pass = oolCheck->rejoin();
        addOutOfLineCode(oolCheck, mir);
    }

    // The bounds check is a comparison with an immediate value. The asm.js
    // module linking process will add the length of the heap to the immediate
    // field, so -access->endOffset() will turn into
    // (heapLength - access->endOffset()), allowing us to test whether the end
    // of the access is beyond the end of the heap.
    uint32_t maybeCmpOffset = masm.cmp32WithPatch(ptr, Imm32(-access->endOffset())).offset();
    masm.j(Assembler::Above, fail);

    if (pass)
        masm.bind(pass);

    return maybeCmpOffset;
}

void
CodeGeneratorX86Shared::cleanupAfterAsmJSBoundsCheckBranch(const MAsmJSHeapAccess* access,
                                                           Register ptr)
{
    // Clean up after performing a heap access checked by a branch.

    MOZ_ASSERT(gen->needsAsmJSBoundsCheckBranch(access));

#ifdef JS_CODEGEN_X64
    // If the offset is 0, we don't use an OffsetBoundsCheck.
    if (access->offset() != 0) {
        // Zero out the high 32 bits, in case the OffsetBoundsCheck code had to
        // sign-extend (movslq) the pointer value to get wraparound to work.
        masm.movl(ptr, ptr);
    }
#endif
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

        JitCode* handler = gen->jitRuntime()->getGenericBailoutHandler();
        masm.jmp(ImmPtr(handler->raw()), Relocation::JITCODE);
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
        binder(masm, deoptTable_->raw() + snapshot->bailoutId() * BAILOUT_TABLE_ENTRY_SIZE);
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
    MOZ_ASSERT(label->used() && !label->bound());
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

    Label done, nan, minMaxInst;

    // Do a vucomisd to catch equality and NaNs, which both require special
    // handling. If the operands are ordered and inequal, we branch straight to
    // the min/max instruction. If we wanted, we could also branch for less-than
    // or greater-than here instead of using min/max, however these conditions
    // will sometimes be hard on the branch predictor.
    masm.vucomisd(second, first);
    masm.j(Assembler::NotEqual, &minMaxInst);
    if (!ins->mir()->range() || ins->mir()->range()->canBeNaN())
        masm.j(Assembler::Parity, &nan);

    // Ordered and equal. The operands are bit-identical unless they are zero
    // and negative zero. These instructions merge the sign bits in that
    // case, and are no-ops otherwise.
    if (ins->mir()->isMax())
        masm.vandpd(second, first, first);
    else
        masm.vorpd(second, first, first);
    masm.jump(&done);

    // x86's min/max are not symmetric; if either operand is a NaN, they return
    // the read-only operand. We need to return a NaN if either operand is a
    // NaN, so we explicitly check for a NaN in the read-write operand.
    if (!ins->mir()->range() || ins->mir()->range()->canBeNaN()) {
        masm.bind(&nan);
        masm.vucomisd(first, first);
        masm.j(Assembler::Parity, &done);
    }

    // When the values are inequal, or second is NaN, x86's min and max will
    // return the value we need.
    masm.bind(&minMaxInst);
    if (ins->mir()->isMax())
        masm.vmaxsd(second, first, first);
    else
        masm.vminsd(second, first, first);

    masm.bind(&done);
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

    Label done, nan, minMaxInst;

    // Do a vucomiss to catch equality and NaNs, which both require special
    // handling. If the operands are ordered and inequal, we branch straight to
    // the min/max instruction. If we wanted, we could also branch for less-than
    // or greater-than here instead of using min/max, however these conditions
    // will sometimes be hard on the branch predictor.
    masm.vucomiss(second, first);
    masm.j(Assembler::NotEqual, &minMaxInst);
    if (!ins->mir()->range() || ins->mir()->range()->canBeNaN())
        masm.j(Assembler::Parity, &nan);

    // Ordered and equal. The operands are bit-identical unless they are zero
    // and negative zero. These instructions merge the sign bits in that
    // case, and are no-ops otherwise.
    if (ins->mir()->isMax())
        masm.vandps(second, first, first);
    else
        masm.vorps(second, first, first);
    masm.jump(&done);

    // x86's min/max are not symmetric; if either operand is a NaN, they return
    // the read-only operand. We need to return a NaN if either operand is a
    // NaN, so we explicitly check for a NaN in the read-write operand.
    if (!ins->mir()->range() || ins->mir()->range()->canBeNaN()) {
        masm.bind(&nan);
        masm.vucomiss(first, first);
        masm.j(Assembler::Parity, &done);
    }

    // When the values are inequal, or second is NaN, x86's min and max will
    // return the value we need.
    masm.bind(&minMaxInst);
    if (ins->mir()->isMax())
        masm.vmaxss(second, first, first);
    else
        masm.vminss(second, first, first);

    masm.bind(&done);
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

    // bsr is undefined on 0
    Label done, nonzero;
    if (!ins->mir()->operandIsNeverZero()) {
        masm.test32(input, input);
        masm.j(Assembler::NonZero, &nonzero);
        masm.move32(Imm32(32), output);
        masm.jump(&done);
    }

    masm.bind(&nonzero);
    masm.bsr(input, output);
    masm.xor32(Imm32(0x1F), output);
    masm.bind(&done);
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
        masm.zeroDouble(input);
        masm.subDouble(scratch, input);
        masm.jump(&done);

        masm.bind(&sqrt);
    }

    if (!ins->mir()->operandIsNeverNegativeZero()) {
        // Math.pow(-0, 0.5) == 0 == Math.pow(0, 0.5). Adding 0 converts any -0 to 0.
        masm.zeroDouble(scratch);
        masm.addDouble(scratch, input);
    }

    masm.vsqrtsd(input, output, output);

    masm.bind(&done);
}

class OutOfLineUndoALUOperation : public OutOfLineCodeBase<CodeGeneratorX86Shared>
{
    LInstruction* ins_;

  public:
    explicit OutOfLineUndoALUOperation(LInstruction* ins)
        : ins_(ins)
    { }

    virtual void accept(CodeGeneratorX86Shared* codegen) {
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
CodeGeneratorX86Shared::visitOutOfLineUndoALUOperation(OutOfLineUndoALUOperation* ool)
{
    LInstruction* ins = ool->ins();
    Register reg = ToRegister(ins->getDef(0));

    mozilla::DebugOnly<LAllocation*> lhs = ins->getOperand(0);
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

    virtual void accept(CodeGeneratorX86Shared* codegen) {
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

class ReturnZero : public OutOfLineCodeBase<CodeGeneratorX86Shared>
{
    Register reg_;

  public:
    explicit ReturnZero(Register reg)
      : reg_(reg)
    { }

    virtual void accept(CodeGeneratorX86Shared* codegen) {
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
            if (!ool)
                ool = new(alloc()) ReturnZero(output);
            masm.j(Assembler::Zero, ool->entry());
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
        if (ins->mir()->isTruncated())
            masm.xorl(output, output);
        else
            bailout(ins->snapshot());

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
    mozilla::DebugOnly<Register> output = ToRegister(ins->output());

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

    if (shift != 0) {
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
    } else if (shift == 0) {
        if (negativeDivisor) {
            // INT32_MIN / -1 overflows.
            masm.negl(lhs);
            if (!mir->isTruncated())
                bailoutIf(Assembler::Overflow, ins->snapshot());
        }

        else if (mir->isUnsigned() && !mir->isTruncated()) {
            // Unsigned division by 1 can overflow if output is not
            // truncated.
            masm.test32(lhs, lhs);
            bailoutIf(Assembler::Signed, ins->snapshot());
        }
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
        if (mir->canTruncateInfinities()) {
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
        Label notmin;
        masm.cmp32(lhs, Imm32(INT32_MIN));
        masm.j(Assembler::NotEqual, &notmin);
        masm.cmp32(rhs, Imm32(-1));
        if (mir->canTruncateOverflow()) {
            // (-INT32_MIN)|0 == INT32_MIN and INT32_MIN is already in the
            // output register (lhs == eax).
            masm.j(Assembler::Equal, &done);
        } else {
            MOZ_ASSERT(mir->fallible());
            bailoutIf(Assembler::Equal, ins->snapshot());
        }
        masm.bind(&notmin);
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

    virtual void accept(CodeGeneratorX86Shared* codegen) {
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

    // Prevent divide by zero.
    if (ins->mir()->canBeDivideByZero()) {
        masm.test32(rhs, rhs);
        if (ins->mir()->isTruncated()) {
            if (!ool)
                ool = new(alloc()) ReturnZero(edx);
            masm.j(Assembler::Zero, ool->entry());
        } else {
            bailoutIf(Assembler::Zero, ins->snapshot());
        }
    }

    Label negative;

    // Switch based on sign of the lhs.
    if (ins->mir()->canBeNegativeDividend())
        masm.branchTest32(Assembler::Signed, lhs, lhs, &negative);

    // If lhs >= 0 then remainder = lhs % rhs. The remainder must be positive.
    {
        // Check if rhs is a power-of-two.
        if (ins->mir()->canBePowerOfTwoDivisor()) {
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
    if (ins->mir()->canBeNegativeDividend()) {
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

        if (!ins->mir()->isTruncated()) {
            // A remainder of 0 means that the rval must be -0, which is a double.
            masm.test32(remainder, remainder);
            bailoutIf(Assembler::Zero, ins->snapshot());
        }
    }

    masm.bind(&done);

    if (overflow) {
        addOutOfLineCode(overflow, ins->mir());
        masm.bind(overflow->done());
    }

    if (ool) {
        addOutOfLineCode(ool, ins->mir());
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

    void accept(CodeGeneratorX86Shared* codegen) {
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
    masm.use(ool->jumpLabel()->target());
    masm.addCodeLabel(*ool->jumpLabel());

    for (size_t i = 0; i < mir->numCases(); i++) {
        LBlock* caseblock = skipTrivialBlocks(mir->getCase(i))->lir();
        Label* caseheader = caseblock->label();
        uint32_t caseoffset = caseheader->offset();

        // The entries of the jump table need to be absolute addresses and thus
        // must be patched after codegen is finished.
        CodeLabel cl;
        masm.writeCodePointer(cl.patchAt());
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
    masm.mov(ool->jumpLabel()->patchAt(), base);
    Operand pointer = Operand(base, index, ScalePointer);

    // Jump to the right case
    masm.jmp(pointer);
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
CodeGeneratorX86Shared::visitGuardShape(LGuardShape* guard)
{
    Register obj = ToRegister(guard->input());
    masm.cmpPtr(Operand(obj, JSObject::offsetOfShape()), ImmGCPtr(guard->mir()->shape()));

    bailoutIf(Assembler::NotEqual, guard->snapshot());
}

void
CodeGeneratorX86Shared::visitGuardObjectGroup(LGuardObjectGroup* guard)
{
    Register obj = ToRegister(guard->input());

    masm.cmpPtr(Operand(obj, JSObject::offsetOfGroup()), ImmGCPtr(guard->mir()->group()));

    Assembler::Condition cond =
        guard->mir()->bailOnEquality() ? Assembler::Equal : Assembler::NotEqual;
    bailoutIf(cond, guard->snapshot());
}

void
CodeGeneratorX86Shared::visitGuardClass(LGuardClass* guard)
{
    Register obj = ToRegister(guard->input());
    Register tmp = ToRegister(guard->tempInt());

    masm.loadPtr(Address(obj, JSObject::offsetOfGroup()), tmp);
    masm.cmpPtr(Operand(tmp, ObjectGroup::offsetOfClasp()), ImmPtr(guard->mir()->getClass()));
    bailoutIf(Assembler::NotEqual, guard->snapshot());
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
    JitCode* thunk = gen->jitRuntime()->getInvalidationThunk();

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
CodeGeneratorX86Shared::visitInt32x4(LInt32x4* ins)
{
    const LDefinition* out = ins->getDef(0);
    masm.loadConstantInt32x4(ins->getValue(), ToFloatRegister(out));
}

void
CodeGeneratorX86Shared::visitFloat32x4(LFloat32x4* ins)
{
    const LDefinition* out = ins->getDef(0);
    masm.loadConstantFloat32x4(ins->getValue(), ToFloatRegister(out));
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

    OutOfLineSimdFloatToIntCheck *ool = new(alloc()) OutOfLineSimdFloatToIntCheck(temp, in, ins);
    addOutOfLineCode(ool, ins->mir());

    static const SimdConstant InvalidResult = SimdConstant::SplatX4(int32_t(-2147483648));

    ScratchSimd128Scope scratch(masm);
    masm.loadConstantInt32x4(InvalidResult, scratch);
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

    Label bail;
    Label* onConversionError = gen->compilingAsmJS() ? masm.asmOnConversionErrorLabel() : &bail;

    FloatRegister input = ool->input();
    Register temp = ool->temp();

    ScratchSimd128Scope scratch(masm);
    masm.loadConstantFloat32x4(Int32MinX4, scratch);
    masm.vcmpleps(Operand(input), scratch, scratch);
    masm.vmovmskps(scratch, temp);
    masm.cmp32(temp, Imm32(15));
    masm.j(Assembler::NotEqual, onConversionError);

    masm.loadConstantFloat32x4(Int32MaxX4, scratch);
    masm.vcmpleps(Operand(input), scratch, scratch);
    masm.vmovmskps(scratch, temp);
    masm.cmp32(temp, Imm32(0));
    masm.j(Assembler::NotEqual, onConversionError);

    masm.jump(ool->rejoin());

    if (bail.used()) {
        masm.bind(&bail);
        bailout(ool->ins()->snapshot());
    }
}

void
CodeGeneratorX86Shared::visitSimdValueInt32x4(LSimdValueInt32x4* ins)
{
    MOZ_ASSERT(ins->mir()->type() == MIRType_Int32x4);

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
    masm.loadAlignedInt32x4(Address(StackPointer, 0), output);
    masm.freeStack(Simd128DataSize);
}

void
CodeGeneratorX86Shared::visitSimdValueFloat32x4(LSimdValueFloat32x4* ins)
{
    MOZ_ASSERT(ins->mir()->type() == MIRType_Float32x4);

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
CodeGeneratorX86Shared::visitSimdSplatX4(LSimdSplatX4* ins)
{
    FloatRegister output = ToFloatRegister(ins->output());

    MSimdSplatX4* mir = ins->mir();
    MOZ_ASSERT(IsSimdType(mir->type()));
    JS_STATIC_ASSERT(sizeof(float) == sizeof(int32_t));

    switch (mir->type()) {
      case MIRType_Int32x4: {
        Register r = ToRegister(ins->getOperand(0));
        masm.vmovd(r, output);
        masm.vpshufd(0, output, output);
        break;
      }
      case MIRType_Float32x4: {
        FloatRegister r = ToFloatRegister(ins->getOperand(0));
        FloatRegister rCopy = masm.reusedInputFloat32x4(r, output);
        masm.vshufps(0, rCopy, rCopy, output);
        break;
      }
      default:
        MOZ_CRASH("Unknown SIMD kind");
    }
}

void
CodeGeneratorX86Shared::visitSimdReinterpretCast(LSimdReinterpretCast* ins)
{
    FloatRegister input = ToFloatRegister(ins->input());
    FloatRegister output = ToFloatRegister(ins->output());

    if (input.aliases(output))
        return;

    switch (ins->mir()->type()) {
      case MIRType_Int32x4:
        masm.vmovdqa(input, output);
        break;
      case MIRType_Float32x4:
        masm.vmovaps(input, output);
        break;
      default:
        MOZ_CRASH("Unknown SIMD kind");
    }
}

void
CodeGeneratorX86Shared::visitSimdExtractElementI(LSimdExtractElementI* ins)
{
    FloatRegister input = ToFloatRegister(ins->input());
    Register output = ToRegister(ins->output());

    SimdLane lane = ins->lane();
    if (lane == LaneX) {
        // The value we want to extract is in the low double-word
        masm.moveLowInt32(input, output);
    } else if (AssemblerX86Shared::HasSSE41()) {
        masm.vpextrd(lane, input, output);
    } else {
        uint32_t mask = MacroAssembler::ComputeShuffleMask(lane);
        ScratchSimd128Scope scratch(masm);
        masm.shuffleInt32(mask, input, scratch);
        masm.moveLowInt32(scratch, output);
    }
}

void
CodeGeneratorX86Shared::visitSimdExtractElementF(LSimdExtractElementF* ins)
{
    FloatRegister input = ToFloatRegister(ins->input());
    FloatRegister output = ToFloatRegister(ins->output());

    SimdLane lane = ins->lane();
    if (lane == LaneX) {
        // The value we want to extract is in the low double-word
        if (input != output)
            masm.moveFloat32(input, output);
    } else if (lane == LaneZ) {
        masm.moveHighPairToLowPairFloat32(input, output);
    } else {
        uint32_t mask = MacroAssembler::ComputeShuffleMask(lane);
        masm.shuffleFloat32(mask, input, output);
    }
    // NaNs contained within SIMD values are not enforced to be canonical, so
    // when we extract an element into a "regular" scalar JS value, we have to
    // canonicalize. In asm.js code, we can skip this, as asm.js only has to
    // canonicalize NaNs at FFI boundaries.
    if (!gen->compilingAsmJS())
        masm.canonicalizeFloat(output);
}

void
CodeGeneratorX86Shared::visitSimdInsertElementI(LSimdInsertElementI* ins)
{
    FloatRegister vector = ToFloatRegister(ins->vector());
    Register value = ToRegister(ins->value());
    FloatRegister output = ToFloatRegister(ins->output());
    MOZ_ASSERT(vector == output); // defineReuseInput(0)

    unsigned component = unsigned(ins->lane());

    // Note that, contrarily to float32x4, we cannot use vmovd if the inserted
    // value goes into the first component, as vmovd clears out the higher lanes
    // of the output.
    if (AssemblerX86Shared::HasSSE41()) {
        // TODO: Teach Lowering that we don't need defineReuseInput if we have AVX.
        masm.vpinsrd(component, value, vector, output);
        return;
    }

    masm.reserveStack(Simd128DataSize);
    masm.storeAlignedInt32x4(vector, Address(StackPointer, 0));
    masm.store32(value, Address(StackPointer, component * sizeof(int32_t)));
    masm.loadAlignedInt32x4(Address(StackPointer, 0), output);
    masm.freeStack(Simd128DataSize);
}

void
CodeGeneratorX86Shared::visitSimdInsertElementF(LSimdInsertElementF* ins)
{
    FloatRegister vector = ToFloatRegister(ins->vector());
    FloatRegister value = ToFloatRegister(ins->value());
    FloatRegister output = ToFloatRegister(ins->output());
    MOZ_ASSERT(vector == output); // defineReuseInput(0)

    if (ins->lane() == SimdLane::LaneX) {
        // As both operands are registers, vmovss doesn't modify the upper bits
        // of the destination operand.
        if (value != output)
            masm.vmovss(value, vector, output);
        return;
    }

    if (AssemblerX86Shared::HasSSE41()) {
        // The input value is in the low float32 of the 'value' FloatRegister.
        masm.vinsertps(masm.vinsertpsMask(SimdLane::LaneX, ins->lane()), value, output, output);
        return;
    }

    unsigned component = unsigned(ins->lane());
    masm.reserveStack(Simd128DataSize);
    masm.storeAlignedFloat32x4(vector, Address(StackPointer, 0));
    masm.storeFloat32(value, Address(StackPointer, component * sizeof(int32_t)));
    masm.loadAlignedFloat32x4(Address(StackPointer, 0), output);
    masm.freeStack(Simd128DataSize);
}

void
CodeGeneratorX86Shared::visitSimdSignMaskX4(LSimdSignMaskX4* ins)
{
    FloatRegister input = ToFloatRegister(ins->input());
    Register output = ToRegister(ins->output());

    // For Float32x4 and Int32x4.
    masm.vmovmskps(input, output);
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

    for (size_t i = 0; i < mir->numLanes(); i++) {
        Operand lane = ToOperand(ins->lane(i));

        masm.cmp32(lane, Imm32(numVectors * mir->numLanes() - 1));
        masm.j(Assembler::Above, &bail);

        if (lane.kind() == Operand::REG) {
            masm.loadScalar<T>(Operand(StackPointer, ToRegister(ins->lane(i)), TimesFour, Simd128DataSize),
                               tempRegister);
        } else {
            masm.load32(lane, laneTemp);
            masm.loadScalar<T>(Operand(StackPointer, laneTemp, TimesFour, Simd128DataSize), tempRegister);
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
    visitSimdGeneralShuffle<int32_t, Register>(ins, ToRegister(ins->temp()));
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

    uint32_t x = ins->laneX();
    uint32_t y = ins->laneY();
    uint32_t z = ins->laneZ();
    uint32_t w = ins->laneW();

    uint32_t mask = MacroAssembler::ComputeShuffleMask(x, y, z, w);
    masm.shuffleInt32(mask, input, output);
}

void
CodeGeneratorX86Shared::visitSimdSwizzleF(LSimdSwizzleF* ins)
{
    FloatRegister input = ToFloatRegister(ins->input());
    FloatRegister output = ToFloatRegister(ins->output());

    uint32_t x = ins->laneX();
    uint32_t y = ins->laneY();
    uint32_t z = ins->laneZ();
    uint32_t w = ins->laneW();

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
    Operand rhs = ToOperand(ins->rhs());
    FloatRegister out = ToFloatRegister(ins->output());

    uint32_t x = ins->laneX();
    uint32_t y = ins->laneY();
    uint32_t z = ins->laneZ();
    uint32_t w = ins->laneW();

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
            SimdLane srcLane;
            SimdLane dstLane;
            if (x >= 4) {
                srcLane = SimdLane(x - 4);
                dstLane = LaneX;
            } else if (y >= 4) {
                srcLane = SimdLane(y - 4);
                dstLane = LaneY;
            } else if (z >= 4) {
                srcLane = SimdLane(z - 4);
                dstLane = LaneZ;
            } else {
                MOZ_ASSERT(w >= 4);
                srcLane = SimdLane(w - 4);
                dstLane = LaneW;
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
                secondMask = MacroAssembler::ComputeShuffleMask(x, y, LaneZ, LaneX);
            } else {
                MOZ_ASSERT(z >= 4);
                z %= 4;
                // T = (Rz Rz Lw Lw) = vshufps(firstMask, lhs, rhs, rhsCopy)
                firstMask = MacroAssembler::ComputeShuffleMask(z, z, w, w);
                // (Lx Ly Rz Lw) = (Lx Ly Tx Tz) = vshufps(secondMask, T, lhs, out)
                secondMask = MacroAssembler::ComputeShuffleMask(x, y, LaneX, LaneZ);
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
            secondMask = MacroAssembler::ComputeShuffleMask(LaneZ, LaneX, z, w);
        } else {
            MOZ_ASSERT(x >= 4);
            x %= 4;
            // T = (Rx Rx Ly Ly) = vshufps(firstMask, lhs, rhs, rhsCopy)
            firstMask = MacroAssembler::ComputeShuffleMask(x, x, y, y);
            // (Rx Ly Lz Lw) = (Tx Tz Lz Lw) = vshufps(secondMask, lhs, T, out)
            secondMask = MacroAssembler::ComputeShuffleMask(LaneX, LaneZ, z, w);
        }

        masm.vshufps(firstMask, lhs, rhsCopy, rhsCopy);
        if (AssemblerX86Shared::HasAVX()) {
            masm.vshufps(secondMask, lhs, rhsCopy, out);
        } else {
            masm.vshufps(secondMask, lhs, rhsCopy, rhsCopy);
            masm.moveFloat32x4(rhsCopy, out);
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
            masm.loadAlignedFloat32x4(rhs, scratch);
            masm.vmovhlps(lhs, scratch, scratch);
            masm.moveFloat32x4(scratch, out);
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
            masm.loadAlignedFloat32x4(rhs, scratch);
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
            masm.loadAlignedFloat32x4(rhs, scratch);
            masm.vunpcklps(lhs, scratch, scratch);
            masm.moveFloat32x4(scratch, out);
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
            masm.loadAlignedFloat32x4(rhs, scratch);
            masm.vunpckhps(lhs, scratch, scratch);
            masm.moveFloat32x4(scratch, out);
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
            masm.moveInt32x4(ToFloatRegister(ins->rhs()), scratch);
        else
            masm.loadAlignedInt32x4(rhs, scratch);

        // src := src > lhs (i.e. lhs < rhs)
        // Improve by doing custom lowering (rhs is tied to the output register)
        masm.packedGreaterThanInt32x4(ToOperand(ins->lhs()), scratch);
        masm.moveInt32x4(scratch, lhs);
        return;
      case MSimdBinaryComp::notEqual:
        // Ideally for notEqual, greaterThanOrEqual, and lessThanOrEqual, we
        // should invert the comparison by, e.g. swapping the arms of a select
        // if that's what it's used in.
        masm.loadConstantInt32x4(allOnes, scratch);
        masm.packedEqualInt32x4(rhs, lhs);
        masm.bitwiseXorX4(Operand(scratch), lhs);
        return;
      case MSimdBinaryComp::greaterThanOrEqual:
        // src := rhs
        if (rhs.kind() == Operand::FPREG)
            masm.moveInt32x4(ToFloatRegister(ins->rhs()), scratch);
        else
            masm.loadAlignedInt32x4(rhs, scratch);
        masm.packedGreaterThanInt32x4(ToOperand(ins->lhs()), scratch);
        masm.loadConstantInt32x4(allOnes, lhs);
        masm.bitwiseXorX4(Operand(scratch), lhs);
        return;
      case MSimdBinaryComp::lessThanOrEqual:
        // lhs <= rhs is equivalent to !(rhs < lhs), which we compute here.
        masm.loadConstantInt32x4(allOnes, scratch);
        masm.packedGreaterThanInt32x4(rhs, lhs);
        masm.bitwiseXorX4(Operand(scratch), lhs);
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

        masm.loadAlignedInt32x4(rhs, scratch);
        masm.vpmuludq(lhs, scratch, scratch);
        // scratch contains (Rx, _, Rz, _) where R is the resulting vector.

        FloatRegister temp = ToFloatRegister(ins->temp());
        masm.vpshufd(MacroAssembler::ComputeShuffleMask(LaneY, LaneY, LaneW, LaneW), lhs, lhs);
        masm.vpshufd(MacroAssembler::ComputeShuffleMask(LaneY, LaneY, LaneW, LaneW), rhs, temp);
        masm.vpmuludq(temp, lhs, lhs);
        // lhs contains (Ry, _, Rw, _) where R is the resulting vector.

        masm.vshufps(MacroAssembler::ComputeShuffleMask(LaneX, LaneZ, LaneX, LaneZ), scratch, lhs, lhs);
        // lhs contains (Ry, Rw, Rx, Rz)
        masm.vshufps(MacroAssembler::ComputeShuffleMask(LaneZ, LaneX, LaneW, LaneY), lhs, lhs, lhs);
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
        masm.loadConstantInt32x4(SimdConstant::SplatX4(int32_t(0x80000000)), tmp);

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
                masm.moveFloat32x4(lhs, output);
            masm.vandps(Operand(mask), output, output);
            masm.vandnps(Operand(tmp), mask, mask);
            masm.vorps(Operand(mask), output, output);
        }
        return;
      }
      case MSimdBinaryArith::Op_maxNum: {
        FloatRegister mask = scratch;
        masm.loadConstantInt32x4(SimdConstant::SplatX4(0), mask);
        masm.vpcmpeqd(Operand(lhs), mask, mask);

        FloatRegister tmp = ToFloatRegister(ins->temp());
        masm.loadConstantInt32x4(SimdConstant::SplatX4(int32_t(0x80000000)), tmp);
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
                masm.moveFloat32x4(lhs, output);
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
CodeGeneratorX86Shared::visitSimdUnaryArithIx4(LSimdUnaryArithIx4* ins)
{
    Operand in = ToOperand(ins->input());
    FloatRegister out = ToFloatRegister(ins->output());

    static const SimdConstant allOnes = SimdConstant::CreateX4(-1, -1, -1, -1);

    switch (ins->operation()) {
      case MSimdUnaryArith::neg:
        masm.zeroInt32x4(out);
        masm.packedSubInt32(in, out);
        return;
      case MSimdUnaryArith::not_:
        masm.loadConstantInt32x4(allOnes, out);
        masm.bitwiseXorX4(in, out);
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
        masm.loadConstantFloat32x4(signMasks, out);
        masm.bitwiseAndX4(in, out);
        return;
      case MSimdUnaryArith::neg:
        masm.loadConstantFloat32x4(minusZero, out);
        masm.bitwiseXorX4(in, out);
        return;
      case MSimdUnaryArith::not_:
        masm.loadConstantFloat32x4(allOnes, out);
        masm.bitwiseXorX4(in, out);
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
CodeGeneratorX86Shared::visitSimdBinaryBitwiseX4(LSimdBinaryBitwiseX4* ins)
{
    FloatRegister lhs = ToFloatRegister(ins->lhs());
    Operand rhs = ToOperand(ins->rhs());
    FloatRegister output = ToFloatRegister(ins->output());

    MSimdBinaryBitwise::Operation op = ins->operation();
    switch (op) {
      case MSimdBinaryBitwise::and_:
        if (ins->type() == MIRType_Float32x4)
            masm.vandps(rhs, lhs, output);
        else
            masm.vpand(rhs, lhs, output);
        return;
      case MSimdBinaryBitwise::or_:
        if (ins->type() == MIRType_Float32x4)
            masm.vorps(rhs, lhs, output);
        else
            masm.vpor(rhs, lhs, output);
        return;
      case MSimdBinaryBitwise::xor_:
        if (ins->type() == MIRType_Float32x4)
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

    // If the shift count is greater than 31, this will just zero all lanes by
    // default for lsh and ursh, and for rsh extend the sign bit to all bits,
    // per the SIMD.js spec (as of March 19th 2015).
    const LAllocation* val = ins->value();
    if (val->isConstant()) {
        uint32_t c = uint32_t(ToInt32(val));
        if (c > 31) {
            switch (ins->operation()) {
              case MSimdShift::lsh:
              case MSimdShift::ursh:
                masm.zeroInt32x4(out);
                return;
              default:
                c = 31;
                break;
            }
        }
        Imm32 count(c);
        switch (ins->operation()) {
          case MSimdShift::lsh:
            masm.packedLeftShiftByScalar(count, out);
            return;
          case MSimdShift::rsh:
            masm.packedRightShiftByScalar(count, out);
            return;
          case MSimdShift::ursh:
            masm.packedUnsignedRightShiftByScalar(count, out);
            return;
        }
        MOZ_CRASH("unexpected SIMD bitwise op");
    }

    MOZ_ASSERT(val->isRegister());
    ScratchFloat32Scope scratch(masm);
    masm.vmovd(ToRegister(val), scratch);

    switch (ins->operation()) {
      case MSimdShift::lsh:
        masm.packedLeftShiftByScalar(scratch, out);
        return;
      case MSimdShift::rsh:
        masm.packedRightShiftByScalar(scratch, out);
        return;
      case MSimdShift::ursh:
        masm.packedUnsignedRightShiftByScalar(scratch, out);
        return;
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
    if (mir->isElementWise()) {
        if (AssemblerX86Shared::HasAVX()) {
            masm.vblendvps(mask, onTrue, onFalse, output);
            return;
        }

        // SSE4.1 has plain blendvps which can do this, but it is awkward
        // to use because it requires the mask to be in xmm0.

        // Propagate sign to all bits of mask vector, if necessary.
        if (!mir->mask()->isSimdBinaryComp())
            masm.packedRightShiftByScalar(Imm32(31), temp);
    }

    masm.bitwiseAndX4(Operand(temp), output);
    masm.bitwiseAndNotX4(Operand(onFalse), temp);
    masm.bitwiseOrX4(Operand(temp), output);
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
        masm.compareExchangeToTypedIntArray(arrayType, dest, oldval, newval, temp, output);
    } else {
        BaseIndex dest(elements, ToRegister(lir->index()), ScaleFromElemWidth(width));
        masm.compareExchangeToTypedIntArray(arrayType, dest, oldval, newval, temp, output);
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
        masm.atomicExchangeToTypedIntArray(arrayType, dest, value, temp, output);
    } else {
        BaseIndex dest(elements, ToRegister(lir->index()), ScaleFromElemWidth(width));
        masm.atomicExchangeToTypedIntArray(arrayType, dest, value, temp, output);
    }
}

template<typename S, typename T>
void
CodeGeneratorX86Shared::atomicBinopToTypedIntArray(AtomicOp op, Scalar::Type arrayType, const S& value,
                                                   const T& mem, Register temp1, Register temp2, AnyRegister output)
{
    switch (arrayType) {
      case Scalar::Int8:
        switch (op) {
          case AtomicFetchAddOp:
            masm.atomicFetchAdd8SignExtend(value, mem, temp1, output.gpr());
            break;
          case AtomicFetchSubOp:
            masm.atomicFetchSub8SignExtend(value, mem, temp1, output.gpr());
            break;
          case AtomicFetchAndOp:
            masm.atomicFetchAnd8SignExtend(value, mem, temp1, output.gpr());
            break;
          case AtomicFetchOrOp:
            masm.atomicFetchOr8SignExtend(value, mem, temp1, output.gpr());
            break;
          case AtomicFetchXorOp:
            masm.atomicFetchXor8SignExtend(value, mem, temp1, output.gpr());
            break;
          default:
            MOZ_CRASH("Invalid typed array atomic operation");
        }
        break;
      case Scalar::Uint8:
        switch (op) {
          case AtomicFetchAddOp:
            masm.atomicFetchAdd8ZeroExtend(value, mem, temp1, output.gpr());
            break;
          case AtomicFetchSubOp:
            masm.atomicFetchSub8ZeroExtend(value, mem, temp1, output.gpr());
            break;
          case AtomicFetchAndOp:
            masm.atomicFetchAnd8ZeroExtend(value, mem, temp1, output.gpr());
            break;
          case AtomicFetchOrOp:
            masm.atomicFetchOr8ZeroExtend(value, mem, temp1, output.gpr());
            break;
          case AtomicFetchXorOp:
            masm.atomicFetchXor8ZeroExtend(value, mem, temp1, output.gpr());
            break;
          default:
            MOZ_CRASH("Invalid typed array atomic operation");
        }
        break;
      case Scalar::Int16:
        switch (op) {
          case AtomicFetchAddOp:
            masm.atomicFetchAdd16SignExtend(value, mem, temp1, output.gpr());
            break;
          case AtomicFetchSubOp:
            masm.atomicFetchSub16SignExtend(value, mem, temp1, output.gpr());
            break;
          case AtomicFetchAndOp:
            masm.atomicFetchAnd16SignExtend(value, mem, temp1, output.gpr());
            break;
          case AtomicFetchOrOp:
            masm.atomicFetchOr16SignExtend(value, mem, temp1, output.gpr());
            break;
          case AtomicFetchXorOp:
            masm.atomicFetchXor16SignExtend(value, mem, temp1, output.gpr());
            break;
          default:
            MOZ_CRASH("Invalid typed array atomic operation");
        }
        break;
      case Scalar::Uint16:
        switch (op) {
          case AtomicFetchAddOp:
            masm.atomicFetchAdd16ZeroExtend(value, mem, temp1, output.gpr());
            break;
          case AtomicFetchSubOp:
            masm.atomicFetchSub16ZeroExtend(value, mem, temp1, output.gpr());
            break;
          case AtomicFetchAndOp:
            masm.atomicFetchAnd16ZeroExtend(value, mem, temp1, output.gpr());
            break;
          case AtomicFetchOrOp:
            masm.atomicFetchOr16ZeroExtend(value, mem, temp1, output.gpr());
            break;
          case AtomicFetchXorOp:
            masm.atomicFetchXor16ZeroExtend(value, mem, temp1, output.gpr());
            break;
          default:
            MOZ_CRASH("Invalid typed array atomic operation");
        }
        break;
      case Scalar::Int32:
        switch (op) {
          case AtomicFetchAddOp:
            masm.atomicFetchAdd32(value, mem, temp1, output.gpr());
            break;
          case AtomicFetchSubOp:
            masm.atomicFetchSub32(value, mem, temp1, output.gpr());
            break;
          case AtomicFetchAndOp:
            masm.atomicFetchAnd32(value, mem, temp1, output.gpr());
            break;
          case AtomicFetchOrOp:
            masm.atomicFetchOr32(value, mem, temp1, output.gpr());
            break;
          case AtomicFetchXorOp:
            masm.atomicFetchXor32(value, mem, temp1, output.gpr());
            break;
          default:
            MOZ_CRASH("Invalid typed array atomic operation");
        }
        break;
      case Scalar::Uint32:
        // At the moment, the code in MCallOptimize.cpp requires the output
        // type to be double for uint32 arrays.  See bug 1077305.
        MOZ_ASSERT(output.isFloat());
        switch (op) {
          case AtomicFetchAddOp:
            masm.atomicFetchAdd32(value, mem, InvalidReg, temp1);
            break;
          case AtomicFetchSubOp:
            masm.atomicFetchSub32(value, mem, InvalidReg, temp1);
            break;
          case AtomicFetchAndOp:
            masm.atomicFetchAnd32(value, mem, temp2, temp1);
            break;
          case AtomicFetchOrOp:
            masm.atomicFetchOr32(value, mem, temp2, temp1);
            break;
          case AtomicFetchXorOp:
            masm.atomicFetchXor32(value, mem, temp2, temp1);
            break;
          default:
            MOZ_CRASH("Invalid typed array atomic operation");
        }
        masm.convertUInt32ToDouble(temp1, output.fpu());
        break;
      default:
        MOZ_CRASH("Invalid typed array type");
    }
}

template void
CodeGeneratorX86Shared::atomicBinopToTypedIntArray(AtomicOp op, Scalar::Type arrayType,
                                                    const Imm32& value, const Address& mem,
                                                    Register temp1, Register temp2, AnyRegister output);
template void
CodeGeneratorX86Shared::atomicBinopToTypedIntArray(AtomicOp op, Scalar::Type arrayType,
                                                    const Imm32& value, const BaseIndex& mem,
                                                    Register temp1, Register temp2, AnyRegister output);
template void
CodeGeneratorX86Shared::atomicBinopToTypedIntArray(AtomicOp op, Scalar::Type arrayType,
                                                    const Register& value, const Address& mem,
                                                    Register temp1, Register temp2, AnyRegister output);
template void
CodeGeneratorX86Shared::atomicBinopToTypedIntArray(AtomicOp op, Scalar::Type arrayType,
                                                    const Register& value, const BaseIndex& mem,
                                                    Register temp1, Register temp2, AnyRegister output);

// Binary operation for effect, result discarded.
template<typename S, typename T>
void
CodeGeneratorX86Shared::atomicBinopToTypedIntArray(AtomicOp op, Scalar::Type arrayType, const S& value,
                                                    const T& mem)
{
    switch (arrayType) {
      case Scalar::Int8:
      case Scalar::Uint8:
        switch (op) {
          case AtomicFetchAddOp:
            masm.atomicAdd8(value, mem);
            break;
          case AtomicFetchSubOp:
            masm.atomicSub8(value, mem);
            break;
          case AtomicFetchAndOp:
            masm.atomicAnd8(value, mem);
            break;
          case AtomicFetchOrOp:
            masm.atomicOr8(value, mem);
            break;
          case AtomicFetchXorOp:
            masm.atomicXor8(value, mem);
            break;
          default:
            MOZ_CRASH("Invalid typed array atomic operation");
        }
        break;
      case Scalar::Int16:
      case Scalar::Uint16:
        switch (op) {
          case AtomicFetchAddOp:
            masm.atomicAdd16(value, mem);
            break;
          case AtomicFetchSubOp:
            masm.atomicSub16(value, mem);
            break;
          case AtomicFetchAndOp:
            masm.atomicAnd16(value, mem);
            break;
          case AtomicFetchOrOp:
            masm.atomicOr16(value, mem);
            break;
          case AtomicFetchXorOp:
            masm.atomicXor16(value, mem);
            break;
          default:
            MOZ_CRASH("Invalid typed array atomic operation");
        }
        break;
      case Scalar::Int32:
      case Scalar::Uint32:
        switch (op) {
          case AtomicFetchAddOp:
            masm.atomicAdd32(value, mem);
            break;
          case AtomicFetchSubOp:
            masm.atomicSub32(value, mem);
            break;
          case AtomicFetchAndOp:
            masm.atomicAnd32(value, mem);
            break;
          case AtomicFetchOrOp:
            masm.atomicOr32(value, mem);
            break;
          case AtomicFetchXorOp:
            masm.atomicXor32(value, mem);
            break;
          default:
            MOZ_CRASH("Invalid typed array atomic operation");
        }
        break;
      default:
        MOZ_CRASH("Invalid typed array type");
    }
}

template void
CodeGeneratorX86Shared::atomicBinopToTypedIntArray(AtomicOp op, Scalar::Type arrayType,
                                                   const Imm32& value, const Address& mem);
template void
CodeGeneratorX86Shared::atomicBinopToTypedIntArray(AtomicOp op, Scalar::Type arrayType,
                                                   const Imm32& value, const BaseIndex& mem);
template void
CodeGeneratorX86Shared::atomicBinopToTypedIntArray(AtomicOp op, Scalar::Type arrayType,
                                                   const Register& value, const Address& mem);
template void
CodeGeneratorX86Shared::atomicBinopToTypedIntArray(AtomicOp op, Scalar::Type arrayType,
                                                   const Register& value, const BaseIndex& mem);


template <typename T>
static inline void
AtomicBinopToTypedArray(CodeGeneratorX86Shared* cg, AtomicOp op,
                        Scalar::Type arrayType, const LAllocation* value, const T& mem,
                        Register temp1, Register temp2, AnyRegister output)
{
    if (value->isConstant())
        cg->atomicBinopToTypedIntArray(op, arrayType, Imm32(ToInt32(value)), mem, temp1, temp2, output);
    else
        cg->atomicBinopToTypedIntArray(op, arrayType, ToRegister(value), mem, temp1, temp2, output);
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
        AtomicBinopToTypedArray(this, lir->mir()->operation(), arrayType, value, mem, temp1, temp2, output);
    } else {
        BaseIndex mem(elements, ToRegister(lir->index()), ScaleFromElemWidth(width));
        AtomicBinopToTypedArray(this, lir->mir()->operation(), arrayType, value, mem, temp1, temp2, output);
    }
}

template <typename T>
static inline void
AtomicBinopToTypedArray(CodeGeneratorX86Shared* cg, AtomicOp op,
                        Scalar::Type arrayType, const LAllocation* value, const T& mem)
{
    if (value->isConstant())
        cg->atomicBinopToTypedIntArray(op, arrayType, Imm32(ToInt32(value)), mem);
    else
        cg->atomicBinopToTypedIntArray(op, arrayType, ToRegister(value), mem);
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
        AtomicBinopToTypedArray(this, lir->mir()->operation(), arrayType, value, mem);
    } else {
        BaseIndex mem(elements, ToRegister(lir->index()), ScaleFromElemWidth(width));
        AtomicBinopToTypedArray(this, lir->mir()->operation(), arrayType, value, mem);
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

} // namespace jit
} // namespace js
