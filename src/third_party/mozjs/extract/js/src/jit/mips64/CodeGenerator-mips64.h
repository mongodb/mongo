/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_mips64_CodeGenerator_mips64_h
#define jit_mips64_CodeGenerator_mips64_h

#include "jit/mips-shared/CodeGenerator-mips-shared.h"

namespace js {
namespace jit {

class CodeGeneratorMIPS64 : public CodeGeneratorMIPSShared {
 protected:
  CodeGeneratorMIPS64(MIRGenerator* gen, LIRGraph* graph, MacroAssembler* masm)
      : CodeGeneratorMIPSShared(gen, graph, masm) {}

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

typedef CodeGeneratorMIPS64 CodeGeneratorSpecific;

}  // namespace jit
}  // namespace js

#endif /* jit_mips64_CodeGenerator_mips64_h */
