/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/Disassemble.h"

#include <stddef.h>  // size_t
#include <stdint.h>  // uint8_t, int32_t

#include "js/Printf.h"   // JS_smprintf
#include "js/Utility.h"  // JS::UniqueChars

#if defined(JS_JITSPEW)
#  if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
#    include "zydis/ZydisAPI.h"  // zydisDisassemble
#  elif defined(JS_CODEGEN_ARM64)
#    include "jit/arm/disasm/Disasm-arm.h"         // js::jit::disasm::*
#    include "jit/arm64/vixl/Decoder-vixl.h"       // vixl::Decoder
#    include "jit/arm64/vixl/Disasm-vixl.h"        // vixl::Disassembler
#    include "jit/arm64/vixl/Instructions-vixl.h"  // vixl::Instruction
#  elif defined(JS_CODEGEN_ARM)
#    include "jit/arm/disasm/Disasm-arm.h"  // js::jit::disasm::*
#  endif
#endif

namespace js {
namespace jit {

#if defined(JS_JITSPEW) && (defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64))

bool HasDisassembler() { return true; }

void Disassemble(uint8_t* code, size_t length, InstrCallback callback) {
  zydisDisassemble(code, length, callback);
}

#elif defined(JS_JITSPEW) && defined(JS_CODEGEN_ARM64)

class ARM64Disassembler : public vixl::Disassembler {
 public:
  explicit ARM64Disassembler(InstrCallback callback) : callback_(callback) {}

 protected:
  void ProcessOutput(const vixl::Instruction* instr) override {
    JS::UniqueChars formatted = JS_smprintf(
        "0x%p  %08x  %s", instr, instr->InstructionBits(), GetOutput());
    callback_(formatted.get());
  }

 private:
  InstrCallback callback_;
};

bool HasDisassembler() { return true; }

void Disassemble(uint8_t* code, size_t length, InstrCallback callback) {
  ARM64Disassembler dis(callback);
  vixl::Decoder decoder;
  decoder.AppendVisitor(&dis);

  uint8_t* instr = code;
  uint8_t* end = code + length;

  while (instr < end) {
    decoder.Decode(reinterpret_cast<vixl::Instruction*>(instr));

    instr += sizeof(vixl::Instr);
  }
}

#elif defined(JS_JITSPEW) && defined(JS_CODEGEN_ARM)

bool HasDisassembler() { return true; }

void Disassemble(uint8_t* code, size_t length, InstrCallback callback) {
  disasm::NameConverter converter;
  disasm::Disassembler d(converter);

  uint8_t* instr = code;
  uint8_t* end = code + length;

  while (instr < end) {
    disasm::EmbeddedVector<char, disasm::ReasonableBufferSize> buffer;
    buffer[0] = '\0';
    uint8_t* next_instr = instr + d.InstructionDecode(buffer, instr);

    JS::UniqueChars formatted =
        JS_smprintf("0x%p  %08x  %s", instr, *reinterpret_cast<int32_t*>(instr),
                    buffer.start());
    callback(formatted.get());

    instr = next_instr;
  }
}

#else

bool HasDisassembler() { return false; }

void Disassemble(uint8_t* code, size_t length, InstrCallback callback) {
  callback("*** No disassembly available ***\n");
}

#endif

}  // namespace jit
}  // namespace js
