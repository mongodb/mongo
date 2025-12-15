/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x86_CodeGenerator_x86_h
#define jit_x86_CodeGenerator_x86_h

#include "jit/x86-shared/CodeGenerator-x86-shared.h"
#include "jit/x86/Assembler-x86.h"

namespace js {
namespace jit {

class OutOfLineTruncate;
class OutOfLineTruncateFloat32;

class CodeGeneratorX86 : public CodeGeneratorX86Shared {
 protected:
  CodeGeneratorX86(MIRGenerator* gen, LIRGraph* graph, MacroAssembler* masm,
                   const wasm::CodeMetadata* wasmCodeMeta);

  void emitBigIntPtrDiv(LBigIntPtrDiv* ins, Register dividend, Register divisor,
                        Register output);
  void emitBigIntPtrMod(LBigIntPtrMod* ins, Register dividend, Register divisor,
                        Register output);

  template <typename T>
  void emitWasmLoad(T* ins);
  template <typename T>
  void emitWasmStore(T* ins);
  template <typename T>
  void emitWasmStoreOrExchangeAtomicI64(T* ins,
                                        const wasm::MemoryAccessDesc& access);

 public:
  void visitOutOfLineTruncate(OutOfLineTruncate* ool);
  void visitOutOfLineTruncateFloat32(OutOfLineTruncateFloat32* ool);
};

using CodeGeneratorSpecific = CodeGeneratorX86;

}  // namespace jit
}  // namespace js

#endif /* jit_x86_CodeGenerator_x86_h */
