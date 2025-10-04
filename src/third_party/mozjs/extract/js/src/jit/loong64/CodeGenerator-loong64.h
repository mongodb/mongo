/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_loong64_CodeGenerator_loong64_h
#define jit_loong64_CodeGenerator_loong64_h

#include "jit/loong64/Assembler-loong64.h"
#include "jit/shared/CodeGenerator-shared.h"

namespace js {
namespace jit {

class CodeGeneratorLOONG64;
class OutOfLineBailout;
class OutOfLineTableSwitch;

using OutOfLineWasmTruncateCheck =
    OutOfLineWasmTruncateCheckBase<CodeGeneratorLOONG64>;

class CodeGeneratorLOONG64 : public CodeGeneratorShared {
  friend class MoveResolverLA;

 protected:
  CodeGeneratorLOONG64(MIRGenerator* gen, LIRGraph* graph,
                       MacroAssembler* masm);

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
  void bailoutCmp32(Assembler::Condition c, T1 lhs, T2 rhs,
                    LSnapshot* snapshot) {
    Label bail;
    masm.branch32(c, lhs, rhs, &bail);
    bailoutFrom(&bail, snapshot);
  }
  template <typename T1, typename T2>
  void bailoutTest32(Assembler::Condition c, T1 lhs, T2 rhs,
                     LSnapshot* snapshot) {
    Label bail;
    masm.branchTest32(c, lhs, rhs, &bail);
    bailoutFrom(&bail, snapshot);
  }
  template <typename T1, typename T2>
  void bailoutCmpPtr(Assembler::Condition c, T1 lhs, T2 rhs,
                     LSnapshot* snapshot) {
    Label bail;
    masm.branchPtr(c, lhs, rhs, &bail);
    bailoutFrom(&bail, snapshot);
  }
  void bailoutTestPtr(Assembler::Condition c, Register lhs, Register rhs,
                      LSnapshot* snapshot) {
    // TODO(loong64) Didn't use branchTestPtr due to '-Wundefined-inline'.
    MOZ_ASSERT(c == Assembler::Zero || c == Assembler::NonZero ||
               c == Assembler::Signed || c == Assembler::NotSigned);
    Label bail;
    if (lhs == rhs) {
      masm.ma_b(lhs, rhs, &bail, c);
    } else {
      ScratchRegisterScope scratch(masm);
      masm.as_and(scratch, lhs, rhs);
      masm.ma_b(scratch, scratch, &bail, c);
    }
    bailoutFrom(&bail, snapshot);
  }
  void bailoutIfFalseBool(Register reg, LSnapshot* snapshot) {
    Label bail;
    ScratchRegisterScope scratch(masm);
    masm.ma_and(scratch, reg, Imm32(0xFF));
    masm.ma_b(scratch, scratch, &bail, Assembler::Zero);
    bailoutFrom(&bail, snapshot);
  }

  void bailoutFrom(Label* label, LSnapshot* snapshot);
  void bailout(LSnapshot* snapshot);

  bool generateOutOfLineCode();

  template <typename T>
  void branchToBlock(Register lhs, T rhs, MBasicBlock* mir,
                     Assembler::Condition cond) {
    masm.ma_b(lhs, rhs, skipTrivialBlocks(mir)->lir()->label(), cond);
  }
  void branchToBlock(Assembler::FloatFormat fmt, FloatRegister lhs,
                     FloatRegister rhs, MBasicBlock* mir,
                     Assembler::DoubleCondition cond);

  // Emits a branch that directs control flow to the true block if |cond| is
  // true, and the false block if |cond| is false.
  template <typename T>
  void emitBranch(Register lhs, T rhs, Assembler::Condition cond,
                  MBasicBlock* mirTrue, MBasicBlock* mirFalse) {
    if (isNextBlock(mirFalse->lir())) {
      branchToBlock(lhs, rhs, mirTrue, cond);
    } else {
      branchToBlock(lhs, rhs, mirFalse, Assembler::InvertCondition(cond));
      jumpToBlock(mirTrue);
    }
  }
  void testZeroEmitBranch(Assembler::Condition cond, Register reg,
                          MBasicBlock* ifTrue, MBasicBlock* ifFalse) {
    emitBranch(reg, Imm32(0), cond, ifTrue, ifFalse);
  }

  void emitTableSwitchDispatch(MTableSwitch* mir, Register index,
                               Register base);

  template <typename T>
  void emitWasmLoad(T* ins);
  template <typename T>
  void emitWasmStore(T* ins);

  void generateInvalidateEpilogue();

  // Generating a result.
  template <typename S, typename T>
  void atomicBinopToTypedIntArray(AtomicOp op, Scalar::Type arrayType,
                                  const S& value, const T& mem,
                                  Register flagTemp, Register outTemp,
                                  Register valueTemp, Register offsetTemp,
                                  Register maskTemp, AnyRegister output);

  // Generating no result.
  template <typename S, typename T>
  void atomicBinopToTypedIntArray(AtomicOp op, Scalar::Type arrayType,
                                  const S& value, const T& mem,
                                  Register flagTemp, Register valueTemp,
                                  Register offsetTemp, Register maskTemp);

 public:
  // Out of line visitors.
  void visitOutOfLineBailout(OutOfLineBailout* ool);
  void visitOutOfLineTableSwitch(OutOfLineTableSwitch* ool);
  void visitOutOfLineWasmTruncateCheck(OutOfLineWasmTruncateCheck* ool);

 protected:
  void testNullEmitBranch(Assembler::Condition cond, const ValueOperand& value,
                          MBasicBlock* ifTrue, MBasicBlock* ifFalse) {
    MOZ_ASSERT(value.valueReg() != SecondScratchReg);
    masm.splitTag(value.valueReg(), SecondScratchReg);
    emitBranch(SecondScratchReg, ImmTag(JSVAL_TAG_NULL), cond, ifTrue, ifFalse);
  }
  void testUndefinedEmitBranch(Assembler::Condition cond,
                               const ValueOperand& value, MBasicBlock* ifTrue,
                               MBasicBlock* ifFalse) {
    MOZ_ASSERT(value.valueReg() != SecondScratchReg);
    masm.splitTag(value.valueReg(), SecondScratchReg);
    emitBranch(SecondScratchReg, ImmTag(JSVAL_TAG_UNDEFINED), cond, ifTrue,
               ifFalse);
  }
  void testObjectEmitBranch(Assembler::Condition cond,
                            const ValueOperand& value, MBasicBlock* ifTrue,
                            MBasicBlock* ifFalse) {
    MOZ_ASSERT(value.valueReg() != SecondScratchReg);
    masm.splitTag(value.valueReg(), SecondScratchReg);
    emitBranch(SecondScratchReg, ImmTag(JSVAL_TAG_OBJECT), cond, ifTrue,
               ifFalse);
  }

  void emitBigIntDiv(LBigIntDiv* ins, Register dividend, Register divisor,
                     Register output, Label* fail);
  void emitBigIntMod(LBigIntMod* ins, Register dividend, Register divisor,
                     Register output, Label* fail);

  template <typename T>
  void emitWasmLoadI64(T* ins);
  template <typename T>
  void emitWasmStoreI64(T* ins);

  ValueOperand ToValue(LInstruction* ins, size_t pos);
  ValueOperand ToTempValue(LInstruction* ins, size_t pos);

  // Functions for LTestVAndBranch.
  void splitTagForTest(const ValueOperand& value, ScratchTagScope& tag);
};

typedef CodeGeneratorLOONG64 CodeGeneratorSpecific;

// An out-of-line bailout thunk.
class OutOfLineBailout : public OutOfLineCodeBase<CodeGeneratorLOONG64> {
 protected:
  LSnapshot* snapshot_;

 public:
  OutOfLineBailout(LSnapshot* snapshot) : snapshot_(snapshot) {}

  void accept(CodeGeneratorLOONG64* codegen) override;

  LSnapshot* snapshot() const { return snapshot_; }
};

}  // namespace jit
}  // namespace js

#endif /* jit_loong64_CodeGenerator_loong64_h */
