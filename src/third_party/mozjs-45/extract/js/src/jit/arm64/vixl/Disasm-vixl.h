// Copyright 2015, ARM Limited
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of ARM Limited nor the names of its contributors may be
//     used to endorse or promote products derived from this software without
//     specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#ifndef VIXL_A64_DISASM_A64_H
#define VIXL_A64_DISASM_A64_H

#include "jit/arm64/vixl/Assembler-vixl.h"
#include "jit/arm64/vixl/Decoder-vixl.h"
#include "jit/arm64/vixl/Globals-vixl.h"
#include "jit/arm64/vixl/Instructions-vixl.h"
#include "jit/arm64/vixl/Utils-vixl.h"

namespace vixl {

class Disassembler: public DecoderVisitor {
 public:
  Disassembler();
  Disassembler(char* text_buffer, int buffer_size);
  virtual ~Disassembler();
  char* GetOutput();

  // Declare all Visitor functions.
  #define DECLARE(A) virtual void Visit##A(const Instruction* instr);
  VISITOR_LIST(DECLARE)
  #undef DECLARE

 protected:
  virtual void ProcessOutput(const Instruction* instr);

  // Default output functions.  The functions below implement a default way of
  // printing elements in the disassembly. A sub-class can override these to
  // customize the disassembly output.

  // Prints the name of a register.
  // TODO: This currently doesn't allow renaming of V registers.
  virtual void AppendRegisterNameToOutput(const Instruction* instr,
                                          const CPURegister& reg);

  // Prints a PC-relative offset. This is used for example when disassembling
  // branches to immediate offsets.
  virtual void AppendPCRelativeOffsetToOutput(const Instruction* instr,
                                              int64_t offset);

  // Prints an address, in the general case. It can be code or data. This is
  // used for example to print the target address of an ADR instruction.
  virtual void AppendCodeRelativeAddressToOutput(const Instruction* instr,
                                                 const void* addr);

  // Prints the address of some code.
  // This is used for example to print the target address of a branch to an
  // immediate offset.
  // A sub-class can for example override this method to lookup the address and
  // print an appropriate name.
  virtual void AppendCodeRelativeCodeAddressToOutput(const Instruction* instr,
                                                     const void* addr);

  // Prints the address of some data.
  // This is used for example to print the source address of a load literal
  // instruction.
  virtual void AppendCodeRelativeDataAddressToOutput(const Instruction* instr,
                                                     const void* addr);

  // Same as the above, but for addresses that are not relative to the code
  // buffer. They are currently not used by VIXL.
  virtual void AppendAddressToOutput(const Instruction* instr,
                                     const void* addr);
  virtual void AppendCodeAddressToOutput(const Instruction* instr,
                                         const void* addr);
  virtual void AppendDataAddressToOutput(const Instruction* instr,
                                         const void* addr);

 public:
  // Get/Set the offset that should be added to code addresses when printing
  // code-relative addresses in the AppendCodeRelative<Type>AddressToOutput()
  // helpers.
  // Below is an example of how a branch immediate instruction in memory at
  // address 0xb010200 would disassemble with different offsets.
  // Base address | Disassembly
  //          0x0 | 0xb010200:  b #+0xcc  (addr 0xb0102cc)
  //      0x10000 | 0xb000200:  b #+0xcc  (addr 0xb0002cc)
  //    0xb010200 |       0x0:  b #+0xcc  (addr 0xcc)
  void MapCodeAddress(int64_t base_address, const Instruction* instr_address);
  int64_t CodeRelativeAddress(const void* instr);

 private:
  void Format(
      const Instruction* instr, const char* mnemonic, const char* format);
  void Substitute(const Instruction* instr, const char* string);
  int SubstituteField(const Instruction* instr, const char* format);
  int SubstituteRegisterField(const Instruction* instr, const char* format);
  int SubstituteImmediateField(const Instruction* instr, const char* format);
  int SubstituteLiteralField(const Instruction* instr, const char* format);
  int SubstituteBitfieldImmediateField(
      const Instruction* instr, const char* format);
  int SubstituteShiftField(const Instruction* instr, const char* format);
  int SubstituteExtendField(const Instruction* instr, const char* format);
  int SubstituteConditionField(const Instruction* instr, const char* format);
  int SubstitutePCRelAddressField(const Instruction* instr, const char* format);
  int SubstituteBranchTargetField(const Instruction* instr, const char* format);
  int SubstituteLSRegOffsetField(const Instruction* instr, const char* format);
  int SubstitutePrefetchField(const Instruction* instr, const char* format);
  int SubstituteBarrierField(const Instruction* instr, const char* format);
  int SubstituteSysOpField(const Instruction* instr, const char* format);
  int SubstituteCrField(const Instruction* instr, const char* format);
  bool RdIsZROrSP(const Instruction* instr) const {
    return (instr->Rd() == kZeroRegCode);
  }

  bool RnIsZROrSP(const Instruction* instr) const {
    return (instr->Rn() == kZeroRegCode);
  }

  bool RmIsZROrSP(const Instruction* instr) const {
    return (instr->Rm() == kZeroRegCode);
  }

  bool RaIsZROrSP(const Instruction* instr) const {
    return (instr->Ra() == kZeroRegCode);
  }

  bool IsMovzMovnImm(unsigned reg_size, uint64_t value);

  int64_t code_address_offset() const { return code_address_offset_; }

 protected:
  void ResetOutput();
  void AppendToOutput(const char* string, ...) PRINTF_CHECK(2, 3);

  void set_code_address_offset(int64_t code_address_offset) {
    code_address_offset_ = code_address_offset;
  }

  char* buffer_;
  uint32_t buffer_pos_;
  uint32_t buffer_size_;
  bool own_buffer_;

  int64_t code_address_offset_;
};


class PrintDisassembler: public Disassembler {
 public:
  explicit PrintDisassembler(FILE* stream) : stream_(stream) { }

 protected:
  virtual void ProcessOutput(const Instruction* instr);

 private:
  FILE *stream_;
};
}  // namespace vixl

#endif  // VIXL_A64_DISASM_A64_H
