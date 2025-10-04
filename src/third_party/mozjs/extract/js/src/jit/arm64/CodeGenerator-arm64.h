/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_arm64_CodeGenerator_arm64_h
#define jit_arm64_CodeGenerator_arm64_h

#include "jit/arm64/Assembler-arm64.h"
#include "jit/shared/CodeGenerator-shared.h"

namespace js {
namespace jit {

class CodeGeneratorARM64;
class OutOfLineBailout;
class OutOfLineTableSwitch;

using OutOfLineWasmTruncateCheck =
    OutOfLineWasmTruncateCheckBase<CodeGeneratorARM64>;

class CodeGeneratorARM64 : public CodeGeneratorShared {
  friend class MoveResolverARM64;

 protected:
  CodeGeneratorARM64(MIRGenerator* gen, LIRGraph* graph, MacroAssembler* masm);

  NonAssertingLabel deoptLabel_;

  MoveOperand toMoveOperand(const LAllocation a) const;

  void bailoutIf(Assembler::Condition condition, LSnapshot* snapshot);
  void bailoutFrom(Label* label, LSnapshot* snapshot);
  void bailout(LSnapshot* snapshot);

  template <typename T1, typename T2>
  void bailoutCmpPtr(Assembler::Condition c, T1 lhs, T2 rhs,
                     LSnapshot* snapshot) {
    masm.cmpPtr(lhs, rhs);
    return bailoutIf(c, snapshot);
  }
  void bailoutTestPtr(Assembler::Condition c, Register lhs, Register rhs,
                      LSnapshot* snapshot) {
    masm.testPtr(lhs, rhs);
    return bailoutIf(c, snapshot);
  }
  template <typename T1, typename T2>
  void bailoutCmp32(Assembler::Condition c, T1 lhs, T2 rhs,
                    LSnapshot* snapshot) {
    masm.cmp32(lhs, rhs);
    return bailoutIf(c, snapshot);
  }
  template <typename T1, typename T2>
  void bailoutTest32(Assembler::Condition c, T1 lhs, T2 rhs,
                     LSnapshot* snapshot) {
    masm.test32(lhs, rhs);
    return bailoutIf(c, snapshot);
  }
  void bailoutIfFalseBool(Register reg, LSnapshot* snapshot) {
    masm.test32(reg, Imm32(0xFF));
    return bailoutIf(Assembler::Zero, snapshot);
  }

  bool generateOutOfLineCode();

  // Emits a branch that directs control flow to the true block if |cond| is
  // true, and the false block if |cond| is false.
  void emitBranch(Assembler::Condition cond, MBasicBlock* ifTrue,
                  MBasicBlock* ifFalse);

  void testNullEmitBranch(Assembler::Condition cond, const ValueOperand& value,
                          MBasicBlock* ifTrue, MBasicBlock* ifFalse) {
    cond = masm.testNull(cond, value);
    emitBranch(cond, ifTrue, ifFalse);
  }
  void testUndefinedEmitBranch(Assembler::Condition cond,
                               const ValueOperand& value, MBasicBlock* ifTrue,
                               MBasicBlock* ifFalse) {
    cond = masm.testUndefined(cond, value);
    emitBranch(cond, ifTrue, ifFalse);
  }
  void testObjectEmitBranch(Assembler::Condition cond,
                            const ValueOperand& value, MBasicBlock* ifTrue,
                            MBasicBlock* ifFalse) {
    cond = masm.testObject(cond, value);
    emitBranch(cond, ifTrue, ifFalse);
  }
  void testZeroEmitBranch(Assembler::Condition cond, Register reg,
                          MBasicBlock* ifTrue, MBasicBlock* ifFalse) {
    MOZ_ASSERT(cond == Assembler::Equal || cond == Assembler::NotEqual);
    masm.cmpPtr(reg, ImmWord(0));
    emitBranch(cond, ifTrue, ifFalse);
  }

  void emitTableSwitchDispatch(MTableSwitch* mir, Register index,
                               Register base);

  void emitBigIntDiv(LBigIntDiv* ins, Register dividend, Register divisor,
                     Register output, Label* fail);
  void emitBigIntMod(LBigIntMod* ins, Register dividend, Register divisor,
                     Register output, Label* fail);
  void emitSimpleBinaryI64(
      LInstructionHelper<INT64_PIECES, 2 * INT64_PIECES, 0>* lir, JSOp op);

  ValueOperand ToValue(LInstruction* ins, size_t pos);
  ValueOperand ToTempValue(LInstruction* ins, size_t pos);

  void generateInvalidateEpilogue();

 public:
  void visitOutOfLineBailout(OutOfLineBailout* ool);
  void visitOutOfLineTableSwitch(OutOfLineTableSwitch* ool);
  void visitOutOfLineWasmTruncateCheck(OutOfLineWasmTruncateCheck* ool);
};

typedef CodeGeneratorARM64 CodeGeneratorSpecific;

// An out-of-line bailout thunk.
class OutOfLineBailout : public OutOfLineCodeBase<CodeGeneratorARM64> {
 protected:  // Silence Clang warning.
  LSnapshot* snapshot_;

 public:
  explicit OutOfLineBailout(LSnapshot* snapshot) : snapshot_(snapshot) {}

  void accept(CodeGeneratorARM64* codegen) override;

  LSnapshot* snapshot() const { return snapshot_; }
};

}  // namespace jit
}  // namespace js

#endif /* jit_arm64_CodeGenerator_arm64_h */
