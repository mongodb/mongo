/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_shared_CodeGenerator_x86_shared_h
#define jit_shared_CodeGenerator_x86_shared_h

#include "jit/shared/CodeGenerator-shared.h"

namespace js {
namespace jit {

class OutOfLineBailout;
class OutOfLineUndoALUOperation;
class OutOfLineLoadTypedArrayOutOfBounds;
class MulNegativeZeroCheck;
class ModOverflowCheck;
class ReturnZero;
class OutOfLineTableSwitch;

class CodeGeneratorX86Shared : public CodeGeneratorShared
{
    friend class MoveResolverX86;

    CodeGeneratorX86Shared* thisFromCtor() {
        return this;
    }

    template <typename T>
    void bailout(const T& t, LSnapshot* snapshot);

  protected:

    // Load a NaN or zero into a register for an out of bounds AsmJS or static
    // typed array load.
    class OutOfLineLoadTypedArrayOutOfBounds : public OutOfLineCodeBase<CodeGeneratorX86Shared>
    {
        AnyRegister dest_;
        Scalar::Type viewType_;
      public:
        OutOfLineLoadTypedArrayOutOfBounds(AnyRegister dest, Scalar::Type viewType)
          : dest_(dest), viewType_(viewType)
        {}

        AnyRegister dest() const { return dest_; }
        Scalar::Type viewType() const { return viewType_; }
        void accept(CodeGeneratorX86Shared* codegen) {
            codegen->visitOutOfLineLoadTypedArrayOutOfBounds(this);
        }
    };

    // Label for the common return path.
    NonAssertingLabel returnLabel_;
    NonAssertingLabel deoptLabel_;

    inline Operand ToOperand(const LAllocation& a) {
        if (a.isGeneralReg())
            return Operand(a.toGeneralReg()->reg());
        if (a.isFloatReg())
            return Operand(a.toFloatReg()->reg());
        return Operand(StackPointer, ToStackOffset(&a));
    }
    inline Operand ToOperand(const LAllocation* a) {
        return ToOperand(*a);
    }
    inline Operand ToOperand(const LDefinition* def) {
        return ToOperand(def->output());
    }

    MoveOperand toMoveOperand(const LAllocation* a) const;

    void bailoutIf(Assembler::Condition condition, LSnapshot* snapshot);
    void bailoutIf(Assembler::DoubleCondition condition, LSnapshot* snapshot);
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
    void bailoutCvttsd2si(FloatRegister src, Register dest, LSnapshot* snapshot) {
        // vcvttsd2si returns 0x80000000 on failure. Test for it by
        // subtracting 1 and testing overflow. The other possibility is to test
        // equality for INT_MIN after a comparison, but 1 costs fewer bytes to
        // materialize.
        masm.vcvttsd2si(src, dest);
        masm.cmp32(dest, Imm32(1));
        bailoutIf(Assembler::Overflow, snapshot);
    }
    void bailoutCvttss2si(FloatRegister src, Register dest, LSnapshot* snapshot) {
        // Same trick as explained in the above comment.
        masm.vcvttss2si(src, dest);
        masm.cmp32(dest, Imm32(1));
        bailoutIf(Assembler::Overflow, snapshot);
    }

  protected:
    bool generatePrologue();
    bool generateEpilogue();
    bool generateOutOfLineCode();

    void emitCompare(MCompare::CompareType type, const LAllocation* left, const LAllocation* right);

    // Emits a branch that directs control flow to the true block if |cond| is
    // true, and the false block if |cond| is false.
    void emitBranch(Assembler::Condition cond, MBasicBlock* ifTrue, MBasicBlock* ifFalse,
                    Assembler::NaNCond ifNaN = Assembler::NaN_HandledByCond);
    void emitBranch(Assembler::DoubleCondition cond, MBasicBlock* ifTrue, MBasicBlock* ifFalse);

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
    CodeGeneratorX86Shared(MIRGenerator* gen, LIRGraph* graph, MacroAssembler* masm);

  public:
    // Instruction visitors.
    virtual void visitDouble(LDouble* ins);
    virtual void visitFloat32(LFloat32* ins);
    virtual void visitMinMaxD(LMinMaxD* ins);
    virtual void visitMinMaxF(LMinMaxF* ins);
    virtual void visitAbsD(LAbsD* ins);
    virtual void visitAbsF(LAbsF* ins);
    virtual void visitClzI(LClzI* ins);
    virtual void visitSqrtD(LSqrtD* ins);
    virtual void visitSqrtF(LSqrtF* ins);
    virtual void visitPowHalfD(LPowHalfD* ins);
    virtual void visitAddI(LAddI* ins);
    virtual void visitSubI(LSubI* ins);
    virtual void visitMulI(LMulI* ins);
    virtual void visitDivI(LDivI* ins);
    virtual void visitDivPowTwoI(LDivPowTwoI* ins);
    virtual void visitDivOrModConstantI(LDivOrModConstantI* ins);
    virtual void visitModI(LModI* ins);
    virtual void visitModPowTwoI(LModPowTwoI* ins);
    virtual void visitBitNotI(LBitNotI* ins);
    virtual void visitBitOpI(LBitOpI* ins);
    virtual void visitShiftI(LShiftI* ins);
    virtual void visitUrshD(LUrshD* ins);
    virtual void visitTestIAndBranch(LTestIAndBranch* test);
    virtual void visitTestDAndBranch(LTestDAndBranch* test);
    virtual void visitTestFAndBranch(LTestFAndBranch* test);
    virtual void visitCompare(LCompare* comp);
    virtual void visitCompareAndBranch(LCompareAndBranch* comp);
    virtual void visitCompareD(LCompareD* comp);
    virtual void visitCompareDAndBranch(LCompareDAndBranch* comp);
    virtual void visitCompareF(LCompareF* comp);
    virtual void visitCompareFAndBranch(LCompareFAndBranch* comp);
    virtual void visitBitAndAndBranch(LBitAndAndBranch* baab);
    virtual void visitNotI(LNotI* comp);
    virtual void visitNotD(LNotD* comp);
    virtual void visitNotF(LNotF* comp);
    virtual void visitMathD(LMathD* math);
    virtual void visitMathF(LMathF* math);
    virtual void visitFloor(LFloor* lir);
    virtual void visitFloorF(LFloorF* lir);
    virtual void visitCeil(LCeil* lir);
    virtual void visitCeilF(LCeilF* lir);
    virtual void visitRound(LRound* lir);
    virtual void visitRoundF(LRoundF* lir);
    virtual void visitGuardShape(LGuardShape* guard);
    virtual void visitGuardObjectGroup(LGuardObjectGroup* guard);
    virtual void visitGuardClass(LGuardClass* guard);
    virtual void visitEffectiveAddress(LEffectiveAddress* ins);
    virtual void visitUDivOrMod(LUDivOrMod* ins);
    virtual void visitAsmJSPassStackArg(LAsmJSPassStackArg* ins);
    virtual void visitMemoryBarrier(LMemoryBarrier* ins);

    void visitOutOfLineLoadTypedArrayOutOfBounds(OutOfLineLoadTypedArrayOutOfBounds* ool);

    void visitNegI(LNegI* lir);
    void visitNegD(LNegD* lir);
    void visitNegF(LNegF* lir);

    // SIMD operators
    void visitSimdValueInt32x4(LSimdValueInt32x4* lir);
    void visitSimdValueFloat32x4(LSimdValueFloat32x4* lir);
    void visitSimdSplatX4(LSimdSplatX4* lir);
    void visitInt32x4(LInt32x4* ins);
    void visitFloat32x4(LFloat32x4* ins);
    void visitInt32x4ToFloat32x4(LInt32x4ToFloat32x4* ins);
    void visitFloat32x4ToInt32x4(LFloat32x4ToInt32x4* ins);
    void visitSimdExtractElementI(LSimdExtractElementI* lir);
    void visitSimdExtractElementF(LSimdExtractElementF* lir);
    void visitSimdInsertElementI(LSimdInsertElementI* lir);
    void visitSimdInsertElementF(LSimdInsertElementF* lir);
    void visitSimdSignMaskX4(LSimdSignMaskX4* ins);
    void visitSimdSwizzleI(LSimdSwizzleI* lir);
    void visitSimdSwizzleF(LSimdSwizzleF* lir);
    void visitSimdShuffle(LSimdShuffle* lir);
    void visitSimdUnaryArithIx4(LSimdUnaryArithIx4* lir);
    void visitSimdUnaryArithFx4(LSimdUnaryArithFx4* lir);
    void visitSimdBinaryCompIx4(LSimdBinaryCompIx4* lir);
    void visitSimdBinaryCompFx4(LSimdBinaryCompFx4* lir);
    void visitSimdBinaryArithIx4(LSimdBinaryArithIx4* lir);
    void visitSimdBinaryArithFx4(LSimdBinaryArithFx4* lir);
    void visitSimdBinaryBitwiseX4(LSimdBinaryBitwiseX4* lir);
    void visitSimdShift(LSimdShift* lir);
    void visitSimdSelect(LSimdSelect* ins);

    // Out of line visitors.
    void visitOutOfLineBailout(OutOfLineBailout* ool);
    void visitOutOfLineUndoALUOperation(OutOfLineUndoALUOperation* ool);
    void visitMulNegativeZeroCheck(MulNegativeZeroCheck* ool);
    void visitModOverflowCheck(ModOverflowCheck* ool);
    void visitReturnZero(ReturnZero* ool);
    void visitOutOfLineTableSwitch(OutOfLineTableSwitch* ool);
    void generateInvalidateEpilogue();
};

// An out-of-line bailout thunk.
class OutOfLineBailout : public OutOfLineCodeBase<CodeGeneratorX86Shared>
{
    LSnapshot* snapshot_;

  public:
    explicit OutOfLineBailout(LSnapshot* snapshot)
      : snapshot_(snapshot)
    { }

    void accept(CodeGeneratorX86Shared* codegen);

    LSnapshot* snapshot() const {
        return snapshot_;
    }
};

} // namespace jit
} // namespace js

#endif /* jit_shared_CodeGenerator_x86_shared_h */
