/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Copyright (c) 1994-2006 Sun Microsystems Inc.
// All Rights Reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// - Redistributions of source code must retain the above copyright notice,
// this list of conditions and the following disclaimer.
//
// - Redistribution in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// - Neither the name of Sun Microsystems or the names of contributors may
// be used to endorse or promote products derived from this software without
// specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
// IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// The original source code covered by the above license above has been
// modified significantly by Google Inc.
// Copyright 2021 the V8 project authors. All rights reserved.
#include "jit/riscv64/Assembler-riscv64.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/Maybe.h"

#include "gc/Marking.h"
#include "jit/AutoWritableJitCode.h"
#include "jit/ExecutableAllocator.h"
#include "jit/riscv64/disasm/Disasm-riscv64.h"
#include "vm/Realm.h"

using mozilla::DebugOnly;
namespace js {
namespace jit {

#define UNIMPLEMENTED_RISCV() MOZ_CRASH("RISC_V not implemented");

bool Assembler::FLAG_riscv_debug = false;

void Assembler::nop() { addi(ToRegister(0), ToRegister(0), 0); }

// Size of the instruction stream, in bytes.
size_t Assembler::size() const { return m_buffer.size(); }

bool Assembler::swapBuffer(wasm::Bytes& bytes) {
  // For now, specialize to the one use case. As long as wasm::Bytes is a
  // Vector, not a linked-list of chunks, there's not much we can do other
  // than copy.
  MOZ_ASSERT(bytes.empty());
  if (!bytes.resize(bytesNeeded())) {
    return false;
  }
  m_buffer.executableCopy(bytes.begin());
  return true;
}

// Size of the relocation table, in bytes.
size_t Assembler::jumpRelocationTableBytes() const {
  return jumpRelocations_.length();
}

size_t Assembler::dataRelocationTableBytes() const {
  return dataRelocations_.length();
}
// Size of the data table, in bytes.
size_t Assembler::bytesNeeded() const {
  return size() + jumpRelocationTableBytes() + dataRelocationTableBytes();
}

void Assembler::executableCopy(uint8_t* buffer) {
  MOZ_ASSERT(isFinished);
  m_buffer.executableCopy(buffer);
}

uint32_t Assembler::AsmPoolMaxOffset = 1024;

uint32_t Assembler::GetPoolMaxOffset() {
  static bool isSet = false;
  if (!isSet) {
    char* poolMaxOffsetStr = getenv("ASM_POOL_MAX_OFFSET");
    uint32_t poolMaxOffset;
    if (poolMaxOffsetStr &&
        sscanf(poolMaxOffsetStr, "%u", &poolMaxOffset) == 1) {
      AsmPoolMaxOffset = poolMaxOffset;
    }
    isSet = true;
  }
  return AsmPoolMaxOffset;
}

// Pool callbacks stuff:
void Assembler::InsertIndexIntoTag(uint8_t* load_, uint32_t index) {
  MOZ_CRASH("Unimplement");
}

void Assembler::PatchConstantPoolLoad(void* loadAddr, void* constPoolAddr) {
  MOZ_CRASH("Unimplement");
}

void Assembler::processCodeLabels(uint8_t* rawCode) {
  for (const CodeLabel& label : codeLabels_) {
    Bind(rawCode, label);
  }
}

void Assembler::WritePoolGuard(BufferOffset branch, Instruction* dest,
                               BufferOffset afterPool) {
  DEBUG_PRINTF("\tWritePoolGuard\n");
  int32_t off = afterPool.getOffset() - branch.getOffset();
  if (!is_int21(off) || !((off & 0x1) == 0)) {
    printf("%d\n", off);
    MOZ_CRASH("imm invalid");
  }
  // JAL encode is
  //   31    | 30    21  |  20     | 19     12  | 11 7 |  6   0 |
  // imm[20] | imm[10:1] | imm[11] | imm[19:12] |  rd  |  opcode|
  //   1           10         1           8         5       7
  //                   offset[20:1]               dest      JAL
  int32_t imm20 = (off & 0xff000) |          // bits 19-12
                  ((off & 0x800) << 9) |     // bit  11
                  ((off & 0x7fe) << 20) |    // bits 10-1
                  ((off & 0x100000) << 11);  // bit  20
  Instr instr = JAL | (imm20 & kImm20Mask);
  dest->SetInstructionBits(instr);
  DEBUG_PRINTF("%p(%x): ", dest, branch.getOffset());
  disassembleInstr(dest->InstructionBits(), JitSpew_Codegen);
}

void Assembler::WritePoolHeader(uint8_t* start, Pool* p, bool isNatural) {
  static_assert(sizeof(PoolHeader) == 4);

  // Get the total size of the pool.
  const uintptr_t totalPoolSize = sizeof(PoolHeader) + p->getPoolSize();
  const uintptr_t totalPoolInstructions = totalPoolSize / kInstrSize;

  MOZ_ASSERT((totalPoolSize & 0x3) == 0);
  MOZ_ASSERT(totalPoolInstructions < (1 << 15));

  PoolHeader header(totalPoolInstructions, isNatural);
  *(PoolHeader*)start = header;
}

void Assembler::copyJumpRelocationTable(uint8_t* dest) {
  if (jumpRelocations_.length()) {
    memcpy(dest, jumpRelocations_.buffer(), jumpRelocations_.length());
  }
}

void Assembler::copyDataRelocationTable(uint8_t* dest) {
  if (dataRelocations_.length()) {
    memcpy(dest, dataRelocations_.buffer(), dataRelocations_.length());
  }
}

void Assembler::RV_li(Register rd, int64_t imm) {
  UseScratchRegisterScope temps(this);
  if (RecursiveLiCount(imm) > GeneralLiCount(imm, temps.hasAvailable())) {
    GeneralLi(rd, imm);
  } else {
    RecursiveLi(rd, imm);
  }
}

int Assembler::RV_li_count(int64_t imm, bool is_get_temp_reg) {
  if (RecursiveLiCount(imm) > GeneralLiCount(imm, is_get_temp_reg)) {
    return GeneralLiCount(imm, is_get_temp_reg);
  } else {
    return RecursiveLiCount(imm);
  }
}

void Assembler::GeneralLi(Register rd, int64_t imm) {
  // 64-bit imm is put in the register rd.
  // In most cases the imm is 32 bit and 2 instructions are generated. If a
  // temporary register is available, in the worst case, 6 instructions are
  // generated for a full 64-bit immediate. If temporay register is not
  // available the maximum will be 8 instructions. If imm is more than 32 bits
  // and a temp register is available, imm is divided into two 32-bit parts,
  // low_32 and up_32. Each part is built in a separate register. low_32 is
  // built before up_32. If low_32 is negative (upper 32 bits are 1), 0xffffffff
  // is subtracted from up_32 before up_32 is built. This compensates for 32
  // bits of 1's in the lower when the two registers are added. If no temp is
  // available, the upper 32 bit is built in rd, and the lower 32 bits are
  // devided to 3 parts (11, 11, and 10 bits). The parts are shifted and added
  // to the upper part built in rd.
  if (is_int32(imm + 0x800)) {
    // 32-bit case. Maximum of 2 instructions generated
    int64_t high_20 = ((imm + 0x800) >> 12);
    int64_t low_12 = imm << 52 >> 52;
    if (high_20) {
      lui(rd, (int32_t)high_20);
      if (low_12) {
        addi(rd, rd, low_12);
      }
    } else {
      addi(rd, zero_reg, low_12);
    }
    return;
  } else {
    UseScratchRegisterScope temps(this);
    BlockTrampolinePoolScope block_trampoline_pool(this, 8);
    // 64-bit case: divide imm into two 32-bit parts, upper and lower
    int64_t up_32 = imm >> 32;
    int64_t low_32 = imm & 0xffffffffull;
    Register temp_reg = rd;
    // Check if a temporary register is available
    if (up_32 == 0 || low_32 == 0) {
      // No temp register is needed
    } else {
      temp_reg = temps.hasAvailable() ? temps.Acquire() : InvalidReg;
    }
    if (temp_reg != InvalidReg) {
      // keep track of hardware behavior for lower part in sim_low
      int64_t sim_low = 0;
      // Build lower part
      if (low_32 != 0) {
        int64_t high_20 = ((low_32 + 0x800) >> 12);
        int64_t low_12 = low_32 & 0xfff;
        if (high_20) {
          // Adjust to 20 bits for the case of overflow
          high_20 &= 0xfffff;
          sim_low = ((high_20 << 12) << 32) >> 32;
          lui(rd, (int32_t)high_20);
          if (low_12) {
            sim_low += (low_12 << 52 >> 52) | low_12;
            addi(rd, rd, low_12);
          }
        } else {
          sim_low = low_12;
          ori(rd, zero_reg, low_12);
        }
      }
      if (sim_low & 0x100000000) {
        // Bit 31 is 1. Either an overflow or a negative 64 bit
        if (up_32 == 0) {
          // Positive number, but overflow because of the add 0x800
          slli(rd, rd, 32);
          srli(rd, rd, 32);
          return;
        }
        // low_32 is a negative 64 bit after the build
        up_32 = (up_32 - 0xffffffff) & 0xffffffff;
      }
      if (up_32 == 0) {
        return;
      }
      // Build upper part in a temporary register
      if (low_32 == 0) {
        // Build upper part in rd
        temp_reg = rd;
      }
      int64_t high_20 = (up_32 + 0x800) >> 12;
      int64_t low_12 = up_32 & 0xfff;
      if (high_20) {
        // Adjust to 20 bits for the case of overflow
        high_20 &= 0xfffff;
        lui(temp_reg, (int32_t)high_20);
        if (low_12) {
          addi(temp_reg, temp_reg, low_12);
        }
      } else {
        ori(temp_reg, zero_reg, low_12);
      }
      // Put it at the bgining of register
      slli(temp_reg, temp_reg, 32);
      if (low_32 != 0) {
        add(rd, rd, temp_reg);
      }
      return;
    }
    // No temp register. Build imm in rd.
    // Build upper 32 bits first in rd. Divide lower 32 bits parts and add
    // parts to the upper part by doing shift and add.
    // First build upper part in rd.
    int64_t high_20 = (up_32 + 0x800) >> 12;
    int64_t low_12 = up_32 & 0xfff;
    if (high_20) {
      // Adjust to 20 bits for the case of overflow
      high_20 &= 0xfffff;
      lui(rd, (int32_t)high_20);
      if (low_12) {
        addi(rd, rd, low_12);
      }
    } else {
      ori(rd, zero_reg, low_12);
    }
    // upper part already in rd. Each part to be added to rd, has maximum of 11
    // bits, and always starts with a 1. rd is shifted by the size of the part
    // plus the number of zeros between the parts. Each part is added after the
    // left shift.
    uint32_t mask = 0x80000000;
    int32_t shift_val = 0;
    int32_t i;
    for (i = 0; i < 32; i++) {
      if ((low_32 & mask) == 0) {
        mask >>= 1;
        shift_val++;
        if (i == 31) {
          // rest is zero
          slli(rd, rd, shift_val);
        }
        continue;
      }
      // The first 1 seen
      int32_t part;
      if ((i + 11) < 32) {
        // Pick 11 bits
        part = ((uint32_t)(low_32 << i) >> i) >> (32 - (i + 11));
        slli(rd, rd, shift_val + 11);
        ori(rd, rd, part);
        i += 10;
        mask >>= 11;
      } else {
        part = (uint32_t)(low_32 << i) >> i;
        slli(rd, rd, shift_val + (32 - i));
        ori(rd, rd, part);
        break;
      }
      shift_val = 0;
    }
  }
}

int Assembler::GeneralLiCount(int64_t imm, bool is_get_temp_reg) {
  int count = 0;
  // imitate Assembler::RV_li
  if (is_int32(imm + 0x800)) {
    // 32-bit case. Maximum of 2 instructions generated
    int64_t high_20 = ((imm + 0x800) >> 12);
    int64_t low_12 = imm << 52 >> 52;
    if (high_20) {
      count++;
      if (low_12) {
        count++;
      }
    } else {
      count++;
    }
    return count;
  } else {
    // 64-bit case: divide imm into two 32-bit parts, upper and lower
    int64_t up_32 = imm >> 32;
    int64_t low_32 = imm & 0xffffffffull;
    // Check if a temporary register is available
    if (is_get_temp_reg) {
      // keep track of hardware behavior for lower part in sim_low
      int64_t sim_low = 0;
      // Build lower part
      if (low_32 != 0) {
        int64_t high_20 = ((low_32 + 0x800) >> 12);
        int64_t low_12 = low_32 & 0xfff;
        if (high_20) {
          // Adjust to 20 bits for the case of overflow
          high_20 &= 0xfffff;
          sim_low = ((high_20 << 12) << 32) >> 32;
          count++;
          if (low_12) {
            sim_low += (low_12 << 52 >> 52) | low_12;
            count++;
          }
        } else {
          sim_low = low_12;
          count++;
        }
      }
      if (sim_low & 0x100000000) {
        // Bit 31 is 1. Either an overflow or a negative 64 bit
        if (up_32 == 0) {
          // Positive number, but overflow because of the add 0x800
          count++;
          count++;
          return count;
        }
        // low_32 is a negative 64 bit after the build
        up_32 = (up_32 - 0xffffffff) & 0xffffffff;
      }
      if (up_32 == 0) {
        return count;
      }
      int64_t high_20 = (up_32 + 0x800) >> 12;
      int64_t low_12 = up_32 & 0xfff;
      if (high_20) {
        // Adjust to 20 bits for the case of overflow
        high_20 &= 0xfffff;
        count++;
        if (low_12) {
          count++;
        }
      } else {
        count++;
      }
      // Put it at the bgining of register
      count++;
      if (low_32 != 0) {
        count++;
      }
      return count;
    }
    // No temp register. Build imm in rd.
    // Build upper 32 bits first in rd. Divide lower 32 bits parts and add
    // parts to the upper part by doing shift and add.
    // First build upper part in rd.
    int64_t high_20 = (up_32 + 0x800) >> 12;
    int64_t low_12 = up_32 & 0xfff;
    if (high_20) {
      // Adjust to 20 bits for the case of overflow
      high_20 &= 0xfffff;
      count++;
      if (low_12) {
        count++;
      }
    } else {
      count++;
    }
    // upper part already in rd. Each part to be added to rd, has maximum of 11
    // bits, and always starts with a 1. rd is shifted by the size of the part
    // plus the number of zeros between the parts. Each part is added after the
    // left shift.
    uint32_t mask = 0x80000000;
    int32_t i;
    for (i = 0; i < 32; i++) {
      if ((low_32 & mask) == 0) {
        mask >>= 1;
        if (i == 31) {
          // rest is zero
          count++;
        }
        continue;
      }
      // The first 1 seen
      if ((i + 11) < 32) {
        // Pick 11 bits
        count++;
        count++;
        i += 10;
        mask >>= 11;
      } else {
        count++;
        count++;
        break;
      }
    }
  }
  return count;
}

void Assembler::li_ptr(Register rd, int64_t imm) {
  m_buffer.enterNoNops();
  m_buffer.assertNoPoolAndNoNops();
  // Initialize rd with an address
  // Pointers are 48 bits
  // 6 fixed instructions are generated
  DEBUG_PRINTF("li_ptr(%d, %lx <%ld>)\n", ToNumber(rd), imm, imm);
  MOZ_ASSERT((imm & 0xfff0000000000000ll) == 0);
  int64_t a6 = imm & 0x3f;                      // bits 0:5. 6 bits
  int64_t b11 = (imm >> 6) & 0x7ff;             // bits 6:11. 11 bits
  int64_t high_31 = (imm >> 17) & 0x7fffffff;   // 31 bits
  int64_t high_20 = ((high_31 + 0x800) >> 12);  // 19 bits
  int64_t low_12 = high_31 & 0xfff;             // 12 bits
  lui(rd, (int32_t)high_20);
  addi(rd, rd, low_12);  // 31 bits in rd.
  slli(rd, rd, 11);      // Space for next 11 bis
  ori(rd, rd, b11);      // 11 bits are put in. 42 bit in rd
  slli(rd, rd, 6);       // Space for next 6 bits
  ori(rd, rd, a6);       // 6 bits are put in. 48 bis in rd
  m_buffer.leaveNoNops();
}

void Assembler::li_constant(Register rd, int64_t imm) {
  m_buffer.enterNoNops();
  m_buffer.assertNoPoolAndNoNops();
  DEBUG_PRINTF("li_constant(%d, %lx <%ld>)\n", ToNumber(rd), imm, imm);
  lui(rd, (imm + (1LL << 47) + (1LL << 35) + (1LL << 23) + (1LL << 11)) >>
              48);  // Bits 63:48
  addiw(rd, rd,
        (imm + (1LL << 35) + (1LL << 23) + (1LL << 11)) << 16 >>
            52);  // Bits 47:36
  slli(rd, rd, 12);
  addi(rd, rd, (imm + (1LL << 23) + (1LL << 11)) << 28 >> 52);  // Bits 35:24
  slli(rd, rd, 12);
  addi(rd, rd, (imm + (1LL << 11)) << 40 >> 52);  // Bits 23:12
  slli(rd, rd, 12);
  addi(rd, rd, imm << 52 >> 52);  // Bits 11:0
  m_buffer.leaveNoNops();
}

ABIArg ABIArgGenerator::next(MIRType type) {
  switch (type) {
    case MIRType::Int32:
    case MIRType::Int64:
    case MIRType::Pointer:
    case MIRType::WasmAnyRef:
    case MIRType::StackResults: {
      if (intRegIndex_ == NumIntArgRegs) {
        current_ = ABIArg(stackOffset_);
        stackOffset_ += sizeof(uintptr_t);
        break;
      }
      current_ = ABIArg(Register::FromCode(intRegIndex_ + a0.encoding()));
      intRegIndex_++;
      break;
    }
    case MIRType::Float32:
    case MIRType::Double: {
      if (floatRegIndex_ == NumFloatArgRegs) {
        current_ = ABIArg(stackOffset_);
        stackOffset_ += sizeof(double);
        break;
      }
      current_ = ABIArg(FloatRegister(
          FloatRegisters::Encoding(floatRegIndex_ + fa0.encoding()),
          type == MIRType::Double ? FloatRegisters::Double
                                  : FloatRegisters::Single));
      floatRegIndex_++;
      break;
    }
    case MIRType::Simd128: {
      MOZ_CRASH("RISCV64 does not support simd yet.");
      break;
    }
    default:
      MOZ_CRASH("Unexpected argument type");
  }
  return current_;
}

bool Assembler::oom() const {
  return AssemblerShared::oom() || m_buffer.oom() || jumpRelocations_.oom() ||
         dataRelocations_.oom() || !enoughLabelCache_;
}

int Assembler::disassembleInstr(Instr instr, bool enable_spew) {
  if (!FLAG_riscv_debug && !enable_spew) return -1;
  disasm::NameConverter converter;
  disasm::Disassembler disasm(converter);
  EmbeddedVector<char, 128> disasm_buffer;

  int size =
      disasm.InstructionDecode(disasm_buffer, reinterpret_cast<byte*>(&instr));
  DEBUG_PRINTF("%s\n", disasm_buffer.start());
  if (enable_spew) {
    JitSpew(JitSpew_Codegen, "%s", disasm_buffer.start());
  }
  return size;
}

uintptr_t Assembler::target_address_at(Instruction* pc) {
  Instruction* instr0 = pc;
  DEBUG_PRINTF("target_address_at: pc: 0x%p\t", instr0);
  Instruction* instr1 = pc + 1 * kInstrSize;
  Instruction* instr2 = pc + 2 * kInstrSize;
  Instruction* instr3 = pc + 3 * kInstrSize;
  Instruction* instr4 = pc + 4 * kInstrSize;
  Instruction* instr5 = pc + 5 * kInstrSize;

  // Interpret instructions for address generated by li: See listing in
  // Assembler::set_target_address_at() just below.
  if (IsLui(*reinterpret_cast<Instr*>(instr0)) &&
      IsAddi(*reinterpret_cast<Instr*>(instr1)) &&
      IsSlli(*reinterpret_cast<Instr*>(instr2)) &&
      IsOri(*reinterpret_cast<Instr*>(instr3)) &&
      IsSlli(*reinterpret_cast<Instr*>(instr4)) &&
      IsOri(*reinterpret_cast<Instr*>(instr5))) {
    // Assemble the 64 bit value.
    int64_t addr = (int64_t)(instr0->Imm20UValue() << kImm20Shift) +
                   (int64_t)instr1->Imm12Value();
    MOZ_ASSERT(instr2->Imm12Value() == 11);
    addr <<= 11;
    addr |= (int64_t)instr3->Imm12Value();
    MOZ_ASSERT(instr4->Imm12Value() == 6);
    addr <<= 6;
    addr |= (int64_t)instr5->Imm12Value();

    DEBUG_PRINTF("addr: %lx\n", addr);
    return static_cast<uintptr_t>(addr);
  }
  // We should never get here, force a bad address if we do.
  MOZ_CRASH("RISC-V  UNREACHABLE");
}

void Assembler::PatchDataWithValueCheck(CodeLocationLabel label,
                                        ImmPtr newValue, ImmPtr expectedValue) {
  PatchDataWithValueCheck(label, PatchedImmPtr(newValue.value),
                          PatchedImmPtr(expectedValue.value));
}

void Assembler::PatchDataWithValueCheck(CodeLocationLabel label,
                                        PatchedImmPtr newValue,
                                        PatchedImmPtr expectedValue) {
  Instruction* inst = (Instruction*)label.raw();

  // Extract old Value
  DebugOnly<uint64_t> value = Assembler::ExtractLoad64Value(inst);
  MOZ_ASSERT(value == uint64_t(expectedValue.value));

  // Replace with new value
  Assembler::UpdateLoad64Value(inst, uint64_t(newValue.value));
}

uint64_t Assembler::ExtractLoad64Value(Instruction* inst0) {
  DEBUG_PRINTF("\tExtractLoad64Value: \tpc:%p ", inst0);
  if (IsJal(*reinterpret_cast<Instr*>(inst0))) {
    int offset = inst0->Imm20JValue();
    inst0 = inst0 + offset;
  }
  Instruction* instr1 = inst0 + 1 * kInstrSize;
  if (IsAddiw(*reinterpret_cast<Instr*>(instr1))) {
    // Li64
    Instruction* instr2 = inst0 + 2 * kInstrSize;
    Instruction* instr3 = inst0 + 3 * kInstrSize;
    Instruction* instr4 = inst0 + 4 * kInstrSize;
    Instruction* instr5 = inst0 + 5 * kInstrSize;
    Instruction* instr6 = inst0 + 6 * kInstrSize;
    Instruction* instr7 = inst0 + 7 * kInstrSize;
    if (IsLui(*reinterpret_cast<Instr*>(inst0)) &&
        IsAddiw(*reinterpret_cast<Instr*>(instr1)) &&
        IsSlli(*reinterpret_cast<Instr*>(instr2)) &&
        IsAddi(*reinterpret_cast<Instr*>(instr3)) &&
        IsSlli(*reinterpret_cast<Instr*>(instr4)) &&
        IsAddi(*reinterpret_cast<Instr*>(instr5)) &&
        IsSlli(*reinterpret_cast<Instr*>(instr6)) &&
        IsAddi(*reinterpret_cast<Instr*>(instr7))) {
      int64_t imm = (int64_t)(inst0->Imm20UValue() << kImm20Shift) +
                    (int64_t)instr1->Imm12Value();
      MOZ_ASSERT(instr2->Imm12Value() == 12);
      imm <<= 12;
      imm += (int64_t)instr3->Imm12Value();
      MOZ_ASSERT(instr4->Imm12Value() == 12);
      imm <<= 12;
      imm += (int64_t)instr5->Imm12Value();
      MOZ_ASSERT(instr6->Imm12Value() == 12);
      imm <<= 12;
      imm += (int64_t)instr7->Imm12Value();
      DEBUG_PRINTF("imm:%lx\n", imm);
      return imm;
    } else {
      FLAG_riscv_debug = true;
      disassembleInstr(inst0->InstructionBits());
      disassembleInstr(instr1->InstructionBits());
      disassembleInstr(instr2->InstructionBits());
      disassembleInstr(instr3->InstructionBits());
      disassembleInstr(instr4->InstructionBits());
      disassembleInstr(instr5->InstructionBits());
      disassembleInstr(instr6->InstructionBits());
      disassembleInstr(instr7->InstructionBits());
      MOZ_CRASH();
    }
  } else {
    DEBUG_PRINTF("\n");
    Instruction* instrf1 = (inst0 - 1 * kInstrSize);
    Instruction* instr2 = inst0 + 2 * kInstrSize;
    Instruction* instr3 = inst0 + 3 * kInstrSize;
    Instruction* instr4 = inst0 + 4 * kInstrSize;
    Instruction* instr5 = inst0 + 5 * kInstrSize;
    Instruction* instr6 = inst0 + 6 * kInstrSize;
    Instruction* instr7 = inst0 + 7 * kInstrSize;
    disassembleInstr(instrf1->InstructionBits());
    disassembleInstr(inst0->InstructionBits());
    disassembleInstr(instr1->InstructionBits());
    disassembleInstr(instr2->InstructionBits());
    disassembleInstr(instr3->InstructionBits());
    disassembleInstr(instr4->InstructionBits());
    disassembleInstr(instr5->InstructionBits());
    disassembleInstr(instr6->InstructionBits());
    disassembleInstr(instr7->InstructionBits());
    MOZ_ASSERT(IsAddi(*reinterpret_cast<Instr*>(instr1)));
    // Li48
    return target_address_at(inst0);
  }
}

void Assembler::UpdateLoad64Value(Instruction* pc, uint64_t value) {
  DEBUG_PRINTF("\tUpdateLoad64Value: pc: %p\tvalue: %lx\n", pc, value);
  Instruction* instr1 = pc + 1 * kInstrSize;
  if (IsJal(*reinterpret_cast<Instr*>(pc))) {
    pc = pc + pc->Imm20JValue();
    instr1 = pc + 1 * kInstrSize;
  }
  if (IsAddiw(*reinterpret_cast<Instr*>(instr1))) {
    Instruction* instr0 = pc;
    Instruction* instr2 = pc + 2 * kInstrSize;
    Instruction* instr3 = pc + 3 * kInstrSize;
    Instruction* instr4 = pc + 4 * kInstrSize;
    Instruction* instr5 = pc + 5 * kInstrSize;
    Instruction* instr6 = pc + 6 * kInstrSize;
    Instruction* instr7 = pc + 7 * kInstrSize;
    MOZ_ASSERT(IsLui(*reinterpret_cast<Instr*>(pc)) &&
               IsAddiw(*reinterpret_cast<Instr*>(instr1)) &&
               IsSlli(*reinterpret_cast<Instr*>(instr2)) &&
               IsAddi(*reinterpret_cast<Instr*>(instr3)) &&
               IsSlli(*reinterpret_cast<Instr*>(instr4)) &&
               IsAddi(*reinterpret_cast<Instr*>(instr5)) &&
               IsSlli(*reinterpret_cast<Instr*>(instr6)) &&
               IsAddi(*reinterpret_cast<Instr*>(instr7)));
    // lui(rd, (imm + (1LL << 47) + (1LL << 35) + (1LL << 23) + (1LL << 11)) >>
    //             48);  // Bits 63:48
    // addiw(rd, rd,
    //       (imm + (1LL << 35) + (1LL << 23) + (1LL << 11)) << 16 >>
    //           52);  // Bits 47:36
    // slli(rd, rd, 12);
    // addi(rd, rd, (imm + (1LL << 23) + (1LL << 11)) << 28 >> 52);  // Bits
    // 35:24 slli(rd, rd, 12); addi(rd, rd, (imm + (1LL << 11)) << 40 >> 52); //
    // Bits 23:12 slli(rd, rd, 12); addi(rd, rd, imm << 52 >> 52);  // Bits 11:0
    *reinterpret_cast<Instr*>(instr0) &= 0xfff;
    *reinterpret_cast<Instr*>(instr0) |=
        (((value + (1LL << 47) + (1LL << 35) + (1LL << 23) + (1LL << 11)) >> 48)
         << 12);
    *reinterpret_cast<Instr*>(instr1) &= 0xfffff;
    *reinterpret_cast<Instr*>(instr1) |=
        (((value + (1LL << 35) + (1LL << 23) + (1LL << 11)) << 16 >> 52) << 20);
    *reinterpret_cast<Instr*>(instr3) &= 0xfffff;
    *reinterpret_cast<Instr*>(instr3) |=
        (((value + (1LL << 23) + (1LL << 11)) << 28 >> 52) << 20);
    *reinterpret_cast<Instr*>(instr5) &= 0xfffff;
    *reinterpret_cast<Instr*>(instr5) |=
        (((value + (1LL << 11)) << 40 >> 52) << 20);
    *reinterpret_cast<Instr*>(instr7) &= 0xfffff;
    *reinterpret_cast<Instr*>(instr7) |= ((value << 52 >> 52) << 20);
    disassembleInstr(instr0->InstructionBits());
    disassembleInstr(instr1->InstructionBits());
    disassembleInstr(instr2->InstructionBits());
    disassembleInstr(instr3->InstructionBits());
    disassembleInstr(instr4->InstructionBits());
    disassembleInstr(instr5->InstructionBits());
    disassembleInstr(instr6->InstructionBits());
    disassembleInstr(instr7->InstructionBits());
    MOZ_ASSERT(ExtractLoad64Value(pc) == value);
  } else {
    Instruction* instr0 = pc;
    Instruction* instr2 = pc + 2 * kInstrSize;
    Instruction* instr3 = pc + 3 * kInstrSize;
    Instruction* instr4 = pc + 4 * kInstrSize;
    Instruction* instr5 = pc + 5 * kInstrSize;
    Instruction* instr6 = pc + 6 * kInstrSize;
    Instruction* instr7 = pc + 7 * kInstrSize;
    disassembleInstr(instr0->InstructionBits());
    disassembleInstr(instr1->InstructionBits());
    disassembleInstr(instr2->InstructionBits());
    disassembleInstr(instr3->InstructionBits());
    disassembleInstr(instr4->InstructionBits());
    disassembleInstr(instr5->InstructionBits());
    disassembleInstr(instr6->InstructionBits());
    disassembleInstr(instr7->InstructionBits());
    MOZ_ASSERT(IsAddi(*reinterpret_cast<Instr*>(instr1)));
    set_target_value_at(pc, value);
  }
}

void Assembler::set_target_value_at(Instruction* pc, uint64_t target) {
  DEBUG_PRINTF("\tset_target_value_at: pc: %p\ttarget: %lx\n", pc, target);
  uint32_t* p = reinterpret_cast<uint32_t*>(pc);
  MOZ_ASSERT((target & 0xffff000000000000ll) == 0);
#ifdef DEBUG
  // Check we have the result from a li macro-instruction.
  Instruction* instr0 = pc;
  Instruction* instr1 = pc + 1 * kInstrSize;
  Instruction* instr3 = pc + 3 * kInstrSize;
  Instruction* instr5 = pc + 5 * kInstrSize;
  MOZ_ASSERT(IsLui(*reinterpret_cast<Instr*>(instr0)) &&
             IsAddi(*reinterpret_cast<Instr*>(instr1)) &&
             IsOri(*reinterpret_cast<Instr*>(instr3)) &&
             IsOri(*reinterpret_cast<Instr*>(instr5)));
#endif
  int64_t a6 = target & 0x3f;                     // bits 0:6. 6 bits
  int64_t b11 = (target >> 6) & 0x7ff;            // bits 6:11. 11 bits
  int64_t high_31 = (target >> 17) & 0x7fffffff;  // 31 bits
  int64_t high_20 = ((high_31 + 0x800) >> 12);    // 19 bits
  int64_t low_12 = high_31 & 0xfff;               // 12 bits
  *p = *p & 0xfff;
  *p = *p | ((int32_t)high_20 << 12);
  *(p + 1) = *(p + 1) & 0xfffff;
  *(p + 1) = *(p + 1) | ((int32_t)low_12 << 20);
  *(p + 2) = *(p + 2) & 0xfffff;
  *(p + 2) = *(p + 2) | (11 << 20);
  *(p + 3) = *(p + 3) & 0xfffff;
  *(p + 3) = *(p + 3) | ((int32_t)b11 << 20);
  *(p + 4) = *(p + 4) & 0xfffff;
  *(p + 4) = *(p + 4) | (6 << 20);
  *(p + 5) = *(p + 5) & 0xfffff;
  *(p + 5) = *(p + 5) | ((int32_t)a6 << 20);
  MOZ_ASSERT(target_address_at(pc) == target);
}

void Assembler::WriteLoad64Instructions(Instruction* inst0, Register reg,
                                        uint64_t value) {
  DEBUG_PRINTF("\tWriteLoad64Instructions\n");
  // Initialize rd with an address
  // Pointers are 48 bits
  // 6 fixed instructions are generated
  MOZ_ASSERT((value & 0xfff0000000000000ll) == 0);
  int64_t a6 = value & 0x3f;                     // bits 0:5. 6 bits
  int64_t b11 = (value >> 6) & 0x7ff;            // bits 6:11. 11 bits
  int64_t high_31 = (value >> 17) & 0x7fffffff;  // 31 bits
  int64_t high_20 = ((high_31 + 0x800) >> 12);   // 19 bits
  int64_t low_12 = high_31 & 0xfff;              // 12 bits
  Instr lui_ = LUI | (reg.code() << kRdShift) |
               ((int32_t)high_20 << kImm20Shift);  // lui(rd, (int32_t)high_20);
  *reinterpret_cast<Instr*>(inst0) = lui_;

  Instr addi_ =
      OP_IMM | (reg.code() << kRdShift) | (0b000 << kFunct3Shift) |
      (reg.code() << kRs1Shift) |
      (low_12 << kImm12Shift);  // addi(rd, rd, low_12);  // 31 bits in rd.
  *reinterpret_cast<Instr*>(inst0 + 1 * kInstrSize) = addi_;

  Instr slli_ =
      OP_IMM | (reg.code() << kRdShift) | (0b001 << kFunct3Shift) |
      (reg.code() << kRs1Shift) |
      (11 << kImm12Shift);  // slli(rd, rd, 11);      // Space for next 11 bis
  *reinterpret_cast<Instr*>(inst0 + 2 * kInstrSize) = slli_;

  Instr ori_b11 = OP_IMM | (reg.code() << kRdShift) | (0b110 << kFunct3Shift) |
                  (reg.code() << kRs1Shift) |
                  (b11 << kImm12Shift);  // ori(rd, rd, b11);      // 11 bits
                                         // are put in. 42 bit in rd
  *reinterpret_cast<Instr*>(inst0 + 3 * kInstrSize) = ori_b11;

  slli_ = OP_IMM | (reg.code() << kRdShift) | (0b001 << kFunct3Shift) |
          (reg.code() << kRs1Shift) |
          (6 << kImm12Shift);  // slli(rd, rd, 6);      // Space for next 11 bis
  *reinterpret_cast<Instr*>(inst0 + 4 * kInstrSize) =
      slli_;  // slli(rd, rd, 6);       // Space for next 6 bits

  Instr ori_a6 = OP_IMM | (reg.code() << kRdShift) | (0b110 << kFunct3Shift) |
                 (reg.code() << kRs1Shift) |
                 (a6 << kImm12Shift);  // ori(rd, rd, a6);       // 6 bits are
                                       // put in. 48 bis in rd
  *reinterpret_cast<Instr*>(inst0 + 5 * kInstrSize) = ori_a6;
  disassembleInstr((inst0 + 0 * kInstrSize)->InstructionBits());
  disassembleInstr((inst0 + 1 * kInstrSize)->InstructionBits());
  disassembleInstr((inst0 + 2 * kInstrSize)->InstructionBits());
  disassembleInstr((inst0 + 3 * kInstrSize)->InstructionBits());
  disassembleInstr((inst0 + 4 * kInstrSize)->InstructionBits());
  disassembleInstr((inst0 + 5 * kInstrSize)->InstructionBits());
  disassembleInstr((inst0 + 6 * kInstrSize)->InstructionBits());
  MOZ_ASSERT(ExtractLoad64Value(inst0) == value);
}

// This just stomps over memory with 32 bits of raw data. Its purpose is to
// overwrite the call of JITed code with 32 bits worth of an offset. This will
// is only meant to function on code that has been invalidated, so it should
// be totally safe. Since that instruction will never be executed again, a
// ICache flush should not be necessary
void Assembler::PatchWrite_Imm32(CodeLocationLabel label, Imm32 imm) {
  // Raw is going to be the return address.
  uint32_t* raw = (uint32_t*)label.raw();
  // Overwrite the 4 bytes before the return address, which will
  // end up being the call instruction.
  *(raw - 1) = imm.value;
}

void Assembler::target_at_put(BufferOffset pos, BufferOffset target_pos,
                              bool trampoline) {
  if (m_buffer.oom()) {
    return;
  }
  DEBUG_PRINTF("\ttarget_at_put: %p (%d) to %p (%d)\n",
               reinterpret_cast<Instr*>(editSrc(pos)), pos.getOffset(),
               reinterpret_cast<Instr*>(editSrc(pos)) + target_pos.getOffset() -
                   pos.getOffset(),
               target_pos.getOffset());
  Instruction* instruction = editSrc(pos);
  Instr instr = instruction->InstructionBits();
  switch (instruction->InstructionOpcodeType()) {
    case BRANCH: {
      instr = SetBranchOffset(pos.getOffset(), target_pos.getOffset(), instr);
      instr_at_put(pos, instr);
    } break;
    case JAL: {
      MOZ_ASSERT(IsJal(instr));
      instr = SetJalOffset(pos.getOffset(), target_pos.getOffset(), instr);
      instr_at_put(pos, instr);
    } break;
    case LUI: {
      set_target_value_at(instruction,
                          reinterpret_cast<uintptr_t>(editSrc(target_pos)));
    } break;
    case AUIPC: {
      Instr instr_auipc = instr;
      Instr instr_I =
          editSrc(BufferOffset(pos.getOffset() + 4))->InstructionBits();
      MOZ_ASSERT(IsJalr(instr_I) || IsAddi(instr_I));

      intptr_t offset = target_pos.getOffset() - pos.getOffset();
      if (is_int21(offset) && IsJalr(instr_I) && trampoline) {
        MOZ_ASSERT(is_int21(offset) && ((offset & 1) == 0));
        Instr instr = JAL;
        instr = SetJalOffset(pos.getOffset(), target_pos.getOffset(), instr);
        MOZ_ASSERT(IsJal(instr));
        MOZ_ASSERT(JumpOffset(instr) == offset);
        instr_at_put(pos, instr);
        instr_at_put(BufferOffset(pos.getOffset() + 4), kNopByte);
      } else {
        MOZ_RELEASE_ASSERT(is_int32(offset + 0x800));
        MOZ_ASSERT(instruction->RdValue() ==
                   editSrc(BufferOffset(pos.getOffset() + 4))->Rs1Value());
        int32_t Hi20 = (((int32_t)offset + 0x800) >> 12);
        int32_t Lo12 = (int32_t)offset << 20 >> 20;

        instr_auipc =
            (instr_auipc & ~kImm31_12Mask) | ((Hi20 & kImm19_0Mask) << 12);
        instr_at_put(pos, instr_auipc);

        const int kImm31_20Mask = ((1 << 12) - 1) << 20;
        const int kImm11_0Mask = ((1 << 12) - 1);
        instr_I = (instr_I & ~kImm31_20Mask) | ((Lo12 & kImm11_0Mask) << 20);
        instr_at_put(BufferOffset(pos.getOffset() + 4), instr_I);
      }
    } break;
    default:
      UNIMPLEMENTED_RISCV();
      break;
  }
}

const int kEndOfChain = -1;
const int32_t kEndOfJumpChain = 0;

int Assembler::target_at(BufferOffset pos, bool is_internal) {
  if (oom()) {
    return kEndOfChain;
  }
  Instruction* instruction = editSrc(pos);
  Instruction* instruction2 = nullptr;
  if (IsAuipc(instruction->InstructionBits())) {
    instruction2 = editSrc(BufferOffset(pos.getOffset() + kInstrSize));
  }
  return target_at(instruction, pos, is_internal, instruction2);
}

int Assembler::target_at(Instruction* instruction, BufferOffset pos,
                         bool is_internal, Instruction* instruction2) {
  DEBUG_PRINTF("\t target_at: %p(%x)\n\t",
               reinterpret_cast<Instr*>(instruction), pos.getOffset());
  disassembleInstr(instruction->InstructionBits());
  Instr instr = instruction->InstructionBits();
  switch (instruction->InstructionOpcodeType()) {
    case BRANCH: {
      int32_t imm13 = BranchOffset(instr);
      if (imm13 == kEndOfJumpChain) {
        // EndOfChain sentinel is returned directly, not relative to pc or pos.
        return kEndOfChain;
      } else {
        DEBUG_PRINTF("\t target_at: %d %d\n", imm13, pos.getOffset() + imm13);
        return pos.getOffset() + imm13;
      }
    }
    case JAL: {
      int32_t imm21 = JumpOffset(instr);
      if (imm21 == kEndOfJumpChain) {
        // EndOfChain sentinel is returned directly, not relative to pc or pos.
        return kEndOfChain;
      } else {
        DEBUG_PRINTF("\t target_at: %d %d\n", imm21, pos.getOffset() + imm21);
        return pos.getOffset() + imm21;
      }
    }
    case JALR: {
      int32_t imm12 = instr >> 20;
      if (imm12 == kEndOfJumpChain) {
        // EndOfChain sentinel is returned directly, not relative to pc or pos.
        return kEndOfChain;
      } else {
        DEBUG_PRINTF("\t target_at: %d %d\n", imm12, pos.getOffset() + imm12);
        return pos.getOffset() + imm12;
      }
    }
    case LUI: {
      uintptr_t imm = target_address_at(instruction);
      uintptr_t instr_address = reinterpret_cast<uintptr_t>(instruction);
      if (imm == kEndOfJumpChain) {
        return kEndOfChain;
      } else {
        MOZ_ASSERT(instr_address - imm < INT_MAX);
        int32_t delta = static_cast<int32_t>(instr_address - imm);
        MOZ_ASSERT(pos.getOffset() > delta);
        return pos.getOffset() - delta;
      }
    }
    case AUIPC: {
      MOZ_ASSERT(instruction2 != nullptr);
      Instr instr_auipc = instr;
      Instr instr_I = instruction2->InstructionBits();
      MOZ_ASSERT(IsJalr(instr_I) || IsAddi(instr_I));
      int32_t offset = BrachlongOffset(instr_auipc, instr_I);
      if (offset == kEndOfJumpChain) return kEndOfChain;
      DEBUG_PRINTF("\t target_at: %d %d\n", offset, pos.getOffset() + offset);
      return offset + pos.getOffset();
    }
    default: {
      UNIMPLEMENTED_RISCV();
    }
  }
}

uint32_t Assembler::next_link(Label* L, bool is_internal) {
  MOZ_ASSERT(L->used());
  BufferOffset pos(L);
  int link = target_at(pos, is_internal);
  if (link == kEndOfChain) {
    L->reset();
    return LabelBase::INVALID_OFFSET;
  } else {
    MOZ_ASSERT(link >= 0);
    DEBUG_PRINTF("next: %p to offset %d\n", L, link);
    L->use(link);
    return link;
  }
}

void Assembler::bind(Label* label, BufferOffset boff) {
  JitSpew(JitSpew_Codegen, ".set Llabel %p %d", label, currentOffset());
  DEBUG_PRINTF(".set Llabel %p\n", label);
  // If our caller didn't give us an explicit target to bind to
  // then we want to bind to the location of the next instruction
  BufferOffset dest = boff.assigned() ? boff : nextOffset();
  if (label->used()) {
    uint32_t next;

    // A used label holds a link to branch that uses it.
    do {
      BufferOffset b(label);
      DEBUG_PRINTF("\tbind next:%d\n", b.getOffset());
      // Even a 0 offset may be invalid if we're out of memory.
      if (oom()) {
        return;
      }
      int fixup_pos = b.getOffset();
      int dist = dest.getOffset() - fixup_pos;
      next = next_link(label, false);
      DEBUG_PRINTF("\t%p fixup: %d next: %d\n", label, fixup_pos, next);
      DEBUG_PRINTF("\t   fixup: %d dest: %d dist: %d %d %d\n", fixup_pos,
                   dest.getOffset(), dist, nextOffset().getOffset(),
                   currentOffset());
      Instruction* instruction = editSrc(b);
      Instr instr = instruction->InstructionBits();
      if (IsBranch(instr)) {
        if (dist > kMaxBranchOffset) {
          MOZ_ASSERT(next != LabelBase::INVALID_OFFSET);
          MOZ_RELEASE_ASSERT((next - fixup_pos) <= kMaxBranchOffset);
          MOZ_ASSERT(IsAuipc(editSrc(BufferOffset(next))->InstructionBits()));
          MOZ_ASSERT(
              IsJalr(editSrc(BufferOffset(next + 4))->InstructionBits()));
          DEBUG_PRINTF("\t\ttrampolining: %d\n", next);
        } else {
          target_at_put(b, dest);
          BufferOffset deadline(b.getOffset() +
                                ImmBranchMaxForwardOffset(CondBranchRangeType));
          m_buffer.unregisterBranchDeadline(CondBranchRangeType, deadline);
        }
      } else if (IsJal(instr)) {
        if (dist > kMaxJumpOffset) {
          MOZ_ASSERT(next != LabelBase::INVALID_OFFSET);
          MOZ_RELEASE_ASSERT((next - fixup_pos) <= kMaxJumpOffset);
          MOZ_ASSERT(IsAuipc(editSrc(BufferOffset(next))->InstructionBits()));
          MOZ_ASSERT(
              IsJalr(editSrc(BufferOffset(next + 4))->InstructionBits()));
          DEBUG_PRINTF("\t\ttrampolining: %d\n", next);
        } else {
          target_at_put(b, dest);
          BufferOffset deadline(
              b.getOffset() + ImmBranchMaxForwardOffset(UncondBranchRangeType));
          m_buffer.unregisterBranchDeadline(UncondBranchRangeType, deadline);
        }
      } else {
        MOZ_ASSERT(IsAuipc(instr));
        target_at_put(b, dest);
      }
    } while (next != LabelBase::INVALID_OFFSET);
  }
  label->bind(dest.getOffset());
}

void Assembler::Bind(uint8_t* rawCode, const CodeLabel& label) {
  if (label.patchAt().bound()) {
    auto mode = label.linkMode();
    intptr_t offset = label.patchAt().offset();
    intptr_t target = label.target().offset();

    if (mode == CodeLabel::RawPointer) {
      *reinterpret_cast<const void**>(rawCode + offset) = rawCode + target;
    } else {
      MOZ_ASSERT(mode == CodeLabel::MoveImmediate ||
                 mode == CodeLabel::JumpImmediate);
      Instruction* inst = (Instruction*)(rawCode + offset);
      Assembler::UpdateLoad64Value(inst, (uint64_t)(rawCode + target));
    }
  }
}

bool Assembler::is_near(Label* L) {
  MOZ_ASSERT(L->bound());
  return is_intn((currentOffset() - L->offset()), kJumpOffsetBits);
}

bool Assembler::is_near(Label* L, OffsetSize bits) {
  if (L == nullptr || !L->bound()) return true;
  return is_intn((currentOffset() - L->offset()), bits);
}

bool Assembler::is_near_branch(Label* L) {
  MOZ_ASSERT(L->bound());
  return is_intn((currentOffset() - L->offset()), kBranchOffsetBits);
}

int32_t Assembler::branch_long_offset(Label* L) {
  if (oom()) {
    return kEndOfJumpChain;
  }
  intptr_t target_pos;
  BufferOffset next_instr_offset = nextInstrOffset(2);
  DEBUG_PRINTF("\tbranch_long_offset: %p to (%d)\n", L,
               next_instr_offset.getOffset());
  if (L->bound()) {
    JitSpew(JitSpew_Codegen, ".use Llabel %p on %d", L,
            next_instr_offset.getOffset());
    target_pos = L->offset();
  } else {
    if (L->used()) {
      LabelCahe::Ptr p = label_cache_.lookup(L->offset());
      MOZ_ASSERT(p);
      MOZ_ASSERT(p->key() == L->offset());
      target_pos = p->value().getOffset();
      target_at_put(BufferOffset(target_pos), next_instr_offset);
      DEBUG_PRINTF("\tLabel  %p added to link: %d\n", L,
                   next_instr_offset.getOffset());
      bool ok = label_cache_.put(L->offset(), next_instr_offset);
      if (!ok) {
        NoEnoughLabelCache();
      }
      return kEndOfJumpChain;
    } else {
      JitSpew(JitSpew_Codegen, ".use Llabel %p on %d", L,
              next_instr_offset.getOffset());
      L->use(next_instr_offset.getOffset());
      DEBUG_PRINTF("\tLabel  %p added to link: %d\n", L,
                   next_instr_offset.getOffset());
      bool ok = label_cache_.putNew(L->offset(), next_instr_offset);
      if (!ok) {
        NoEnoughLabelCache();
      }
      return kEndOfJumpChain;
    }
  }
  intptr_t offset = target_pos - next_instr_offset.getOffset();
  MOZ_ASSERT((offset & 3) == 0);
  MOZ_ASSERT(is_int32(offset));
  return static_cast<int32_t>(offset);
}

int32_t Assembler::branch_offset_helper(Label* L, OffsetSize bits) {
  if (oom()) {
    return kEndOfJumpChain;
  }
  int32_t target_pos;
  BufferOffset next_instr_offset = nextInstrOffset();
  DEBUG_PRINTF("\tbranch_offset_helper: %p to %d\n", L,
               next_instr_offset.getOffset());
  // This is the last possible branch target.
  if (L->bound()) {
    JitSpew(JitSpew_Codegen, ".use Llabel %p on %d", L,
            next_instr_offset.getOffset());
    target_pos = L->offset();
  } else {
    BufferOffset deadline(next_instr_offset.getOffset() +
                          ImmBranchMaxForwardOffset(bits));
    DEBUG_PRINTF("\tregisterBranchDeadline %d type %d\n", deadline.getOffset(),
                 OffsetSizeToImmBranchRangeType(bits));
    m_buffer.registerBranchDeadline(OffsetSizeToImmBranchRangeType(bits),
                                    deadline);
    if (L->used()) {
      LabelCahe::Ptr p = label_cache_.lookup(L->offset());
      MOZ_ASSERT(p);
      MOZ_ASSERT(p->key() == L->offset());
      target_pos = p->value().getOffset();
      target_at_put(BufferOffset(target_pos), next_instr_offset);
      DEBUG_PRINTF("\tLabel  %p added to link: %d\n", L,
                   next_instr_offset.getOffset());
      bool ok = label_cache_.put(L->offset(), next_instr_offset);
      if (!ok) {
        NoEnoughLabelCache();
      }
      return kEndOfJumpChain;
    } else {
      JitSpew(JitSpew_Codegen, ".use Llabel %p on %d", L,
              next_instr_offset.getOffset());
      L->use(next_instr_offset.getOffset());
      bool ok = label_cache_.putNew(L->offset(), next_instr_offset);
      if (!ok) {
        NoEnoughLabelCache();
      }
      DEBUG_PRINTF("\tLabel  %p added to link: %d\n", L,
                   next_instr_offset.getOffset());
      return kEndOfJumpChain;
    }
  }

  int32_t offset = target_pos - next_instr_offset.getOffset();
  DEBUG_PRINTF("\toffset = %d\n", offset);
  MOZ_ASSERT(is_intn(offset, bits));
  MOZ_ASSERT((offset & 1) == 0);
  return offset;
}

Assembler::Condition Assembler::InvertCondition(Condition cond) {
  switch (cond) {
    case Equal:
      return NotEqual;
    case NotEqual:
      return Equal;
    case Zero:
      return NonZero;
    case NonZero:
      return Zero;
    case LessThan:
      return GreaterThanOrEqual;
    case LessThanOrEqual:
      return GreaterThan;
    case GreaterThan:
      return LessThanOrEqual;
    case GreaterThanOrEqual:
      return LessThan;
    case Above:
      return BelowOrEqual;
    case AboveOrEqual:
      return Below;
    case Below:
      return AboveOrEqual;
    case BelowOrEqual:
      return Above;
    case Signed:
      return NotSigned;
    case NotSigned:
      return Signed;
    default:
      MOZ_CRASH("unexpected condition");
  }
}

Assembler::DoubleCondition Assembler::InvertCondition(DoubleCondition cond) {
  switch (cond) {
    case DoubleOrdered:
      return DoubleUnordered;
    case DoubleEqual:
      return DoubleNotEqualOrUnordered;
    case DoubleNotEqual:
      return DoubleEqualOrUnordered;
    case DoubleGreaterThan:
      return DoubleLessThanOrEqualOrUnordered;
    case DoubleGreaterThanOrEqual:
      return DoubleLessThanOrUnordered;
    case DoubleLessThan:
      return DoubleGreaterThanOrEqualOrUnordered;
    case DoubleLessThanOrEqual:
      return DoubleGreaterThanOrUnordered;
    case DoubleUnordered:
      return DoubleOrdered;
    case DoubleEqualOrUnordered:
      return DoubleNotEqual;
    case DoubleNotEqualOrUnordered:
      return DoubleEqual;
    case DoubleGreaterThanOrUnordered:
      return DoubleLessThanOrEqual;
    case DoubleGreaterThanOrEqualOrUnordered:
      return DoubleLessThan;
    case DoubleLessThanOrUnordered:
      return DoubleGreaterThanOrEqual;
    case DoubleLessThanOrEqualOrUnordered:
      return DoubleGreaterThan;
    default:
      MOZ_CRASH("unexpected condition");
  }
}

// Break / Trap instructions.
void Assembler::break_(uint32_t code, bool break_as_stop) {
  // We need to invalidate breaks that could be stops as well because the
  // simulator expects a char pointer after the stop instruction.
  // See constants-mips.h for explanation.
  MOZ_ASSERT(
      (break_as_stop && code <= kMaxStopCode && code > kMaxTracepointCode) ||
      (!break_as_stop && (code > kMaxStopCode || code <= kMaxTracepointCode)));

  // since ebreak does not allow additional immediate field, we use the
  // immediate field of lui instruction immediately following the ebreak to
  // encode the "code" info
  ebreak();
  MOZ_ASSERT(is_uint20(code));
  lui(zero_reg, code);
}

void Assembler::ToggleToJmp(CodeLocationLabel inst_) {
  Instruction* inst = (Instruction*)inst_.raw();
  MOZ_ASSERT(IsAddi(inst->InstructionBits()));
  int32_t offset = inst->Imm12Value();
  MOZ_ASSERT(is_int12(offset));
  Instr jal_ = JAL | (0b000 << kFunct3Shift) |
               (offset & 0xff000) |          // bits 19-12
               ((offset & 0x800) << 9) |     // bit  11
               ((offset & 0x7fe) << 20) |    // bits 10-1
               ((offset & 0x100000) << 11);  // bit  20
  // jal(zero, offset);
  *reinterpret_cast<Instr*>(inst) = jal_;
}

void Assembler::ToggleToCmp(CodeLocationLabel inst_) {
  Instruction* inst = (Instruction*)inst_.raw();

  // toggledJump is allways used for short jumps.
  MOZ_ASSERT(IsJal(inst->InstructionBits()));
  // Replace "jal zero_reg, offset" with "addi $zero, $zero, offset"
  int32_t offset = inst->Imm20JValue();
  MOZ_ASSERT(is_int12(offset));
  Instr addi_ = OP_IMM | (0b000 << kFunct3Shift) |
                (offset << kImm12Shift);  // addi(zero, zero, low_12);
  *reinterpret_cast<Instr*>(inst) = addi_;
}

bool Assembler::reserve(size_t size) {
  // This buffer uses fixed-size chunks so there's no point in reserving
  // now vs. on-demand.
  return !oom();
}

static JitCode* CodeFromJump(Instruction* jump) {
  uint8_t* target = (uint8_t*)Assembler::ExtractLoad64Value(jump);
  return JitCode::FromExecutable(target);
}

void Assembler::TraceJumpRelocations(JSTracer* trc, JitCode* code,
                                     CompactBufferReader& reader) {
  while (reader.more()) {
    JitCode* child =
        CodeFromJump((Instruction*)(code->raw() + reader.readUnsigned()));
    TraceManuallyBarrieredEdge(trc, &child, "rel32");
  }
}

static void TraceOneDataRelocation(JSTracer* trc,
                                   mozilla::Maybe<AutoWritableJitCode>& awjc,
                                   JitCode* code, Instruction* inst) {
  void* ptr = (void*)Assembler::ExtractLoad64Value(inst);
  void* prior = ptr;

  // Data relocations can be for Values or for raw pointers. If a Value is
  // zero-tagged, we can trace it as if it were a raw pointer. If a Value
  // is not zero-tagged, we have to interpret it as a Value to ensure that the
  // tag bits are masked off to recover the actual pointer.
  uintptr_t word = reinterpret_cast<uintptr_t>(ptr);
  if (word >> JSVAL_TAG_SHIFT) {
    // This relocation is a Value with a non-zero tag.
    Value v = Value::fromRawBits(word);
    TraceManuallyBarrieredEdge(trc, &v, "jit-masm-value");
    ptr = (void*)v.bitsAsPunboxPointer();
  } else {
    // This relocation is a raw pointer or a Value with a zero tag.
    // No barrier needed since these are constants.
    TraceManuallyBarrieredGenericPointerEdge(
        trc, reinterpret_cast<gc::Cell**>(&ptr), "jit-masm-ptr");
  }

  if (ptr != prior) {
    if (awjc.isNothing()) {
      awjc.emplace(code);
    }
    Assembler::UpdateLoad64Value(inst, uint64_t(ptr));
  }
}

/* static */
void Assembler::TraceDataRelocations(JSTracer* trc, JitCode* code,
                                     CompactBufferReader& reader) {
  mozilla::Maybe<AutoWritableJitCode> awjc;
  while (reader.more()) {
    size_t offset = reader.readUnsigned();
    Instruction* inst = (Instruction*)(code->raw() + offset);
    TraceOneDataRelocation(trc, awjc, code, inst);
  }
}

UseScratchRegisterScope::UseScratchRegisterScope(Assembler* assembler)
    : available_(assembler->GetScratchRegisterList()),
      old_available_(*available_) {}

UseScratchRegisterScope::~UseScratchRegisterScope() {
  *available_ = old_available_;
}

Register UseScratchRegisterScope::Acquire() {
  MOZ_ASSERT(available_ != nullptr);
  MOZ_ASSERT(!available_->empty());
  Register index = GeneralRegisterSet::FirstRegister(available_->bits());
  available_->takeRegisterIndex(index);
  return index;
}

bool UseScratchRegisterScope::hasAvailable() const {
  return (available_->size()) != 0;
}

void Assembler::retarget(Label* label, Label* target) {
  spew("retarget %p -> %p", label, target);
  if (label->used() && !oom()) {
    if (target->bound()) {
      bind(label, BufferOffset(target));
    } else if (target->used()) {
      // The target is not bound but used. Prepend label's branch list
      // onto target's.
      int32_t next;
      BufferOffset labelBranchOffset(label);

      // Find the head of the use chain for label.
      do {
        next = next_link(label, false);
        labelBranchOffset = BufferOffset(next);
      } while (next != LabelBase::INVALID_OFFSET);

      // Then patch the head of label's use chain to the tail of
      // target's use chain, prepending the entire use chain of target.
      target->use(label->offset());
      target_at_put(labelBranchOffset, BufferOffset(target));
      MOZ_CRASH("check");
    } else {
      // The target is unbound and unused.  We can just take the head of
      // the list hanging off of label, and dump that into target.
      target->use(label->offset());
    }
  }
  label->reset();
}

bool Assembler::appendRawCode(const uint8_t* code, size_t numBytes) {
  if (m_buffer.oom()) {
    return false;
  }
  while (numBytes > SliceSize) {
    m_buffer.putBytes(SliceSize, code);
    numBytes -= SliceSize;
    code += SliceSize;
  }
  m_buffer.putBytes(numBytes, code);
  return !m_buffer.oom();
}

void Assembler::ToggleCall(CodeLocationLabel inst_, bool enabled) {
  Instruction* i0 = (Instruction*)inst_.raw();
  Instruction* i1 = (Instruction*)(inst_.raw() + 1 * kInstrSize);
  Instruction* i2 = (Instruction*)(inst_.raw() + 2 * kInstrSize);
  Instruction* i3 = (Instruction*)(inst_.raw() + 3 * kInstrSize);
  Instruction* i4 = (Instruction*)(inst_.raw() + 4 * kInstrSize);
  Instruction* i5 = (Instruction*)(inst_.raw() + 5 * kInstrSize);
  Instruction* i6 = (Instruction*)(inst_.raw() + 6 * kInstrSize);

  MOZ_ASSERT(IsLui(i0->InstructionBits()));
  MOZ_ASSERT(IsAddi(i1->InstructionBits()));
  MOZ_ASSERT(IsSlli(i2->InstructionBits()));
  MOZ_ASSERT(IsOri(i3->InstructionBits()));
  MOZ_ASSERT(IsSlli(i4->InstructionBits()));
  MOZ_ASSERT(IsOri(i5->InstructionBits()));
  if (enabled) {
    Instr jalr_ = JALR | (ra.code() << kRdShift) | (0x0 << kFunct3Shift) |
                  (i5->RdValue() << kRs1Shift) | (0x0 << kImm12Shift);
    *((Instr*)i6) = jalr_;
  } else {
    *((Instr*)i6) = kNopByte;
  }
}

void Assembler::PatchShortRangeBranchToVeneer(Buffer* buffer, unsigned rangeIdx,
                                              BufferOffset deadline,
                                              BufferOffset veneer) {
  if (buffer->oom()) {
    return;
  }
  DEBUG_PRINTF("\tPatchShortRangeBranchToVeneer\n");
  // Reconstruct the position of the branch from (rangeIdx, deadline).
  ImmBranchRangeType branchRange = static_cast<ImmBranchRangeType>(rangeIdx);
  BufferOffset branch(deadline.getOffset() -
                      ImmBranchMaxForwardOffset(branchRange));
  Instruction* branchInst = buffer->getInst(branch);
  Instruction* veneerInst_1 = buffer->getInst(veneer);
  Instruction* veneerInst_2 =
      buffer->getInst(BufferOffset(veneer.getOffset() + 4));
  // Verify that the branch range matches what's encoded.
  DEBUG_PRINTF("\t%p(%x): ", branchInst, branch.getOffset());
  disassembleInstr(branchInst->InstructionBits(), JitSpew_Codegen);
  DEBUG_PRINTF("\t instert veneer %x, branch:%x deadline: %x\n",
               veneer.getOffset(), branch.getOffset(), deadline.getOffset());
  MOZ_ASSERT(branchRange <= UncondBranchRangeType);
  MOZ_ASSERT(branchInst->GetImmBranchRangeType() == branchRange);
  // emit a long jump slot
  Instr auipc = AUIPC | (t6.code() << kRdShift) | (0x0 << kImm20Shift);
  Instr jalr = JALR | (zero_reg.code() << kRdShift) | (0x0 << kFunct3Shift) |
               (t6.code() << kRs1Shift) | (0x0 << kImm12Shift);

  // We want to insert veneer after branch in the linked list of instructions
  // that use the same unbound label.
  // The veneer should be an unconditional branch.
  int32_t nextElemOffset = target_at(buffer->getInst(branch), branch, false);
  int32_t dist;
  // If offset is 0, this is the end of the linked list.
  if (nextElemOffset != kEndOfChain) {
    // Make the offset relative to veneer so it targets the same instruction
    // as branchInst.
    dist = nextElemOffset - veneer.getOffset();
  } else {
    dist = 0;
  }
  int32_t Hi20 = (((int32_t)dist + 0x800) >> 12);
  int32_t Lo12 = (int32_t)dist << 20 >> 20;
  auipc = SetAuipcOffset(Hi20, auipc);
  jalr = SetJalrOffset(Lo12, jalr);
  // insert veneer
  veneerInst_1->SetInstructionBits(auipc);
  veneerInst_2->SetInstructionBits(jalr);
  // Now link branchInst to veneer.
  if (IsBranch(branchInst->InstructionBits())) {
    branchInst->SetInstructionBits(SetBranchOffset(
        branch.getOffset(), veneer.getOffset(), branchInst->InstructionBits()));
  } else {
    MOZ_ASSERT(IsJal(branchInst->InstructionBits()));
    branchInst->SetInstructionBits(SetJalOffset(
        branch.getOffset(), veneer.getOffset(), branchInst->InstructionBits()));
  }
  DEBUG_PRINTF("\tfix to veneer:");
  disassembleInstr(branchInst->InstructionBits());
}
}  // namespace jit
}  // namespace js
