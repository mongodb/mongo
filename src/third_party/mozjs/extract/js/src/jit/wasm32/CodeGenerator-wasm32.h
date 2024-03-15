/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_wasm32_CodeGenerator_wasm32_h
#define jit_wasm32_CodeGenerator_wasm32_h

#include "jit/shared/CodeGenerator-shared.h"

namespace js::jit {

class CodeGeneratorWasm32 : public CodeGeneratorShared {
 protected:
  CodeGeneratorWasm32(MIRGenerator* gen, LIRGraph* graph, MacroAssembler* masm)
      : CodeGeneratorShared(gen, graph, masm) {
    MOZ_CRASH();
  }

  MoveOperand toMoveOperand(LAllocation) const { MOZ_CRASH(); }
  template <typename T1, typename T2>
  void bailoutCmp32(Assembler::Condition, T1, T2, LSnapshot*) {
    MOZ_CRASH();
  }
  template <typename T1, typename T2>
  void bailoutTest32(Assembler::Condition, T1, T2, LSnapshot*) {
    MOZ_CRASH();
  }
  template <typename T1, typename T2>
  void bailoutCmpPtr(Assembler::Condition, T1, T2, LSnapshot*) {
    MOZ_CRASH();
  }
  void bailoutTestPtr(Assembler::Condition, Register, Register, LSnapshot*) {
    MOZ_CRASH();
  }
  void bailoutIfFalseBool(Register, LSnapshot*) { MOZ_CRASH(); }
  void bailoutFrom(Label*, LSnapshot*) { MOZ_CRASH(); }
  void bailout(LSnapshot*) { MOZ_CRASH(); }
  void bailoutIf(Assembler::Condition, LSnapshot*) { MOZ_CRASH(); }
  bool generateOutOfLineCode() { MOZ_CRASH(); }
  void testNullEmitBranch(Assembler::Condition, ValueOperand, MBasicBlock*,
                          MBasicBlock*) {
    MOZ_CRASH();
  }
  void testUndefinedEmitBranch(Assembler::Condition, ValueOperand, MBasicBlock*,
                               MBasicBlock*) {
    MOZ_CRASH();
  }
  void testObjectEmitBranch(Assembler::Condition, ValueOperand, MBasicBlock*,
                            MBasicBlock*) {
    MOZ_CRASH();
  }
  void testZeroEmitBranch(Assembler::Condition, Register, MBasicBlock*,
                          MBasicBlock*) {
    MOZ_CRASH();
  }
  void emitTableSwitchDispatch(MTableSwitch*, Register, Register) {
    MOZ_CRASH();
  }
  void emitBigIntDiv(LBigIntDiv*, Register, Register, Register, Label*) {
    MOZ_CRASH();
  }
  void emitBigIntMod(LBigIntMod*, Register, Register, Register, Label*) {
    MOZ_CRASH();
  }
  ValueOperand ToValue(LInstruction*, size_t) { MOZ_CRASH(); }
  ValueOperand ToTempValue(LInstruction*, size_t) { MOZ_CRASH(); }
  void generateInvalidateEpilogue() { MOZ_CRASH(); }
};

typedef CodeGeneratorWasm32 CodeGeneratorSpecific;

}  // namespace js::jit

#endif /* jit_wasm32_CodeGenerator_wasm32_h */
