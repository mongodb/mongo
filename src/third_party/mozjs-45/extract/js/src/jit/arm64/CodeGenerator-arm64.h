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

    // FIXME: VIXL Operand does not match the platform-agnostic Operand,
    // which is just a union of possible arguments.
    inline Operand ToOperand(const LAllocation& a) {
        MOZ_CRASH("ToOperand");
    }
    inline Operand ToOperand(const LAllocation* a) {
        return ToOperand(*a);
    }
    inline Operand ToOperand(const LDefinition* def) {
        return ToOperand(def->output());
    }

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
    virtual void visitMinMaxD(LMinMaxD* ins);
    virtual void visitMinMaxF(LMinMaxF* math);
    virtual void visitAbsD(LAbsD* ins);
    virtual void visitAbsF(LAbsF* ins);
    virtual void visitSqrtD(LSqrtD* ins);
    virtual void visitSqrtF(LSqrtF* ins);
    virtual void visitAddI(LAddI* ins);
    virtual void visitSubI(LSubI* ins);
    virtual void visitBitNotI(LBitNotI* ins);
    virtual void visitBitOpI(LBitOpI* ins);

    virtual void visitMulI(LMulI* ins);

    virtual void visitDivI(LDivI* ins);
    virtual void visitDivPowTwoI(LDivPowTwoI* ins);
    virtual void visitModI(LModI* ins);
    virtual void visitModPowTwoI(LModPowTwoI* ins);
    virtual void visitModMaskI(LModMaskI* ins);
    virtual void visitPowHalfD(LPowHalfD* ins);
    virtual void visitShiftI(LShiftI* ins);
    virtual void visitUrshD(LUrshD* ins);

    virtual void visitTestIAndBranch(LTestIAndBranch* test);
    virtual void visitCompare(LCompare* comp);
    virtual void visitCompareAndBranch(LCompareAndBranch* comp);
    virtual void visitTestDAndBranch(LTestDAndBranch* test);
    virtual void visitTestFAndBranch(LTestFAndBranch* test);
    virtual void visitCompareD(LCompareD* comp);
    virtual void visitCompareF(LCompareF* comp);
    virtual void visitCompareDAndBranch(LCompareDAndBranch* comp);
    virtual void visitCompareFAndBranch(LCompareFAndBranch* comp);
    virtual void visitCompareB(LCompareB* lir);
    virtual void visitCompareBAndBranch(LCompareBAndBranch* lir);
    virtual void visitCompareBitwise(LCompareBitwise* lir);
    virtual void visitCompareBitwiseAndBranch(LCompareBitwiseAndBranch* lir);
    virtual void visitBitAndAndBranch(LBitAndAndBranch* baab);
    virtual void visitAsmJSUInt32ToDouble(LAsmJSUInt32ToDouble* lir);
    virtual void visitAsmJSUInt32ToFloat32(LAsmJSUInt32ToFloat32* lir);
    virtual void visitNotI(LNotI* ins);
    virtual void visitNotD(LNotD* ins);
    virtual void visitNotF(LNotF* ins);

    virtual void visitMathD(LMathD* math);
    virtual void visitMathF(LMathF* math);
    virtual void visitFloor(LFloor* lir);
    virtual void visitFloorF(LFloorF* lir);
    virtual void visitCeil(LCeil* lir);
    virtual void visitCeilF(LCeilF* lir);
    virtual void visitRound(LRound* lir);
    virtual void visitRoundF(LRoundF* lir);
    virtual void visitTruncateDToInt32(LTruncateDToInt32* ins);
    virtual void visitTruncateFToInt32(LTruncateFToInt32* ins);

    virtual void visitClzI(LClzI* lir);
    // Out of line visitors.
    void visitOutOfLineBailout(OutOfLineBailout* ool);
    void visitOutOfLineTableSwitch(OutOfLineTableSwitch* ool);

  protected:
    ValueOperand ToValue(LInstruction* ins, size_t pos);
    ValueOperand ToOutValue(LInstruction* ins);
    ValueOperand ToTempValue(LInstruction* ins, size_t pos);

    // Functions for LTestVAndBranch.
    Register splitTagForTest(const ValueOperand& value);

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

    void visitGuardShape(LGuardShape* guard);
    void visitGuardObjectGroup(LGuardObjectGroup* guard);
    void visitGuardClass(LGuardClass* guard);

    void visitInterruptCheck(LInterruptCheck* lir);

    void visitNegI(LNegI* lir);
    void visitNegD(LNegD* lir);
    void visitNegF(LNegF* lir);
    void visitLoadTypedArrayElementStatic(LLoadTypedArrayElementStatic* ins);
    void visitStoreTypedArrayElementStatic(LStoreTypedArrayElementStatic* ins);
    void visitCompareExchangeTypedArrayElement(LCompareExchangeTypedArrayElement* lir);
    void visitAtomicExchangeTypedArrayElement(LAtomicExchangeTypedArrayElement* lir);
    void visitAsmJSCall(LAsmJSCall* ins);
    void visitAsmJSLoadHeap(LAsmJSLoadHeap* ins);
    void visitAsmJSStoreHeap(LAsmJSStoreHeap* ins);
    void visitAsmJSCompareExchangeHeap(LAsmJSCompareExchangeHeap* ins);
    void visitAsmJSAtomicBinopHeap(LAsmJSAtomicBinopHeap* ins);
    void visitAsmJSLoadGlobalVar(LAsmJSLoadGlobalVar* ins);
    void visitAsmJSStoreGlobalVar(LAsmJSStoreGlobalVar* ins);
    void visitAsmJSLoadFuncPtr(LAsmJSLoadFuncPtr* ins);
    void visitAsmJSLoadFFIFunc(LAsmJSLoadFFIFunc* ins);
    void visitAsmJSPassStackArg(LAsmJSPassStackArg* ins);

    void generateInvalidateEpilogue();

    void setReturnDoubleRegs(LiveRegisterSet* regs);

  protected:
    void postAsmJSCall(LAsmJSCall* lir) {
        MOZ_CRASH("postAsmJSCall");
    }

    void visitEffectiveAddress(LEffectiveAddress* ins);
    void visitUDiv(LUDiv* ins);
    void visitUMod(LUMod* ins);

  public:
    // Unimplemented SIMD instructions.
    void visitSimdSplatX4(LSimdSplatX4* lir) { MOZ_CRASH("NYI"); }
    void visitInt32x4(LInt32x4* ins) { MOZ_CRASH("NYI"); }
    void visitFloat32x4(LFloat32x4* ins) { MOZ_CRASH("NYI"); }
    void visitSimdExtractElementI(LSimdExtractElementI* ins) { MOZ_CRASH("NYI"); }
    void visitSimdExtractElementF(LSimdExtractElementF* ins) { MOZ_CRASH("NYI"); }
    void visitSimdSignMaskX4(LSimdSignMaskX4* ins) { MOZ_CRASH("NYI"); }
    void visitSimdBinaryCompIx4(LSimdBinaryCompIx4* lir) { MOZ_CRASH("NYI"); }
    void visitSimdBinaryCompFx4(LSimdBinaryCompFx4* lir) { MOZ_CRASH("NYI"); }
    void visitSimdBinaryArithIx4(LSimdBinaryArithIx4* lir) { MOZ_CRASH("NYI"); }
    void visitSimdBinaryArithFx4(LSimdBinaryArithFx4* lir) { MOZ_CRASH("NYI"); }
    void visitSimdBinaryBitwiseX4(LSimdBinaryBitwiseX4* lir) { MOZ_CRASH("NYI"); }
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

    void accept(CodeGeneratorARM64* codegen);

    LSnapshot* snapshot() const {
        return snapshot_;
    }
};

} // namespace jit
} // namespace js

#endif /* jit_arm64_CodeGenerator_arm64_h */
