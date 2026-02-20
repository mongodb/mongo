/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x64_CodeGenerator_x64_h
#define jit_x64_CodeGenerator_x64_h

#include "jit/x86-shared/CodeGenerator-x86-shared.h"

namespace js {
namespace jit {

class OutOfLineTruncate;

class CodeGeneratorX64 : public CodeGeneratorX86Shared {
 protected:
  CodeGeneratorX64(MIRGenerator* gen, LIRGraph* graph, MacroAssembler* masm,
                   const wasm::CodeMetadata* wasmCodeMeta);

  Operand ToOperand64(const LInt64Allocation& a);

  void emitBigIntPtrDiv(LBigIntPtrDiv* ins, Register dividend, Register divisor,
                        Register output);
  void emitBigIntPtrMod(LBigIntPtrMod* ins, Register dividend, Register divisor,
                        Register output);

  void wasmStore(const wasm::MemoryAccessDesc& access, const LAllocation* value,
                 Operand dstAddr);
  template <typename T>
  void emitWasmLoad(T* ins);
  template <typename T>
  void emitWasmStore(T* ins);

 public:
  void visitOutOfLineTruncate(OutOfLineTruncate* ool);
};

using CodeGeneratorSpecific = CodeGeneratorX64;

}  // namespace jit
}  // namespace js

#endif /* jit_x64_CodeGenerator_x64_h */
