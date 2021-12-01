/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_arm64_CodeGenerator_arm64_h
#define jit_arm64_CodeGenerator_arm64_h

#include "jit/arm64/Assembler-arm64.h"
#include "jit/shared/CodeGenerator-shared.h"

namespace js {
namespace jit {

class OutOfLineBailout;
class OutOfLineTableSwitch;

class CodeGeneratorARM64 : public CodeGeneratorShared
{
    friend class MoveResolverARM64;

    CodeGeneratorARM64* thisFromCtor() { return this; }

  public:
    CodeGeneratorARM64(MIRGenerator* gen, LIRGraph* graph, MacroAssembler* masm);

  protected:
    NonAssertingLabel deoptLabel_;

    MoveOperand toMoveOperand(const LAllocation a) const;

    void bailoutIf(Assembler::Condition condition, LSnapshot* snapshot);
    void bailoutFrom(Label* label, LSnapshot* snapshot);
    void bailout(LSnapshot* snapshot);

    template <typename T1, typename T2>
    void bailoutCmpPtr(Assembler::Condition c, T1 lhs, T2 rhs, LSnapshot* snapshot) {
        masm.cmpPtr(lhs, rhs);
        return bailoutIf(c, snapshot);
    }
    void bailoutTestPtr(Assembler::Condition c, Register lhs, Register rhs, LSnapshot* snapshot) {
        masm.testPtr(lhs, rhs);
        return bailoutIf(c, snapshot);
    }
    template <typename T1, typename T2>
    void bailoutCmp32(Assembler::Condition c, T1 lhs, T2 rhs, LSnapshot* snapshot) {
        masm.cmp32(lhs, rhs);
        return bailoutIf(c, snapshot);
    }
    template <typename T1, typename T2>
    void bailoutTest32(Assembler::Condition c, T1 lhs, T2 rhs, LSnapshot* snapshot) {
        masm.test32(lhs, rhs);
        return bailoutIf(c, snapshot);
    }
    void bailoutIfFalseBool(Register reg, LSnapshot* snapshot) {
        masm.test32(reg, Imm32(0xFF));
        return bailoutIf(Assembler::Zero, snapshot);
    }

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

  public:
    // Instruction visitors.
    void visitMinMaxD(LMinMaxD* ins);
    void visitMinMaxF(LMinMaxF* math);
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
    void visitDivPowTwoI(LDivPowTwoI* ins);
    void visitModI(LModI* ins);
    void visitModPowTwoI(LModPowTwoI* ins);
    void visitModMaskI(LModMaskI* ins);
    void visitPowHalfD(LPowHalfD* ins);
    void visitShiftI(LShiftI* ins);
    void visitUrshD(LUrshD* ins);

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

    void visitClzI(LClzI* lir);
    void visitCtzI(LCtzI* lir);
    // Out of line visitors.
    void visitOutOfLineBailout(OutOfLineBailout* ool);
    void visitOutOfLineTableSwitch(OutOfLineTableSwitch* ool);

  protected:
    ValueOperand ToValue(LInstruction* ins, size_t pos);
    ValueOperand ToTempValue(LInstruction* ins, size_t pos);

    // Functions for LTestVAndBranch.
    void splitTagForTest(const ValueOperand& value, ScratchTagScope& tag);

    void storeElementTyped(const LAllocation* value, MIRType valueType, MIRType elementType,
                           Register elements, const LAllocation* index);

    void divICommon(MDiv* mir, Register lhs, Register rhs, Register output, LSnapshot* snapshot,
                    Label& done);
    void modICommon(MMod* mir, Register lhs, Register rhs, Register output, LSnapshot* snapshot,
                    Label& done);

  public:
    void visitBox(LBox* box);
    void visitUnbox(LUnbox* unbox);
    void visitValue(LValue* value);
    void visitDouble(LDouble* ins);
    void visitFloat32(LFloat32* ins);

    void visitLoadSlotV(LLoadSlotV* load);
    void visitLoadSlotT(LLoadSlotT* load);
    void visitStoreSlotT(LStoreSlotT* load);

    void visitLoadElementT(LLoadElementT* load);

    void visitInterruptCheck(LInterruptCheck* lir);

    void visitNegI(LNegI* lir);
    void visitNegD(LNegD* lir);
    void visitNegF(LNegF* lir);
    void visitCompareExchangeTypedArrayElement(LCompareExchangeTypedArrayElement* lir);
    void visitAtomicExchangeTypedArrayElement(LAtomicExchangeTypedArrayElement* lir);
    void visitAsmJSLoadHeap(LAsmJSLoadHeap* ins);
    void visitAsmJSStoreHeap(LAsmJSStoreHeap* ins);
    void visitWasmCompareExchangeHeap(LWasmCompareExchangeHeap* ins);
    void visitWasmAtomicBinopHeap(LWasmAtomicBinopHeap* ins);
    void visitWasmStackArg(LWasmStackArg* ins);

    void generateInvalidateEpilogue();

    void setReturnDoubleRegs(LiveRegisterSet* regs);

  protected:
    void postWasmCall(LWasmCall* lir) {
        MOZ_CRASH("postWasmCall");
    }

    void visitEffectiveAddress(LEffectiveAddress* ins);
    void visitUDiv(LUDiv* ins);
    void visitUMod(LUMod* ins);

  public:
    // Unimplemented SIMD instructions.
    void visitSimdSplatX4(LSimdSplatX4* lir) { MOZ_CRASH("NYI"); }
    void visitSimd128Int(LSimd128Int* ins) { MOZ_CRASH("NYI"); }
    void visitSimd128Float(LSimd128Float* ins) { MOZ_CRASH("NYI"); }
    void visitSimdExtractElementI(LSimdExtractElementI* ins) { MOZ_CRASH("NYI"); }
    void visitSimdExtractElementF(LSimdExtractElementF* ins) { MOZ_CRASH("NYI"); }
    void visitSimdBinaryCompIx4(LSimdBinaryCompIx4* lir) { MOZ_CRASH("NYI"); }
    void visitSimdBinaryCompFx4(LSimdBinaryCompFx4* lir) { MOZ_CRASH("NYI"); }
    void visitSimdBinaryArithIx4(LSimdBinaryArithIx4* lir) { MOZ_CRASH("NYI"); }
    void visitSimdBinaryArithFx4(LSimdBinaryArithFx4* lir) { MOZ_CRASH("NYI"); }
    void visitSimdBinaryBitwise(LSimdBinaryBitwise* lir) { MOZ_CRASH("NYI"); }
};

typedef CodeGeneratorARM64 CodeGeneratorSpecific;

// An out-of-line bailout thunk.
class OutOfLineBailout : public OutOfLineCodeBase<CodeGeneratorARM64>
{
  protected: // Silence Clang warning.
    LSnapshot* snapshot_;

  public:
    OutOfLineBailout(LSnapshot* snapshot)
      : snapshot_(snapshot)
    { }

    void accept(CodeGeneratorARM64* codegen) override;

    LSnapshot* snapshot() const {
        return snapshot_;
    }
};

} // namespace jit
} // namespace js

#endif /* jit_arm64_CodeGenerator_arm64_h */
