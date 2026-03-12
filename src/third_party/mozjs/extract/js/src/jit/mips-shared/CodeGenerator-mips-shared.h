/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_mips_shared_CodeGenerator_mips_shared_h
#define jit_mips_shared_CodeGenerator_mips_shared_h

#include "jit/shared/CodeGenerator-shared.h"

namespace js {
namespace jit {

class CodeGeneratorMIPSShared;
class OutOfLineTableSwitch;

using OutOfLineWasmTruncateCheck =
    OutOfLineWasmTruncateCheckBase<CodeGeneratorMIPSShared>;

class CodeGeneratorMIPSShared : public CodeGeneratorShared {
  friend class MoveResolverMIPS;

 protected:
  CodeGeneratorMIPSShared(MIRGenerator* gen, LIRGraph* graph,
                          MacroAssembler* masm,
                          const wasm::CodeMetadata* wasmCodeMeta);

  NonAssertingLabel deoptLabel_;

  Operand ToOperand(const LAllocation& a);
  Operand ToOperand(const LAllocation* a);
  Operand ToOperand(const LDefinition* def);

  Operand ToOperandOrRegister64(const LInt64Allocation& input);

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
  void visitOutOfLineTableSwitch(OutOfLineTableSwitch* ool);
  void visitOutOfLineWasmTruncateCheck(OutOfLineWasmTruncateCheck* ool);
};

}  // namespace jit
}  // namespace js

#endif /* jit_mips_shared_CodeGenerator_mips_shared_h */
