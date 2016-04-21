/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x86_shared_CodeGenerator_x86_shared_h
#define jit_x86_shared_CodeGenerator_x86_shared_h

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

    // Additional bounds checking for heap accesses with constant offsets.
    class OffsetBoundsCheck : public OutOfLineCodeBase<CodeGeneratorX86Shared>
    {
        Label* outOfBounds_;
        Register ptrReg_;
        int32_t offset_;
      public:
        OffsetBoundsCheck(Label* outOfBounds, Register ptrReg, int32_t offset)
          : outOfBounds_(outOfBounds), ptrReg_(ptrReg), offset_(offset)
        {}

        Label* outOfBounds() const { return outOfBounds_; }
        Register ptrReg() const { return ptrReg_; }
        int32_t offset() const { return offset_; }
        void accept(CodeGeneratorX86Shared* codegen) {
            codegen->visitOffsetBoundsCheck(this);
        }
    };

    // Additional bounds check for vector Float to Int conversion, when the
    // undefined pattern is seen. Might imply a bailout.
    class OutOfLineSimdFloatToIntCheck : public OutOfLineCodeBase<CodeGeneratorX86Shared>
    {
        Register temp_;
        FloatRegister input_;
        LInstruction* ins_;

      public:
        OutOfLineSimdFloatToIntCheck(Register temp, FloatRegister input, LInstruction *ins)
          : temp_(temp), input_(input), ins_(ins)
        {}

        Register temp() const { return temp_; }
        FloatRegister input() const { return input_; }
        LInstruction* ins() const { return ins_; }

        void accept(CodeGeneratorX86Shared* codegen) {
            codegen->visitOutOfLineSimdFloatToIntCheck(this);
        }
    };

    // Functions for emitting bounds-checking code with branches.
    MOZ_WARN_UNUSED_RESULT
    uint32_t emitAsmJSBoundsCheckBranch(const MAsmJSHeapAccess* mir, const MInstruction* ins,
                                        Register ptr, Label* fail);
    void cleanupAfterAsmJSBoundsCheckBranch(const MAsmJSHeapAccess* mir, Register ptr);

    NonAssertingLabel deoptLabel_;

    MoveOperand toMoveOperand(LAllocation a) const;

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
    virtual void visitUDivOrModConstant(LUDivOrModConstant *ins);
    virtual void visitAsmJSPassStackArg(LAsmJSPassStackArg* ins);
    virtual void visitMemoryBarrier(LMemoryBarrier* ins);
    virtual void visitAtomicTypedArrayElementBinop(LAtomicTypedArrayElementBinop* lir);
    virtual void visitAtomicTypedArrayElementBinopForEffect(LAtomicTypedArrayElementBinopForEffect* lir);
    virtual void visitCompareExchangeTypedArrayElement(LCompareExchangeTypedArrayElement* lir);
    virtual void visitAtomicExchangeTypedArrayElement(LAtomicExchangeTypedArrayElement* lir);

    void visitOutOfLineLoadTypedArrayOutOfBounds(OutOfLineLoadTypedArrayOutOfBounds* ool);
    void visitOffsetBoundsCheck(OffsetBoundsCheck* oolCheck);

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
    void visitSimdReinterpretCast(LSimdReinterpretCast* lir);
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

    template <class T, class Reg> void visitSimdGeneralShuffle(LSimdGeneralShuffleBase* lir, Reg temp);
    void visitSimdGeneralShuffleI(LSimdGeneralShuffleI* lir);
    void visitSimdGeneralShuffleF(LSimdGeneralShuffleF* lir);

    // Out of line visitors.
    void visitOutOfLineBailout(OutOfLineBailout* ool);
    void visitOutOfLineUndoALUOperation(OutOfLineUndoALUOperation* ool);
    void visitMulNegativeZeroCheck(MulNegativeZeroCheck* ool);
    void visitModOverflowCheck(ModOverflowCheck* ool);
    void visitReturnZero(ReturnZero* ool);
    void visitOutOfLineTableSwitch(OutOfLineTableSwitch* ool);
    void visitOutOfLineSimdFloatToIntCheck(OutOfLineSimdFloatToIntCheck* ool);
    void generateInvalidateEpilogue();

    // Generating a result.
    template<typename S, typename T>
    void atomicBinopToTypedIntArray(AtomicOp op, Scalar::Type arrayType, const S& value,
                                    const T& mem, Register temp1, Register temp2, AnyRegister output);

    // Generating no result.
    template<typename S, typename T>
    void atomicBinopToTypedIntArray(AtomicOp op, Scalar::Type arrayType, const S& value, const T& mem);

    void setReturnDoubleRegs(LiveRegisterSet* regs);
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

#endif /* jit_x86_shared_CodeGenerator_x86_shared_h */
