/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_arm_CodeGenerator_arm_h
#define jit_arm_CodeGenerator_arm_h

#include "jit/arm/Assembler-arm.h"
#include "jit/shared/CodeGenerator-shared.h"

namespace js {
namespace jit {

class CodeGeneratorARM;
class OutOfLineBailout;
class OutOfLineTableSwitch;

using OutOfLineWasmTruncateCheck = OutOfLineWasmTruncateCheckBase<CodeGeneratorARM>;

class CodeGeneratorARM : public CodeGeneratorShared
{
    friend class MoveResolverARM;

    CodeGeneratorARM* thisFromCtor() {return this;}

  protected:
    NonAssertingLabel deoptLabel_;

    MoveOperand toMoveOperand(LAllocation a) const;

    void bailoutIf(Assembler::Condition condition, LSnapshot* snapshot);
    void bailoutFrom(Label* label, LSnapshot* snapshot);
    void bailout(LSnapshot* snapshot);

    template <typename T1, typename T2>
    void bailoutCmpPtr(Assembler::Condition c, T1 lhs, T2 rhs, LSnapshot* snapshot) {
        masm.cmpPtr(lhs, rhs);
        bailoutIf(c, snapshot);
    }
    void bailoutTestPtr(Assembler::Condition c, Register lhs, Register rhs, LSnapshot* snapshot) {
        masm.testPtr(lhs, rhs);
        bailoutIf(c, snapshot);
    }
    template <typename T1, typename T2>
    void bailoutCmp32(Assembler::Condition c, T1 lhs, T2 rhs, LSnapshot* snapshot) {
        masm.cmp32(lhs, rhs);
        bailoutIf(c, snapshot);
    }
    template <typename T1, typename T2>
    void bailoutTest32(Assembler::Condition c, T1 lhs, T2 rhs, LSnapshot* snapshot) {
        masm.test32(lhs, rhs);
        bailoutIf(c, snapshot);
    }
    void bailoutIfFalseBool(Register reg, LSnapshot* snapshot) {
        masm.test32(reg, Imm32(0xFF));
        bailoutIf(Assembler::Zero, snapshot);
    }

    template<class T>
    void generateUDivModZeroCheck(Register rhs, Register output, Label* done, LSnapshot* snapshot,
                                  T* mir);

  protected:
    bool generateOutOfLineCode();

    void emitRoundDouble(FloatRegister src, Register dest, Label* fail);

    // Emits a branch that directs control flow to the true block if |cond| is
    // true, and the false block if |cond| is false.
    void emitBranch(Assembler::Condition cond, MBasicBlock* ifTrue, MBasicBlock* ifFalse);

    void testNullEmitBranch(Assembler::Condition cond, const ValueOperand& value,
                            MBasicBlock* ifTrue, MBasicBlock* ifFalse)
    {
        cond = masm.testNull(cond, value);
        emitBranch(cond, ifTrue, ifFalse);
    }
    void testUndefinedEmitBranch(Assembler::Condition cond, const ValueOperand& value,
                                 MBasicBlock* ifTrue, MBasicBlock* ifFalse)
    {
        cond = masm.testUndefined(cond, value);
        emitBranch(cond, ifTrue, ifFalse);
    }
    void testObjectEmitBranch(Assembler::Condition cond, const ValueOperand& value,
                              MBasicBlock* ifTrue, MBasicBlock* ifFalse)
    {
        cond = masm.testObject(cond, value);
        emitBranch(cond, ifTrue, ifFalse);
    }
    void testZeroEmitBranch(Assembler::Condition cond, Register reg,
                            MBasicBlock* ifTrue, MBasicBlock* ifFalse)
    {
        MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
        masm.cmpPtr(reg, ImmWord(0));
        emitBranch(cond, ifTrue, ifFalse);
    }

    void emitTableSwitchDispatch(MTableSwitch* mir, Register index, Register base);

    template <typename T>
    void emitWasmLoad(T* ins);
    template <typename T>
    void emitWasmUnalignedLoad(T* ins);
    template <typename T>
    void emitWasmStore(T* ins);
    template <typename T>
    void emitWasmUnalignedStore(T* ins);

  public:
    // Instruction visitors.
    void visitMinMaxD(LMinMaxD* ins);
    void visitMinMaxF(LMinMaxF* ins);
    void visitAbsD(LAbsD* ins);
    void visitAbsF(LAbsF* ins);
    void visitSqrtD(LSqrtD* ins);
    void visitSqrtF(LSqrtF* ins);
    void visitAddI(LAddI* ins);
    void visitSubI(LSubI* ins);
    void visitBitNotI(LBitNotI* ins);
    void visitBitOpI(LBitOpI* ins);

    void visitMulI(LMulI* ins);

    void visitDivI(LDivI* ins);
    void visitSoftDivI(LSoftDivI* ins);
    void visitDivPowTwoI(LDivPowTwoI* ins);
    void visitModI(LModI* ins);
    void visitSoftModI(LSoftModI* ins);
    void visitModPowTwoI(LModPowTwoI* ins);
    void visitModMaskI(LModMaskI* ins);
    void visitPowHalfD(LPowHalfD* ins);
    void visitShiftI(LShiftI* ins);
    void visitShiftI64(LShiftI64* ins);
    void visitUrshD(LUrshD* ins);

    void visitClzI(LClzI* ins);
    void visitCtzI(LCtzI* ins);
    void visitPopcntI(LPopcntI* ins);

    void visitTestIAndBranch(LTestIAndBranch* test);
    void visitCompare(LCompare* comp);
    void visitCompareAndBranch(LCompareAndBranch* comp);
    void visitTestDAndBranch(LTestDAndBranch* test);
    void visitTestFAndBranch(LTestFAndBranch* test);
    void visitCompareD(LCompareD* comp);
    void visitCompareF(LCompareF* comp);
    void visitCompareDAndBranch(LCompareDAndBranch* comp);
    void visitCompareFAndBranch(LCompareFAndBranch* comp);
    void visitCompareB(LCompareB* lir);
    void visitCompareBAndBranch(LCompareBAndBranch* lir);
    void visitCompareBitwise(LCompareBitwise* lir);
    void visitCompareBitwiseAndBranch(LCompareBitwiseAndBranch* lir);
    void visitBitAndAndBranch(LBitAndAndBranch* baab);
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

    void visitWrapInt64ToInt32(LWrapInt64ToInt32* lir);
    void visitExtendInt32ToInt64(LExtendInt32ToInt64* lir);
    void visitSignExtendInt64(LSignExtendInt64* ins);
    void visitAddI64(LAddI64* lir);
    void visitSubI64(LSubI64* lir);
    void visitMulI64(LMulI64* lir);
    void visitDivOrModI64(LDivOrModI64* lir);
    void visitUDivOrModI64(LUDivOrModI64* lir);
    void visitCompareI64(LCompareI64* lir);
    void visitCompareI64AndBranch(LCompareI64AndBranch* lir);
    void visitBitOpI64(LBitOpI64* lir);
    void visitRotateI64(LRotateI64* lir);
    void visitWasmStackArgI64(LWasmStackArgI64* lir);
    void visitWasmSelectI64(LWasmSelectI64* lir);
    void visitWasmReinterpretFromI64(LWasmReinterpretFromI64* lir);
    void visitWasmReinterpretToI64(LWasmReinterpretToI64* lir);
    void visitPopcntI64(LPopcntI64* ins);
    void visitClzI64(LClzI64* ins);
    void visitCtzI64(LCtzI64* ins);
    void visitNotI64(LNotI64* ins);
    void visitWasmTruncateToInt64(LWasmTruncateToInt64* ins);
    void visitInt64ToFloatingPointCall(LInt64ToFloatingPointCall* lir);
    void visitTestI64AndBranch(LTestI64AndBranch* lir);

    // Out of line visitors.
    void visitOutOfLineBailout(OutOfLineBailout* ool);
    void visitOutOfLineTableSwitch(OutOfLineTableSwitch* ool);

  protected:
    ValueOperand ToValue(LInstruction* ins, size_t pos);
    ValueOperand ToTempValue(LInstruction* ins, size_t pos);

    Register64 ToOperandOrRegister64(const LInt64Allocation input);

    // Functions for LTestVAndBranch.
    void splitTagForTest(const ValueOperand& value, ScratchTagScope& tag);

    void divICommon(MDiv* mir, Register lhs, Register rhs, Register output, LSnapshot* snapshot,
                    Label& done);
    void modICommon(MMod* mir, Register lhs, Register rhs, Register output, LSnapshot* snapshot,
                    Label& done);

  public:
    CodeGeneratorARM(MIRGenerator* gen, LIRGraph* graph, MacroAssembler* masm);

  public:
    void visitBox(LBox* box);
    void visitBoxFloatingPoint(LBoxFloatingPoint* box);
    void visitUnbox(LUnbox* unbox);
    void visitValue(LValue* value);
    void visitDouble(LDouble* ins);
    void visitFloat32(LFloat32* ins);
    void visitNegI(LNegI* lir);
    void visitNegD(LNegD* lir);
    void visitNegF(LNegF* lir);
    void visitAtomicTypedArrayElementBinop(LAtomicTypedArrayElementBinop* lir);
    void visitAtomicTypedArrayElementBinopForEffect(LAtomicTypedArrayElementBinopForEffect* lir);
    void visitCompareExchangeTypedArrayElement(LCompareExchangeTypedArrayElement* lir);
    void visitAtomicExchangeTypedArrayElement(LAtomicExchangeTypedArrayElement* lir);
    void visitWasmSelect(LWasmSelect* ins);
    void visitWasmReinterpret(LWasmReinterpret* ins);
    void visitWasmLoad(LWasmLoad* ins);
    void visitWasmLoadI64(LWasmLoadI64* ins);
    void visitWasmUnalignedLoad(LWasmUnalignedLoad* ins);
    void visitWasmUnalignedLoadI64(LWasmUnalignedLoadI64* ins);
    void visitWasmAddOffset(LWasmAddOffset* ins);
    void visitWasmStore(LWasmStore* ins);
    void visitWasmStoreI64(LWasmStoreI64* ins);
    void visitWasmUnalignedStore(LWasmUnalignedStore* ins);
    void visitWasmUnalignedStoreI64(LWasmUnalignedStoreI64* ins);
    void visitAsmJSLoadHeap(LAsmJSLoadHeap* ins);
    void visitAsmJSStoreHeap(LAsmJSStoreHeap* ins);
    void visitWasmCompareExchangeHeap(LWasmCompareExchangeHeap* ins);
    void visitWasmAtomicExchangeHeap(LWasmAtomicExchangeHeap* ins);
    void visitWasmAtomicBinopHeap(LWasmAtomicBinopHeap* ins);
    void visitWasmAtomicBinopHeapForEffect(LWasmAtomicBinopHeapForEffect* ins);
    void visitWasmStackArg(LWasmStackArg* ins);
    void visitWasmTruncateToInt32(LWasmTruncateToInt32* ins);
    void visitOutOfLineWasmTruncateCheck(OutOfLineWasmTruncateCheck* ool);
    void visitCopySignD(LCopySignD* ins);
    void visitCopySignF(LCopySignF* ins);

    void visitMemoryBarrier(LMemoryBarrier* ins);

    void generateInvalidateEpilogue();

    void setReturnDoubleRegs(LiveRegisterSet* regs);

    // Generating a result.
    template<typename S, typename T>
    void atomicBinopToTypedIntArray(AtomicOp op, Scalar::Type arrayType, const S& value,
                                    const T& mem, Register flagTemp, Register outTemp,
                                    AnyRegister output);

    // Generating no result.
    template<typename S, typename T>
    void atomicBinopToTypedIntArray(AtomicOp op, Scalar::Type arrayType, const S& value,
                                    const T& mem, Register flagTemp);

    void visitWasmAtomicLoadI64(LWasmAtomicLoadI64* lir);
    void visitWasmAtomicStoreI64(LWasmAtomicStoreI64* lir);
    void visitWasmCompareExchangeI64(LWasmCompareExchangeI64* lir);
    void visitWasmAtomicBinopI64(LWasmAtomicBinopI64* lir);
    void visitWasmAtomicExchangeI64(LWasmAtomicExchangeI64* lir);

  protected:
    void visitEffectiveAddress(LEffectiveAddress* ins);
    void visitUDiv(LUDiv* ins);
    void visitUMod(LUMod* ins);
    void visitSoftUDivOrMod(LSoftUDivOrMod* ins);

  public:
    // Unimplemented SIMD instructions
    void visitSimdSplatX4(LSimdSplatX4* lir) { MOZ_CRASH("NYI"); }
    void visitSimd128Int(LSimd128Int* ins) { MOZ_CRASH("NYI"); }
    void visitSimd128Float(LSimd128Float* ins) { MOZ_CRASH("NYI"); }
    void visitSimdReinterpretCast(LSimdReinterpretCast* ins) { MOZ_CRASH("NYI"); }
    void visitSimdExtractElementI(LSimdExtractElementI* ins) { MOZ_CRASH("NYI"); }
    void visitSimdExtractElementF(LSimdExtractElementF* ins) { MOZ_CRASH("NYI"); }
    void visitSimdGeneralShuffleI(LSimdGeneralShuffleI* lir) { MOZ_CRASH("NYI"); }
    void visitSimdGeneralShuffleF(LSimdGeneralShuffleF* lir) { MOZ_CRASH("NYI"); }
    void visitSimdSwizzleI(LSimdSwizzleI* lir) { MOZ_CRASH("NYI"); }
    void visitSimdSwizzleF(LSimdSwizzleF* lir) { MOZ_CRASH("NYI"); }
    void visitSimdBinaryCompIx4(LSimdBinaryCompIx4* lir) { MOZ_CRASH("NYI"); }
    void visitSimdBinaryCompFx4(LSimdBinaryCompFx4* lir) { MOZ_CRASH("NYI"); }
    void visitSimdBinaryArithIx4(LSimdBinaryArithIx4* lir) { MOZ_CRASH("NYI"); }
    void visitSimdBinaryArithFx4(LSimdBinaryArithFx4* lir) { MOZ_CRASH("NYI"); }
    void visitSimdBinaryBitwise(LSimdBinaryBitwise* lir) { MOZ_CRASH("NYI"); }
};

typedef CodeGeneratorARM CodeGeneratorSpecific;

// An out-of-line bailout thunk.
class OutOfLineBailout : public OutOfLineCodeBase<CodeGeneratorARM>
{
  protected: // Silence Clang warning.
    LSnapshot* snapshot_;
    uint32_t frameSize_;

  public:
    OutOfLineBailout(LSnapshot* snapshot, uint32_t frameSize)
      : snapshot_(snapshot),
        frameSize_(frameSize)
    { }

    void accept(CodeGeneratorARM* codegen) override;

    LSnapshot* snapshot() const {
        return snapshot_;
    }
};

} // namespace jit
} // namespace js

#endif /* jit_arm_CodeGenerator_arm_h */
