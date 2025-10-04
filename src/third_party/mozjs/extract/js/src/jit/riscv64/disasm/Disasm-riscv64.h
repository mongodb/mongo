/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 */
// Copyright 2007-2008 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef jit_riscv64_disasm_Disasm_riscv64_h
#define jit_riscv64_disasm_Disasm_riscv64_h

#include "mozilla/Assertions.h"
#include "mozilla/Types.h"

#include <stdio.h>

#include "jit/riscv64/constant/Constant-riscv64.h"
#include "jit/riscv64/constant/util-riscv64.h"
namespace js {
namespace jit {
namespace disasm {

typedef unsigned char byte;

// Interface and default implementation for converting addresses and
// register-numbers to text.  The default implementation is machine
// specific.
class NameConverter {
 public:
  virtual ~NameConverter() {}
  virtual const char* NameOfCPURegister(int reg) const;
  virtual const char* NameOfByteCPURegister(int reg) const;
  virtual const char* NameOfXMMRegister(int reg) const;
  virtual const char* NameOfAddress(byte* addr) const;
  virtual const char* NameOfConstant(byte* addr) const;
  virtual const char* NameInCode(byte* addr) const;

 protected:
  EmbeddedVector<char, 128> tmp_buffer_;
};

// A generic Disassembler interface
class Disassembler {
 public:
  // Caller deallocates converter.
  explicit Disassembler(const NameConverter& converter);

  virtual ~Disassembler();

  // Writes one disassembled instruction into 'buffer' (0-terminated).
  // Returns the length of the disassembled machine instruction in bytes.
  int InstructionDecode(V8Vector<char> buffer, uint8_t* instruction);

  // Returns -1 if instruction does not mark the beginning of a constant pool,
  // or the number of entries in the constant pool beginning here.
  int ConstantPoolSizeAt(byte* instruction);

  // Write disassembly into specified file 'f' using specified NameConverter
  // (see constructor).
  static void Disassemble(FILE* f, uint8_t* begin, uint8_t* end);

 private:
  const NameConverter& converter_;

  // Disallow implicit constructors.
  Disassembler() = delete;
  Disassembler(const Disassembler&) = delete;
  void operator=(const Disassembler&) = delete;
};

}  // namespace disasm
}  // namespace jit
}  // namespace js

#endif  // jit_riscv64_disasm_Disasm_riscv64_h
