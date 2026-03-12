/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x86_shared_CodeGenerator_x86_shared_h
#define jit_x86_shared_CodeGenerator_x86_shared_h

#include "jit/shared/CodeGenerator-shared.h"
#include "js/ScalarType.h"  // js::Scalar::Type

namespace js {
namespace jit {

class CodeGeneratorX86Shared;
class ModOverflowCheck;
class OutOfLineTableSwitch;

using OutOfLineWasmTruncateCheck =
    OutOfLineWasmTruncateCheckBase<CodeGeneratorX86Shared>;

class CodeGeneratorX86Shared : public CodeGeneratorShared {
  friend class MoveResolverX86;

  template <typename T>
  void bailout(const T& t, LSnapshot* snapshot);

 protected:
  CodeGeneratorX86Shared(MIRGenerator* gen, LIRGraph* graph,
                         MacroAssembler* masm,
                         const wasm::CodeMetadata* wasmCodeMeta);

  NonAssertingLabel deoptLabel_;

  Operand ToOperand(const LAllocation& a);
  Operand ToOperand(const LAllocation* a);
  Operand ToOperand(const LDefinition* def);

#ifdef JS_PUNBOX64
  Operand ToOperandOrRegister64(const LInt64Allocation& input);
#else
  Register64 ToOperandOrRegister64(const LInt64Allocation& input);
#endif

  MoveOperand toMoveOperand(LAllocation a) const;

  void bailoutIf(Assembler::Condition condition, LSnapshot* snapshot);
  void bailoutIf(Assembler::DoubleCondition condition, LSnapshot* snapshot);
  void bailoutFrom(Label* label, LSnapshot* snapshot);
  void bailout(LSnapshot* snapshot);

  template <typename T1, typename T2>
  void bailoutCmpPtr(Assembler::Condition c, T1 lhs, T2 rhs,
                     LSnapshot* snapshot) {
    masm.cmpPtr(lhs, rhs);
    bailoutIf(c, snapshot);
  }
  void bailoutTestPtr(Assembler::Condition c, Register lhs, Register rhs,
                      LSnapshot* snapshot) {
    masm.testPtr(lhs, rhs);
    bailoutIf(c, snapshot);
  }
  template <typename T1, typename T2>
  void bailoutCmp32(Assembler::Condition c, T1 lhs, T2 rhs,
                    LSnapshot* snapshot) {
    masm.cmp32(lhs, rhs);
    bailoutIf(c, snapshot);
  }
  template <typename T1, typename T2>
  void bailoutTest32(Assembler::Condition c, T1 lhs, T2 rhs,
                     LSnapshot* snapshot) {
    masm.test32(lhs, rhs);
    bailoutIf(c, snapshot);
  }
  void bailoutIfFalseBool(Register reg, LSnapshot* snapshot) {
    masm.test32(reg, Imm32(0xFF));
    bailoutIf(Assembler::Zero, snapshot);
  }

  bool generateOutOfLineCode();

  // Emits a branch that directs control flow to the true block if |cond| is
  // true, and the false block if |cond| is false.
  void emitBranch(Assembler::Condition cond, MBasicBlock* ifTrue,
                  MBasicBlock* ifFalse);
  void emitBranch(Assembler::DoubleCondition cond, MBasicBlock* ifTrue,
                  MBasicBlock* ifFalse, Assembler::NaNCond ifNaN);

  void emitTableSwitchDispatch(MTableSwitch* mir, Register index,
                               Register base);

  void generateInvalidateEpilogue();

  template <typename T>
  Operand toMemoryAccessOperand(T* lir, int32_t disp);

 public:
  void emitUndoALUOperationOOL(LInstruction* ins);

  // Out of line visitors.
  void visitModOverflowCheck(ModOverflowCheck* ool);
  void visitOutOfLineTableSwitch(OutOfLineTableSwitch* ool);
  void visitOutOfLineWasmTruncateCheck(OutOfLineWasmTruncateCheck* ool);
};

}  // namespace jit
}  // namespace js

#endif /* jit_x86_shared_CodeGenerator_x86_shared_h */
