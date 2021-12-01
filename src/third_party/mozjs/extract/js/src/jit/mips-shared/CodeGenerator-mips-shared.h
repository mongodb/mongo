/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_mips_shared_CodeGenerator_mips_shared_h
#define jit_mips_shared_CodeGenerator_mips_shared_h

#include "jit/shared/CodeGenerator-shared.h"

namespace js {
namespace jit {

class CodeGeneratorMIPSShared;
class OutOfLineBailout;
class OutOfLineTableSwitch;

using OutOfLineWasmTruncateCheck = OutOfLineWasmTruncateCheckBase<CodeGeneratorMIPSShared>;

class CodeGeneratorMIPSShared : public CodeGeneratorShared
{
    friend class MoveResolverMIPS;

    CodeGeneratorMIPSShared* thisFromCtor() {
        return this;
    }

  protected:
    NonAssertingLabel deoptLabel_;

    Operand ToOperand(const LAllocation& a);
    Operand ToOperand(const LAllocation* a);
    Operand ToOperand(const LDefinition* def);

#ifdef JS_PUNBOX64
    Operand ToOperandOrRegister64(const LInt64Allocation input);
#else
    Register64 ToOperandOrRegister64(const LInt64Allocation input);
#endif

    MoveOperand toMoveOperand(LAllocation a) const;

    template <typename T1, typename T2>
    void bailoutCmp32(Assembler::Condition c, T1 lhs, T2 rhs, LSnapshot* snapshot) {
        Label bail;
        masm.branch32(c, lhs, rhs, &bail);
        bailoutFrom(&bail, snapshot);
    }
    template<typename T>
    void bailoutTest32(Assembler::Condition c, Register lhs, T rhs, LSnapshot* snapshot) {
        Label bail;
        masm.branchTest32(c, lhs, rhs, &bail);
        bailoutFrom(&bail, snapshot);
    }
    template <typename T1, typename T2>
    void bailoutCmpPtr(Assembler::Condition c, T1 lhs, T2 rhs, LSnapshot* snapshot) {
        Label bail;
        masm.branchPtr(c, lhs, rhs, &bail);
        bailoutFrom(&bail, snapshot);
    }
    void bailoutTestPtr(Assembler::Condition c, Register lhs, Register rhs, LSnapshot* snapshot) {
        Label bail;
        masm.branchTestPtr(c, lhs, rhs, &bail);
        bailoutFrom(&bail, snapshot);
    }
    void bailoutIfFalseBool(Register reg, LSnapshot* snapshot) {
        Label bail;
        masm.branchTest32(Assembler::Zero, reg, Imm32(0xFF), &bail);
        bailoutFrom(&bail, snapshot);
    }

    void bailoutFrom(Label* label, LSnapshot* snapshot);
    void bailout(LSnapshot* snapshot);

  protected:
    bool generateOutOfLineCode();

    template <typename T>
    void branchToBlock(Register lhs, T rhs, MBasicBlock* mir, Assembler::Condition cond)
    {
        mir = skipTrivialBlocks(mir);

        Label* label = mir->lir()->label();
        if (Label* oolEntry = labelForBackedgeWithImplicitCheck(mir)) {
            // Note: the backedge is initially a jump to the next instruction.
            // It will be patched to the target block's label during link().
            RepatchLabel rejoin;
            CodeOffsetJump backedge;
            Label skip;

            masm.ma_b(lhs, rhs, &skip, Assembler::InvertCondition(cond), ShortJump);
            backedge = masm.backedgeJump(&rejoin);
            masm.bind(&rejoin);
            masm.bind(&skip);

            if (!patchableBackedges_.append(PatchableBackedgeInfo(backedge, label, oolEntry)))
                MOZ_CRASH();
        } else {
            masm.ma_b(lhs, rhs, label, cond);
        }
    }
    void branchToBlock(Assembler::FloatFormat fmt, FloatRegister lhs, FloatRegister rhs,
                       MBasicBlock* mir, Assembler::DoubleCondition cond);

    // Emits a branch that directs control flow to the true block if |cond| is
    // true, and the false block if |cond| is false.
    template <typename T>
    void emitBranch(Register lhs, T rhs, Assembler::Condition cond,
                    MBasicBlock* mirTrue, MBasicBlock* mirFalse)
    {
        if (isNextBlock(mirFalse->lir())) {
            branchToBlock(lhs, rhs, mirTrue, cond);
        } else {
            branchToBlock(lhs, rhs, mirFalse, Assembler::InvertCondition(cond));
            jumpToBlock(mirTrue);
        }
    }
    void testZeroEmitBranch(Assembler::Condition cond, Register reg,
                            MBasicBlock* ifTrue, MBasicBlock* ifFalse)
    {
        emitBranch(reg, Imm32(0), cond, ifTrue, ifFalse);
    }

    void emitTableSwitchDispatch(MTableSwitch* mir, Register index, Register base);

    template <typename T>
    void emitWasmLoad(T* ins);
    template <typename T>
    void emitWasmStore(T* ins);

  public:
    // Instruction visitors.
    void visitMinMaxD(LMinMaxD* ins);
    void visitMinMaxF(LMinMaxF* ins);
    void visitAbsD(LAbsD* ins);
    void visitAbsF(LAbsF* ins);
    void visitSqrtD(LSqrtD* ins);
    void visitSqrtF(LSqrtF* ins);
    void visitAddI(LAddI* ins);
    void visitAddI64(LAddI64* ins);
    void visitSubI(LSubI* ins);
    void visitSubI64(LSubI64* ins);
    void visitBitNotI(LBitNotI* ins);
    void visitBitOpI(LBitOpI* ins);
    void visitBitOpI64(LBitOpI64* ins);

    void visitMulI(LMulI* ins);
    void visitMulI64(LMulI64* ins);

    void visitDivI(LDivI* ins);
    void visitDivPowTwoI(LDivPowTwoI* ins);
    void visitModI(LModI* ins);
    void visitModPowTwoI(LModPowTwoI* ins);
    void visitModMaskI(LModMaskI* ins);
    void visitPowHalfD(LPowHalfD* ins);
    void visitShiftI(LShiftI* ins);
    void visitShiftI64(LShiftI64* ins);
    void visitRotateI64(LRotateI64* lir);
    void visitUrshD(LUrshD* ins);

    void visitClzI(LClzI* ins);
    void visitCtzI(LCtzI* ins);
    void visitPopcntI(LPopcntI* ins);
    void visitPopcntI64(LPopcntI64* lir);

    void visitTestIAndBranch(LTestIAndBranch* test);
    void visitCompare(LCompare* comp);
    void visitCompareAndBranch(LCompareAndBranch* comp);
    void visitTestDAndBranch(LTestDAndBranch* test);
    void visitTestFAndBranch(LTestFAndBranch* test);
    void visitCompareD(LCompareD* comp);
    void visitCompareF(LCompareF* comp);
    void visitCompareDAndBranch(LCompareDAndBranch* comp);
    void visitCompareFAndBranch(LCompareFAndBranch* comp);
    void visitBitAndAndBranch(LBitAndAndBranch* lir);
    void visitWasmUint32ToDouble(LWasmUint32ToDouble* lir);
    void visitWasmUint32ToFloat32(LWasmUint32ToFloat32* lir);
    void visitNotI(LNotI* ins);
    void visitNotD(LNotD* ins);
    void visitNotF(LNotF* ins);

    void visitMathD(LMathD* math);
    void visitMathF(LMathF* math);
    void visitFloor(LFloor* lir);
    void visitFloorF(LFloorF* lir);
    void visitCeil(LCeil* lir);
    void visitCeilF(LCeilF* lir);
    void visitRound(LRound* lir);
    void visitRoundF(LRoundF* lir);
    void visitTruncateDToInt32(LTruncateDToInt32* ins);
    void visitTruncateFToInt32(LTruncateFToInt32* ins);

    void visitWasmTruncateToInt32(LWasmTruncateToInt32* lir);

    // Out of line visitors.
    void visitOutOfLineBailout(OutOfLineBailout* ool);
    void visitOutOfLineTableSwitch(OutOfLineTableSwitch* ool);
    void visitOutOfLineWasmTruncateCheck(OutOfLineWasmTruncateCheck* ool);
    void visitCopySignD(LCopySignD* ins);
    void visitCopySignF(LCopySignF* ins);

  public:
    CodeGeneratorMIPSShared(MIRGenerator* gen, LIRGraph* graph, MacroAssembler* masm);

    void visitValue(LValue* value);
    void visitDouble(LDouble* ins);
    void visitFloat32(LFloat32* ins);
    void visitNegI(LNegI* lir);
    void visitNegD(LNegD* lir);
    void visitNegF(LNegF* lir);
    void visitWasmLoad(LWasmLoad* ins);
    void visitWasmUnalignedLoad(LWasmUnalignedLoad* ins);
    void visitWasmStore(LWasmStore* ins);
    void visitWasmUnalignedStore(LWasmUnalignedStore* ins);
    void visitWasmAddOffset(LWasmAddOffset* ins);
    void visitAsmJSLoadHeap(LAsmJSLoadHeap* ins);
    void visitAsmJSStoreHeap(LAsmJSStoreHeap* ins);
    void visitWasmCompareExchangeHeap(LWasmCompareExchangeHeap* ins);
    void visitWasmAtomicExchangeHeap(LWasmAtomicExchangeHeap* ins);
    void visitWasmAtomicBinopHeap(LWasmAtomicBinopHeap* ins);
    void visitWasmAtomicBinopHeapForEffect(LWasmAtomicBinopHeapForEffect* ins);

    void visitWasmStackArg(LWasmStackArg* ins);
    void visitWasmStackArgI64(LWasmStackArgI64* ins);
    void visitWasmSelect(LWasmSelect* ins);
    void visitWasmReinterpret(LWasmReinterpret* ins);

    void visitMemoryBarrier(LMemoryBarrier* ins);
    void visitAtomicTypedArrayElementBinop(LAtomicTypedArrayElementBinop* lir);
    void visitAtomicTypedArrayElementBinopForEffect(LAtomicTypedArrayElementBinopForEffect* lir);
    void visitCompareExchangeTypedArrayElement(LCompareExchangeTypedArrayElement* lir);
    void visitAtomicExchangeTypedArrayElement(LAtomicExchangeTypedArrayElement* lir);

    void generateInvalidateEpilogue();

    // Generating a result.
    template<typename S, typename T>
    void atomicBinopToTypedIntArray(AtomicOp op, Scalar::Type arrayType, const S& value,
                                    const T& mem, Register flagTemp, Register outTemp,
                                    Register valueTemp, Register offsetTemp, Register maskTemp,
                                    AnyRegister output);

    // Generating no result.
    template<typename S, typename T>
    void atomicBinopToTypedIntArray(AtomicOp op, Scalar::Type arrayType, const S& value,
                                    const T& mem, Register flagTemp, Register valueTemp,
                                    Register offsetTemp, Register maskTemp);

    void visitWasmCompareExchangeI64(LWasmCompareExchangeI64* lir);
    void visitWasmAtomicExchangeI64(LWasmAtomicExchangeI64* lir);
    void visitWasmAtomicBinopI64(LWasmAtomicBinopI64* lir);

  protected:
    void visitEffectiveAddress(LEffectiveAddress* ins);
    void visitUDivOrMod(LUDivOrMod* ins);

  public:
    // Unimplemented SIMD instructions
    void visitSimdSplatX4(LSimdSplatX4* lir) { MOZ_CRASH("NYI"); }
    void visitSimd128Int(LSimd128Int* ins) { MOZ_CRASH("NYI"); }
    void visitSimd128Float(LSimd128Float* ins) { MOZ_CRASH("NYI"); }
    void visitSimdReinterpretCast(LSimdReinterpretCast* ins) { MOZ_CRASH("NYI"); }
    void visitSimdExtractElementI(LSimdExtractElementI* ins) { MOZ_CRASH("NYI"); }
    void visitSimdExtractElementF(LSimdExtractElementF* ins) { MOZ_CRASH("NYI"); }
    void visitSimdBinaryCompIx4(LSimdBinaryCompIx4* lir) { MOZ_CRASH("NYI"); }
    void visitSimdBinaryCompFx4(LSimdBinaryCompFx4* lir) { MOZ_CRASH("NYI"); }
    void visitSimdBinaryArithIx4(LSimdBinaryArithIx4* lir) { MOZ_CRASH("NYI"); }
    void visitSimdBinaryArithFx4(LSimdBinaryArithFx4* lir) { MOZ_CRASH("NYI"); }
    void visitSimdBinaryBitwise(LSimdBinaryBitwise* lir) { MOZ_CRASH("NYI"); }
    void visitSimdGeneralShuffleI(LSimdGeneralShuffleI* lir) { MOZ_CRASH("NYI"); }
    void visitSimdGeneralShuffleF(LSimdGeneralShuffleF* lir) { MOZ_CRASH("NYI"); }
};

// An out-of-line bailout thunk.
class OutOfLineBailout : public OutOfLineCodeBase<CodeGeneratorMIPSShared>
{
    LSnapshot* snapshot_;
    uint32_t frameSize_;

  public:
    OutOfLineBailout(LSnapshot* snapshot, uint32_t frameSize)
      : snapshot_(snapshot),
        frameSize_(frameSize)
    { }

    void accept(CodeGeneratorMIPSShared* codegen) override;

    LSnapshot* snapshot() const {
        return snapshot_;
    }
};

} // namespace jit
} // namespace js

#endif /* jit_mips_shared_CodeGenerator_mips_shared_h */
