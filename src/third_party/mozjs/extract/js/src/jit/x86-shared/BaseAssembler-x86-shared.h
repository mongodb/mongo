/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * ***** BEGIN LICENSE BLOCK *****
 * Copyright (C) 2008 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * ***** END LICENSE BLOCK ***** */

#ifndef jit_x86_shared_BaseAssembler_x86_shared_h
#define jit_x86_shared_BaseAssembler_x86_shared_h

#include "mozilla/IntegerPrintfMacros.h"

#include "jit/x86-shared/AssemblerBuffer-x86-shared.h"
#include "jit/x86-shared/Encoding-x86-shared.h"
#include "jit/x86-shared/Patching-x86-shared.h"
#include "wasm/WasmTypeDecls.h"

namespace js {
namespace jit {

namespace X86Encoding {

class BaseAssembler;

class BaseAssembler : public GenericAssembler {
 public:
  BaseAssembler() : useVEX_(true) {}

  void disableVEX() { useVEX_ = false; }

  size_t size() const { return m_formatter.size(); }
  const unsigned char* buffer() const { return m_formatter.buffer(); }
  unsigned char* data() { return m_formatter.data(); }
  bool oom() const { return m_formatter.oom(); }
  bool reserve(size_t size) { return m_formatter.reserve(size); }
  bool swapBuffer(wasm::Bytes& other) { return m_formatter.swapBuffer(other); }

  void nop() {
    spew("nop");
    m_formatter.oneByteOp(OP_NOP);
  }

  void comment(const char* msg) { spew("; %s", msg); }

  static void patchFiveByteNopToCall(uint8_t* callsite, uint8_t* target) {
    // Note: the offset is relative to the address of the instruction after
    // the call which is five bytes.
    uint8_t* inst = callsite - sizeof(int32_t) - 1;
    // The nop can be already patched as call, overriding the call.
    // See also nop_five.
    MOZ_ASSERT(inst[0] == OP_NOP_0F || inst[0] == OP_CALL_rel32);
    MOZ_ASSERT_IF(inst[0] == OP_NOP_0F,
                  inst[1] == OP_NOP_1F || inst[2] == OP_NOP_44 ||
                      inst[3] == OP_NOP_00 || inst[4] == OP_NOP_00);
    inst[0] = OP_CALL_rel32;
    SetRel32(callsite, target);
  }

  static void patchCallToFiveByteNop(uint8_t* callsite) {
    // See also patchFiveByteNopToCall and nop_five.
    uint8_t* inst = callsite - sizeof(int32_t) - 1;
    // The call can be already patched as nop.
    if (inst[0] == OP_NOP_0F) {
      MOZ_ASSERT(inst[1] == OP_NOP_1F || inst[2] == OP_NOP_44 ||
                 inst[3] == OP_NOP_00 || inst[4] == OP_NOP_00);
      return;
    }
    MOZ_ASSERT(inst[0] == OP_CALL_rel32);
    inst[0] = OP_NOP_0F;
    inst[1] = OP_NOP_1F;
    inst[2] = OP_NOP_44;
    inst[3] = OP_NOP_00;
    inst[4] = OP_NOP_00;
  }

  /*
   * The nop multibytes sequences are directly taken from the Intel's
   * architecture software developer manual.
   * They are defined for sequences of sizes from 1 to 9 included.
   */
  void nop_one() { m_formatter.oneByteOp(OP_NOP); }

  void nop_two() {
    m_formatter.oneByteOp(OP_NOP_66);
    m_formatter.oneByteOp(OP_NOP);
  }

  void nop_three() {
    m_formatter.oneByteOp(OP_NOP_0F);
    m_formatter.oneByteOp(OP_NOP_1F);
    m_formatter.oneByteOp(OP_NOP_00);
  }

  void nop_four() {
    m_formatter.oneByteOp(OP_NOP_0F);
    m_formatter.oneByteOp(OP_NOP_1F);
    m_formatter.oneByteOp(OP_NOP_40);
    m_formatter.oneByteOp(OP_NOP_00);
  }

  void nop_five() {
    m_formatter.oneByteOp(OP_NOP_0F);
    m_formatter.oneByteOp(OP_NOP_1F);
    m_formatter.oneByteOp(OP_NOP_44);
    m_formatter.oneByteOp(OP_NOP_00);
    m_formatter.oneByteOp(OP_NOP_00);
  }

  void nop_six() {
    m_formatter.oneByteOp(OP_NOP_66);
    nop_five();
  }

  void nop_seven() {
    m_formatter.oneByteOp(OP_NOP_0F);
    m_formatter.oneByteOp(OP_NOP_1F);
    m_formatter.oneByteOp(OP_NOP_80);
    for (int i = 0; i < 4; ++i) {
      m_formatter.oneByteOp(OP_NOP_00);
    }
  }

  void nop_eight() {
    m_formatter.oneByteOp(OP_NOP_0F);
    m_formatter.oneByteOp(OP_NOP_1F);
    m_formatter.oneByteOp(OP_NOP_84);
    for (int i = 0; i < 5; ++i) {
      m_formatter.oneByteOp(OP_NOP_00);
    }
  }

  void nop_nine() {
    m_formatter.oneByteOp(OP_NOP_66);
    nop_eight();
  }

  void insert_nop(int size) {
    switch (size) {
      case 1:
        nop_one();
        break;
      case 2:
        nop_two();
        break;
      case 3:
        nop_three();
        break;
      case 4:
        nop_four();
        break;
      case 5:
        nop_five();
        break;
      case 6:
        nop_six();
        break;
      case 7:
        nop_seven();
        break;
      case 8:
        nop_eight();
        break;
      case 9:
        nop_nine();
        break;
      case 10:
        nop_three();
        nop_seven();
        break;
      case 11:
        nop_four();
        nop_seven();
        break;
      case 12:
        nop_six();
        nop_six();
        break;
      case 13:
        nop_six();
        nop_seven();
        break;
      case 14:
        nop_seven();
        nop_seven();
        break;
      case 15:
        nop_one();
        nop_seven();
        nop_seven();
        break;
      default:
        MOZ_CRASH("Unhandled alignment");
    }
  }

  // Stack operations:

  void push_r(RegisterID reg) {
    spew("push       %s", GPRegName(reg));
    m_formatter.oneByteOp(OP_PUSH_EAX, reg);
  }

  void pop_r(RegisterID reg) {
    spew("pop        %s", GPRegName(reg));
    m_formatter.oneByteOp(OP_POP_EAX, reg);
  }

  void push_i(int32_t imm) {
    spew("push       $%s0x%x", PRETTYHEX(imm));
    if (CAN_SIGN_EXTEND_8_32(imm)) {
      m_formatter.oneByteOp(OP_PUSH_Ib);
      m_formatter.immediate8s(imm);
    } else {
      m_formatter.oneByteOp(OP_PUSH_Iz);
      m_formatter.immediate32(imm);
    }
  }

  void push_i32(int32_t imm) {
    spew("push       $%s0x%04x", PRETTYHEX(imm));
    m_formatter.oneByteOp(OP_PUSH_Iz);
    m_formatter.immediate32(imm);
  }

  void push_m(int32_t offset, RegisterID base) {
    spew("push       " MEM_ob, ADDR_ob(offset, base));
    m_formatter.oneByteOp(OP_GROUP5_Ev, offset, base, GROUP5_OP_PUSH);
  }
  void push_m(int32_t offset, RegisterID base, RegisterID index, int scale) {
    spew("push       " MEM_obs, ADDR_obs(offset, base, index, scale));
    m_formatter.oneByteOp(OP_GROUP5_Ev, offset, base, index, scale,
                          GROUP5_OP_PUSH);
  }

  void pop_m(int32_t offset, RegisterID base) {
    spew("pop        " MEM_ob, ADDR_ob(offset, base));
    m_formatter.oneByteOp(OP_GROUP1A_Ev, offset, base, GROUP1A_OP_POP);
  }

  void push_flags() {
    spew("pushf");
    m_formatter.oneByteOp(OP_PUSHFLAGS);
  }

  void pop_flags() {
    spew("popf");
    m_formatter.oneByteOp(OP_POPFLAGS);
  }

  // Arithmetic operations:

  void addl_rr(RegisterID src, RegisterID dst) {
    spew("addl       %s, %s", GPReg32Name(src), GPReg32Name(dst));
    m_formatter.oneByteOp(OP_ADD_GvEv, src, dst);
  }

  void addw_rr(RegisterID src, RegisterID dst) {
    spew("addw       %s, %s", GPReg16Name(src), GPReg16Name(dst));
    m_formatter.prefix(PRE_OPERAND_SIZE);
    m_formatter.oneByteOp(OP_ADD_GvEv, src, dst);
  }

  void addl_mr(int32_t offset, RegisterID base, RegisterID dst) {
    spew("addl       " MEM_ob ", %s", ADDR_ob(offset, base), GPReg32Name(dst));
    m_formatter.oneByteOp(OP_ADD_GvEv, offset, base, dst);
  }

  void addl_rm(RegisterID src, int32_t offset, RegisterID base) {
    spew("addl       %s, " MEM_ob, GPReg32Name(src), ADDR_ob(offset, base));
    m_formatter.oneByteOp(OP_ADD_EvGv, offset, base, src);
  }

  void addl_rm(RegisterID src, int32_t offset, RegisterID base,
               RegisterID index, int scale) {
    spew("addl       %s, " MEM_obs, GPReg32Name(src),
         ADDR_obs(offset, base, index, scale));
    m_formatter.oneByteOp(OP_ADD_EvGv, offset, base, index, scale, src);
  }

  void addl_ir(int32_t imm, RegisterID dst) {
    spew("addl       $%d, %s", imm, GPReg32Name(dst));
    if (CAN_SIGN_EXTEND_8_32(imm)) {
      m_formatter.oneByteOp(OP_GROUP1_EvIb, dst, GROUP1_OP_ADD);
      m_formatter.immediate8s(imm);
    } else {
      if (dst == rax) {
        m_formatter.oneByteOp(OP_ADD_EAXIv);
      } else {
        m_formatter.oneByteOp(OP_GROUP1_EvIz, dst, GROUP1_OP_ADD);
      }
      m_formatter.immediate32(imm);
    }
  }

  void addw_ir(int32_t imm, RegisterID dst) {
    spew("addw       $%d, %s", int16_t(imm), GPReg16Name(dst));
    m_formatter.prefix(PRE_OPERAND_SIZE);
    m_formatter.oneByteOp(OP_GROUP1_EvIz, dst, GROUP1_OP_ADD);
    m_formatter.immediate16(imm);
  }

  void addl_i32r(int32_t imm, RegisterID dst) {
    // 32-bit immediate always, for patching.
    spew("addl       $0x%04x, %s", uint32_t(imm), GPReg32Name(dst));
    if (dst == rax) {
      m_formatter.oneByteOp(OP_ADD_EAXIv);
    } else {
      m_formatter.oneByteOp(OP_GROUP1_EvIz, dst, GROUP1_OP_ADD);
    }
    m_formatter.immediate32(imm);
  }

  void addl_im(int32_t imm, int32_t offset, RegisterID base) {
    spew("addl       $%d, " MEM_ob, imm, ADDR_ob(offset, base));
    if (CAN_SIGN_EXTEND_8_32(imm)) {
      m_formatter.oneByteOp(OP_GROUP1_EvIb, offset, base, GROUP1_OP_ADD);
      m_formatter.immediate8s(imm);
    } else {
      m_formatter.oneByteOp(OP_GROUP1_EvIz, offset, base, GROUP1_OP_ADD);
      m_formatter.immediate32(imm);
    }
  }

  void addl_im(int32_t imm, int32_t offset, RegisterID base, RegisterID index,
               int scale) {
    spew("addl       $%d, " MEM_obs, imm, ADDR_obs(offset, base, index, scale));
    if (CAN_SIGN_EXTEND_8_32(imm)) {
      m_formatter.oneByteOp(OP_GROUP1_EvIb, offset, base, index, scale,
                            GROUP1_OP_ADD);
      m_formatter.immediate8s(imm);
    } else {
      m_formatter.oneByteOp(OP_GROUP1_EvIz, offset, base, index, scale,
                            GROUP1_OP_ADD);
      m_formatter.immediate32(imm);
    }
  }

  void addl_im(int32_t imm, const void* addr) {
    spew("addl       $%d, %p", imm, addr);
    if (CAN_SIGN_EXTEND_8_32(imm)) {
      m_formatter.oneByteOp(OP_GROUP1_EvIb, addr, GROUP1_OP_ADD);
      m_formatter.immediate8s(imm);
    } else {
      m_formatter.oneByteOp(OP_GROUP1_EvIz, addr, GROUP1_OP_ADD);
      m_formatter.immediate32(imm);
    }
  }
  void addw_im(int32_t imm, const void* addr) {
    spew("addw       $%d, %p", int16_t(imm), addr);
    m_formatter.prefix(PRE_OPERAND_SIZE);
    if (CAN_SIGN_EXTEND_8_32(imm)) {
      m_formatter.oneByteOp(OP_GROUP1_EvIb, addr, GROUP1_OP_ADD);
      m_formatter.immediate8s(imm);
    } else {
      m_formatter.oneByteOp(OP_GROUP1_EvIz, addr, GROUP1_OP_ADD);
      m_formatter.immediate16(imm);
    }
  }

  void addw_im(int32_t imm, int32_t offset, RegisterID base) {
    spew("addw       $%d, " MEM_ob, int16_t(imm), ADDR_ob(offset, base));
    m_formatter.prefix(PRE_OPERAND_SIZE);
    m_formatter.oneByteOp(OP_GROUP1_EvIz, offset, base, GROUP1_OP_ADD);
    m_formatter.immediate16(imm);
  }

  void addw_im(int32_t imm, int32_t offset, RegisterID base, RegisterID index,
               int scale) {
    spew("addw       $%d, " MEM_obs, int16_t(imm),
         ADDR_obs(offset, base, index, scale));
    m_formatter.prefix(PRE_OPERAND_SIZE);
    m_formatter.oneByteOp(OP_GROUP1_EvIz, offset, base, index, scale,
                          GROUP1_OP_ADD);
    m_formatter.immediate16(imm);
  }

  void addw_rm(RegisterID src, int32_t offset, RegisterID base) {
    spew("addw       %s, " MEM_ob, GPReg16Name(src), ADDR_ob(offset, base));
    m_formatter.prefix(PRE_OPERAND_SIZE);
    m_formatter.oneByteOp(OP_ADD_EvGv, offset, base, src);
  }

  void addw_rm(RegisterID src, int32_t offset, RegisterID base,
               RegisterID index, int scale) {
    spew("addw       %s, " MEM_obs, GPReg16Name(src),
         ADDR_obs(offset, base, index, scale));
    m_formatter.prefix(PRE_OPERAND_SIZE);
    m_formatter.oneByteOp(OP_ADD_EvGv, offset, base, index, scale, src);
  }

  void addb_im(int32_t imm, int32_t offset, RegisterID base) {
    spew("addb       $%d, " MEM_ob, int8_t(imm), ADDR_ob(offset, base));
    m_formatter.oneByteOp(OP_GROUP1_EbIb, offset, base, GROUP1_OP_ADD);
    m_formatter.immediate8(imm);
  }

  void addb_im(int32_t imm, int32_t offset, RegisterID base, RegisterID index,
               int scale) {
    spew("addb       $%d, " MEM_obs, int8_t(imm),
         ADDR_obs(offset, base, index, scale));
    m_formatter.oneByteOp(OP_GROUP1_EbIb, offset, base, index, scale,
                          GROUP1_OP_ADD);
    m_formatter.immediate8(imm);
  }

  void addb_rm(RegisterID src, int32_t offset, RegisterID base) {
    spew("addb       %s, " MEM_ob, GPReg8Name(src), ADDR_ob(offset, base));
    m_formatter.oneByteOp8(OP_ADD_EbGb, offset, base, src);
  }

  void addb_rm(RegisterID src, int32_t offset, RegisterID base,
               RegisterID index, int scale) {
    spew("addb       %s, " MEM_obs, GPReg8Name(src),
         ADDR_obs(offset, base, index, scale));
    m_formatter.oneByteOp8(OP_ADD_EbGb, offset, base, index, scale, src);
  }

  void subb_im(int32_t imm, int32_t offset, RegisterID base) {
    spew("subb       $%d, " MEM_ob, int8_t(imm), ADDR_ob(offset, base));
    m_formatter.oneByteOp(OP_GROUP1_EbIb, offset, base, GROUP1_OP_SUB);
    m_formatter.immediate8(imm);
  }

  void subb_im(int32_t imm, int32_t offset, RegisterID base, RegisterID index,
               int scale) {
    spew("subb       $%d, " MEM_obs, int8_t(imm),
         ADDR_obs(offset, base, index, scale));
    m_formatter.oneByteOp(OP_GROUP1_EbIb, offset, base, index, scale,
                          GROUP1_OP_SUB);
    m_formatter.immediate8(imm);
  }

  void subb_rm(RegisterID src, int32_t offset, RegisterID base) {
    spew("subb       %s, " MEM_ob, GPReg8Name(src), ADDR_ob(offset, base));
    m_formatter.oneByteOp8(OP_SUB_EbGb, offset, base, src);
  }

  void subb_rm(RegisterID src, int32_t offset, RegisterID base,
               RegisterID index, int scale) {
    spew("subb       %s, " MEM_obs, GPReg8Name(src),
         ADDR_obs(offset, base, index, scale));
    m_formatter.oneByteOp8(OP_SUB_EbGb, offset, base, index, scale, src);
  }

  void andb_im(int32_t imm, int32_t offset, RegisterID base) {
    spew("andb       $%d, " MEM_ob, int8_t(imm), ADDR_ob(offset, base));
    m_formatter.oneByteOp(OP_GROUP1_EbIb, offset, base, GROUP1_OP_AND);
    m_formatter.immediate8(imm);
  }

  void andb_im(int32_t imm, int32_t offset, RegisterID base, RegisterID index,
               int scale) {
    spew("andb       $%d, " MEM_obs, int8_t(imm),
         ADDR_obs(offset, base, index, scale));
    m_formatter.oneByteOp(OP_GROUP1_EbIb, offset, base, index, scale,
                          GROUP1_OP_AND);
    m_formatter.immediate8(imm);
  }

  void andb_rm(RegisterID src, int32_t offset, RegisterID base) {
    spew("andb       %s, " MEM_ob, GPReg8Name(src), ADDR_ob(offset, base));
    m_formatter.oneByteOp8(OP_AND_EbGb, offset, base, src);
  }

  void andb_rm(RegisterID src, int32_t offset, RegisterID base,
               RegisterID index, int scale) {
    spew("andb       %s, " MEM_obs, GPReg8Name(src),
         ADDR_obs(offset, base, index, scale));
    m_formatter.oneByteOp8(OP_AND_EbGb, offset, base, index, scale, src);
  }

  void orb_im(int32_t imm, int32_t offset, RegisterID base) {
    spew("orb       $%d, " MEM_ob, int8_t(imm), ADDR_ob(offset, base));
    m_formatter.oneByteOp(OP_GROUP1_EbIb, offset, base, GROUP1_OP_OR);
    m_formatter.immediate8(imm);
  }

  void orb_im(int32_t imm, int32_t offset, RegisterID base, RegisterID index,
              int scale) {
    spew("orb        $%d, " MEM_obs, int8_t(imm),
         ADDR_obs(offset, base, index, scale));
    m_formatter.oneByteOp(OP_GROUP1_EbIb, offset, base, index, scale,
                          GROUP1_OP_OR);
    m_formatter.immediate8(imm);
  }

  void orb_rm(RegisterID src, int32_t offset, RegisterID base) {
    spew("orb       %s, " MEM_ob, GPReg8Name(src), ADDR_ob(offset, base));
    m_formatter.oneByteOp8(OP_OR_EbGb, offset, base, src);
  }

  void orb_rm(RegisterID src, int32_t offset, RegisterID base, RegisterID index,
              int scale) {
    spew("orb        %s, " MEM_obs, GPReg8Name(src),
         ADDR_obs(offset, base, index, scale));
    m_formatter.oneByteOp8(OP_OR_EbGb, offset, base, index, scale, src);
  }

  void xorb_im(int32_t imm, int32_t offset, RegisterID base) {
    spew("xorb       $%d, " MEM_ob, int8_t(imm), ADDR_ob(offset, base));
    m_formatter.oneByteOp(OP_GROUP1_EbIb, offset, base, GROUP1_OP_XOR);
    m_formatter.immediate8(imm);
  }

  void xorb_im(int32_t imm, int32_t offset, RegisterID base, RegisterID index,
               int scale) {
    spew("xorb       $%d, " MEM_obs, int8_t(imm),
         ADDR_obs(offset, base, index, scale));
    m_formatter.oneByteOp(OP_GROUP1_EbIb, offset, base, index, scale,
                          GROUP1_OP_XOR);
    m_formatter.immediate8(imm);
  }

  void xorb_rm(RegisterID src, int32_t offset, RegisterID base) {
    spew("xorb       %s, " MEM_ob, GPReg8Name(src), ADDR_ob(offset, base));
    m_formatter.oneByteOp8(OP_XOR_EbGb, offset, base, src);
  }

  void xorb_rm(RegisterID src, int32_t offset, RegisterID base,
               RegisterID index, int scale) {
    spew("xorb       %s, " MEM_obs, GPReg8Name(src),
         ADDR_obs(offset, base, index, scale));
    m_formatter.oneByteOp8(OP_XOR_EbGb, offset, base, index, scale, src);
  }

  void lock_xaddb_rm(RegisterID srcdest, int32_t offset, RegisterID base) {
    spew("lock xaddb %s, " MEM_ob, GPReg8Name(srcdest), ADDR_ob(offset, base));
    m_formatter.oneByteOp(PRE_LOCK);
    m_formatter.twoByteOp8(OP2_XADD_EbGb, offset, base, srcdest);
  }

  void lock_xaddb_rm(RegisterID srcdest, int32_t offset, RegisterID base,
                     RegisterID index, int scale) {
    spew("lock xaddb %s, " MEM_obs, GPReg8Name(srcdest),
         ADDR_obs(offset, base, index, scale));
    m_formatter.oneByteOp(PRE_LOCK);
    m_formatter.twoByteOp8(OP2_XADD_EbGb, offset, base, index, scale, srcdest);
  }

  void lock_xaddl_rm(RegisterID srcdest, int32_t offset, RegisterID base) {
    spew("lock xaddl %s, " MEM_ob, GPReg32Name(srcdest), ADDR_ob(offset, base));
    m_formatter.oneByteOp(PRE_LOCK);
    m_formatter.twoByteOp(OP2_XADD_EvGv, offset, base, srcdest);
  }

  void lock_xaddl_rm(RegisterID srcdest, int32_t offset, RegisterID base,
                     RegisterID index, int scale) {
    spew("lock xaddl %s, " MEM_obs, GPReg32Name(srcdest),
         ADDR_obs(offset, base, index, scale));
    m_formatter.oneByteOp(PRE_LOCK);
    m_formatter.twoByteOp(OP2_XADD_EvGv, offset, base, index, scale, srcdest);
  }

  void vpmaddubsw_rr(XMMRegisterID src1, XMMRegisterID src0,
                     XMMRegisterID dst) {
    threeByteOpSimd("vpmaddubsw", VEX_PD, OP3_PMADDUBSW_VdqWdq, ESCAPE_38, src1,
                    src0, dst);
  }
  void vpmaddubsw_mr(const void* address, XMMRegisterID src0,
                     XMMRegisterID dst) {
    threeByteOpSimd("vpmaddubsw", VEX_PD, OP3_PMADDUBSW_VdqWdq, ESCAPE_38,
                    address, src0, dst);
  }

  void vpaddb_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpaddb", VEX_PD, OP2_PADDB_VdqWdq, src1, src0, dst);
  }
  void vpaddb_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                 XMMRegisterID dst) {
    twoByteOpSimd("vpaddb", VEX_PD, OP2_PADDB_VdqWdq, offset, base, src0, dst);
  }
  void vpaddb_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpaddb", VEX_PD, OP2_PADDB_VdqWdq, address, src0, dst);
  }

  void vpaddsb_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpaddsb", VEX_PD, OP2_PADDSB_VdqWdq, src1, src0, dst);
  }
  void vpaddsb_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                  XMMRegisterID dst) {
    twoByteOpSimd("vpaddsb", VEX_PD, OP2_PADDSB_VdqWdq, offset, base, src0,
                  dst);
  }
  void vpaddsb_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpaddsb", VEX_PD, OP2_PADDSB_VdqWdq, address, src0, dst);
  }

  void vpaddusb_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpaddusb", VEX_PD, OP2_PADDUSB_VdqWdq, src1, src0, dst);
  }
  void vpaddusb_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                   XMMRegisterID dst) {
    twoByteOpSimd("vpaddusb", VEX_PD, OP2_PADDUSB_VdqWdq, offset, base, src0,
                  dst);
  }
  void vpaddusb_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpaddusb", VEX_PD, OP2_PADDUSB_VdqWdq, address, src0, dst);
  }

  void vpaddw_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpaddw", VEX_PD, OP2_PADDW_VdqWdq, src1, src0, dst);
  }
  void vpaddw_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                 XMMRegisterID dst) {
    twoByteOpSimd("vpaddw", VEX_PD, OP2_PADDW_VdqWdq, offset, base, src0, dst);
  }
  void vpaddw_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpaddw", VEX_PD, OP2_PADDW_VdqWdq, address, src0, dst);
  }

  void vpaddsw_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpaddsw", VEX_PD, OP2_PADDSW_VdqWdq, src1, src0, dst);
  }
  void vpaddsw_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                  XMMRegisterID dst) {
    twoByteOpSimd("vpaddsw", VEX_PD, OP2_PADDSW_VdqWdq, offset, base, src0,
                  dst);
  }
  void vpaddsw_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpaddsw", VEX_PD, OP2_PADDSW_VdqWdq, address, src0, dst);
  }

  void vpaddusw_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpaddusw", VEX_PD, OP2_PADDUSW_VdqWdq, src1, src0, dst);
  }
  void vpaddusw_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                   XMMRegisterID dst) {
    twoByteOpSimd("vpaddusw", VEX_PD, OP2_PADDUSW_VdqWdq, offset, base, src0,
                  dst);
  }
  void vpaddusw_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpaddusw", VEX_PD, OP2_PADDUSW_VdqWdq, address, src0, dst);
  }

  void vpaddd_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpaddd", VEX_PD, OP2_PADDD_VdqWdq, src1, src0, dst);
  }
  void vpaddd_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                 XMMRegisterID dst) {
    twoByteOpSimd("vpaddd", VEX_PD, OP2_PADDD_VdqWdq, offset, base, src0, dst);
  }
  void vpaddd_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpaddd", VEX_PD, OP2_PADDD_VdqWdq, address, src0, dst);
  }

  void vpaddq_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpaddq", VEX_PD, OP2_PADDQ_VdqWdq, address, src0, dst);
  }

  void vpsubb_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpsubb", VEX_PD, OP2_PSUBB_VdqWdq, src1, src0, dst);
  }
  void vpsubb_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                 XMMRegisterID dst) {
    twoByteOpSimd("vpsubb", VEX_PD, OP2_PSUBB_VdqWdq, offset, base, src0, dst);
  }
  void vpsubb_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpsubb", VEX_PD, OP2_PSUBB_VdqWdq, address, src0, dst);
  }

  void vpsubsb_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpsubsb", VEX_PD, OP2_PSUBSB_VdqWdq, src1, src0, dst);
  }
  void vpsubsb_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                  XMMRegisterID dst) {
    twoByteOpSimd("vpsubsb", VEX_PD, OP2_PSUBSB_VdqWdq, offset, base, src0,
                  dst);
  }
  void vpsubsb_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpsubsb", VEX_PD, OP2_PSUBSB_VdqWdq, address, src0, dst);
  }

  void vpsubusb_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpsubusb", VEX_PD, OP2_PSUBUSB_VdqWdq, src1, src0, dst);
  }
  void vpsubusb_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                   XMMRegisterID dst) {
    twoByteOpSimd("vpsubusb", VEX_PD, OP2_PSUBUSB_VdqWdq, offset, base, src0,
                  dst);
  }
  void vpsubusb_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpsubusb", VEX_PD, OP2_PSUBUSB_VdqWdq, address, src0, dst);
  }

  void vpsubw_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpsubw", VEX_PD, OP2_PSUBW_VdqWdq, src1, src0, dst);
  }
  void vpsubw_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                 XMMRegisterID dst) {
    twoByteOpSimd("vpsubw", VEX_PD, OP2_PSUBW_VdqWdq, offset, base, src0, dst);
  }
  void vpsubw_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpsubw", VEX_PD, OP2_PSUBW_VdqWdq, address, src0, dst);
  }

  void vpsubsw_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpsubsw", VEX_PD, OP2_PSUBSW_VdqWdq, src1, src0, dst);
  }
  void vpsubsw_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                  XMMRegisterID dst) {
    twoByteOpSimd("vpsubsw", VEX_PD, OP2_PSUBSW_VdqWdq, offset, base, src0,
                  dst);
  }
  void vpsubsw_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpsubsw", VEX_PD, OP2_PSUBSW_VdqWdq, address, src0, dst);
  }

  void vpsubusw_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpsubusw", VEX_PD, OP2_PSUBUSW_VdqWdq, src1, src0, dst);
  }
  void vpsubusw_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                   XMMRegisterID dst) {
    twoByteOpSimd("vpsubusw", VEX_PD, OP2_PSUBUSW_VdqWdq, offset, base, src0,
                  dst);
  }
  void vpsubusw_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpsubusw", VEX_PD, OP2_PSUBUSW_VdqWdq, address, src0, dst);
  }

  void vpsubd_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpsubd", VEX_PD, OP2_PSUBD_VdqWdq, src1, src0, dst);
  }
  void vpsubd_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                 XMMRegisterID dst) {
    twoByteOpSimd("vpsubd", VEX_PD, OP2_PSUBD_VdqWdq, offset, base, src0, dst);
  }
  void vpsubd_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpsubd", VEX_PD, OP2_PSUBD_VdqWdq, address, src0, dst);
  }

  void vpsubq_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpsubq", VEX_PD, OP2_PSUBQ_VdqWdq, address, src0, dst);
  }

  void vpmuldq_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    threeByteOpSimd("vpmuldq", VEX_PD, OP3_PMULDQ_VdqWdq, ESCAPE_38, src1, src0,
                    dst);
  }

  void vpmuludq_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpmuludq", VEX_PD, OP2_PMULUDQ_VdqWdq, src1, src0, dst);
  }
  void vpmuludq_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                   XMMRegisterID dst) {
    twoByteOpSimd("vpmuludq", VEX_PD, OP2_PMULUDQ_VdqWdq, offset, base, src0,
                  dst);
  }
  void vpmuludq_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpmuludq", VEX_PD, OP2_PMULUDQ_VdqWdq, address, src0, dst);
  }

  void vpmaddwd_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpmaddwd", VEX_PD, OP2_PMADDWD_VdqWdq, src1, src0, dst);
  }
  void vpmaddwd_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpmaddwd", VEX_PD, OP2_PMADDWD_VdqWdq, address, src0, dst);
  }

  void vpmullw_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpmullw", VEX_PD, OP2_PMULLW_VdqWdq, src1, src0, dst);
  }
  void vpmulhw_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpmulhw", VEX_PD, OP2_PMULHW_VdqWdq, src1, src0, dst);
  }
  void vpmulhuw_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpmulhuw", VEX_PD, OP2_PMULHUW_VdqWdq, src1, src0, dst);
  }
  void vpmullw_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                  XMMRegisterID dst) {
    twoByteOpSimd("vpmullw", VEX_PD, OP2_PMULLW_VdqWdq, offset, base, src0,
                  dst);
  }
  void vpmulhw_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                  XMMRegisterID dst) {
    twoByteOpSimd("vpmulhw", VEX_PD, OP2_PMULHW_VdqWdq, offset, base, src0,
                  dst);
  }
  void vpmulhuw_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                   XMMRegisterID dst) {
    twoByteOpSimd("vpmulhuw", VEX_PD, OP2_PMULHUW_VdqWdq, offset, base, src0,
                  dst);
  }
  void vpmullw_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpmullw", VEX_PD, OP2_PMULLW_VdqWdq, address, src0, dst);
  }

  void vpmulld_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    threeByteOpSimd("vpmulld", VEX_PD, OP3_PMULLD_VdqWdq, ESCAPE_38, src1, src0,
                    dst);
  }
  void vpmulld_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                  XMMRegisterID dst) {
    threeByteOpSimd("vpmulld", VEX_PD, OP3_PMULLD_VdqWdq, ESCAPE_38, offset,
                    base, src0, dst);
  }
  void vpmulld_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    threeByteOpSimd("vpmulld", VEX_PD, OP3_PMULLD_VdqWdq, ESCAPE_38, address,
                    src0, dst);
  }
  void vpmulhrsw_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    threeByteOpSimd("vpmulhrsw", VEX_PD, OP3_PMULHRSW_VdqWdq, ESCAPE_38, src1,
                    src0, dst);
  }
  void vpmulhrsw_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                    XMMRegisterID dst) {
    threeByteOpSimd("vpmulhrsw", VEX_PD, OP3_PMULHRSW_VdqWdq, ESCAPE_38, offset,
                    base, src0, dst);
  }

  void vaddps_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vaddps", VEX_PS, OP2_ADDPS_VpsWps, src1, src0, dst);
  }
  void vaddps_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                 XMMRegisterID dst) {
    twoByteOpSimd("vaddps", VEX_PS, OP2_ADDPS_VpsWps, offset, base, src0, dst);
  }
  void vaddps_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vaddps", VEX_PS, OP2_ADDPS_VpsWps, address, src0, dst);
  }

  void vsubps_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vsubps", VEX_PS, OP2_SUBPS_VpsWps, src1, src0, dst);
  }
  void vsubps_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                 XMMRegisterID dst) {
    twoByteOpSimd("vsubps", VEX_PS, OP2_SUBPS_VpsWps, offset, base, src0, dst);
  }
  void vsubps_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vsubps", VEX_PS, OP2_SUBPS_VpsWps, address, src0, dst);
  }

  void vmulps_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vmulps", VEX_PS, OP2_MULPS_VpsWps, src1, src0, dst);
  }
  void vmulps_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                 XMMRegisterID dst) {
    twoByteOpSimd("vmulps", VEX_PS, OP2_MULPS_VpsWps, offset, base, src0, dst);
  }
  void vmulps_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vmulps", VEX_PS, OP2_MULPS_VpsWps, address, src0, dst);
  }

  void vdivps_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vdivps", VEX_PS, OP2_DIVPS_VpsWps, src1, src0, dst);
  }
  void vdivps_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                 XMMRegisterID dst) {
    twoByteOpSimd("vdivps", VEX_PS, OP2_DIVPS_VpsWps, offset, base, src0, dst);
  }
  void vdivps_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vdivps", VEX_PS, OP2_DIVPS_VpsWps, address, src0, dst);
  }

  void vmaxps_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vmaxps", VEX_PS, OP2_MAXPS_VpsWps, src1, src0, dst);
  }
  void vmaxps_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                 XMMRegisterID dst) {
    twoByteOpSimd("vmaxps", VEX_PS, OP2_MAXPS_VpsWps, offset, base, src0, dst);
  }
  void vmaxps_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vmaxps", VEX_PS, OP2_MAXPS_VpsWps, address, src0, dst);
  }

  void vmaxpd_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vmaxpd", VEX_PD, OP2_MAXPD_VpdWpd, src1, src0, dst);
  }

  void vminps_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vminps", VEX_PS, OP2_MINPS_VpsWps, src1, src0, dst);
  }
  void vminps_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                 XMMRegisterID dst) {
    twoByteOpSimd("vminps", VEX_PS, OP2_MINPS_VpsWps, offset, base, src0, dst);
  }
  void vminps_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vminps", VEX_PS, OP2_MINPS_VpsWps, address, src0, dst);
  }

  void vminpd_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vminpd", VEX_PD, OP2_MINPD_VpdWpd, src1, src0, dst);
  }
  void vminpd_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vminpd", VEX_PD, OP2_MINPD_VpdWpd, address, src0, dst);
  }

  void vaddpd_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vaddpd", VEX_PD, OP2_ADDPD_VpdWpd, src1, src0, dst);
  }
  void vaddpd_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vaddpd", VEX_PD, OP2_ADDPD_VpdWpd, address, src0, dst);
  }

  void vsubpd_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vsubpd", VEX_PD, OP2_SUBPD_VpdWpd, src1, src0, dst);
  }
  void vsubpd_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vsubpd", VEX_PD, OP2_SUBPD_VpdWpd, address, src0, dst);
  }

  void vmulpd_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vmulpd", VEX_PD, OP2_MULPD_VpdWpd, src1, src0, dst);
  }
  void vmulpd_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vmulpd", VEX_PD, OP2_MULPD_VpdWpd, address, src0, dst);
  }

  void vdivpd_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vdivpd", VEX_PD, OP2_DIVPD_VpdWpd, src1, src0, dst);
  }
  void vdivpd_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vdivpd", VEX_PD, OP2_DIVPD_VpdWpd, address, src0, dst);
  }

  void andl_rr(RegisterID src, RegisterID dst) {
    spew("andl       %s, %s", GPReg32Name(src), GPReg32Name(dst));
    m_formatter.oneByteOp(OP_AND_GvEv, src, dst);
  }

  void andw_rr(RegisterID src, RegisterID dst) {
    spew("andw       %s, %s", GPReg16Name(src), GPReg16Name(dst));
    m_formatter.prefix(PRE_OPERAND_SIZE);
    m_formatter.oneByteOp(OP_AND_GvEv, src, dst);
  }

  void andl_mr(int32_t offset, RegisterID base, RegisterID dst) {
    spew("andl       " MEM_ob ", %s", ADDR_ob(offset, base), GPReg32Name(dst));
    m_formatter.oneByteOp(OP_AND_GvEv, offset, base, dst);
  }

  void andl_mr(int32_t offset, RegisterID base, RegisterID index, int scale,
               RegisterID dst) {
    spew("andl       " MEM_obs ", %s", ADDR_obs(offset, base, index, scale),
         GPReg32Name(dst));
    m_formatter.oneByteOp(OP_AND_GvEv, offset, base, index, scale, dst);
  }

  void andl_rm(RegisterID src, int32_t offset, RegisterID base) {
    spew("andl       %s, " MEM_ob, GPReg32Name(src), ADDR_ob(offset, base));
    m_formatter.oneByteOp(OP_AND_EvGv, offset, base, src);
  }

  void andw_rm(RegisterID src, int32_t offset, RegisterID base) {
    spew("andw       %s, " MEM_ob, GPReg16Name(src), ADDR_ob(offset, base));
    m_formatter.prefix(PRE_OPERAND_SIZE);
    m_formatter.oneByteOp(OP_AND_EvGv, offset, base, src);
  }

  void andl_rm(RegisterID src, int32_t offset, RegisterID base,
               RegisterID index, int scale) {
    spew("andl       %s, " MEM_obs, GPReg32Name(src),
         ADDR_obs(offset, base, index, scale));
    m_formatter.oneByteOp(OP_AND_EvGv, offset, base, index, scale, src);
  }

  void andw_rm(RegisterID src, int32_t offset, RegisterID base,
               RegisterID index, int scale) {
    spew("andw       %s, " MEM_obs, GPReg16Name(src),
         ADDR_obs(offset, base, index, scale));
    m_formatter.prefix(PRE_OPERAND_SIZE);
    m_formatter.oneByteOp(OP_AND_EvGv, offset, base, index, scale, src);
  }

  void andl_ir(int32_t imm, RegisterID dst) {
    spew("andl       $0x%x, %s", uint32_t(imm), GPReg32Name(dst));
    if (CAN_SIGN_EXTEND_8_32(imm)) {
      m_formatter.oneByteOp(OP_GROUP1_EvIb, dst, GROUP1_OP_AND);
      m_formatter.immediate8s(imm);
    } else {
      if (dst == rax) {
        m_formatter.oneByteOp(OP_AND_EAXIv);
      } else {
        m_formatter.oneByteOp(OP_GROUP1_EvIz, dst, GROUP1_OP_AND);
      }
      m_formatter.immediate32(imm);
    }
  }

  void andw_ir(int32_t imm, RegisterID dst) {
    spew("andw       $0x%x, %s", uint16_t(imm), GPReg16Name(dst));
    m_formatter.prefix(PRE_OPERAND_SIZE);
    if (CAN_SIGN_EXTEND_8_32(imm)) {
      m_formatter.oneByteOp(OP_GROUP1_EvIb, dst, GROUP1_OP_AND);
      m_formatter.immediate8s(imm);
    } else {
      if (dst == rax) {
        m_formatter.oneByteOp(OP_AND_EAXIv);
      } else {
        m_formatter.oneByteOp(OP_GROUP1_EvIz, dst, GROUP1_OP_AND);
      }
      m_formatter.immediate16(imm);
    }
  }

  void andl_im(int32_t imm, int32_t offset, RegisterID base) {
    spew("andl       $0x%x, " MEM_ob, uint32_t(imm), ADDR_ob(offset, base));
    if (CAN_SIGN_EXTEND_8_32(imm)) {
      m_formatter.oneByteOp(OP_GROUP1_EvIb, offset, base, GROUP1_OP_AND);
      m_formatter.immediate8s(imm);
    } else {
      m_formatter.oneByteOp(OP_GROUP1_EvIz, offset, base, GROUP1_OP_AND);
      m_formatter.immediate32(imm);
    }
  }

  void andw_im(int32_t imm, int32_t offset, RegisterID base) {
    spew("andw       $0x%x, " MEM_ob, uint16_t(imm), ADDR_ob(offset, base));
    m_formatter.prefix(PRE_OPERAND_SIZE);
    if (CAN_SIGN_EXTEND_8_32(imm)) {
      m_formatter.oneByteOp(OP_GROUP1_EvIb, offset, base, GROUP1_OP_AND);
      m_formatter.immediate8s(imm);
    } else {
      m_formatter.oneByteOp(OP_GROUP1_EvIz, offset, base, GROUP1_OP_AND);
      m_formatter.immediate16(imm);
    }
  }

  void andl_im(int32_t imm, int32_t offset, RegisterID base, RegisterID index,
               int scale) {
    spew("andl       $%d, " MEM_obs, imm, ADDR_obs(offset, base, index, scale));
    if (CAN_SIGN_EXTEND_8_32(imm)) {
      m_formatter.oneByteOp(OP_GROUP1_EvIb, offset, base, index, scale,
                            GROUP1_OP_AND);
      m_formatter.immediate8s(imm);
    } else {
      m_formatter.oneByteOp(OP_GROUP1_EvIz, offset, base, index, scale,
                            GROUP1_OP_AND);
      m_formatter.immediate32(imm);
    }
  }

  void andw_im(int32_t imm, int32_t offset, RegisterID base, RegisterID index,
               int scale) {
    spew("andw       $%d, " MEM_obs, int16_t(imm),
         ADDR_obs(offset, base, index, scale));
    m_formatter.prefix(PRE_OPERAND_SIZE);
    if (CAN_SIGN_EXTEND_8_32(imm)) {
      m_formatter.oneByteOp(OP_GROUP1_EvIb, offset, base, index, scale,
                            GROUP1_OP_AND);
      m_formatter.immediate8s(imm);
    } else {
      m_formatter.oneByteOp(OP_GROUP1_EvIz, offset, base, index, scale,
                            GROUP1_OP_AND);
      m_formatter.immediate16(imm);
    }
  }

  void fld_m(int32_t offset, RegisterID base) {
    spew("fld        " MEM_ob, ADDR_ob(offset, base));
    m_formatter.oneByteOp(OP_FPU6, offset, base, FPU6_OP_FLD);
  }
  void fld32_m(int32_t offset, RegisterID base) {
    spew("fld        " MEM_ob, ADDR_ob(offset, base));
    m_formatter.oneByteOp(OP_FPU6_F32, offset, base, FPU6_OP_FLD);
  }
  void faddp() {
    spew("addp       ");
    m_formatter.oneByteOp(OP_FPU6_ADDP);
    m_formatter.oneByteOp(OP_ADDP_ST0_ST1);
  }
  void fisttp_m(int32_t offset, RegisterID base) {
    spew("fisttp     " MEM_ob, ADDR_ob(offset, base));
    m_formatter.oneByteOp(OP_FPU6, offset, base, FPU6_OP_FISTTP);
  }
  void fistp_m(int32_t offset, RegisterID base) {
    spew("fistp      " MEM_ob, ADDR_ob(offset, base));
    m_formatter.oneByteOp(OP_FILD, offset, base, FPU6_OP_FISTP);
  }
  void fstp_m(int32_t offset, RegisterID base) {
    spew("fstp       " MEM_ob, ADDR_ob(offset, base));
    m_formatter.oneByteOp(OP_FPU6, offset, base, FPU6_OP_FSTP);
  }
  void fstp32_m(int32_t offset, RegisterID base) {
    spew("fstp32     " MEM_ob, ADDR_ob(offset, base));
    m_formatter.oneByteOp(OP_FPU6_F32, offset, base, FPU6_OP_FSTP);
  }
  void fnstcw_m(int32_t offset, RegisterID base) {
    spew("fnstcw     " MEM_ob, ADDR_ob(offset, base));
    m_formatter.oneByteOp(OP_FPU6_F32, offset, base, FPU6_OP_FISTP);
  }
  void fldcw_m(int32_t offset, RegisterID base) {
    spew("fldcw      " MEM_ob, ADDR_ob(offset, base));
    m_formatter.oneByteOp(OP_FPU6_F32, offset, base, FPU6_OP_FLDCW);
  }
  void fnstsw_m(int32_t offset, RegisterID base) {
    spew("fnstsw     " MEM_ob, ADDR_ob(offset, base));
    m_formatter.oneByteOp(OP_FPU6, offset, base, FPU6_OP_FISTP);
  }

  void negl_r(RegisterID dst) {
    spew("negl       %s", GPReg32Name(dst));
    m_formatter.oneByteOp(OP_GROUP3_Ev, dst, GROUP3_OP_NEG);
  }

  void negl_m(int32_t offset, RegisterID base) {
    spew("negl       " MEM_ob, ADDR_ob(offset, base));
    m_formatter.oneByteOp(OP_GROUP3_Ev, offset, base, GROUP3_OP_NEG);
  }

  void notl_r(RegisterID dst) {
    spew("notl       %s", GPReg32Name(dst));
    m_formatter.oneByteOp(OP_GROUP3_Ev, dst, GROUP3_OP_NOT);
  }

  void notl_m(int32_t offset, RegisterID base) {
    spew("notl       " MEM_ob, ADDR_ob(offset, base));
    m_formatter.oneByteOp(OP_GROUP3_Ev, offset, base, GROUP3_OP_NOT);
  }

  void orl_rr(RegisterID src, RegisterID dst) {
    spew("orl        %s, %s", GPReg32Name(src), GPReg32Name(dst));
    m_formatter.oneByteOp(OP_OR_GvEv, src, dst);
  }

  void orw_rr(RegisterID src, RegisterID dst) {
    spew("orw        %s, %s", GPReg16Name(src), GPReg16Name(dst));
    m_formatter.prefix(PRE_OPERAND_SIZE);
    m_formatter.oneByteOp(OP_OR_GvEv, src, dst);
  }

  void orl_mr(int32_t offset, RegisterID base, RegisterID dst) {
    spew("orl        " MEM_ob ", %s", ADDR_ob(offset, base), GPReg32Name(dst));
    m_formatter.oneByteOp(OP_OR_GvEv, offset, base, dst);
  }

  void orl_rm(RegisterID src, int32_t offset, RegisterID base) {
    spew("orl        %s, " MEM_ob, GPReg32Name(src), ADDR_ob(offset, base));
    m_formatter.oneByteOp(OP_OR_EvGv, offset, base, src);
  }

  void orw_rm(RegisterID src, int32_t offset, RegisterID base) {
    spew("orw        %s, " MEM_ob, GPReg16Name(src), ADDR_ob(offset, base));
    m_formatter.prefix(PRE_OPERAND_SIZE);
    m_formatter.oneByteOp(OP_OR_EvGv, offset, base, src);
  }

  void orl_rm(RegisterID src, int32_t offset, RegisterID base, RegisterID index,
              int scale) {
    spew("orl        %s, " MEM_obs, GPReg32Name(src),
         ADDR_obs(offset, base, index, scale));
    m_formatter.oneByteOp(OP_OR_EvGv, offset, base, index, scale, src);
  }

  void orw_rm(RegisterID src, int32_t offset, RegisterID base, RegisterID index,
              int scale) {
    spew("orw        %s, " MEM_obs, GPReg16Name(src),
         ADDR_obs(offset, base, index, scale));
    m_formatter.prefix(PRE_OPERAND_SIZE);
    m_formatter.oneByteOp(OP_OR_EvGv, offset, base, index, scale, src);
  }

  void orl_ir(int32_t imm, RegisterID dst) {
    spew("orl        $0x%x, %s", uint32_t(imm), GPReg32Name(dst));
    if (CAN_SIGN_EXTEND_8_32(imm)) {
      m_formatter.oneByteOp(OP_GROUP1_EvIb, dst, GROUP1_OP_OR);
      m_formatter.immediate8s(imm);
    } else {
      if (dst == rax) {
        m_formatter.oneByteOp(OP_OR_EAXIv);
      } else {
        m_formatter.oneByteOp(OP_GROUP1_EvIz, dst, GROUP1_OP_OR);
      }
      m_formatter.immediate32(imm);
    }
  }

  void orw_ir(int32_t imm, RegisterID dst) {
    spew("orw        $0x%x, %s", uint16_t(imm), GPReg16Name(dst));
    m_formatter.prefix(PRE_OPERAND_SIZE);
    if (CAN_SIGN_EXTEND_8_32(imm)) {
      m_formatter.oneByteOp(OP_GROUP1_EvIb, dst, GROUP1_OP_OR);
      m_formatter.immediate8s(imm);
    } else {
      if (dst == rax) {
        m_formatter.oneByteOp(OP_OR_EAXIv);
      } else {
        m_formatter.oneByteOp(OP_GROUP1_EvIz, dst, GROUP1_OP_OR);
      }
      m_formatter.immediate16(imm);
    }
  }

  void orl_im(int32_t imm, int32_t offset, RegisterID base) {
    spew("orl        $0x%x, " MEM_ob, uint32_t(imm), ADDR_ob(offset, base));
    if (CAN_SIGN_EXTEND_8_32(imm)) {
      m_formatter.oneByteOp(OP_GROUP1_EvIb, offset, base, GROUP1_OP_OR);
      m_formatter.immediate8s(imm);
    } else {
      m_formatter.oneByteOp(OP_GROUP1_EvIz, offset, base, GROUP1_OP_OR);
      m_formatter.immediate32(imm);
    }
  }

  void orw_im(int32_t imm, int32_t offset, RegisterID base) {
    spew("orw        $0x%x, " MEM_ob, uint16_t(imm), ADDR_ob(offset, base));
    m_formatter.prefix(PRE_OPERAND_SIZE);
    if (CAN_SIGN_EXTEND_8_32(imm)) {
      m_formatter.oneByteOp(OP_GROUP1_EvIb, offset, base, GROUP1_OP_OR);
      m_formatter.immediate8s(imm);
    } else {
      m_formatter.oneByteOp(OP_GROUP1_EvIz, offset, base, GROUP1_OP_OR);
      m_formatter.immediate16(imm);
    }
  }

  void orl_im(int32_t imm, int32_t offset, RegisterID base, RegisterID index,
              int scale) {
    spew("orl        $%d, " MEM_obs, imm, ADDR_obs(offset, base, index, scale));
    if (CAN_SIGN_EXTEND_8_32(imm)) {
      m_formatter.oneByteOp(OP_GROUP1_EvIb, offset, base, index, scale,
                            GROUP1_OP_OR);
      m_formatter.immediate8s(imm);
    } else {
      m_formatter.oneByteOp(OP_GROUP1_EvIz, offset, base, index, scale,
                            GROUP1_OP_OR);
      m_formatter.immediate32(imm);
    }
  }

  void orw_im(int32_t imm, int32_t offset, RegisterID base, RegisterID index,
              int scale) {
    spew("orw        $%d, " MEM_obs, int16_t(imm),
         ADDR_obs(offset, base, index, scale));
    m_formatter.prefix(PRE_OPERAND_SIZE);
    if (CAN_SIGN_EXTEND_8_32(imm)) {
      m_formatter.oneByteOp(OP_GROUP1_EvIb, offset, base, index, scale,
                            GROUP1_OP_OR);
      m_formatter.immediate8s(imm);
    } else {
      m_formatter.oneByteOp(OP_GROUP1_EvIz, offset, base, index, scale,
                            GROUP1_OP_OR);
      m_formatter.immediate16(imm);
    }
  }

  void sbbl_rr(RegisterID src, RegisterID dst) {
    spew("sbbl       %s, %s", GPReg32Name(src), GPReg32Name(dst));
    m_formatter.oneByteOp(OP_SBB_GvEv, src, dst);
  }

  void subl_rr(RegisterID src, RegisterID dst) {
    spew("subl       %s, %s", GPReg32Name(src), GPReg32Name(dst));
    m_formatter.oneByteOp(OP_SUB_GvEv, src, dst);
  }

  void subw_rr(RegisterID src, RegisterID dst) {
    spew("subw       %s, %s", GPReg16Name(src), GPReg16Name(dst));
    m_formatter.prefix(PRE_OPERAND_SIZE);
    m_formatter.oneByteOp(OP_SUB_GvEv, src, dst);
  }

  void subl_mr(int32_t offset, RegisterID base, RegisterID dst) {
    spew("subl       " MEM_ob ", %s", ADDR_ob(offset, base), GPReg32Name(dst));
    m_formatter.oneByteOp(OP_SUB_GvEv, offset, base, dst);
  }

  void subl_rm(RegisterID src, int32_t offset, RegisterID base) {
    spew("subl       %s, " MEM_ob, GPReg32Name(src), ADDR_ob(offset, base));
    m_formatter.oneByteOp(OP_SUB_EvGv, offset, base, src);
  }

  void subw_rm(RegisterID src, int32_t offset, RegisterID base) {
    spew("subw       %s, " MEM_ob, GPReg16Name(src), ADDR_ob(offset, base));
    m_formatter.prefix(PRE_OPERAND_SIZE);
    m_formatter.oneByteOp(OP_SUB_EvGv, offset, base, src);
  }

  void subl_rm(RegisterID src, int32_t offset, RegisterID base,
               RegisterID index, int scale) {
    spew("subl       %s, " MEM_obs, GPReg32Name(src),
         ADDR_obs(offset, base, index, scale));
    m_formatter.oneByteOp(OP_SUB_EvGv, offset, base, index, scale, src);
  }

  void subw_rm(RegisterID src, int32_t offset, RegisterID base,
               RegisterID index, int scale) {
    spew("subw       %s, " MEM_obs, GPReg16Name(src),
         ADDR_obs(offset, base, index, scale));
    m_formatter.prefix(PRE_OPERAND_SIZE);
    m_formatter.oneByteOp(OP_SUB_EvGv, offset, base, index, scale, src);
  }

  void subl_ir(int32_t imm, RegisterID dst) {
    spew("subl       $%d, %s", imm, GPReg32Name(dst));
    if (CAN_SIGN_EXTEND_8_32(imm)) {
      m_formatter.oneByteOp(OP_GROUP1_EvIb, dst, GROUP1_OP_SUB);
      m_formatter.immediate8s(imm);
    } else {
      if (dst == rax) {
        m_formatter.oneByteOp(OP_SUB_EAXIv);
      } else {
        m_formatter.oneByteOp(OP_GROUP1_EvIz, dst, GROUP1_OP_SUB);
      }
      m_formatter.immediate32(imm);
    }
  }

  void subw_ir(int32_t imm, RegisterID dst) {
    spew("subw       $%d, %s", int16_t(imm), GPReg16Name(dst));
    m_formatter.prefix(PRE_OPERAND_SIZE);
    if (CAN_SIGN_EXTEND_8_32(imm)) {
      m_formatter.oneByteOp(OP_GROUP1_EvIb, dst, GROUP1_OP_SUB);
      m_formatter.immediate8s(imm);
    } else {
      if (dst == rax) {
        m_formatter.oneByteOp(OP_SUB_EAXIv);
      } else {
        m_formatter.oneByteOp(OP_GROUP1_EvIz, dst, GROUP1_OP_SUB);
      }
      m_formatter.immediate16(imm);
    }
  }

  void subl_im(int32_t imm, int32_t offset, RegisterID base) {
    spew("subl       $%d, " MEM_ob, imm, ADDR_ob(offset, base));
    if (CAN_SIGN_EXTEND_8_32(imm)) {
      m_formatter.oneByteOp(OP_GROUP1_EvIb, offset, base, GROUP1_OP_SUB);
      m_formatter.immediate8s(imm);
    } else {
      m_formatter.oneByteOp(OP_GROUP1_EvIz, offset, base, GROUP1_OP_SUB);
      m_formatter.immediate32(imm);
    }
  }

  void subw_im(int32_t imm, int32_t offset, RegisterID base) {
    spew("subw       $%d, " MEM_ob, int16_t(imm), ADDR_ob(offset, base));
    m_formatter.prefix(PRE_OPERAND_SIZE);
    if (CAN_SIGN_EXTEND_8_32(imm)) {
      m_formatter.oneByteOp(OP_GROUP1_EvIb, offset, base, GROUP1_OP_SUB);
      m_formatter.immediate8s(imm);
    } else {
      m_formatter.oneByteOp(OP_GROUP1_EvIz, offset, base, GROUP1_OP_SUB);
      m_formatter.immediate16(imm);
    }
  }

  void subl_im(int32_t imm, int32_t offset, RegisterID base, RegisterID index,
               int scale) {
    spew("subl       $%d, " MEM_obs, imm, ADDR_obs(offset, base, index, scale));
    if (CAN_SIGN_EXTEND_8_32(imm)) {
      m_formatter.oneByteOp(OP_GROUP1_EvIb, offset, base, index, scale,
                            GROUP1_OP_SUB);
      m_formatter.immediate8s(imm);
    } else {
      m_formatter.oneByteOp(OP_GROUP1_EvIz, offset, base, index, scale,
                            GROUP1_OP_SUB);
      m_formatter.immediate32(imm);
    }
  }

  void subw_im(int32_t imm, int32_t offset, RegisterID base, RegisterID index,
               int scale) {
    spew("subw       $%d, " MEM_obs, int16_t(imm),
         ADDR_obs(offset, base, index, scale));
    m_formatter.prefix(PRE_OPERAND_SIZE);
    if (CAN_SIGN_EXTEND_8_32(imm)) {
      m_formatter.oneByteOp(OP_GROUP1_EvIb, offset, base, index, scale,
                            GROUP1_OP_SUB);
      m_formatter.immediate8s(imm);
    } else {
      m_formatter.oneByteOp(OP_GROUP1_EvIz, offset, base, index, scale,
                            GROUP1_OP_SUB);
      m_formatter.immediate16(imm);
    }
  }

  void xorl_rr(RegisterID src, RegisterID dst) {
    spew("xorl       %s, %s", GPReg32Name(src), GPReg32Name(dst));
    m_formatter.oneByteOp(OP_XOR_GvEv, src, dst);
  }

  void xorw_rr(RegisterID src, RegisterID dst) {
    spew("xorw       %s, %s", GPReg16Name(src), GPReg16Name(dst));
    m_formatter.prefix(PRE_OPERAND_SIZE);
    m_formatter.oneByteOp(OP_XOR_GvEv, src, dst);
  }

  void xorl_mr(int32_t offset, RegisterID base, RegisterID dst) {
    spew("xorl       " MEM_ob ", %s", ADDR_ob(offset, base), GPReg32Name(dst));
    m_formatter.oneByteOp(OP_XOR_GvEv, offset, base, dst);
  }

  void xorl_rm(RegisterID src, int32_t offset, RegisterID base) {
    spew("xorl       %s, " MEM_ob, GPReg32Name(src), ADDR_ob(offset, base));
    m_formatter.oneByteOp(OP_XOR_EvGv, offset, base, src);
  }

  void xorw_rm(RegisterID src, int32_t offset, RegisterID base) {
    spew("xorw       %s, " MEM_ob, GPReg16Name(src), ADDR_ob(offset, base));
    m_formatter.prefix(PRE_OPERAND_SIZE);
    m_formatter.oneByteOp(OP_XOR_EvGv, offset, base, src);
  }

  void xorl_rm(RegisterID src, int32_t offset, RegisterID base,
               RegisterID index, int scale) {
    spew("xorl       %s, " MEM_obs, GPReg32Name(src),
         ADDR_obs(offset, base, index, scale));
    m_formatter.oneByteOp(OP_XOR_EvGv, offset, base, index, scale, src);
  }

  void xorw_rm(RegisterID src, int32_t offset, RegisterID base,
               RegisterID index, int scale) {
    spew("xorw       %s, " MEM_obs, GPReg16Name(src),
         ADDR_obs(offset, base, index, scale));
    m_formatter.prefix(PRE_OPERAND_SIZE);
    m_formatter.oneByteOp(OP_XOR_EvGv, offset, base, index, scale, src);
  }

  void xorl_im(int32_t imm, int32_t offset, RegisterID base) {
    spew("xorl       $0x%x, " MEM_ob, uint32_t(imm), ADDR_ob(offset, base));
    if (CAN_SIGN_EXTEND_8_32(imm)) {
      m_formatter.oneByteOp(OP_GROUP1_EvIb, offset, base, GROUP1_OP_XOR);
      m_formatter.immediate8s(imm);
    } else {
      m_formatter.oneByteOp(OP_GROUP1_EvIz, offset, base, GROUP1_OP_XOR);
      m_formatter.immediate32(imm);
    }
  }

  void xorw_im(int32_t imm, int32_t offset, RegisterID base) {
    spew("xorw       $0x%x, " MEM_ob, uint16_t(imm), ADDR_ob(offset, base));
    m_formatter.prefix(PRE_OPERAND_SIZE);
    if (CAN_SIGN_EXTEND_8_32(imm)) {
      m_formatter.oneByteOp(OP_GROUP1_EvIb, offset, base, GROUP1_OP_XOR);
      m_formatter.immediate8s(imm);
    } else {
      m_formatter.oneByteOp(OP_GROUP1_EvIz, offset, base, GROUP1_OP_XOR);
      m_formatter.immediate16(imm);
    }
  }

  void xorl_im(int32_t imm, int32_t offset, RegisterID base, RegisterID index,
               int scale) {
    spew("xorl       $%d, " MEM_obs, imm, ADDR_obs(offset, base, index, scale));
    if (CAN_SIGN_EXTEND_8_32(imm)) {
      m_formatter.oneByteOp(OP_GROUP1_EvIb, offset, base, index, scale,
                            GROUP1_OP_XOR);
      m_formatter.immediate8s(imm);
    } else {
      m_formatter.oneByteOp(OP_GROUP1_EvIz, offset, base, index, scale,
                            GROUP1_OP_XOR);
      m_formatter.immediate32(imm);
    }
  }

  void xorw_im(int32_t imm, int32_t offset, RegisterID base, RegisterID index,
               int scale) {
    spew("xorw       $%d, " MEM_obs, int16_t(imm),
         ADDR_obs(offset, base, index, scale));
    m_formatter.prefix(PRE_OPERAND_SIZE);
    if (CAN_SIGN_EXTEND_8_32(imm)) {
      m_formatter.oneByteOp(OP_GROUP1_EvIb, offset, base, index, scale,
                            GROUP1_OP_XOR);
      m_formatter.immediate8s(imm);
    } else {
      m_formatter.oneByteOp(OP_GROUP1_EvIz, offset, base, index, scale,
                            GROUP1_OP_XOR);
      m_formatter.immediate16(imm);
    }
  }

  void xorl_ir(int32_t imm, RegisterID dst) {
    spew("xorl       $%d, %s", imm, GPReg32Name(dst));
    if (CAN_SIGN_EXTEND_8_32(imm)) {
      m_formatter.oneByteOp(OP_GROUP1_EvIb, dst, GROUP1_OP_XOR);
      m_formatter.immediate8s(imm);
    } else {
      if (dst == rax) {
        m_formatter.oneByteOp(OP_XOR_EAXIv);
      } else {
        m_formatter.oneByteOp(OP_GROUP1_EvIz, dst, GROUP1_OP_XOR);
      }
      m_formatter.immediate32(imm);
    }
  }

  void xorw_ir(int32_t imm, RegisterID dst) {
    spew("xorw       $%d, %s", int16_t(imm), GPReg16Name(dst));
    m_formatter.prefix(PRE_OPERAND_SIZE);
    if (CAN_SIGN_EXTEND_8_32(imm)) {
      m_formatter.oneByteOp(OP_GROUP1_EvIb, dst, GROUP1_OP_XOR);
      m_formatter.immediate8s(imm);
    } else {
      if (dst == rax) {
        m_formatter.oneByteOp(OP_XOR_EAXIv);
      } else {
        m_formatter.oneByteOp(OP_GROUP1_EvIz, dst, GROUP1_OP_XOR);
      }
      m_formatter.immediate16(imm);
    }
  }

  void bswapl_r(RegisterID dst) {
    spew("bswap      %s", GPReg32Name(dst));
    m_formatter.twoByteOp(OP2_BSWAP, dst);
  }

  void sarl_ir(int32_t imm, RegisterID dst) {
    MOZ_ASSERT(imm < 32);
    spew("sarl       $%d, %s", imm, GPReg32Name(dst));
    if (imm == 1) {
      m_formatter.oneByteOp(OP_GROUP2_Ev1, dst, GROUP2_OP_SAR);
    } else {
      m_formatter.oneByteOp(OP_GROUP2_EvIb, dst, GROUP2_OP_SAR);
      m_formatter.immediate8u(imm);
    }
  }

  void sarl_CLr(RegisterID dst) {
    spew("sarl       %%cl, %s", GPReg32Name(dst));
    m_formatter.oneByteOp(OP_GROUP2_EvCL, dst, GROUP2_OP_SAR);
  }

  void shrl_ir(int32_t imm, RegisterID dst) {
    MOZ_ASSERT(imm < 32);
    spew("shrl       $%d, %s", imm, GPReg32Name(dst));
    if (imm == 1) {
      m_formatter.oneByteOp(OP_GROUP2_Ev1, dst, GROUP2_OP_SHR);
    } else {
      m_formatter.oneByteOp(OP_GROUP2_EvIb, dst, GROUP2_OP_SHR);
      m_formatter.immediate8u(imm);
    }
  }

  void shrl_CLr(RegisterID dst) {
    spew("shrl       %%cl, %s", GPReg32Name(dst));
    m_formatter.oneByteOp(OP_GROUP2_EvCL, dst, GROUP2_OP_SHR);
  }

  void shrdl_CLr(RegisterID src, RegisterID dst) {
    spew("shrdl      %%cl, %s, %s", GPReg32Name(src), GPReg32Name(dst));
    m_formatter.twoByteOp(OP2_SHRD_GvEv, dst, src);
  }

  void shldl_CLr(RegisterID src, RegisterID dst) {
    spew("shldl      %%cl, %s, %s", GPReg32Name(src), GPReg32Name(dst));
    m_formatter.twoByteOp(OP2_SHLD_GvEv, dst, src);
  }

  void shll_ir(int32_t imm, RegisterID dst) {
    MOZ_ASSERT(imm < 32);
    spew("shll       $%d, %s", imm, GPReg32Name(dst));
    if (imm == 1) {
      m_formatter.oneByteOp(OP_GROUP2_Ev1, dst, GROUP2_OP_SHL);
    } else {
      m_formatter.oneByteOp(OP_GROUP2_EvIb, dst, GROUP2_OP_SHL);
      m_formatter.immediate8u(imm);
    }
  }

  void shll_CLr(RegisterID dst) {
    spew("shll       %%cl, %s", GPReg32Name(dst));
    m_formatter.oneByteOp(OP_GROUP2_EvCL, dst, GROUP2_OP_SHL);
  }

  void roll_ir(int32_t imm, RegisterID dst) {
    MOZ_ASSERT(imm < 32);
    spew("roll       $%d, %s", imm, GPReg32Name(dst));
    if (imm == 1) {
      m_formatter.oneByteOp(OP_GROUP2_Ev1, dst, GROUP2_OP_ROL);
    } else {
      m_formatter.oneByteOp(OP_GROUP2_EvIb, dst, GROUP2_OP_ROL);
      m_formatter.immediate8u(imm);
    }
  }
  void rolw_ir(int32_t imm, RegisterID dst) {
    MOZ_ASSERT(imm < 32);
    spew("roll       $%d, %s", imm, GPReg16Name(dst));
    m_formatter.prefix(PRE_OPERAND_SIZE);
    if (imm == 1) {
      m_formatter.oneByteOp(OP_GROUP2_Ev1, dst, GROUP2_OP_ROL);
    } else {
      m_formatter.oneByteOp(OP_GROUP2_EvIb, dst, GROUP2_OP_ROL);
      m_formatter.immediate8u(imm);
    }
  }
  void roll_CLr(RegisterID dst) {
    spew("roll       %%cl, %s", GPReg32Name(dst));
    m_formatter.oneByteOp(OP_GROUP2_EvCL, dst, GROUP2_OP_ROL);
  }

  void rorl_ir(int32_t imm, RegisterID dst) {
    MOZ_ASSERT(imm < 32);
    spew("rorl       $%d, %s", imm, GPReg32Name(dst));
    if (imm == 1) {
      m_formatter.oneByteOp(OP_GROUP2_Ev1, dst, GROUP2_OP_ROR);
    } else {
      m_formatter.oneByteOp(OP_GROUP2_EvIb, dst, GROUP2_OP_ROR);
      m_formatter.immediate8u(imm);
    }
  }
  void rorl_CLr(RegisterID dst) {
    spew("rorl       %%cl, %s", GPReg32Name(dst));
    m_formatter.oneByteOp(OP_GROUP2_EvCL, dst, GROUP2_OP_ROR);
  }

  void bsrl_rr(RegisterID src, RegisterID dst) {
    spew("bsrl       %s, %s", GPReg32Name(src), GPReg32Name(dst));
    m_formatter.twoByteOp(OP2_BSR_GvEv, src, dst);
  }

  void bsfl_rr(RegisterID src, RegisterID dst) {
    spew("bsfl       %s, %s", GPReg32Name(src), GPReg32Name(dst));
    m_formatter.twoByteOp(OP2_BSF_GvEv, src, dst);
  }

  void lzcntl_rr(RegisterID src, RegisterID dst) {
    spew("lzcntl     %s, %s", GPReg32Name(src), GPReg32Name(dst));
    m_formatter.legacySSEPrefix(VEX_SS);
    m_formatter.twoByteOp(OP2_LZCNT_GvEv, src, dst);
  }

  void tzcntl_rr(RegisterID src, RegisterID dst) {
    spew("tzcntl     %s, %s", GPReg32Name(src), GPReg32Name(dst));
    m_formatter.legacySSEPrefix(VEX_SS);
    m_formatter.twoByteOp(OP2_TZCNT_GvEv, src, dst);
  }

  void popcntl_rr(RegisterID src, RegisterID dst) {
    spew("popcntl    %s, %s", GPReg32Name(src), GPReg32Name(dst));
    m_formatter.legacySSEPrefix(VEX_SS);
    m_formatter.twoByteOp(OP2_POPCNT_GvEv, src, dst);
  }

  void imull_rr(RegisterID src, RegisterID dst) {
    spew("imull      %s, %s", GPReg32Name(src), GPReg32Name(dst));
    m_formatter.twoByteOp(OP2_IMUL_GvEv, src, dst);
  }

  void imull_r(RegisterID multiplier) {
    spew("imull      %s", GPReg32Name(multiplier));
    m_formatter.oneByteOp(OP_GROUP3_Ev, multiplier, GROUP3_OP_IMUL);
  }

  void imull_mr(int32_t offset, RegisterID base, RegisterID dst) {
    spew("imull      " MEM_ob ", %s", ADDR_ob(offset, base), GPReg32Name(dst));
    m_formatter.twoByteOp(OP2_IMUL_GvEv, offset, base, dst);
  }

  void imull_ir(int32_t value, RegisterID src, RegisterID dst) {
    spew("imull      $%d, %s, %s", value, GPReg32Name(src), GPReg32Name(dst));
    if (CAN_SIGN_EXTEND_8_32(value)) {
      m_formatter.oneByteOp(OP_IMUL_GvEvIb, src, dst);
      m_formatter.immediate8s(value);
    } else {
      m_formatter.oneByteOp(OP_IMUL_GvEvIz, src, dst);
      m_formatter.immediate32(value);
    }
  }

  void mull_r(RegisterID multiplier) {
    spew("mull       %s", GPReg32Name(multiplier));
    m_formatter.oneByteOp(OP_GROUP3_Ev, multiplier, GROUP3_OP_MUL);
  }

  void idivl_r(RegisterID divisor) {
    spew("idivl      %s", GPReg32Name(divisor));
    m_formatter.oneByteOp(OP_GROUP3_Ev, divisor, GROUP3_OP_IDIV);
  }

  void divl_r(RegisterID divisor) {
    spew("div        %s", GPReg32Name(divisor));
    m_formatter.oneByteOp(OP_GROUP3_Ev, divisor, GROUP3_OP_DIV);
  }

  void prefix_lock() {
    spew("lock");
    m_formatter.oneByteOp(PRE_LOCK);
  }

  void prefix_16_for_32() {
    spew("[16-bit operands next]");
    m_formatter.prefix(PRE_OPERAND_SIZE);
  }

  void incl_m32(int32_t offset, RegisterID base) {
    spew("incl       " MEM_ob, ADDR_ob(offset, base));
    m_formatter.oneByteOp(OP_GROUP5_Ev, offset, base, GROUP5_OP_INC);
  }

  void decl_m32(int32_t offset, RegisterID base) {
    spew("decl       " MEM_ob, ADDR_ob(offset, base));
    m_formatter.oneByteOp(OP_GROUP5_Ev, offset, base, GROUP5_OP_DEC);
  }

  // Note that CMPXCHG performs comparison against REG = %al/%ax/%eax/%rax.
  // If %REG == [%base+offset], then %src -> [%base+offset].
  // Otherwise, [%base+offset] -> %REG.
  // For the 8-bit operations src must also be an 8-bit register.

  void cmpxchgb(RegisterID src, int32_t offset, RegisterID base) {
    spew("cmpxchgb   %s, " MEM_ob, GPReg8Name(src), ADDR_ob(offset, base));
    m_formatter.twoByteOp8(OP2_CMPXCHG_GvEb, offset, base, src);
  }
  void cmpxchgb(RegisterID src, int32_t offset, RegisterID base,
                RegisterID index, int scale) {
    spew("cmpxchgb   %s, " MEM_obs, GPReg8Name(src),
         ADDR_obs(offset, base, index, scale));
    m_formatter.twoByteOp8(OP2_CMPXCHG_GvEb, offset, base, index, scale, src);
  }
  void cmpxchgw(RegisterID src, int32_t offset, RegisterID base) {
    spew("cmpxchgw   %s, " MEM_ob, GPReg16Name(src), ADDR_ob(offset, base));
    m_formatter.prefix(PRE_OPERAND_SIZE);
    m_formatter.twoByteOp(OP2_CMPXCHG_GvEw, offset, base, src);
  }
  void cmpxchgw(RegisterID src, int32_t offset, RegisterID base,
                RegisterID index, int scale) {
    spew("cmpxchgw   %s, " MEM_obs, GPReg16Name(src),
         ADDR_obs(offset, base, index, scale));
    m_formatter.prefix(PRE_OPERAND_SIZE);
    m_formatter.twoByteOp(OP2_CMPXCHG_GvEw, offset, base, index, scale, src);
  }
  void cmpxchgl(RegisterID src, int32_t offset, RegisterID base) {
    spew("cmpxchgl   %s, " MEM_ob, GPReg32Name(src), ADDR_ob(offset, base));
    m_formatter.twoByteOp(OP2_CMPXCHG_GvEw, offset, base, src);
  }
  void cmpxchgl(RegisterID src, int32_t offset, RegisterID base,
                RegisterID index, int scale) {
    spew("cmpxchgl   %s, " MEM_obs, GPReg32Name(src),
         ADDR_obs(offset, base, index, scale));
    m_formatter.twoByteOp(OP2_CMPXCHG_GvEw, offset, base, index, scale, src);
  }

  void cmpxchg8b(RegisterID srcHi, RegisterID srcLo, RegisterID newHi,
                 RegisterID newLo, int32_t offset, RegisterID base) {
    MOZ_ASSERT(srcHi == edx.code() && srcLo == eax.code());
    MOZ_ASSERT(newHi == ecx.code() && newLo == ebx.code());
    spew("cmpxchg8b  %s, " MEM_ob, "edx:eax", ADDR_ob(offset, base));
    m_formatter.twoByteOp(OP2_CMPXCHGNB, offset, base, 1);
  }
  void cmpxchg8b(RegisterID srcHi, RegisterID srcLo, RegisterID newHi,
                 RegisterID newLo, int32_t offset, RegisterID base,
                 RegisterID index, int scale) {
    MOZ_ASSERT(srcHi == edx.code() && srcLo == eax.code());
    MOZ_ASSERT(newHi == ecx.code() && newLo == ebx.code());
    spew("cmpxchg8b  %s, " MEM_obs, "edx:eax",
         ADDR_obs(offset, base, index, scale));
    m_formatter.twoByteOp(OP2_CMPXCHGNB, offset, base, index, scale, 1);
  }

  // Comparisons:

  void cmpl_rr(RegisterID rhs, RegisterID lhs) {
    spew("cmpl       %s, %s", GPReg32Name(rhs), GPReg32Name(lhs));
    m_formatter.oneByteOp(OP_CMP_GvEv, rhs, lhs);
  }

  void cmpl_rm(RegisterID rhs, int32_t offset, RegisterID base) {
    spew("cmpl       %s, " MEM_ob, GPReg32Name(rhs), ADDR_ob(offset, base));
    m_formatter.oneByteOp(OP_CMP_EvGv, offset, base, rhs);
  }

  void cmpl_rm(RegisterID rhs, int32_t offset, RegisterID base,
               RegisterID index, int scale) {
    spew("cmpl       %s, " MEM_obs, GPReg32Name(rhs),
         ADDR_obs(offset, base, index, scale));
    m_formatter.oneByteOp(OP_CMP_EvGv, offset, base, index, scale, rhs);
  }

  void cmpl_mr(int32_t offset, RegisterID base, RegisterID lhs) {
    spew("cmpl       " MEM_ob ", %s", ADDR_ob(offset, base), GPReg32Name(lhs));
    m_formatter.oneByteOp(OP_CMP_GvEv, offset, base, lhs);
  }

  void cmpl_mr(const void* address, RegisterID lhs) {
    spew("cmpl       %p, %s", address, GPReg32Name(lhs));
    m_formatter.oneByteOp(OP_CMP_GvEv, address, lhs);
  }

  void cmpl_ir(int32_t rhs, RegisterID lhs) {
    if (rhs == 0) {
      testl_rr(lhs, lhs);
      return;
    }

    spew("cmpl       $0x%x, %s", uint32_t(rhs), GPReg32Name(lhs));
    if (CAN_SIGN_EXTEND_8_32(rhs)) {
      m_formatter.oneByteOp(OP_GROUP1_EvIb, lhs, GROUP1_OP_CMP);
      m_formatter.immediate8s(rhs);
    } else {
      if (lhs == rax) {
        m_formatter.oneByteOp(OP_CMP_EAXIv);
      } else {
        m_formatter.oneByteOp(OP_GROUP1_EvIz, lhs, GROUP1_OP_CMP);
      }
      m_formatter.immediate32(rhs);
    }
  }

  void cmpl_i32r(int32_t rhs, RegisterID lhs) {
    spew("cmpl       $0x%04x, %s", uint32_t(rhs), GPReg32Name(lhs));
    if (lhs == rax) {
      m_formatter.oneByteOp(OP_CMP_EAXIv);
    } else {
      m_formatter.oneByteOp(OP_GROUP1_EvIz, lhs, GROUP1_OP_CMP);
    }
    m_formatter.immediate32(rhs);
  }

  void cmpl_im(int32_t rhs, int32_t offset, RegisterID base) {
    spew("cmpl       $0x%x, " MEM_ob, uint32_t(rhs), ADDR_ob(offset, base));
    if (CAN_SIGN_EXTEND_8_32(rhs)) {
      m_formatter.oneByteOp(OP_GROUP1_EvIb, offset, base, GROUP1_OP_CMP);
      m_formatter.immediate8s(rhs);
    } else {
      m_formatter.oneByteOp(OP_GROUP1_EvIz, offset, base, GROUP1_OP_CMP);
      m_formatter.immediate32(rhs);
    }
  }

  void cmpb_rr(RegisterID rhs, RegisterID lhs) {
    spew("cmpb       %s, %s", GPReg8Name(rhs), GPReg8Name(lhs));
    m_formatter.oneByteOp(OP_CMP_GbEb, rhs, lhs);
  }

  void cmpb_rm(RegisterID rhs, int32_t offset, RegisterID base) {
    spew("cmpb       %s, " MEM_ob, GPReg8Name(rhs), ADDR_ob(offset, base));
    m_formatter.oneByteOp(OP_CMP_EbGb, offset, base, rhs);
  }

  void cmpb_rm(RegisterID rhs, int32_t offset, RegisterID base,
               RegisterID index, int scale) {
    spew("cmpb       %s, " MEM_obs, GPReg8Name(rhs),
         ADDR_obs(offset, base, index, scale));
    m_formatter.oneByteOp(OP_CMP_EbGb, offset, base, index, scale, rhs);
  }

  void cmpb_rm(RegisterID rhs, const void* addr) {
    spew("cmpb       %s, %p", GPReg8Name(rhs), addr);
    m_formatter.oneByteOp(OP_CMP_EbGb, addr, rhs);
  }

  void cmpb_ir(int32_t rhs, RegisterID lhs) {
    if (rhs == 0) {
      testb_rr(lhs, lhs);
      return;
    }

    spew("cmpb       $0x%x, %s", uint32_t(rhs), GPReg8Name(lhs));
    if (lhs == rax) {
      m_formatter.oneByteOp(OP_CMP_EAXIb);
    } else {
      m_formatter.oneByteOp(OP_GROUP1_EbIb, lhs, GROUP1_OP_CMP);
    }
    m_formatter.immediate8(rhs);
  }

  void cmpb_im(int32_t rhs, int32_t offset, RegisterID base) {
    spew("cmpb       $0x%x, " MEM_ob, uint32_t(rhs), ADDR_ob(offset, base));
    m_formatter.oneByteOp(OP_GROUP1_EbIb, offset, base, GROUP1_OP_CMP);
    m_formatter.immediate8(rhs);
  }

  void cmpb_im(int32_t rhs, int32_t offset, RegisterID base, RegisterID index,
               int scale) {
    spew("cmpb       $0x%x, " MEM_obs, uint32_t(rhs),
         ADDR_obs(offset, base, index, scale));
    m_formatter.oneByteOp(OP_GROUP1_EbIb, offset, base, index, scale,
                          GROUP1_OP_CMP);
    m_formatter.immediate8(rhs);
  }

  void cmpb_im(int32_t rhs, const void* addr) {
    spew("cmpb       $0x%x, %p", uint32_t(rhs), addr);
    m_formatter.oneByteOp(OP_GROUP1_EbIb, addr, GROUP1_OP_CMP);
    m_formatter.immediate8(rhs);
  }

  void cmpl_im(int32_t rhs, int32_t offset, RegisterID base, RegisterID index,
               int scale) {
    spew("cmpl       $0x%x, " MEM_obs, uint32_t(rhs),
         ADDR_obs(offset, base, index, scale));
    if (CAN_SIGN_EXTEND_8_32(rhs)) {
      m_formatter.oneByteOp(OP_GROUP1_EvIb, offset, base, index, scale,
                            GROUP1_OP_CMP);
      m_formatter.immediate8s(rhs);
    } else {
      m_formatter.oneByteOp(OP_GROUP1_EvIz, offset, base, index, scale,
                            GROUP1_OP_CMP);
      m_formatter.immediate32(rhs);
    }
  }

  [[nodiscard]] JmpSrc cmpl_im_disp32(int32_t rhs, int32_t offset,
                                      RegisterID base) {
    spew("cmpl       $0x%x, " MEM_o32b, uint32_t(rhs), ADDR_o32b(offset, base));
    JmpSrc r;
    if (CAN_SIGN_EXTEND_8_32(rhs)) {
      m_formatter.oneByteOp_disp32(OP_GROUP1_EvIb, offset, base, GROUP1_OP_CMP);
      r = JmpSrc(m_formatter.size());
      m_formatter.immediate8s(rhs);
    } else {
      m_formatter.oneByteOp_disp32(OP_GROUP1_EvIz, offset, base, GROUP1_OP_CMP);
      r = JmpSrc(m_formatter.size());
      m_formatter.immediate32(rhs);
    }
    return r;
  }

  [[nodiscard]] JmpSrc cmpl_im_disp32(int32_t rhs, const void* addr) {
    spew("cmpl       $0x%x, %p", uint32_t(rhs), addr);
    JmpSrc r;
    if (CAN_SIGN_EXTEND_8_32(rhs)) {
      m_formatter.oneByteOp_disp32(OP_GROUP1_EvIb, addr, GROUP1_OP_CMP);
      r = JmpSrc(m_formatter.size());
      m_formatter.immediate8s(rhs);
    } else {
      m_formatter.oneByteOp_disp32(OP_GROUP1_EvIz, addr, GROUP1_OP_CMP);
      r = JmpSrc(m_formatter.size());
      m_formatter.immediate32(rhs);
    }
    return r;
  }

  void cmpl_i32m(int32_t rhs, int32_t offset, RegisterID base) {
    spew("cmpl       $0x%04x, " MEM_ob, uint32_t(rhs), ADDR_ob(offset, base));
    m_formatter.oneByteOp(OP_GROUP1_EvIz, offset, base, GROUP1_OP_CMP);
    m_formatter.immediate32(rhs);
  }

  void cmpl_i32m(int32_t rhs, const void* addr) {
    spew("cmpl       $0x%04x, %p", uint32_t(rhs), addr);
    m_formatter.oneByteOp(OP_GROUP1_EvIz, addr, GROUP1_OP_CMP);
    m_formatter.immediate32(rhs);
  }

  void cmpl_rm(RegisterID rhs, const void* addr) {
    spew("cmpl       %s, %p", GPReg32Name(rhs), addr);
    m_formatter.oneByteOp(OP_CMP_EvGv, addr, rhs);
  }

  void cmpl_rm_disp32(RegisterID rhs, const void* addr) {
    spew("cmpl       %s, %p", GPReg32Name(rhs), addr);
    m_formatter.oneByteOp_disp32(OP_CMP_EvGv, addr, rhs);
  }

  void cmpl_im(int32_t rhs, const void* addr) {
    spew("cmpl       $0x%x, %p", uint32_t(rhs), addr);
    if (CAN_SIGN_EXTEND_8_32(rhs)) {
      m_formatter.oneByteOp(OP_GROUP1_EvIb, addr, GROUP1_OP_CMP);
      m_formatter.immediate8s(rhs);
    } else {
      m_formatter.oneByteOp(OP_GROUP1_EvIz, addr, GROUP1_OP_CMP);
      m_formatter.immediate32(rhs);
    }
  }

  void cmpw_rr(RegisterID rhs, RegisterID lhs) {
    spew("cmpw       %s, %s", GPReg16Name(rhs), GPReg16Name(lhs));
    m_formatter.prefix(PRE_OPERAND_SIZE);
    m_formatter.oneByteOp(OP_CMP_GvEv, rhs, lhs);
  }

  void cmpw_rm(RegisterID rhs, int32_t offset, RegisterID base,
               RegisterID index, int scale) {
    spew("cmpw       %s, " MEM_obs, GPReg16Name(rhs),
         ADDR_obs(offset, base, index, scale));
    m_formatter.prefix(PRE_OPERAND_SIZE);
    m_formatter.oneByteOp(OP_CMP_EvGv, offset, base, index, scale, rhs);
  }

  void cmpw_ir(int32_t rhs, RegisterID lhs) {
    if (rhs == 0) {
      testw_rr(lhs, lhs);
      return;
    }

    spew("cmpw       $0x%x, %s", uint32_t(rhs), GPReg16Name(lhs));
    if (CAN_SIGN_EXTEND_8_32(rhs)) {
      m_formatter.prefix(PRE_OPERAND_SIZE);
      m_formatter.oneByteOp(OP_GROUP1_EvIb, lhs, GROUP1_OP_CMP);
      m_formatter.immediate8s(rhs);
    } else {
      m_formatter.prefix(PRE_OPERAND_SIZE);
      m_formatter.oneByteOp(OP_GROUP1_EvIz, lhs, GROUP1_OP_CMP);
      m_formatter.immediate16(rhs);
    }
  }

  void cmpw_im(int32_t rhs, int32_t offset, RegisterID base) {
    spew("cmpw       $0x%x, " MEM_ob, uint32_t(rhs), ADDR_ob(offset, base));
    if (CAN_SIGN_EXTEND_8_32(rhs)) {
      m_formatter.prefix(PRE_OPERAND_SIZE);
      m_formatter.oneByteOp(OP_GROUP1_EvIb, offset, base, GROUP1_OP_CMP);
      m_formatter.immediate8s(rhs);
    } else {
      m_formatter.prefix(PRE_OPERAND_SIZE);
      m_formatter.oneByteOp(OP_GROUP1_EvIz, offset, base, GROUP1_OP_CMP);
      m_formatter.immediate16(rhs);
    }
  }

  void cmpw_im(int32_t imm, int32_t offset, RegisterID base, RegisterID index,
               int scale) {
    spew("cmpw       $%d, " MEM_obs, imm, ADDR_obs(offset, base, index, scale));
    if (CAN_SIGN_EXTEND_8_32(imm)) {
      m_formatter.prefix(PRE_OPERAND_SIZE);
      m_formatter.oneByteOp(OP_GROUP1_EvIb, offset, base, index, scale,
                            GROUP1_OP_CMP);
      m_formatter.immediate8s(imm);
    } else {
      m_formatter.prefix(PRE_OPERAND_SIZE);
      m_formatter.oneByteOp(OP_GROUP1_EvIz, offset, base, index, scale,
                            GROUP1_OP_CMP);
      m_formatter.immediate16(imm);
    }
  }

  void cmpw_im(int32_t rhs, const void* addr) {
    spew("cmpw       $0x%x, %p", uint32_t(rhs), addr);
    if (CAN_SIGN_EXTEND_8_32(rhs)) {
      m_formatter.prefix(PRE_OPERAND_SIZE);
      m_formatter.oneByteOp(OP_GROUP1_EvIb, addr, GROUP1_OP_CMP);
      m_formatter.immediate8s(rhs);
    } else {
      m_formatter.prefix(PRE_OPERAND_SIZE);
      m_formatter.oneByteOp(OP_GROUP1_EvIz, addr, GROUP1_OP_CMP);
      m_formatter.immediate16(rhs);
    }
  }

  void testl_rr(RegisterID rhs, RegisterID lhs) {
    spew("testl      %s, %s", GPReg32Name(rhs), GPReg32Name(lhs));
    m_formatter.oneByteOp(OP_TEST_EvGv, lhs, rhs);
  }

  void testb_rr(RegisterID rhs, RegisterID lhs) {
    spew("testb      %s, %s", GPReg8Name(rhs), GPReg8Name(lhs));
    m_formatter.oneByteOp(OP_TEST_EbGb, lhs, rhs);
  }

  void testl_ir(int32_t rhs, RegisterID lhs) {
    // If the mask fits in an 8-bit immediate, we can use testb with an
    // 8-bit subreg.
    if (CAN_ZERO_EXTEND_8_32(rhs) && HasSubregL(lhs)) {
      testb_ir(rhs, lhs);
      return;
    }
    // If the mask is a subset of 0xff00, we can use testb with an h reg, if
    // one happens to be available.
    if (CAN_ZERO_EXTEND_8H_32(rhs) && HasSubregH(lhs)) {
      testb_ir_norex(rhs >> 8, GetSubregH(lhs));
      return;
    }
    spew("testl      $0x%x, %s", uint32_t(rhs), GPReg32Name(lhs));
    if (lhs == rax) {
      m_formatter.oneByteOp(OP_TEST_EAXIv);
    } else {
      m_formatter.oneByteOp(OP_GROUP3_EvIz, lhs, GROUP3_OP_TEST);
    }
    m_formatter.immediate32(rhs);
  }

  void testl_i32m(int32_t rhs, int32_t offset, RegisterID base) {
    spew("testl      $0x%x, " MEM_ob, uint32_t(rhs), ADDR_ob(offset, base));
    m_formatter.oneByteOp(OP_GROUP3_EvIz, offset, base, GROUP3_OP_TEST);
    m_formatter.immediate32(rhs);
  }

  void testl_i32m(int32_t rhs, const void* addr) {
    spew("testl      $0x%x, %p", uint32_t(rhs), addr);
    m_formatter.oneByteOp(OP_GROUP3_EvIz, addr, GROUP3_OP_TEST);
    m_formatter.immediate32(rhs);
  }

  void testb_im(int32_t rhs, int32_t offset, RegisterID base) {
    spew("testb      $0x%x, " MEM_ob, uint32_t(rhs), ADDR_ob(offset, base));
    m_formatter.oneByteOp(OP_GROUP3_EbIb, offset, base, GROUP3_OP_TEST);
    m_formatter.immediate8(rhs);
  }

  void testb_im(int32_t rhs, int32_t offset, RegisterID base, RegisterID index,
                int scale) {
    spew("testb      $0x%x, " MEM_obs, uint32_t(rhs),
         ADDR_obs(offset, base, index, scale));
    m_formatter.oneByteOp(OP_GROUP3_EbIb, offset, base, index, scale,
                          GROUP3_OP_TEST);
    m_formatter.immediate8(rhs);
  }

  void testl_i32m(int32_t rhs, int32_t offset, RegisterID base,
                  RegisterID index, int scale) {
    spew("testl      $0x%4x, " MEM_obs, uint32_t(rhs),
         ADDR_obs(offset, base, index, scale));
    m_formatter.oneByteOp(OP_GROUP3_EvIz, offset, base, index, scale,
                          GROUP3_OP_TEST);
    m_formatter.immediate32(rhs);
  }

  void testw_rr(RegisterID rhs, RegisterID lhs) {
    spew("testw      %s, %s", GPReg16Name(rhs), GPReg16Name(lhs));
    m_formatter.prefix(PRE_OPERAND_SIZE);
    m_formatter.oneByteOp(OP_TEST_EvGv, lhs, rhs);
  }

  void testb_ir(int32_t rhs, RegisterID lhs) {
    spew("testb      $0x%x, %s", uint32_t(rhs), GPReg8Name(lhs));
    if (lhs == rax) {
      m_formatter.oneByteOp8(OP_TEST_EAXIb);
    } else {
      m_formatter.oneByteOp8(OP_GROUP3_EbIb, lhs, GROUP3_OP_TEST);
    }
    m_formatter.immediate8(rhs);
  }

  // Like testb_ir, but never emits a REX prefix. This may be used to
  // reference ah..bh.
  void testb_ir_norex(int32_t rhs, HRegisterID lhs) {
    spew("testb      $0x%x, %s", uint32_t(rhs), HRegName8(lhs));
    m_formatter.oneByteOp8_norex(OP_GROUP3_EbIb, lhs, GROUP3_OP_TEST);
    m_formatter.immediate8(rhs);
  }

  void setCC_r(Condition cond, RegisterID lhs) {
    spew("set%s      %s", CCName(cond), GPReg8Name(lhs));
    m_formatter.twoByteOp8(setccOpcode(cond), lhs, (GroupOpcodeID)0);
  }

  void sete_r(RegisterID dst) { setCC_r(ConditionE, dst); }

  void setz_r(RegisterID dst) { sete_r(dst); }

  void setne_r(RegisterID dst) { setCC_r(ConditionNE, dst); }

  void setnz_r(RegisterID dst) { setne_r(dst); }

  // Various move ops:

  void cdq() {
    spew("cdq        ");
    m_formatter.oneByteOp(OP_CDQ);
  }

  void xchgb_rm(RegisterID src, int32_t offset, RegisterID base) {
    spew("xchgb      %s, " MEM_ob, GPReg8Name(src), ADDR_ob(offset, base));
    m_formatter.oneByteOp8(OP_XCHG_GbEb, offset, base, src);
  }
  void xchgb_rm(RegisterID src, int32_t offset, RegisterID base,
                RegisterID index, int scale) {
    spew("xchgb      %s, " MEM_obs, GPReg8Name(src),
         ADDR_obs(offset, base, index, scale));
    m_formatter.oneByteOp8(OP_XCHG_GbEb, offset, base, index, scale, src);
  }

  void xchgw_rm(RegisterID src, int32_t offset, RegisterID base) {
    spew("xchgw      %s, " MEM_ob, GPReg16Name(src), ADDR_ob(offset, base));
    m_formatter.prefix(PRE_OPERAND_SIZE);
    m_formatter.oneByteOp(OP_XCHG_GvEv, offset, base, src);
  }
  void xchgw_rm(RegisterID src, int32_t offset, RegisterID base,
                RegisterID index, int scale) {
    spew("xchgw      %s, " MEM_obs, GPReg16Name(src),
         ADDR_obs(offset, base, index, scale));
    m_formatter.prefix(PRE_OPERAND_SIZE);
    m_formatter.oneByteOp(OP_XCHG_GvEv, offset, base, index, scale, src);
  }

  void xchgl_rr(RegisterID src, RegisterID dst) {
    spew("xchgl      %s, %s", GPReg32Name(src), GPReg32Name(dst));
    m_formatter.oneByteOp(OP_XCHG_GvEv, src, dst);
  }
  void xchgl_rm(RegisterID src, int32_t offset, RegisterID base) {
    spew("xchgl      %s, " MEM_ob, GPReg32Name(src), ADDR_ob(offset, base));
    m_formatter.oneByteOp(OP_XCHG_GvEv, offset, base, src);
  }
  void xchgl_rm(RegisterID src, int32_t offset, RegisterID base,
                RegisterID index, int scale) {
    spew("xchgl      %s, " MEM_obs, GPReg32Name(src),
         ADDR_obs(offset, base, index, scale));
    m_formatter.oneByteOp(OP_XCHG_GvEv, offset, base, index, scale, src);
  }

  void cmovCCl_rr(Condition cond, RegisterID src, RegisterID dst) {
    spew("cmov%s     %s, %s", CCName(cond), GPReg32Name(src), GPReg32Name(dst));
    m_formatter.twoByteOp(cmovccOpcode(cond), src, dst);
  }
  void cmovCCl_mr(Condition cond, int32_t offset, RegisterID base,
                  RegisterID dst) {
    spew("cmov%s     " MEM_ob ", %s", CCName(cond), ADDR_ob(offset, base),
         GPReg32Name(dst));
    m_formatter.twoByteOp(cmovccOpcode(cond), offset, base, dst);
  }
  void cmovCCl_mr(Condition cond, int32_t offset, RegisterID base,
                  RegisterID index, int scale, RegisterID dst) {
    spew("cmov%s     " MEM_obs ", %s", CCName(cond),
         ADDR_obs(offset, base, index, scale), GPReg32Name(dst));
    m_formatter.twoByteOp(cmovccOpcode(cond), offset, base, index, scale, dst);
  }

  void movl_rr(RegisterID src, RegisterID dst) {
    spew("movl       %s, %s", GPReg32Name(src), GPReg32Name(dst));
    m_formatter.oneByteOp(OP_MOV_GvEv, src, dst);
  }

  void movw_rm(RegisterID src, int32_t offset, RegisterID base) {
    spew("movw       %s, " MEM_ob, GPReg16Name(src), ADDR_ob(offset, base));
    m_formatter.prefix(PRE_OPERAND_SIZE);
    m_formatter.oneByteOp(OP_MOV_EvGv, offset, base, src);
  }

  void movw_rm_disp32(RegisterID src, int32_t offset, RegisterID base) {
    spew("movw       %s, " MEM_o32b, GPReg16Name(src), ADDR_o32b(offset, base));
    m_formatter.prefix(PRE_OPERAND_SIZE);
    m_formatter.oneByteOp_disp32(OP_MOV_EvGv, offset, base, src);
  }

  void movw_rm(RegisterID src, int32_t offset, RegisterID base,
               RegisterID index, int scale) {
    spew("movw       %s, " MEM_obs, GPReg16Name(src),
         ADDR_obs(offset, base, index, scale));
    m_formatter.prefix(PRE_OPERAND_SIZE);
    m_formatter.oneByteOp(OP_MOV_EvGv, offset, base, index, scale, src);
  }

  void movw_rm(RegisterID src, const void* addr) {
    spew("movw       %s, %p", GPReg16Name(src), addr);
    m_formatter.prefix(PRE_OPERAND_SIZE);
    m_formatter.oneByteOp_disp32(OP_MOV_EvGv, addr, src);
  }

  void movl_rm(RegisterID src, int32_t offset, RegisterID base) {
    spew("movl       %s, " MEM_ob, GPReg32Name(src), ADDR_ob(offset, base));
    m_formatter.oneByteOp(OP_MOV_EvGv, offset, base, src);
  }

  void movl_rm_disp32(RegisterID src, int32_t offset, RegisterID base) {
    spew("movl       %s, " MEM_o32b, GPReg32Name(src), ADDR_o32b(offset, base));
    m_formatter.oneByteOp_disp32(OP_MOV_EvGv, offset, base, src);
  }

  void movl_rm(RegisterID src, int32_t offset, RegisterID base,
               RegisterID index, int scale) {
    spew("movl       %s, " MEM_obs, GPReg32Name(src),
         ADDR_obs(offset, base, index, scale));
    m_formatter.oneByteOp(OP_MOV_EvGv, offset, base, index, scale, src);
  }

  void movl_mEAX(const void* addr) {
#ifdef JS_CODEGEN_X64
    if (IsAddressImmediate(addr)) {
      movl_mr(addr, rax);
      return;
    }
#endif

#ifdef JS_CODEGEN_X64
    spew("movabs     %p, %%eax", addr);
#else
    spew("movl       %p, %%eax", addr);
#endif
    m_formatter.oneByteOp(OP_MOV_EAXOv);
#ifdef JS_CODEGEN_X64
    m_formatter.immediate64(reinterpret_cast<int64_t>(addr));
#else
    m_formatter.immediate32(reinterpret_cast<int32_t>(addr));
#endif
  }

  void movl_mr(int32_t offset, RegisterID base, RegisterID dst) {
    spew("movl       " MEM_ob ", %s", ADDR_ob(offset, base), GPReg32Name(dst));
    m_formatter.oneByteOp(OP_MOV_GvEv, offset, base, dst);
  }

  void movl_mr_disp32(int32_t offset, RegisterID base, RegisterID dst) {
    spew("movl       " MEM_o32b ", %s", ADDR_o32b(offset, base),
         GPReg32Name(dst));
    m_formatter.oneByteOp_disp32(OP_MOV_GvEv, offset, base, dst);
  }

  void movl_mr(const void* base, RegisterID index, int scale, RegisterID dst) {
    int32_t disp = AddressImmediate(base);

    spew("movl       " MEM_os ", %s", ADDR_os(disp, index, scale),
         GPReg32Name(dst));
    m_formatter.oneByteOp_disp32(OP_MOV_GvEv, disp, index, scale, dst);
  }

  void movl_mr(int32_t offset, RegisterID base, RegisterID index, int scale,
               RegisterID dst) {
    spew("movl       " MEM_obs ", %s", ADDR_obs(offset, base, index, scale),
         GPReg32Name(dst));
    m_formatter.oneByteOp(OP_MOV_GvEv, offset, base, index, scale, dst);
  }

  void movl_mr(const void* addr, RegisterID dst) {
    if (dst == rax
#ifdef JS_CODEGEN_X64
        && !IsAddressImmediate(addr)
#endif
    ) {
      movl_mEAX(addr);
      return;
    }

    spew("movl       %p, %s", addr, GPReg32Name(dst));
    m_formatter.oneByteOp(OP_MOV_GvEv, addr, dst);
  }

  void movl_i32r(int32_t imm, RegisterID dst) {
    spew("movl       $0x%x, %s", uint32_t(imm), GPReg32Name(dst));
    m_formatter.oneByteOp(OP_MOV_EAXIv, dst);
    m_formatter.immediate32(imm);
  }

  void movb_ir(int32_t imm, RegisterID reg) {
    spew("movb       $0x%x, %s", uint32_t(imm), GPReg8Name(reg));
    m_formatter.oneByteOp8(OP_MOV_EbIb, reg);
    m_formatter.immediate8(imm);
  }

  void movb_im(int32_t imm, int32_t offset, RegisterID base) {
    spew("movb       $0x%x, " MEM_ob, uint32_t(imm), ADDR_ob(offset, base));
    m_formatter.oneByteOp(OP_GROUP11_EvIb, offset, base, GROUP11_MOV);
    m_formatter.immediate8(imm);
  }

  void movb_im(int32_t imm, int32_t offset, RegisterID base, RegisterID index,
               int scale) {
    spew("movb       $0x%x, " MEM_obs, uint32_t(imm),
         ADDR_obs(offset, base, index, scale));
    m_formatter.oneByteOp(OP_GROUP11_EvIb, offset, base, index, scale,
                          GROUP11_MOV);
    m_formatter.immediate8(imm);
  }

  void movb_im(int32_t imm, const void* addr) {
    spew("movb       $%d, %p", imm, addr);
    m_formatter.oneByteOp_disp32(OP_GROUP11_EvIb, addr, GROUP11_MOV);
    m_formatter.immediate8(imm);
  }

  void movw_im(int32_t imm, int32_t offset, RegisterID base) {
    spew("movw       $0x%x, " MEM_ob, uint32_t(imm), ADDR_ob(offset, base));
    m_formatter.prefix(PRE_OPERAND_SIZE);
    m_formatter.oneByteOp(OP_GROUP11_EvIz, offset, base, GROUP11_MOV);
    m_formatter.immediate16(imm);
  }

  void movw_im(int32_t imm, const void* addr) {
    spew("movw       $%d, %p", imm, addr);
    m_formatter.prefix(PRE_OPERAND_SIZE);
    m_formatter.oneByteOp_disp32(OP_GROUP11_EvIz, addr, GROUP11_MOV);
    m_formatter.immediate16(imm);
  }

  void movl_i32m(int32_t imm, int32_t offset, RegisterID base) {
    spew("movl       $0x%x, " MEM_ob, uint32_t(imm), ADDR_ob(offset, base));
    m_formatter.oneByteOp(OP_GROUP11_EvIz, offset, base, GROUP11_MOV);
    m_formatter.immediate32(imm);
  }

  void movw_im(int32_t imm, int32_t offset, RegisterID base, RegisterID index,
               int scale) {
    spew("movw       $0x%x, " MEM_obs, uint32_t(imm),
         ADDR_obs(offset, base, index, scale));
    m_formatter.prefix(PRE_OPERAND_SIZE);
    m_formatter.oneByteOp(OP_GROUP11_EvIz, offset, base, index, scale,
                          GROUP11_MOV);
    m_formatter.immediate16(imm);
  }

  void movl_i32m(int32_t imm, int32_t offset, RegisterID base, RegisterID index,
                 int scale) {
    spew("movl       $0x%x, " MEM_obs, uint32_t(imm),
         ADDR_obs(offset, base, index, scale));
    m_formatter.oneByteOp(OP_GROUP11_EvIz, offset, base, index, scale,
                          GROUP11_MOV);
    m_formatter.immediate32(imm);
  }

  void movl_EAXm(const void* addr) {
#ifdef JS_CODEGEN_X64
    if (IsAddressImmediate(addr)) {
      movl_rm(rax, addr);
      return;
    }
#endif

    spew("movl       %%eax, %p", addr);
    m_formatter.oneByteOp(OP_MOV_OvEAX);
#ifdef JS_CODEGEN_X64
    m_formatter.immediate64(reinterpret_cast<int64_t>(addr));
#else
    m_formatter.immediate32(reinterpret_cast<int32_t>(addr));
#endif
  }

  void vmovq_rm(XMMRegisterID src, int32_t offset, RegisterID base) {
    // vmovq_rm can be encoded either as a true vmovq or as a vmovd with a
    // REX prefix modifying it to be 64-bit. We choose the vmovq encoding
    // because it's smaller (when it doesn't need a REX prefix for other
    // reasons) and because it works on 32-bit x86 too.
    twoByteOpSimd("vmovq", VEX_PD, OP2_MOVQ_WdVd, offset, base, invalid_xmm,
                  src);
  }

  void vmovq_rm_disp32(XMMRegisterID src, int32_t offset, RegisterID base) {
    twoByteOpSimd_disp32("vmovq", VEX_PD, OP2_MOVQ_WdVd, offset, base,
                         invalid_xmm, src);
  }

  void vmovq_rm(XMMRegisterID src, int32_t offset, RegisterID base,
                RegisterID index, int scale) {
    twoByteOpSimd("vmovq", VEX_PD, OP2_MOVQ_WdVd, offset, base, index, scale,
                  invalid_xmm, src);
  }

  void vmovq_rm(XMMRegisterID src, const void* addr) {
    twoByteOpSimd("vmovq", VEX_PD, OP2_MOVQ_WdVd, addr, invalid_xmm, src);
  }

  void vmovq_mr(int32_t offset, RegisterID base, XMMRegisterID dst) {
    // vmovq_mr can be encoded either as a true vmovq or as a vmovd with a
    // REX prefix modifying it to be 64-bit. We choose the vmovq encoding
    // because it's smaller (when it doesn't need a REX prefix for other
    // reasons) and because it works on 32-bit x86 too.
    twoByteOpSimd("vmovq", VEX_SS, OP2_MOVQ_VdWd, offset, base, invalid_xmm,
                  dst);
  }

  void vmovq_mr_disp32(int32_t offset, RegisterID base, XMMRegisterID dst) {
    twoByteOpSimd_disp32("vmovq", VEX_SS, OP2_MOVQ_VdWd, offset, base,
                         invalid_xmm, dst);
  }

  void vmovq_mr(int32_t offset, RegisterID base, RegisterID index,
                int32_t scale, XMMRegisterID dst) {
    twoByteOpSimd("vmovq", VEX_SS, OP2_MOVQ_VdWd, offset, base, index, scale,
                  invalid_xmm, dst);
  }

  void vmovq_mr(const void* addr, XMMRegisterID dst) {
    twoByteOpSimd("vmovq", VEX_SS, OP2_MOVQ_VdWd, addr, invalid_xmm, dst);
  }

  void movl_rm(RegisterID src, const void* addr) {
    if (src == rax
#ifdef JS_CODEGEN_X64
        && !IsAddressImmediate(addr)
#endif
    ) {
      movl_EAXm(addr);
      return;
    }

    spew("movl       %s, %p", GPReg32Name(src), addr);
    m_formatter.oneByteOp(OP_MOV_EvGv, addr, src);
  }

  void movl_i32m(int32_t imm, const void* addr) {
    spew("movl       $%d, %p", imm, addr);
    m_formatter.oneByteOp(OP_GROUP11_EvIz, addr, GROUP11_MOV);
    m_formatter.immediate32(imm);
  }

  void movb_rm(RegisterID src, int32_t offset, RegisterID base) {
    spew("movb       %s, " MEM_ob, GPReg8Name(src), ADDR_ob(offset, base));
    m_formatter.oneByteOp8(OP_MOV_EbGv, offset, base, src);
  }

  void movb_rm_disp32(RegisterID src, int32_t offset, RegisterID base) {
    spew("movb       %s, " MEM_o32b, GPReg8Name(src), ADDR_o32b(offset, base));
    m_formatter.oneByteOp8_disp32(OP_MOV_EbGv, offset, base, src);
  }

  void movb_rm(RegisterID src, int32_t offset, RegisterID base,
               RegisterID index, int scale) {
    spew("movb       %s, " MEM_obs, GPReg8Name(src),
         ADDR_obs(offset, base, index, scale));
    m_formatter.oneByteOp8(OP_MOV_EbGv, offset, base, index, scale, src);
  }

  void movb_rm(RegisterID src, const void* addr) {
    spew("movb       %s, %p", GPReg8Name(src), addr);
    m_formatter.oneByteOp8(OP_MOV_EbGv, addr, src);
  }

  void movb_mr(int32_t offset, RegisterID base, RegisterID dst) {
    spew("movb       " MEM_ob ", %s", ADDR_ob(offset, base), GPReg8Name(dst));
    m_formatter.oneByteOp(OP_MOV_GvEb, offset, base, dst);
  }

  void movb_mr(int32_t offset, RegisterID base, RegisterID index, int scale,
               RegisterID dst) {
    spew("movb       " MEM_obs ", %s", ADDR_obs(offset, base, index, scale),
         GPReg8Name(dst));
    m_formatter.oneByteOp(OP_MOV_GvEb, offset, base, index, scale, dst);
  }

  void movzbl_mr(int32_t offset, RegisterID base, RegisterID dst) {
    spew("movzbl     " MEM_ob ", %s", ADDR_ob(offset, base), GPReg32Name(dst));
    m_formatter.twoByteOp(OP2_MOVZX_GvEb, offset, base, dst);
  }

  void movzbl_mr_disp32(int32_t offset, RegisterID base, RegisterID dst) {
    spew("movzbl     " MEM_o32b ", %s", ADDR_o32b(offset, base),
         GPReg32Name(dst));
    m_formatter.twoByteOp_disp32(OP2_MOVZX_GvEb, offset, base, dst);
  }

  void movzbl_mr(int32_t offset, RegisterID base, RegisterID index, int scale,
                 RegisterID dst) {
    spew("movzbl     " MEM_obs ", %s", ADDR_obs(offset, base, index, scale),
         GPReg32Name(dst));
    m_formatter.twoByteOp(OP2_MOVZX_GvEb, offset, base, index, scale, dst);
  }

  void movzbl_mr(const void* addr, RegisterID dst) {
    spew("movzbl     %p, %s", addr, GPReg32Name(dst));
    m_formatter.twoByteOp(OP2_MOVZX_GvEb, addr, dst);
  }

  void movsbl_rr(RegisterID src, RegisterID dst) {
    spew("movsbl     %s, %s", GPReg8Name(src), GPReg32Name(dst));
    m_formatter.twoByteOp8_movx(OP2_MOVSX_GvEb, src, dst);
  }

  void movsbl_mr(int32_t offset, RegisterID base, RegisterID dst) {
    spew("movsbl     " MEM_ob ", %s", ADDR_ob(offset, base), GPReg32Name(dst));
    m_formatter.twoByteOp(OP2_MOVSX_GvEb, offset, base, dst);
  }

  void movsbl_mr_disp32(int32_t offset, RegisterID base, RegisterID dst) {
    spew("movsbl     " MEM_o32b ", %s", ADDR_o32b(offset, base),
         GPReg32Name(dst));
    m_formatter.twoByteOp_disp32(OP2_MOVSX_GvEb, offset, base, dst);
  }

  void movsbl_mr(int32_t offset, RegisterID base, RegisterID index, int scale,
                 RegisterID dst) {
    spew("movsbl     " MEM_obs ", %s", ADDR_obs(offset, base, index, scale),
         GPReg32Name(dst));
    m_formatter.twoByteOp(OP2_MOVSX_GvEb, offset, base, index, scale, dst);
  }

  void movsbl_mr(const void* addr, RegisterID dst) {
    spew("movsbl     %p, %s", addr, GPReg32Name(dst));
    m_formatter.twoByteOp(OP2_MOVSX_GvEb, addr, dst);
  }

  void movzwl_rr(RegisterID src, RegisterID dst) {
    spew("movzwl     %s, %s", GPReg16Name(src), GPReg32Name(dst));
    m_formatter.twoByteOp(OP2_MOVZX_GvEw, src, dst);
  }

  void movzwl_mr(int32_t offset, RegisterID base, RegisterID dst) {
    spew("movzwl     " MEM_ob ", %s", ADDR_ob(offset, base), GPReg32Name(dst));
    m_formatter.twoByteOp(OP2_MOVZX_GvEw, offset, base, dst);
  }

  void movzwl_mr_disp32(int32_t offset, RegisterID base, RegisterID dst) {
    spew("movzwl     " MEM_o32b ", %s", ADDR_o32b(offset, base),
         GPReg32Name(dst));
    m_formatter.twoByteOp_disp32(OP2_MOVZX_GvEw, offset, base, dst);
  }

  void movzwl_mr(int32_t offset, RegisterID base, RegisterID index, int scale,
                 RegisterID dst) {
    spew("movzwl     " MEM_obs ", %s", ADDR_obs(offset, base, index, scale),
         GPReg32Name(dst));
    m_formatter.twoByteOp(OP2_MOVZX_GvEw, offset, base, index, scale, dst);
  }

  void movzwl_mr(const void* addr, RegisterID dst) {
    spew("movzwl     %p, %s", addr, GPReg32Name(dst));
    m_formatter.twoByteOp(OP2_MOVZX_GvEw, addr, dst);
  }

  void movswl_rr(RegisterID src, RegisterID dst) {
    spew("movswl     %s, %s", GPReg16Name(src), GPReg32Name(dst));
    m_formatter.twoByteOp(OP2_MOVSX_GvEw, src, dst);
  }

  void movswl_mr(int32_t offset, RegisterID base, RegisterID dst) {
    spew("movswl     " MEM_ob ", %s", ADDR_ob(offset, base), GPReg32Name(dst));
    m_formatter.twoByteOp(OP2_MOVSX_GvEw, offset, base, dst);
  }

  void movswl_mr_disp32(int32_t offset, RegisterID base, RegisterID dst) {
    spew("movswl     " MEM_o32b ", %s", ADDR_o32b(offset, base),
         GPReg32Name(dst));
    m_formatter.twoByteOp_disp32(OP2_MOVSX_GvEw, offset, base, dst);
  }

  void movswl_mr(int32_t offset, RegisterID base, RegisterID index, int scale,
                 RegisterID dst) {
    spew("movswl     " MEM_obs ", %s", ADDR_obs(offset, base, index, scale),
         GPReg32Name(dst));
    m_formatter.twoByteOp(OP2_MOVSX_GvEw, offset, base, index, scale, dst);
  }

  void movswl_mr(const void* addr, RegisterID dst) {
    spew("movswl     %p, %s", addr, GPReg32Name(dst));
    m_formatter.twoByteOp(OP2_MOVSX_GvEw, addr, dst);
  }

  void movzbl_rr(RegisterID src, RegisterID dst) {
    spew("movzbl     %s, %s", GPReg8Name(src), GPReg32Name(dst));
    m_formatter.twoByteOp8_movx(OP2_MOVZX_GvEb, src, dst);
  }

  void leal_mr(int32_t offset, RegisterID base, RegisterID index, int scale,
               RegisterID dst) {
    spew("leal       " MEM_obs ", %s", ADDR_obs(offset, base, index, scale),
         GPReg32Name(dst));
    m_formatter.oneByteOp(OP_LEA, offset, base, index, scale, dst);
  }

  void leal_mr(int32_t offset, RegisterID base, RegisterID dst) {
    spew("leal       " MEM_ob ", %s", ADDR_ob(offset, base), GPReg32Name(dst));
    m_formatter.oneByteOp(OP_LEA, offset, base, dst);
  }

  // Flow control:

  [[nodiscard]] JmpSrc call() {
    m_formatter.oneByteOp(OP_CALL_rel32);
    JmpSrc r = m_formatter.immediateRel32();
    spew("call       .Lfrom%d", r.offset());
    return r;
  }

  void call_r(RegisterID dst) {
    m_formatter.oneByteOp(OP_GROUP5_Ev, dst, GROUP5_OP_CALLN);
    spew("call       *%s", GPRegName(dst));
  }

  void call_m(int32_t offset, RegisterID base) {
    spew("call       *" MEM_ob, ADDR_ob(offset, base));
    m_formatter.oneByteOp(OP_GROUP5_Ev, offset, base, GROUP5_OP_CALLN);
  }

  // Comparison of EAX against a 32-bit immediate. The immediate is patched
  // in as if it were a jump target. The intention is to toggle the first
  // byte of the instruction between a CMP and a JMP to produce a pseudo-NOP.
  [[nodiscard]] JmpSrc cmp_eax() {
    m_formatter.oneByteOp(OP_CMP_EAXIv);
    JmpSrc r = m_formatter.immediateRel32();
    spew("cmpl       %%eax, .Lfrom%d", r.offset());
    return r;
  }

  void jmp_i(JmpDst dst) {
    int32_t diff = dst.offset() - m_formatter.size();
    spew("jmp        .Llabel%d", dst.offset());

    // The jump immediate is an offset from the end of the jump instruction.
    // A jump instruction is either 1 byte opcode and 1 byte offset, or 1
    // byte opcode and 4 bytes offset.
    if (CAN_SIGN_EXTEND_8_32(diff - 2)) {
      m_formatter.oneByteOp(OP_JMP_rel8);
      m_formatter.immediate8s(diff - 2);
    } else {
      m_formatter.oneByteOp(OP_JMP_rel32);
      m_formatter.immediate32(diff - 5);
    }
  }
  [[nodiscard]] JmpSrc jmp() {
    m_formatter.oneByteOp(OP_JMP_rel32);
    JmpSrc r = m_formatter.immediateRel32();
    spew("jmp        .Lfrom%d", r.offset());
    return r;
  }

  void jmp_r(RegisterID dst) {
    spew("jmp        *%s", GPRegName(dst));
    m_formatter.oneByteOp(OP_GROUP5_Ev, dst, GROUP5_OP_JMPN);
  }

  void jmp_m(int32_t offset, RegisterID base) {
    spew("jmp        *" MEM_ob, ADDR_ob(offset, base));
    m_formatter.oneByteOp(OP_GROUP5_Ev, offset, base, GROUP5_OP_JMPN);
  }

  void jmp_m(int32_t offset, RegisterID base, RegisterID index, int scale) {
    spew("jmp        *" MEM_obs, ADDR_obs(offset, base, index, scale));
    m_formatter.oneByteOp(OP_GROUP5_Ev, offset, base, index, scale,
                          GROUP5_OP_JMPN);
  }

  void jCC_i(Condition cond, JmpDst dst) {
    int32_t diff = dst.offset() - m_formatter.size();
    spew("j%s        .Llabel%d", CCName(cond), dst.offset());

    // The jump immediate is an offset from the end of the jump instruction.
    // A conditional jump instruction is either 1 byte opcode and 1 byte
    // offset, or 2 bytes opcode and 4 bytes offset.
    if (CAN_SIGN_EXTEND_8_32(diff - 2)) {
      m_formatter.oneByteOp(jccRel8(cond));
      m_formatter.immediate8s(diff - 2);
    } else {
      m_formatter.twoByteOp(jccRel32(cond));
      m_formatter.immediate32(diff - 6);
    }
  }

  [[nodiscard]] JmpSrc jCC(Condition cond) {
    m_formatter.twoByteOp(jccRel32(cond));
    JmpSrc r = m_formatter.immediateRel32();
    spew("j%s        .Lfrom%d", CCName(cond), r.offset());
    return r;
  }

  // SSE operations:

  void vpcmpeqb_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpcmpeqb", VEX_PD, OP2_PCMPEQB_VdqWdq, src1, src0, dst);
  }
  void vpcmpeqb_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                   XMMRegisterID dst) {
    twoByteOpSimd("vpcmpeqb", VEX_PD, OP2_PCMPEQB_VdqWdq, offset, base, src0,
                  dst);
  }
  void vpcmpeqb_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpcmpeqb", VEX_PD, OP2_PCMPEQB_VdqWdq, address, src0, dst);
  }

  void vpcmpgtb_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpcmpgtb", VEX_PD, OP2_PCMPGTB_VdqWdq, src1, src0, dst);
  }
  void vpcmpgtb_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                   XMMRegisterID dst) {
    twoByteOpSimd("vpcmpgtb", VEX_PD, OP2_PCMPGTB_VdqWdq, offset, base, src0,
                  dst);
  }
  void vpcmpgtb_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpcmpgtb", VEX_PD, OP2_PCMPGTB_VdqWdq, address, src0, dst);
  }

  void vpcmpeqw_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpcmpeqw", VEX_PD, OP2_PCMPEQW_VdqWdq, src1, src0, dst);
  }
  void vpcmpeqw_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                   XMMRegisterID dst) {
    twoByteOpSimd("vpcmpeqw", VEX_PD, OP2_PCMPEQW_VdqWdq, offset, base, src0,
                  dst);
  }
  void vpcmpeqw_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpcmpeqw", VEX_PD, OP2_PCMPEQW_VdqWdq, address, src0, dst);
  }

  void vpcmpgtw_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpcmpgtw", VEX_PD, OP2_PCMPGTW_VdqWdq, src1, src0, dst);
  }
  void vpcmpgtw_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                   XMMRegisterID dst) {
    twoByteOpSimd("vpcmpgtw", VEX_PD, OP2_PCMPGTW_VdqWdq, offset, base, src0,
                  dst);
  }
  void vpcmpgtw_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpcmpgtw", VEX_PD, OP2_PCMPGTW_VdqWdq, address, src0, dst);
  }

  void vpcmpeqd_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpcmpeqd", VEX_PD, OP2_PCMPEQD_VdqWdq, src1, src0, dst);
  }
  void vpcmpeqd_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                   XMMRegisterID dst) {
    twoByteOpSimd("vpcmpeqd", VEX_PD, OP2_PCMPEQD_VdqWdq, offset, base, src0,
                  dst);
  }
  void vpcmpeqd_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpcmpeqd", VEX_PD, OP2_PCMPEQD_VdqWdq, address, src0, dst);
  }

  void vpcmpgtd_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpcmpgtd", VEX_PD, OP2_PCMPGTD_VdqWdq, src1, src0, dst);
  }
  void vpcmpgtd_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                   XMMRegisterID dst) {
    twoByteOpSimd("vpcmpgtd", VEX_PD, OP2_PCMPGTD_VdqWdq, offset, base, src0,
                  dst);
  }
  void vpcmpgtd_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpcmpgtd", VEX_PD, OP2_PCMPGTD_VdqWdq, address, src0, dst);
  }

  void vpcmpgtq_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    threeByteOpSimd("vpcmpgtq", VEX_PD, OP3_PCMPGTQ_VdqWdq, ESCAPE_38, src1,
                    src0, dst);
  }

  void vpcmpeqq_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    threeByteOpSimd("vpcmpeqq", VEX_PD, OP3_PCMPEQQ_VdqWdq, ESCAPE_38, src1,
                    src0, dst);
  }
  void vpcmpeqq_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                   XMMRegisterID dst) {
    threeByteOpSimd("vpcmpeqq", VEX_PD, OP3_PCMPEQQ_VdqWdq, ESCAPE_38, offset,
                    base, src0, dst);
  }
  void vpcmpeqq_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    threeByteOpSimd("vpcmpeqq", VEX_PD, OP3_PCMPEQQ_VdqWdq, ESCAPE_38, address,
                    src0, dst);
  }

  void vcmpps_rr(uint8_t order, XMMRegisterID src1, XMMRegisterID src0,
                 XMMRegisterID dst) {
    MOZ_ASSERT_IF(!useVEX_,
                  order < uint8_t(X86Encoding::ConditionCmp_AVX_Enabled));
    twoByteOpImmSimd("vcmpps", VEX_PS, OP2_CMPPS_VpsWps, order, src1, src0,
                     dst);
  }
  void vcmpps_mr(uint8_t order, int32_t offset, RegisterID base,
                 XMMRegisterID src0, XMMRegisterID dst) {
    MOZ_ASSERT_IF(!useVEX_,
                  order < uint8_t(X86Encoding::ConditionCmp_AVX_Enabled));
    twoByteOpImmSimd("vcmpps", VEX_PS, OP2_CMPPS_VpsWps, order, offset, base,
                     src0, dst);
  }
  void vcmpps_mr(uint8_t order, const void* address, XMMRegisterID src0,
                 XMMRegisterID dst) {
    MOZ_ASSERT_IF(!useVEX_,
                  order < uint8_t(X86Encoding::ConditionCmp_AVX_Enabled));
    twoByteOpImmSimd("vcmpps", VEX_PS, OP2_CMPPS_VpsWps, order, address, src0,
                     dst);
  }

  static constexpr size_t CMPPS_MR_PATCH_OFFSET = 1;

  size_t vcmpeqps_mr(const void* address, XMMRegisterID src0,
                     XMMRegisterID dst) {
    vcmpps_mr(X86Encoding::ConditionCmp_EQ, address, src0, dst);
    return CMPPS_MR_PATCH_OFFSET;
  }
  size_t vcmpneqps_mr(const void* address, XMMRegisterID src0,
                      XMMRegisterID dst) {
    vcmpps_mr(X86Encoding::ConditionCmp_NEQ, address, src0, dst);
    return CMPPS_MR_PATCH_OFFSET;
  }
  size_t vcmpltps_mr(const void* address, XMMRegisterID src0,
                     XMMRegisterID dst) {
    vcmpps_mr(X86Encoding::ConditionCmp_LT, address, src0, dst);
    return CMPPS_MR_PATCH_OFFSET;
  }
  size_t vcmpleps_mr(const void* address, XMMRegisterID src0,
                     XMMRegisterID dst) {
    vcmpps_mr(X86Encoding::ConditionCmp_LE, address, src0, dst);
    return CMPPS_MR_PATCH_OFFSET;
  }
  size_t vcmpgeps_mr(const void* address, XMMRegisterID src0,
                     XMMRegisterID dst) {
    vcmpps_mr(X86Encoding::ConditionCmp_GE, address, src0, dst);
    return CMPPS_MR_PATCH_OFFSET;
  }

  void vcmppd_rr(uint8_t order, XMMRegisterID src1, XMMRegisterID src0,
                 XMMRegisterID dst) {
    twoByteOpImmSimd("vcmppd", VEX_PD, OP2_CMPPD_VpdWpd, order, src1, src0,
                     dst);
  }
  void vcmppd_mr(uint8_t order, const void* address, XMMRegisterID src0,
                 XMMRegisterID dst) {
    twoByteOpImmSimd("vcmppd", VEX_PD, OP2_CMPPD_VpdWpd, order, address, src0,
                     dst);
  }

  static constexpr size_t CMPPD_MR_PATCH_OFFSET = 1;

  size_t vcmpeqpd_mr(const void* address, XMMRegisterID src0,
                     XMMRegisterID dst) {
    vcmppd_mr(X86Encoding::ConditionCmp_EQ, address, src0, dst);
    return CMPPD_MR_PATCH_OFFSET;
  }
  size_t vcmpneqpd_mr(const void* address, XMMRegisterID src0,
                      XMMRegisterID dst) {
    vcmppd_mr(X86Encoding::ConditionCmp_NEQ, address, src0, dst);
    return CMPPD_MR_PATCH_OFFSET;
  }
  size_t vcmpltpd_mr(const void* address, XMMRegisterID src0,
                     XMMRegisterID dst) {
    vcmppd_mr(X86Encoding::ConditionCmp_LT, address, src0, dst);
    return CMPPD_MR_PATCH_OFFSET;
  }
  size_t vcmplepd_mr(const void* address, XMMRegisterID src0,
                     XMMRegisterID dst) {
    vcmppd_mr(X86Encoding::ConditionCmp_LE, address, src0, dst);
    return CMPPD_MR_PATCH_OFFSET;
  }

  void vrcpps_rr(XMMRegisterID src, XMMRegisterID dst) {
    twoByteOpSimd("vrcpps", VEX_PS, OP2_RCPPS_VpsWps, src, invalid_xmm, dst);
  }
  void vrcpps_mr(int32_t offset, RegisterID base, XMMRegisterID dst) {
    twoByteOpSimd("vrcpps", VEX_PS, OP2_RCPPS_VpsWps, offset, base, invalid_xmm,
                  dst);
  }
  void vrcpps_mr(const void* address, XMMRegisterID dst) {
    twoByteOpSimd("vrcpps", VEX_PS, OP2_RCPPS_VpsWps, address, invalid_xmm,
                  dst);
  }

  void vrsqrtps_rr(XMMRegisterID src, XMMRegisterID dst) {
    twoByteOpSimd("vrsqrtps", VEX_PS, OP2_RSQRTPS_VpsWps, src, invalid_xmm,
                  dst);
  }
  void vrsqrtps_mr(int32_t offset, RegisterID base, XMMRegisterID dst) {
    twoByteOpSimd("vrsqrtps", VEX_PS, OP2_RSQRTPS_VpsWps, offset, base,
                  invalid_xmm, dst);
  }
  void vrsqrtps_mr(const void* address, XMMRegisterID dst) {
    twoByteOpSimd("vrsqrtps", VEX_PS, OP2_RSQRTPS_VpsWps, address, invalid_xmm,
                  dst);
  }

  void vsqrtps_rr(XMMRegisterID src, XMMRegisterID dst) {
    twoByteOpSimd("vsqrtps", VEX_PS, OP2_SQRTPS_VpsWps, src, invalid_xmm, dst);
  }
  void vsqrtps_mr(int32_t offset, RegisterID base, XMMRegisterID dst) {
    twoByteOpSimd("vsqrtps", VEX_PS, OP2_SQRTPS_VpsWps, offset, base,
                  invalid_xmm, dst);
  }
  void vsqrtps_mr(const void* address, XMMRegisterID dst) {
    twoByteOpSimd("vsqrtps", VEX_PS, OP2_SQRTPS_VpsWps, address, invalid_xmm,
                  dst);
  }
  void vsqrtpd_rr(XMMRegisterID src, XMMRegisterID dst) {
    twoByteOpSimd("vsqrtpd", VEX_PD, OP2_SQRTPD_VpdWpd, src, invalid_xmm, dst);
  }

  void vaddsd_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vaddsd", VEX_SD, OP2_ADDSD_VsdWsd, src1, src0, dst);
  }

  void vaddss_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vaddss", VEX_SS, OP2_ADDSD_VsdWsd, src1, src0, dst);
  }

  void vaddsd_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                 XMMRegisterID dst) {
    twoByteOpSimd("vaddsd", VEX_SD, OP2_ADDSD_VsdWsd, offset, base, src0, dst);
  }

  void vaddss_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                 XMMRegisterID dst) {
    twoByteOpSimd("vaddss", VEX_SS, OP2_ADDSD_VsdWsd, offset, base, src0, dst);
  }

  void vaddsd_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vaddsd", VEX_SD, OP2_ADDSD_VsdWsd, address, src0, dst);
  }
  void vaddss_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vaddss", VEX_SS, OP2_ADDSD_VsdWsd, address, src0, dst);
  }

  void vcvtss2sd_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vcvtss2sd", VEX_SS, OP2_CVTSS2SD_VsdEd, src1, src0, dst);
  }

  void vcvtsd2ss_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vcvtsd2ss", VEX_SD, OP2_CVTSD2SS_VsdEd, src1, src0, dst);
  }

  void vcvtsi2ss_rr(RegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpInt32Simd("vcvtsi2ss", VEX_SS, OP2_CVTSI2SD_VsdEd, src1, src0,
                       dst);
  }

  void vcvtsi2sd_rr(RegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpInt32Simd("vcvtsi2sd", VEX_SD, OP2_CVTSI2SD_VsdEd, src1, src0,
                       dst);
  }

  void vcvttps2dq_rr(XMMRegisterID src, XMMRegisterID dst) {
    twoByteOpSimd("vcvttps2dq", VEX_SS, OP2_CVTTPS2DQ_VdqWps, src, invalid_xmm,
                  dst);
  }

  void vcvttpd2dq_rr(XMMRegisterID src, XMMRegisterID dst) {
    twoByteOpSimd("vcvttpd2dq", VEX_PD, OP2_CVTTPD2DQ_VdqWpd, src, invalid_xmm,
                  dst);
  }

  void vcvtdq2ps_rr(XMMRegisterID src, XMMRegisterID dst) {
    twoByteOpSimd("vcvtdq2ps", VEX_PS, OP2_CVTDQ2PS_VpsWdq, src, invalid_xmm,
                  dst);
  }

  void vcvtdq2pd_rr(XMMRegisterID src, XMMRegisterID dst) {
    twoByteOpSimd("vcvtdq2pd", VEX_SS, OP2_CVTDQ2PD_VpdWdq, src, invalid_xmm,
                  dst);
  }

  void vcvtpd2ps_rr(XMMRegisterID src, XMMRegisterID dst) {
    twoByteOpSimd("vcvtpd2ps", VEX_PD, OP2_CVTPD2PS_VpsWpd, src, invalid_xmm,
                  dst);
  }

  void vcvtps2pd_rr(XMMRegisterID src, XMMRegisterID dst) {
    twoByteOpSimd("vcvtps2pd", VEX_PS, OP2_CVTPS2PD_VpdWps, src, invalid_xmm,
                  dst);
  }

  void vcvtsi2sd_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                    XMMRegisterID dst) {
    twoByteOpSimd("vcvtsi2sd", VEX_SD, OP2_CVTSI2SD_VsdEd, offset, base, src0,
                  dst);
  }

  void vcvtsi2sd_mr(int32_t offset, RegisterID base, RegisterID index,
                    int scale, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vcvtsi2sd", VEX_SD, OP2_CVTSI2SD_VsdEd, offset, base, index,
                  scale, src0, dst);
  }

  void vcvtsi2ss_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                    XMMRegisterID dst) {
    twoByteOpSimd("vcvtsi2ss", VEX_SS, OP2_CVTSI2SD_VsdEd, offset, base, src0,
                  dst);
  }

  void vcvtsi2ss_mr(int32_t offset, RegisterID base, RegisterID index,
                    int scale, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vcvtsi2ss", VEX_SS, OP2_CVTSI2SD_VsdEd, offset, base, index,
                  scale, src0, dst);
  }

  void vcvttsd2si_rr(XMMRegisterID src, RegisterID dst) {
    twoByteOpSimdInt32("vcvttsd2si", VEX_SD, OP2_CVTTSD2SI_GdWsd, src, dst);
  }

  void vcvttss2si_rr(XMMRegisterID src, RegisterID dst) {
    twoByteOpSimdInt32("vcvttss2si", VEX_SS, OP2_CVTTSD2SI_GdWsd, src, dst);
  }

  void vunpcklps_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vunpcklps", VEX_PS, OP2_UNPCKLPS_VsdWsd, src1, src0, dst);
  }
  void vunpcklps_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                    XMMRegisterID dst) {
    twoByteOpSimd("vunpcklps", VEX_PS, OP2_UNPCKLPS_VsdWsd, offset, base, src0,
                  dst);
  }
  void vunpcklps_mr(const void* addr, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vunpcklps", VEX_PS, OP2_UNPCKLPS_VsdWsd, addr, src0, dst);
  }

  void vunpckhps_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vunpckhps", VEX_PS, OP2_UNPCKHPS_VsdWsd, src1, src0, dst);
  }
  void vunpckhps_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                    XMMRegisterID dst) {
    twoByteOpSimd("vunpckhps", VEX_PS, OP2_UNPCKHPS_VsdWsd, offset, base, src0,
                  dst);
  }
  void vunpckhps_mr(const void* addr, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vunpckhps", VEX_PS, OP2_UNPCKHPS_VsdWsd, addr, src0, dst);
  }

  void vpand_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpand", VEX_PD, OP2_PANDDQ_VdqWdq, src1, src0, dst);
  }
  void vpand_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                XMMRegisterID dst) {
    twoByteOpSimd("vpand", VEX_PD, OP2_PANDDQ_VdqWdq, offset, base, src0, dst);
  }
  void vpand_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpand", VEX_PD, OP2_PANDDQ_VdqWdq, address, src0, dst);
  }
  void vpor_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpor", VEX_PD, OP2_PORDQ_VdqWdq, src1, src0, dst);
  }
  void vpor_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
               XMMRegisterID dst) {
    twoByteOpSimd("vpor", VEX_PD, OP2_PORDQ_VdqWdq, offset, base, src0, dst);
  }
  void vpor_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpor", VEX_PD, OP2_PORDQ_VdqWdq, address, src0, dst);
  }
  void vpxor_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpxor", VEX_PD, OP2_PXORDQ_VdqWdq, src1, src0, dst);
  }
  void vpxor_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                XMMRegisterID dst) {
    twoByteOpSimd("vpxor", VEX_PD, OP2_PXORDQ_VdqWdq, offset, base, src0, dst);
  }
  void vpxor_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpxor", VEX_PD, OP2_PXORDQ_VdqWdq, address, src0, dst);
  }
  void vpandn_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpandn", VEX_PD, OP2_PANDNDQ_VdqWdq, src1, src0, dst);
  }
  void vpandn_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                 XMMRegisterID dst) {
    twoByteOpSimd("vpandn", VEX_PD, OP2_PANDNDQ_VdqWdq, offset, base, src0,
                  dst);
  }
  void vpandn_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpandn", VEX_PD, OP2_PANDNDQ_VdqWdq, address, src0, dst);
  }
  void vptest_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    threeByteOpSimd("vptest", VEX_PD, OP3_PTEST_VdVd, ESCAPE_38, address, src0,
                    dst);
  }

  void vpshufd_irr(uint32_t mask, XMMRegisterID src, XMMRegisterID dst) {
    twoByteOpImmSimd("vpshufd", VEX_PD, OP2_PSHUFD_VdqWdqIb, mask, src,
                     invalid_xmm, dst);
  }
  void vpshufd_imr(uint32_t mask, int32_t offset, RegisterID base,
                   XMMRegisterID dst) {
    twoByteOpImmSimd("vpshufd", VEX_PD, OP2_PSHUFD_VdqWdqIb, mask, offset, base,
                     invalid_xmm, dst);
  }
  void vpshufd_imr(uint32_t mask, const void* address, XMMRegisterID dst) {
    twoByteOpImmSimd("vpshufd", VEX_PD, OP2_PSHUFD_VdqWdqIb, mask, address,
                     invalid_xmm, dst);
  }

  void vpshuflw_irr(uint32_t mask, XMMRegisterID src, XMMRegisterID dst) {
    twoByteOpImmSimd("vpshuflw", VEX_SD, OP2_PSHUFLW_VdqWdqIb, mask, src,
                     invalid_xmm, dst);
  }

  void vpshufhw_irr(uint32_t mask, XMMRegisterID src, XMMRegisterID dst) {
    twoByteOpImmSimd("vpshufhw", VEX_SS, OP2_PSHUFHW_VdqWdqIb, mask, src,
                     invalid_xmm, dst);
  }

  void vpshufb_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    threeByteOpSimd("vpshufb", VEX_PD, OP3_PSHUFB_VdqWdq, ESCAPE_38, src1, src0,
                    dst);
  }
  void vpshufb_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    threeByteOpSimd("vpshufb", VEX_PD, OP3_PSHUFB_VdqWdq, ESCAPE_38, address,
                    src0, dst);
  }

  void vshufps_irr(uint32_t mask, XMMRegisterID src1, XMMRegisterID src0,
                   XMMRegisterID dst) {
    twoByteOpImmSimd("vshufps", VEX_PS, OP2_SHUFPS_VpsWpsIb, mask, src1, src0,
                     dst);
  }
  void vshufps_imr(uint32_t mask, int32_t offset, RegisterID base,
                   XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpImmSimd("vshufps", VEX_PS, OP2_SHUFPS_VpsWpsIb, mask, offset, base,
                     src0, dst);
  }
  void vshufps_imr(uint32_t mask, const void* address, XMMRegisterID src0,
                   XMMRegisterID dst) {
    twoByteOpImmSimd("vshufps", VEX_PS, OP2_SHUFPS_VpsWpsIb, mask, address,
                     src0, dst);
  }
  void vshufpd_irr(uint32_t mask, XMMRegisterID src1, XMMRegisterID src0,
                   XMMRegisterID dst) {
    twoByteOpImmSimd("vshufpd", VEX_PD, OP2_SHUFPD_VpdWpdIb, mask, src1, src0,
                     dst);
  }

  void vmovddup_rr(XMMRegisterID src, XMMRegisterID dst) {
    twoByteOpSimd("vmovddup", VEX_SD, OP2_MOVDDUP_VqWq, src, invalid_xmm, dst);
  }
  void vmovddup_mr(int32_t offset, RegisterID base, XMMRegisterID dst) {
    twoByteOpSimd("vmovddup", VEX_SD, OP2_MOVDDUP_VqWq, offset, base,
                  invalid_xmm, dst);
  }
  void vmovddup_mr(int32_t offset, RegisterID base, RegisterID index,
                   int32_t scale, XMMRegisterID dst) {
    twoByteOpSimd("vmovddup", VEX_SD, OP2_MOVDDUP_VqWq, offset, base, index,
                  scale, invalid_xmm, dst);
  }

  void vmovhlps_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vmovhlps", VEX_PS, OP2_MOVHLPS_VqUq, src1, src0, dst);
  }

  void vmovlhps_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vmovlhps", VEX_PS, OP2_MOVLHPS_VqUq, src1, src0, dst);
  }

  void vpsrldq_ir(uint32_t count, XMMRegisterID src, XMMRegisterID dst) {
    MOZ_ASSERT(count < 16);
    shiftOpImmSimd("vpsrldq", OP2_PSRLDQ_Vd, ShiftID::vpsrldq, count, src, dst);
  }

  void vpslldq_ir(uint32_t count, XMMRegisterID src, XMMRegisterID dst) {
    MOZ_ASSERT(count < 16);
    shiftOpImmSimd("vpslldq", OP2_PSRLDQ_Vd, ShiftID::vpslldq, count, src, dst);
  }

  void vpsllq_ir(uint32_t count, XMMRegisterID src, XMMRegisterID dst) {
    MOZ_ASSERT(count < 64);
    shiftOpImmSimd("vpsllq", OP2_PSRLDQ_Vd, ShiftID::vpsllx, count, src, dst);
  }

  void vpsllq_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpsllq", VEX_PD, OP2_PSLLQ_VdqWdq, src1, src0, dst);
  }

  void vpsrlq_ir(uint32_t count, XMMRegisterID src, XMMRegisterID dst) {
    MOZ_ASSERT(count < 64);
    shiftOpImmSimd("vpsrlq", OP2_PSRLDQ_Vd, ShiftID::vpsrlx, count, src, dst);
  }

  void vpsrlq_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpsrlq", VEX_PD, OP2_PSRLQ_VdqWdq, src1, src0, dst);
  }

  void vpslld_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpslld", VEX_PD, OP2_PSLLD_VdqWdq, src1, src0, dst);
  }

  void vpslld_ir(uint32_t count, XMMRegisterID src, XMMRegisterID dst) {
    MOZ_ASSERT(count < 32);
    shiftOpImmSimd("vpslld", OP2_PSLLD_UdqIb, ShiftID::vpsllx, count, src, dst);
  }

  void vpsrad_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpsrad", VEX_PD, OP2_PSRAD_VdqWdq, src1, src0, dst);
  }

  void vpsrad_ir(int32_t count, XMMRegisterID src, XMMRegisterID dst) {
    MOZ_ASSERT(count < 32);
    shiftOpImmSimd("vpsrad", OP2_PSRAD_UdqIb, ShiftID::vpsrad, count, src, dst);
  }

  void vpsrld_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpsrld", VEX_PD, OP2_PSRLD_VdqWdq, src1, src0, dst);
  }

  void vpsrld_ir(uint32_t count, XMMRegisterID src, XMMRegisterID dst) {
    MOZ_ASSERT(count < 32);
    shiftOpImmSimd("vpsrld", OP2_PSRLD_UdqIb, ShiftID::vpsrlx, count, src, dst);
  }

  void vpsllw_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpsllw", VEX_PD, OP2_PSLLW_VdqWdq, src1, src0, dst);
  }

  void vpsllw_ir(uint32_t count, XMMRegisterID src, XMMRegisterID dst) {
    MOZ_ASSERT(count < 16);
    shiftOpImmSimd("vpsllw", OP2_PSLLW_UdqIb, ShiftID::vpsllx, count, src, dst);
  }

  void vpsraw_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpsraw", VEX_PD, OP2_PSRAW_VdqWdq, src1, src0, dst);
  }

  void vpsraw_ir(int32_t count, XMMRegisterID src, XMMRegisterID dst) {
    MOZ_ASSERT(count < 16);
    shiftOpImmSimd("vpsraw", OP2_PSRAW_UdqIb, ShiftID::vpsrad, count, src, dst);
  }

  void vpsrlw_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpsrlw", VEX_PD, OP2_PSRLW_VdqWdq, src1, src0, dst);
  }

  void vpsrlw_ir(uint32_t count, XMMRegisterID src, XMMRegisterID dst) {
    MOZ_ASSERT(count < 16);
    shiftOpImmSimd("vpsrlw", OP2_PSRLW_UdqIb, ShiftID::vpsrlx, count, src, dst);
  }

  void vmovmskpd_rr(XMMRegisterID src, RegisterID dst) {
    twoByteOpSimdInt32("vmovmskpd", VEX_PD, OP2_MOVMSKPD_EdVd, src, dst);
  }

  void vmovmskps_rr(XMMRegisterID src, RegisterID dst) {
    twoByteOpSimdInt32("vmovmskps", VEX_PS, OP2_MOVMSKPD_EdVd, src, dst);
  }

  void vpmovmskb_rr(XMMRegisterID src, RegisterID dst) {
    twoByteOpSimdInt32("vpmovmskb", VEX_PD, OP2_PMOVMSKB_EdVd, src, dst);
  }

  void vptest_rr(XMMRegisterID rhs, XMMRegisterID lhs) {
    threeByteOpSimd("vptest", VEX_PD, OP3_PTEST_VdVd, ESCAPE_38, rhs,
                    invalid_xmm, lhs);
  }

  void vmovd_rr(XMMRegisterID src, RegisterID dst) {
    twoByteOpSimdInt32("vmovd", VEX_PD, OP2_MOVD_EdVd, (XMMRegisterID)dst,
                       (RegisterID)src);
  }

  void vmovd_rr(RegisterID src, XMMRegisterID dst) {
    twoByteOpInt32Simd("vmovd", VEX_PD, OP2_MOVD_VdEd, src, invalid_xmm, dst);
  }

  void vmovd_mr(int32_t offset, RegisterID base, XMMRegisterID dst) {
    twoByteOpSimd("vmovd", VEX_PD, OP2_MOVD_VdEd, offset, base, invalid_xmm,
                  dst);
  }

  void vmovd_mr(int32_t offset, RegisterID base, RegisterID index,
                int32_t scale, XMMRegisterID dst) {
    twoByteOpSimd("vmovd", VEX_PD, OP2_MOVD_VdEd, offset, base, index, scale,
                  invalid_xmm, dst);
  }

  void vmovd_mr_disp32(int32_t offset, RegisterID base, XMMRegisterID dst) {
    twoByteOpSimd_disp32("vmovd", VEX_PD, OP2_MOVD_VdEd, offset, base,
                         invalid_xmm, dst);
  }

  void vmovd_mr(const void* address, XMMRegisterID dst) {
    twoByteOpSimd("vmovd", VEX_PD, OP2_MOVD_VdEd, address, invalid_xmm, dst);
  }

  void vmovd_rm(XMMRegisterID src, int32_t offset, RegisterID base) {
    twoByteOpSimd("vmovd", VEX_PD, OP2_MOVD_EdVd, offset, base, invalid_xmm,
                  src);
  }

  void vmovd_rm(XMMRegisterID src, int32_t offset, RegisterID base,
                RegisterID index, int scale) {
    twoByteOpSimd("vmovd", VEX_PD, OP2_MOVD_EdVd, offset, base, index, scale,
                  invalid_xmm, src);
  }

  void vmovd_rm_disp32(XMMRegisterID src, int32_t offset, RegisterID base) {
    twoByteOpSimd_disp32("vmovd", VEX_PD, OP2_MOVD_EdVd, offset, base,
                         invalid_xmm, src);
  }

  void vmovd_rm(XMMRegisterID src, const void* address) {
    twoByteOpSimd("vmovd", VEX_PD, OP2_MOVD_EdVd, address, invalid_xmm, src);
  }

  void vmovsd_rm(XMMRegisterID src, int32_t offset, RegisterID base) {
    twoByteOpSimd("vmovsd", VEX_SD, OP2_MOVSD_WsdVsd, offset, base, invalid_xmm,
                  src);
  }

  void vmovsd_rm_disp32(XMMRegisterID src, int32_t offset, RegisterID base) {
    twoByteOpSimd_disp32("vmovsd", VEX_SD, OP2_MOVSD_WsdVsd, offset, base,
                         invalid_xmm, src);
  }

  void vmovss_rm(XMMRegisterID src, int32_t offset, RegisterID base) {
    twoByteOpSimd("vmovss", VEX_SS, OP2_MOVSD_WsdVsd, offset, base, invalid_xmm,
                  src);
  }

  void vmovss_rm_disp32(XMMRegisterID src, int32_t offset, RegisterID base) {
    twoByteOpSimd_disp32("vmovss", VEX_SS, OP2_MOVSD_WsdVsd, offset, base,
                         invalid_xmm, src);
  }

  void vmovss_mr(int32_t offset, RegisterID base, XMMRegisterID dst) {
    twoByteOpSimd("vmovss", VEX_SS, OP2_MOVSD_VsdWsd, offset, base, invalid_xmm,
                  dst);
  }

  void vmovss_mr_disp32(int32_t offset, RegisterID base, XMMRegisterID dst) {
    twoByteOpSimd_disp32("vmovss", VEX_SS, OP2_MOVSD_VsdWsd, offset, base,
                         invalid_xmm, dst);
  }

  void vmovsd_rm(XMMRegisterID src, int32_t offset, RegisterID base,
                 RegisterID index, int scale) {
    twoByteOpSimd("vmovsd", VEX_SD, OP2_MOVSD_WsdVsd, offset, base, index,
                  scale, invalid_xmm, src);
  }

  void vmovss_rm(XMMRegisterID src, int32_t offset, RegisterID base,
                 RegisterID index, int scale) {
    twoByteOpSimd("vmovss", VEX_SS, OP2_MOVSD_WsdVsd, offset, base, index,
                  scale, invalid_xmm, src);
  }

  void vmovss_mr(int32_t offset, RegisterID base, RegisterID index, int scale,
                 XMMRegisterID dst) {
    twoByteOpSimd("vmovss", VEX_SS, OP2_MOVSD_VsdWsd, offset, base, index,
                  scale, invalid_xmm, dst);
  }

  void vmovsd_mr(int32_t offset, RegisterID base, XMMRegisterID dst) {
    twoByteOpSimd("vmovsd", VEX_SD, OP2_MOVSD_VsdWsd, offset, base, invalid_xmm,
                  dst);
  }

  void vmovsd_mr_disp32(int32_t offset, RegisterID base, XMMRegisterID dst) {
    twoByteOpSimd_disp32("vmovsd", VEX_SD, OP2_MOVSD_VsdWsd, offset, base,
                         invalid_xmm, dst);
  }

  void vmovsd_mr(int32_t offset, RegisterID base, RegisterID index, int scale,
                 XMMRegisterID dst) {
    twoByteOpSimd("vmovsd", VEX_SD, OP2_MOVSD_VsdWsd, offset, base, index,
                  scale, invalid_xmm, dst);
  }

  // Note that the register-to-register form of vmovsd does not write to the
  // entire output register. For general-purpose register-to-register moves,
  // use vmovapd instead.
  void vmovsd_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vmovsd", VEX_SD, OP2_MOVSD_VsdWsd, src1, src0, dst);
  }

  // The register-to-register form of vmovss has the same problem as vmovsd
  // above. Prefer vmovaps for register-to-register moves.
  void vmovss_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vmovss", VEX_SS, OP2_MOVSD_VsdWsd, src1, src0, dst);
  }

  void vmovsd_mr(const void* address, XMMRegisterID dst) {
    twoByteOpSimd("vmovsd", VEX_SD, OP2_MOVSD_VsdWsd, address, invalid_xmm,
                  dst);
  }

  void vmovss_mr(const void* address, XMMRegisterID dst) {
    twoByteOpSimd("vmovss", VEX_SS, OP2_MOVSD_VsdWsd, address, invalid_xmm,
                  dst);
  }

  void vmovups_mr(const void* address, XMMRegisterID dst) {
    twoByteOpSimd("vmovups", VEX_PS, OP2_MOVPS_VpsWps, address, invalid_xmm,
                  dst);
  }

  void vmovdqu_mr(const void* address, XMMRegisterID dst) {
    twoByteOpSimd("vmovdqu", VEX_SS, OP2_MOVDQ_VdqWdq, address, invalid_xmm,
                  dst);
  }

  void vmovsd_rm(XMMRegisterID src, const void* address) {
    twoByteOpSimd("vmovsd", VEX_SD, OP2_MOVSD_WsdVsd, address, invalid_xmm,
                  src);
  }

  void vmovss_rm(XMMRegisterID src, const void* address) {
    twoByteOpSimd("vmovss", VEX_SS, OP2_MOVSD_WsdVsd, address, invalid_xmm,
                  src);
  }

  void vmovdqa_rm(XMMRegisterID src, const void* address) {
    twoByteOpSimd("vmovdqa", VEX_PD, OP2_MOVDQ_WdqVdq, address, invalid_xmm,
                  src);
  }

  void vmovaps_rm(XMMRegisterID src, const void* address) {
    twoByteOpSimd("vmovaps", VEX_PS, OP2_MOVAPS_WsdVsd, address, invalid_xmm,
                  src);
  }

  void vmovdqu_rm(XMMRegisterID src, const void* address) {
    twoByteOpSimd("vmovdqu", VEX_SS, OP2_MOVDQ_WdqVdq, address, invalid_xmm,
                  src);
  }

  void vmovups_rm(XMMRegisterID src, const void* address) {
    twoByteOpSimd("vmovups", VEX_PS, OP2_MOVPS_WpsVps, address, invalid_xmm,
                  src);
  }

  void vmovaps_rr(XMMRegisterID src, XMMRegisterID dst) {
#ifdef JS_CODEGEN_X64
    // There are two opcodes that can encode this instruction. If we have
    // one register in [xmm8,xmm15] and one in [xmm0,xmm7], use the
    // opcode which swaps the operands, as that way we can get a two-byte
    // VEX in that case.
    if (src >= xmm8 && dst < xmm8) {
      twoByteOpSimd("vmovaps", VEX_PS, OP2_MOVAPS_WsdVsd, dst, invalid_xmm,
                    src);
      return;
    }
#endif
    twoByteOpSimd("vmovaps", VEX_PS, OP2_MOVAPS_VsdWsd, src, invalid_xmm, dst);
  }
  void vmovaps_rm(XMMRegisterID src, int32_t offset, RegisterID base) {
    twoByteOpSimd("vmovaps", VEX_PS, OP2_MOVAPS_WsdVsd, offset, base,
                  invalid_xmm, src);
  }
  void vmovaps_rm(XMMRegisterID src, int32_t offset, RegisterID base,
                  RegisterID index, int scale) {
    twoByteOpSimd("vmovaps", VEX_PS, OP2_MOVAPS_WsdVsd, offset, base, index,
                  scale, invalid_xmm, src);
  }
  void vmovaps_mr(int32_t offset, RegisterID base, XMMRegisterID dst) {
    twoByteOpSimd("vmovaps", VEX_PS, OP2_MOVAPS_VsdWsd, offset, base,
                  invalid_xmm, dst);
  }
  void vmovaps_mr(int32_t offset, RegisterID base, RegisterID index, int scale,
                  XMMRegisterID dst) {
    twoByteOpSimd("vmovaps", VEX_PS, OP2_MOVAPS_VsdWsd, offset, base, index,
                  scale, invalid_xmm, dst);
  }

  void vmovups_rm(XMMRegisterID src, int32_t offset, RegisterID base) {
    twoByteOpSimd("vmovups", VEX_PS, OP2_MOVPS_WpsVps, offset, base,
                  invalid_xmm, src);
  }
  void vmovups_rm_disp32(XMMRegisterID src, int32_t offset, RegisterID base) {
    twoByteOpSimd_disp32("vmovups", VEX_PS, OP2_MOVPS_WpsVps, offset, base,
                         invalid_xmm, src);
  }
  void vmovups_rm(XMMRegisterID src, int32_t offset, RegisterID base,
                  RegisterID index, int scale) {
    twoByteOpSimd("vmovups", VEX_PS, OP2_MOVPS_WpsVps, offset, base, index,
                  scale, invalid_xmm, src);
  }
  void vmovups_mr(int32_t offset, RegisterID base, XMMRegisterID dst) {
    twoByteOpSimd("vmovups", VEX_PS, OP2_MOVPS_VpsWps, offset, base,
                  invalid_xmm, dst);
  }
  void vmovups_mr_disp32(int32_t offset, RegisterID base, XMMRegisterID dst) {
    twoByteOpSimd_disp32("vmovups", VEX_PS, OP2_MOVPS_VpsWps, offset, base,
                         invalid_xmm, dst);
  }
  void vmovups_mr(int32_t offset, RegisterID base, RegisterID index, int scale,
                  XMMRegisterID dst) {
    twoByteOpSimd("vmovups", VEX_PS, OP2_MOVPS_VpsWps, offset, base, index,
                  scale, invalid_xmm, dst);
  }

  void vmovapd_rr(XMMRegisterID src, XMMRegisterID dst) {
#ifdef JS_CODEGEN_X64
    // There are two opcodes that can encode this instruction. If we have
    // one register in [xmm8,xmm15] and one in [xmm0,xmm7], use the
    // opcode which swaps the operands, as that way we can get a two-byte
    // VEX in that case.
    if (src >= xmm8 && dst < xmm8) {
      twoByteOpSimd("vmovapd", VEX_PD, OP2_MOVAPS_WsdVsd, dst, invalid_xmm,
                    src);
      return;
    }
#endif
    twoByteOpSimd("vmovapd", VEX_PD, OP2_MOVAPD_VsdWsd, src, invalid_xmm, dst);
  }

  void vmovdqu_rm(XMMRegisterID src, int32_t offset, RegisterID base) {
    twoByteOpSimd("vmovdqu", VEX_SS, OP2_MOVDQ_WdqVdq, offset, base,
                  invalid_xmm, src);
  }

  void vmovdqu_rm_disp32(XMMRegisterID src, int32_t offset, RegisterID base) {
    twoByteOpSimd_disp32("vmovdqu", VEX_SS, OP2_MOVDQ_WdqVdq, offset, base,
                         invalid_xmm, src);
  }

  void vmovdqu_rm(XMMRegisterID src, int32_t offset, RegisterID base,
                  RegisterID index, int scale) {
    twoByteOpSimd("vmovdqu", VEX_SS, OP2_MOVDQ_WdqVdq, offset, base, index,
                  scale, invalid_xmm, src);
  }

  void vmovdqu_mr(int32_t offset, RegisterID base, XMMRegisterID dst) {
    twoByteOpSimd("vmovdqu", VEX_SS, OP2_MOVDQ_VdqWdq, offset, base,
                  invalid_xmm, dst);
  }

  void vmovdqu_mr_disp32(int32_t offset, RegisterID base, XMMRegisterID dst) {
    twoByteOpSimd_disp32("vmovdqu", VEX_SS, OP2_MOVDQ_VdqWdq, offset, base,
                         invalid_xmm, dst);
  }

  void vmovdqu_mr(int32_t offset, RegisterID base, RegisterID index, int scale,
                  XMMRegisterID dst) {
    twoByteOpSimd("vmovdqu", VEX_SS, OP2_MOVDQ_VdqWdq, offset, base, index,
                  scale, invalid_xmm, dst);
  }

  void vmovdqa_rr(XMMRegisterID src, XMMRegisterID dst) {
#ifdef JS_CODEGEN_X64
    // There are two opcodes that can encode this instruction. If we have
    // one register in [xmm8,xmm15] and one in [xmm0,xmm7], use the
    // opcode which swaps the operands, as that way we can get a two-byte
    // VEX in that case.
    if (src >= xmm8 && dst < xmm8) {
      twoByteOpSimd("vmovdqa", VEX_PD, OP2_MOVDQ_WdqVdq, dst, invalid_xmm, src);
      return;
    }
#endif
    twoByteOpSimd("vmovdqa", VEX_PD, OP2_MOVDQ_VdqWdq, src, invalid_xmm, dst);
  }

  void vmovdqa_rm(XMMRegisterID src, int32_t offset, RegisterID base) {
    twoByteOpSimd("vmovdqa", VEX_PD, OP2_MOVDQ_WdqVdq, offset, base,
                  invalid_xmm, src);
  }

  void vmovdqa_rm(XMMRegisterID src, int32_t offset, RegisterID base,
                  RegisterID index, int scale) {
    twoByteOpSimd("vmovdqa", VEX_PD, OP2_MOVDQ_WdqVdq, offset, base, index,
                  scale, invalid_xmm, src);
  }

  void vmovdqa_mr(int32_t offset, RegisterID base, XMMRegisterID dst) {
    twoByteOpSimd("vmovdqa", VEX_PD, OP2_MOVDQ_VdqWdq, offset, base,
                  invalid_xmm, dst);
  }

  void vmovdqa_mr(int32_t offset, RegisterID base, RegisterID index, int scale,
                  XMMRegisterID dst) {
    twoByteOpSimd("vmovdqa", VEX_PD, OP2_MOVDQ_VdqWdq, offset, base, index,
                  scale, invalid_xmm, dst);
  }

  void vmulsd_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vmulsd", VEX_SD, OP2_MULSD_VsdWsd, src1, src0, dst);
  }

  void vmulss_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vmulss", VEX_SS, OP2_MULSD_VsdWsd, src1, src0, dst);
  }

  void vmulsd_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                 XMMRegisterID dst) {
    twoByteOpSimd("vmulsd", VEX_SD, OP2_MULSD_VsdWsd, offset, base, src0, dst);
  }

  void vmulss_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                 XMMRegisterID dst) {
    twoByteOpSimd("vmulss", VEX_SS, OP2_MULSD_VsdWsd, offset, base, src0, dst);
  }

  void vpinsrw_irr(uint32_t whichWord, RegisterID src1, XMMRegisterID src0,
                   XMMRegisterID dst) {
    MOZ_ASSERT(whichWord < 8);
    twoByteOpImmInt32Simd("vpinsrw", VEX_PD, OP2_PINSRW, whichWord, src1, src0,
                          dst);
  }
  void vpinsrw_imr(unsigned lane, int32_t offset, RegisterID base,
                   XMMRegisterID src0, XMMRegisterID dst) {
    MOZ_ASSERT(lane < 16);
    twoByteOpImmInt32Simd("vpinsrw", VEX_PD, OP2_PINSRW, lane, offset, base,
                          src0, dst);
  }
  void vpinsrw_imr(unsigned lane, int32_t offset, RegisterID base,
                   RegisterID index, int32_t scale, XMMRegisterID src0,
                   XMMRegisterID dst) {
    MOZ_ASSERT(lane < 16);
    twoByteOpImmInt32Simd("vpinsrw", VEX_PD, OP2_PINSRW, lane, offset, base,
                          index, scale, src0, dst);
  }

  void vpextrw_irr(uint32_t whichWord, XMMRegisterID src, RegisterID dst) {
    MOZ_ASSERT(whichWord < 8);
    twoByteOpImmSimdInt32("vpextrw", VEX_PD, OP2_PEXTRW_GdUdIb, whichWord, src,
                          dst);
  }

  void vpextrw_irm(unsigned lane, XMMRegisterID src, int32_t offset,
                   RegisterID base) {
    MOZ_ASSERT(lane < 8);
    threeByteOpImmSimdInt32("vpextrw", VEX_PD, OP3_PEXTRW_EwVdqIb, ESCAPE_3A,
                            lane, offset, base, (RegisterID)src);
  }

  void vpextrw_irm(unsigned lane, XMMRegisterID src, int32_t offset,
                   RegisterID base, RegisterID index, int scale) {
    MOZ_ASSERT(lane < 8);
    threeByteOpImmSimdInt32("vpextrw", VEX_PD, OP3_PEXTRW_EwVdqIb, ESCAPE_3A,
                            lane, offset, base, index, scale, (RegisterID)src);
  }

  void vsubsd_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vsubsd", VEX_SD, OP2_SUBSD_VsdWsd, src1, src0, dst);
  }

  void vsubss_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vsubss", VEX_SS, OP2_SUBSD_VsdWsd, src1, src0, dst);
  }

  void vsubsd_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                 XMMRegisterID dst) {
    twoByteOpSimd("vsubsd", VEX_SD, OP2_SUBSD_VsdWsd, offset, base, src0, dst);
  }

  void vsubss_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                 XMMRegisterID dst) {
    twoByteOpSimd("vsubss", VEX_SS, OP2_SUBSD_VsdWsd, offset, base, src0, dst);
  }

  void vucomiss_rr(XMMRegisterID rhs, XMMRegisterID lhs) {
    twoByteOpSimdFlags("vucomiss", VEX_PS, OP2_UCOMISD_VsdWsd, rhs, lhs);
  }

  void vucomisd_rr(XMMRegisterID rhs, XMMRegisterID lhs) {
    twoByteOpSimdFlags("vucomisd", VEX_PD, OP2_UCOMISD_VsdWsd, rhs, lhs);
  }

  void vucomisd_mr(int32_t offset, RegisterID base, XMMRegisterID lhs) {
    twoByteOpSimdFlags("vucomisd", VEX_PD, OP2_UCOMISD_VsdWsd, offset, base,
                       lhs);
  }

  void vdivsd_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vdivsd", VEX_SD, OP2_DIVSD_VsdWsd, src1, src0, dst);
  }

  void vdivss_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vdivss", VEX_SS, OP2_DIVSD_VsdWsd, src1, src0, dst);
  }

  void vdivsd_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                 XMMRegisterID dst) {
    twoByteOpSimd("vdivsd", VEX_SD, OP2_DIVSD_VsdWsd, offset, base, src0, dst);
  }

  void vdivss_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                 XMMRegisterID dst) {
    twoByteOpSimd("vdivss", VEX_SS, OP2_DIVSD_VsdWsd, offset, base, src0, dst);
  }

  void vxorpd_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vxorpd", VEX_PD, OP2_XORPD_VpdWpd, src1, src0, dst);
  }

  void vorpd_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vorpd", VEX_PD, OP2_ORPD_VpdWpd, src1, src0, dst);
  }

  void vandpd_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vandpd", VEX_PD, OP2_ANDPD_VpdWpd, src1, src0, dst);
  }
  void vandpd_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vandpd", VEX_PD, OP2_ANDPD_VpdWpd, address, src0, dst);
  }

  void vandps_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vandps", VEX_PS, OP2_ANDPS_VpsWps, src1, src0, dst);
  }

  void vandps_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                 XMMRegisterID dst) {
    twoByteOpSimd("vandps", VEX_PS, OP2_ANDPS_VpsWps, offset, base, src0, dst);
  }

  void vandps_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vandps", VEX_PS, OP2_ANDPS_VpsWps, address, src0, dst);
  }

  void vandnps_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vandnps", VEX_PS, OP2_ANDNPS_VpsWps, src1, src0, dst);
  }

  void vandnps_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                  XMMRegisterID dst) {
    twoByteOpSimd("vandnps", VEX_PS, OP2_ANDNPS_VpsWps, offset, base, src0,
                  dst);
  }

  void vandnps_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vandnps", VEX_PS, OP2_ANDNPS_VpsWps, address, src0, dst);
  }

  void vorps_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vorps", VEX_PS, OP2_ORPS_VpsWps, src1, src0, dst);
  }

  void vorps_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                XMMRegisterID dst) {
    twoByteOpSimd("vorps", VEX_PS, OP2_ORPS_VpsWps, offset, base, src0, dst);
  }

  void vorps_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vorps", VEX_PS, OP2_ORPS_VpsWps, address, src0, dst);
  }

  void vxorps_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vxorps", VEX_PS, OP2_XORPS_VpsWps, src1, src0, dst);
  }

  void vxorps_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                 XMMRegisterID dst) {
    twoByteOpSimd("vxorps", VEX_PS, OP2_XORPS_VpsWps, offset, base, src0, dst);
  }

  void vxorps_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vxorps", VEX_PS, OP2_XORPS_VpsWps, address, src0, dst);
  }

  void vsqrtsd_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vsqrtsd", VEX_SD, OP2_SQRTSD_VsdWsd, src1, src0, dst);
  }

  void vsqrtss_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vsqrtss", VEX_SS, OP2_SQRTSS_VssWss, src1, src0, dst);
  }

  void vroundsd_irr(RoundingMode mode, XMMRegisterID src, XMMRegisterID dst) {
    threeByteOpImmSimd("vroundsd", VEX_PD, OP3_ROUNDSD_VsdWsd, ESCAPE_3A, mode,
                       src, invalid_xmm, dst);
  }

  void vroundss_irr(RoundingMode mode, XMMRegisterID src, XMMRegisterID dst) {
    threeByteOpImmSimd("vroundss", VEX_PD, OP3_ROUNDSS_VsdWsd, ESCAPE_3A, mode,
                       src, invalid_xmm, dst);
  }
  void vroundps_irr(SSERoundingMode mode, XMMRegisterID src,
                    XMMRegisterID dst) {
    threeByteOpImmSimd("vroundps", VEX_PD, OP3_ROUNDPS_VpsWps, ESCAPE_3A,
                       int(mode), src, invalid_xmm, dst);
  }
  void vroundpd_irr(SSERoundingMode mode, XMMRegisterID src,
                    XMMRegisterID dst) {
    threeByteOpImmSimd("vroundpd", VEX_PD, OP3_ROUNDPD_VpdWpd, ESCAPE_3A,
                       int(mode), src, invalid_xmm, dst);
  }

  void vinsertps_irr(uint32_t mask, XMMRegisterID src1, XMMRegisterID src0,
                     XMMRegisterID dst) {
    threeByteOpImmSimd("vinsertps", VEX_PD, OP3_INSERTPS_VpsUps, ESCAPE_3A,
                       mask, src1, src0, dst);
  }
  void vinsertps_imr(uint32_t mask, int32_t offset, RegisterID base,
                     XMMRegisterID src0, XMMRegisterID dst) {
    threeByteOpImmSimd("vinsertps", VEX_PD, OP3_INSERTPS_VpsUps, ESCAPE_3A,
                       mask, offset, base, src0, dst);
  }
  void vinsertps_imr(uint32_t mask, int32_t offset, RegisterID base,
                     RegisterID index, int scale, XMMRegisterID src0,
                     XMMRegisterID dst) {
    threeByteOpImmSimd("vinsertps", VEX_PD, OP3_INSERTPS_VpsUps, ESCAPE_3A,
                       mask, offset, base, index, scale, src0, dst);
  }

  void vmovlps_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                  XMMRegisterID dst) {
    twoByteOpSimd("vmovlps", VEX_PS, OP2_MOVLPS_VqEq, offset, base, src0, dst);
  }
  void vmovlps_mr(int32_t offset, RegisterID base, RegisterID index, int scale,
                  XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vmovlps", VEX_PS, OP2_MOVLPS_VqEq, offset, base, index,
                  scale, src0, dst);
  }
  void vmovlps_rm(XMMRegisterID src, int32_t offset, RegisterID base) {
    twoByteOpSimd("vmovlps", VEX_PS, OP2_MOVLPS_EqVq, offset, base, invalid_xmm,
                  src);
  }
  void vmovlps_rm(XMMRegisterID src, int32_t offset, RegisterID base,
                  RegisterID index, int scale) {
    twoByteOpSimd("vmovlps", VEX_PS, OP2_MOVLPS_EqVq, offset, base, index,
                  scale, invalid_xmm, src);
  }

  void vmovhps_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                  XMMRegisterID dst) {
    twoByteOpSimd("vmovhps", VEX_PS, OP2_MOVHPS_VqEq, offset, base, src0, dst);
  }
  void vmovhps_mr(int32_t offset, RegisterID base, RegisterID index, int scale,
                  XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vmovhps", VEX_PS, OP2_MOVHPS_VqEq, offset, base, index,
                  scale, src0, dst);
  }

  void vmovhps_rm(XMMRegisterID src, int32_t offset, RegisterID base) {
    twoByteOpSimd("vmovhps", VEX_PS, OP2_MOVHPS_EqVq, offset, base, invalid_xmm,
                  src);
  }
  void vmovhps_rm(XMMRegisterID src, int32_t offset, RegisterID base,
                  RegisterID index, int scale) {
    twoByteOpSimd("vmovhps", VEX_PS, OP2_MOVHPS_EqVq, offset, base, index,
                  scale, invalid_xmm, src);
  }

  void vextractps_rm(unsigned lane, XMMRegisterID src, int32_t offset,
                     RegisterID base) {
    threeByteOpImmSimd("vextractps", VEX_PD, OP3_EXTRACTPS_EdVdqIb, ESCAPE_3A,
                       lane, offset, base, invalid_xmm, src);
  }
  void vextractps_rm(unsigned lane, XMMRegisterID src, int32_t offset,
                     RegisterID base, RegisterID index, int scale) {
    threeByteOpImmSimd("vextractps", VEX_PD, OP3_EXTRACTPS_EdVdqIb, ESCAPE_3A,
                       lane, offset, base, index, scale, invalid_xmm, src);
  }

  void vpblendw_irr(unsigned mask, XMMRegisterID src1, XMMRegisterID src0,
                    XMMRegisterID dst) {
    MOZ_ASSERT(mask < 256);
    threeByteOpImmSimd("vpblendw", VEX_PD, OP3_PBLENDW_VdqWdqIb, ESCAPE_3A,
                       mask, src1, src0, dst);
  }

  void vpblendvb_rr(XMMRegisterID mask, XMMRegisterID src1, XMMRegisterID src0,
                    XMMRegisterID dst) {
    vblendvOpSimd("vpblendvb", OP3_PBLENDVB_VdqWdq, OP3_VPBLENDVB_VdqWdq, mask,
                  src1, src0, dst);
  }

  void vpinsrb_irr(unsigned lane, RegisterID src1, XMMRegisterID src0,
                   XMMRegisterID dst) {
    MOZ_ASSERT(lane < 16);
    threeByteOpImmInt32Simd("vpinsrb", VEX_PD, OP3_PINSRB_VdqEvIb, ESCAPE_3A,
                            lane, src1, src0, dst);
  }
  void vpinsrb_imr(unsigned lane, int32_t offset, RegisterID base,
                   XMMRegisterID src0, XMMRegisterID dst) {
    MOZ_ASSERT(lane < 16);
    threeByteOpImmInt32Simd("vpinsrb", VEX_PD, OP3_PINSRB_VdqEvIb, ESCAPE_3A,
                            lane, offset, base, src0, dst);
  }
  void vpinsrb_imr(unsigned lane, int32_t offset, RegisterID base,
                   RegisterID index, int32_t scale, XMMRegisterID src0,
                   XMMRegisterID dst) {
    MOZ_ASSERT(lane < 16);
    threeByteOpImmInt32Simd("vpinsrb", VEX_PD, OP3_PINSRB_VdqEvIb, ESCAPE_3A,
                            lane, offset, base, index, scale, src0, dst);
  }

  void vpinsrd_irr(unsigned lane, RegisterID src1, XMMRegisterID src0,
                   XMMRegisterID dst) {
    MOZ_ASSERT(lane < 4);
    threeByteOpImmInt32Simd("vpinsrd", VEX_PD, OP3_PINSRD_VdqEvIb, ESCAPE_3A,
                            lane, src1, src0, dst);
  }

  void vpextrb_irr(unsigned lane, XMMRegisterID src, RegisterID dst) {
    MOZ_ASSERT(lane < 16);
    threeByteOpImmSimdInt32("vpextrb", VEX_PD, OP3_PEXTRB_EvVdqIb, ESCAPE_3A,
                            lane, (XMMRegisterID)dst, (RegisterID)src);
  }

  void vpextrb_irm(unsigned lane, XMMRegisterID src, int32_t offset,
                   RegisterID base) {
    MOZ_ASSERT(lane < 16);
    threeByteOpImmSimdInt32("vpextrb", VEX_PD, OP3_PEXTRB_EvVdqIb, ESCAPE_3A,
                            lane, offset, base, (RegisterID)src);
  }

  void vpextrb_irm(unsigned lane, XMMRegisterID src, int32_t offset,
                   RegisterID base, RegisterID index, int scale) {
    MOZ_ASSERT(lane < 16);
    threeByteOpImmSimdInt32("vpextrb", VEX_PD, OP3_PEXTRB_EvVdqIb, ESCAPE_3A,
                            lane, offset, base, index, scale, (RegisterID)src);
  }

  void vpextrd_irr(unsigned lane, XMMRegisterID src, RegisterID dst) {
    MOZ_ASSERT(lane < 4);
    threeByteOpImmSimdInt32("vpextrd", VEX_PD, OP3_PEXTRD_EvVdqIb, ESCAPE_3A,
                            lane, (XMMRegisterID)dst, (RegisterID)src);
  }

  void vblendps_irr(unsigned imm, XMMRegisterID src1, XMMRegisterID src0,
                    XMMRegisterID dst) {
    MOZ_ASSERT(imm < 16);
    // Despite being a "ps" instruction, vblendps is encoded with the "pd"
    // prefix.
    threeByteOpImmSimd("vblendps", VEX_PD, OP3_BLENDPS_VpsWpsIb, ESCAPE_3A, imm,
                       src1, src0, dst);
  }

  void vblendps_imr(unsigned imm, int32_t offset, RegisterID base,
                    XMMRegisterID src0, XMMRegisterID dst) {
    MOZ_ASSERT(imm < 16);
    // Despite being a "ps" instruction, vblendps is encoded with the "pd"
    // prefix.
    threeByteOpImmSimd("vblendps", VEX_PD, OP3_BLENDPS_VpsWpsIb, ESCAPE_3A, imm,
                       offset, base, src0, dst);
  }

  void vblendvps_rr(XMMRegisterID mask, XMMRegisterID src1, XMMRegisterID src0,
                    XMMRegisterID dst) {
    vblendvOpSimd("vblendvps", OP3_BLENDVPS_VdqWdq, OP3_VBLENDVPS_VdqWdq, mask,
                  src1, src0, dst);
  }
  void vblendvps_mr(XMMRegisterID mask, int32_t offset, RegisterID base,
                    XMMRegisterID src0, XMMRegisterID dst) {
    vblendvOpSimd("vblendvps", OP3_BLENDVPS_VdqWdq, OP3_VBLENDVPS_VdqWdq, mask,
                  offset, base, src0, dst);
  }
  void vblendvpd_rr(XMMRegisterID mask, XMMRegisterID src1, XMMRegisterID src0,
                    XMMRegisterID dst) {
    vblendvOpSimd("vblendvpd", OP3_BLENDVPD_VdqWdq, OP3_VBLENDVPD_VdqWdq, mask,
                  src1, src0, dst);
  }

  void vmovsldup_rr(XMMRegisterID src, XMMRegisterID dst) {
    twoByteOpSimd("vmovsldup", VEX_SS, OP2_MOVSLDUP_VpsWps, src, invalid_xmm,
                  dst);
  }
  void vmovsldup_mr(int32_t offset, RegisterID base, XMMRegisterID dst) {
    twoByteOpSimd("vmovsldup", VEX_SS, OP2_MOVSLDUP_VpsWps, offset, base,
                  invalid_xmm, dst);
  }

  void vmovshdup_rr(XMMRegisterID src, XMMRegisterID dst) {
    twoByteOpSimd("vmovshdup", VEX_SS, OP2_MOVSHDUP_VpsWps, src, invalid_xmm,
                  dst);
  }
  void vmovshdup_mr(int32_t offset, RegisterID base, XMMRegisterID dst) {
    twoByteOpSimd("vmovshdup", VEX_SS, OP2_MOVSHDUP_VpsWps, offset, base,
                  invalid_xmm, dst);
  }

  void vminsd_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vminsd", VEX_SD, OP2_MINSD_VsdWsd, src1, src0, dst);
  }
  void vminsd_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                 XMMRegisterID dst) {
    twoByteOpSimd("vminsd", VEX_SD, OP2_MINSD_VsdWsd, offset, base, src0, dst);
  }

  void vminss_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vminss", VEX_SS, OP2_MINSS_VssWss, src1, src0, dst);
  }

  void vmaxsd_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vmaxsd", VEX_SD, OP2_MAXSD_VsdWsd, src1, src0, dst);
  }
  void vmaxsd_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                 XMMRegisterID dst) {
    twoByteOpSimd("vmaxsd", VEX_SD, OP2_MAXSD_VsdWsd, offset, base, src0, dst);
  }

  void vmaxss_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vmaxss", VEX_SS, OP2_MAXSS_VssWss, src1, src0, dst);
  }

  void vpavgb_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpavgb", VEX_PD, OP2_PAVGB_VdqWdq, src1, src0, dst);
  }

  void vpavgw_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpavgw", VEX_PD, OP2_PAVGW_VdqWdq, src1, src0, dst);
  }

  void vpminsb_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    threeByteOpSimd("vpminsb", VEX_PD, OP3_PMINSB_VdqWdq, ESCAPE_38, src1, src0,
                    dst);
  }
  void vpminsb_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    threeByteOpSimd("vpminsb", VEX_PD, OP3_PMINSB_VdqWdq, ESCAPE_38, address,
                    src0, dst);
  }

  void vpmaxsb_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    threeByteOpSimd("vpmaxsb", VEX_PD, OP3_PMAXSB_VdqWdq, ESCAPE_38, src1, src0,
                    dst);
  }
  void vpmaxsb_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    threeByteOpSimd("vpmaxsb", VEX_PD, OP3_PMAXSB_VdqWdq, ESCAPE_38, address,
                    src0, dst);
  }

  void vpminub_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpminub", VEX_PD, OP2_PMINUB_VdqWdq, src1, src0, dst);
  }
  void vpminub_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpminub", VEX_PD, OP2_PMINUB_VdqWdq, address, src0, dst);
  }

  void vpmaxub_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpmaxub", VEX_PD, OP2_PMAXUB_VdqWdq, src1, src0, dst);
  }
  void vpmaxub_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpmaxub", VEX_PD, OP2_PMAXUB_VdqWdq, address, src0, dst);
  }

  void vpminsw_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpminsw", VEX_PD, OP2_PMINSW_VdqWdq, src1, src0, dst);
  }
  void vpminsw_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpminsw", VEX_PD, OP2_PMINSW_VdqWdq, address, src0, dst);
  }

  void vpmaxsw_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpmaxsw", VEX_PD, OP2_PMAXSW_VdqWdq, src1, src0, dst);
  }
  void vpmaxsw_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpmaxsw", VEX_PD, OP2_PMAXSW_VdqWdq, address, src0, dst);
  }

  void vpminuw_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    threeByteOpSimd("vpminuw", VEX_PD, OP3_PMINUW_VdqWdq, ESCAPE_38, src1, src0,
                    dst);
  }
  void vpminuw_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    threeByteOpSimd("vpminuw", VEX_PD, OP3_PMINUW_VdqWdq, ESCAPE_38, address,
                    src0, dst);
  }

  void vpmaxuw_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    threeByteOpSimd("vpmaxuw", VEX_PD, OP3_PMAXUW_VdqWdq, ESCAPE_38, src1, src0,
                    dst);
  }
  void vpmaxuw_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    threeByteOpSimd("vpmaxuw", VEX_PD, OP3_PMAXUW_VdqWdq, ESCAPE_38, address,
                    src0, dst);
  }

  void vpminsd_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    threeByteOpSimd("vpminsd", VEX_PD, OP3_PMINSD_VdqWdq, ESCAPE_38, src1, src0,
                    dst);
  }
  void vpminsd_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    threeByteOpSimd("vpminsd", VEX_PD, OP3_PMINSD_VdqWdq, ESCAPE_38, address,
                    src0, dst);
  }

  void vpmaxsd_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    threeByteOpSimd("vpmaxsd", VEX_PD, OP3_PMAXSD_VdqWdq, ESCAPE_38, src1, src0,
                    dst);
  }
  void vpmaxsd_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    threeByteOpSimd("vpmaxsd", VEX_PD, OP3_PMAXSD_VdqWdq, ESCAPE_38, address,
                    src0, dst);
  }

  void vpminud_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    threeByteOpSimd("vpminud", VEX_PD, OP3_PMINUD_VdqWdq, ESCAPE_38, src1, src0,
                    dst);
  }
  void vpminud_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    threeByteOpSimd("vpminud", VEX_PD, OP3_PMINUD_VdqWdq, ESCAPE_38, address,
                    src0, dst);
  }

  void vpmaxud_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    threeByteOpSimd("vpmaxud", VEX_PD, OP3_PMAXUD_VdqWdq, ESCAPE_38, src1, src0,
                    dst);
  }
  void vpmaxud_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst) {
    threeByteOpSimd("vpmaxud", VEX_PD, OP3_PMAXUD_VdqWdq, ESCAPE_38, address,
                    src0, dst);
  }

  void vpacksswb_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpacksswb", VEX_PD, OP2_PACKSSWB_VdqWdq, src1, src0, dst);
  }
  void vpacksswb_mr(const void* address, XMMRegisterID src0,
                    XMMRegisterID dst) {
    twoByteOpSimd("vpacksswb", VEX_PD, OP2_PACKSSWB_VdqWdq, address, src0, dst);
  }

  void vpackuswb_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpackuswb", VEX_PD, OP2_PACKUSWB_VdqWdq, src1, src0, dst);
  }
  void vpackuswb_mr(const void* address, XMMRegisterID src0,
                    XMMRegisterID dst) {
    twoByteOpSimd("vpackuswb", VEX_PD, OP2_PACKUSWB_VdqWdq, address, src0, dst);
  }

  void vpackssdw_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpackssdw", VEX_PD, OP2_PACKSSDW_VdqWdq, src1, src0, dst);
  }
  void vpackssdw_mr(const void* address, XMMRegisterID src0,
                    XMMRegisterID dst) {
    twoByteOpSimd("vpackssdw", VEX_PD, OP2_PACKSSDW_VdqWdq, address, src0, dst);
  }

  void vpackusdw_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    threeByteOpSimd("vpackusdw", VEX_PD, OP3_PACKUSDW_VdqWdq, ESCAPE_38, src1,
                    src0, dst);
  }
  void vpackusdw_mr(const void* address, XMMRegisterID src0,
                    XMMRegisterID dst) {
    threeByteOpSimd("vpackusdw", VEX_PD, OP3_PACKUSDW_VdqWdq, ESCAPE_38,
                    address, src0, dst);
  }

  void vpabsb_rr(XMMRegisterID src, XMMRegisterID dst) {
    threeByteOpSimd("vpabsb", VEX_PD, OP3_PABSB_VdqWdq, ESCAPE_38, src,
                    invalid_xmm, dst);
  }

  void vpabsw_rr(XMMRegisterID src, XMMRegisterID dst) {
    threeByteOpSimd("vpabsw", VEX_PD, OP3_PABSW_VdqWdq, ESCAPE_38, src,
                    invalid_xmm, dst);
  }

  void vpabsd_rr(XMMRegisterID src, XMMRegisterID dst) {
    threeByteOpSimd("vpabsd", VEX_PD, OP3_PABSD_VdqWdq, ESCAPE_38, src,
                    invalid_xmm, dst);
  }

  void vpmovsxbw_rr(XMMRegisterID src, XMMRegisterID dst) {
    threeByteOpSimd("vpmovsxbw", VEX_PD, OP3_PMOVSXBW_VdqWdq, ESCAPE_38, src,
                    invalid_xmm, dst);
  }
  void vpmovsxbw_mr(int32_t offset, RegisterID base, XMMRegisterID dst) {
    threeByteOpSimd("vpmovsxbw", VEX_PD, OP3_PMOVSXBW_VdqWdq, ESCAPE_38, offset,
                    base, invalid_xmm, dst);
  }
  void vpmovsxbw_mr(int32_t offset, RegisterID base, RegisterID index,
                    int32_t scale, XMMRegisterID dst) {
    threeByteOpSimd("vpmovsxbw", VEX_PD, OP3_PMOVSXBW_VdqWdq, ESCAPE_38, offset,
                    base, index, scale, invalid_xmm, dst);
  }

  void vpmovzxbw_rr(XMMRegisterID src, XMMRegisterID dst) {
    threeByteOpSimd("vpmovzxbw", VEX_PD, OP3_PMOVZXBW_VdqWdq, ESCAPE_38, src,
                    invalid_xmm, dst);
  }
  void vpmovzxbw_mr(int32_t offset, RegisterID base, XMMRegisterID dst) {
    threeByteOpSimd("vpmovzxbw", VEX_PD, OP3_PMOVZXBW_VdqWdq, ESCAPE_38, offset,
                    base, invalid_xmm, dst);
  }
  void vpmovzxbw_mr(int32_t offset, RegisterID base, RegisterID index,
                    int32_t scale, XMMRegisterID dst) {
    threeByteOpSimd("vpmovzxbw", VEX_PD, OP3_PMOVZXBW_VdqWdq, ESCAPE_38, offset,
                    base, index, scale, invalid_xmm, dst);
  }

  void vpmovzxbd_rr(XMMRegisterID src, XMMRegisterID dst) {
    threeByteOpSimd("vpmovzxbd", VEX_PD, OP3_PMOVZXBD_VdqWdq, ESCAPE_38, src,
                    invalid_xmm, dst);
  }

  void vpmovzxbq_rr(XMMRegisterID src, XMMRegisterID dst) {
    threeByteOpSimd("vpmovzxbq", VEX_PD, OP3_PMOVZXBQ_VdqWdq, ESCAPE_38, src,
                    invalid_xmm, dst);
  }

  void vpmovsxwd_rr(XMMRegisterID src, XMMRegisterID dst) {
    threeByteOpSimd("vpmovsxwd", VEX_PD, OP3_PMOVSXWD_VdqWdq, ESCAPE_38, src,
                    invalid_xmm, dst);
  }
  void vpmovsxwd_mr(int32_t offset, RegisterID base, XMMRegisterID dst) {
    threeByteOpSimd("vpmovsxwd", VEX_PD, OP3_PMOVSXWD_VdqWdq, ESCAPE_38, offset,
                    base, invalid_xmm, dst);
  }
  void vpmovsxwd_mr(int32_t offset, RegisterID base, RegisterID index,
                    int32_t scale, XMMRegisterID dst) {
    threeByteOpSimd("vpmovsxwd", VEX_PD, OP3_PMOVSXWD_VdqWdq, ESCAPE_38, offset,
                    base, index, scale, invalid_xmm, dst);
  }

  void vpmovzxwd_rr(XMMRegisterID src, XMMRegisterID dst) {
    threeByteOpSimd("vpmovzxwd", VEX_PD, OP3_PMOVZXWD_VdqWdq, ESCAPE_38, src,
                    invalid_xmm, dst);
  }
  void vpmovzxwd_mr(int32_t offset, RegisterID base, XMMRegisterID dst) {
    threeByteOpSimd("vpmovzxwd", VEX_PD, OP3_PMOVZXWD_VdqWdq, ESCAPE_38, offset,
                    base, invalid_xmm, dst);
  }
  void vpmovzxwd_mr(int32_t offset, RegisterID base, RegisterID index,
                    int32_t scale, XMMRegisterID dst) {
    threeByteOpSimd("vpmovzxwd", VEX_PD, OP3_PMOVZXWD_VdqWdq, ESCAPE_38, offset,
                    base, index, scale, invalid_xmm, dst);
  }

  void vpmovzxwq_rr(XMMRegisterID src, XMMRegisterID dst) {
    threeByteOpSimd("vpmovzxwq", VEX_PD, OP3_PMOVZXWQ_VdqWdq, ESCAPE_38, src,
                    invalid_xmm, dst);
  }

  void vpmovsxdq_rr(XMMRegisterID src, XMMRegisterID dst) {
    threeByteOpSimd("vpmovsxwd", VEX_PD, OP3_PMOVSXDQ_VdqWdq, ESCAPE_38, src,
                    invalid_xmm, dst);
  }
  void vpmovsxdq_mr(int32_t offset, RegisterID base, XMMRegisterID dst) {
    threeByteOpSimd("vpmovsxdq", VEX_PD, OP3_PMOVSXDQ_VdqWdq, ESCAPE_38, offset,
                    base, invalid_xmm, dst);
  }
  void vpmovsxdq_mr(int32_t offset, RegisterID base, RegisterID index,
                    int32_t scale, XMMRegisterID dst) {
    threeByteOpSimd("vpmovsxdq", VEX_PD, OP3_PMOVSXDQ_VdqWdq, ESCAPE_38, offset,
                    base, index, scale, invalid_xmm, dst);
  }

  void vpmovzxdq_rr(XMMRegisterID src, XMMRegisterID dst) {
    threeByteOpSimd("vpmovzxwd", VEX_PD, OP3_PMOVZXDQ_VdqWdq, ESCAPE_38, src,
                    invalid_xmm, dst);
  }
  void vpmovzxdq_mr(int32_t offset, RegisterID base, XMMRegisterID dst) {
    threeByteOpSimd("vpmovzxdq", VEX_PD, OP3_PMOVZXDQ_VdqWdq, ESCAPE_38, offset,
                    base, invalid_xmm, dst);
  }
  void vpmovzxdq_mr(int32_t offset, RegisterID base, RegisterID index,
                    int32_t scale, XMMRegisterID dst) {
    threeByteOpSimd("vpmovzxdq", VEX_PD, OP3_PMOVZXDQ_VdqWdq, ESCAPE_38, offset,
                    base, index, scale, invalid_xmm, dst);
  }

  void vphaddd_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    threeByteOpSimd("vphaddd", VEX_PD, OP3_PHADDD_VdqWdq, ESCAPE_38, src1, src0,
                    dst);
  }

  void vpalignr_irr(unsigned imm, XMMRegisterID src1, XMMRegisterID src0,
                    XMMRegisterID dst) {
    MOZ_ASSERT(imm < 32);
    threeByteOpImmSimd("vpalignr", VEX_PD, OP3_PALIGNR_VdqWdqIb, ESCAPE_3A, imm,
                       src1, src0, dst);
  }

  void vpunpcklbw_rr(XMMRegisterID src1, XMMRegisterID src0,
                     XMMRegisterID dst) {
    twoByteOpSimd("vpunpcklbw", VEX_PD, OP2_PUNPCKLBW_VdqWdq, src1, src0, dst);
  }
  void vpunpckhbw_rr(XMMRegisterID src1, XMMRegisterID src0,
                     XMMRegisterID dst) {
    twoByteOpSimd("vpunpckhbw", VEX_PD, OP2_PUNPCKHBW_VdqWdq, src1, src0, dst);
  }

  void vpunpckldq_rr(XMMRegisterID src1, XMMRegisterID src0,
                     XMMRegisterID dst) {
    twoByteOpSimd("vpunpckldq", VEX_PD, OP2_PUNPCKLDQ_VdqWdq, src1, src0, dst);
  }
  void vpunpckldq_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                     XMMRegisterID dst) {
    twoByteOpSimd("vpunpckldq", VEX_PD, OP2_PUNPCKLDQ_VdqWdq, offset, base,
                  src0, dst);
  }
  void vpunpckldq_mr(const void* addr, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpunpckldq", VEX_PD, OP2_PUNPCKLDQ_VdqWdq, addr, src0, dst);
  }
  void vpunpcklqdq_rr(XMMRegisterID src1, XMMRegisterID src0,
                      XMMRegisterID dst) {
    twoByteOpSimd("vpunpcklqdq", VEX_PD, OP2_PUNPCKLQDQ_VdqWdq, src1, src0,
                  dst);
  }
  void vpunpcklqdq_mr(int32_t offset, RegisterID base, XMMRegisterID src0,
                      XMMRegisterID dst) {
    twoByteOpSimd("vpunpcklqdq", VEX_PD, OP2_PUNPCKLQDQ_VdqWdq, offset, base,
                  src0, dst);
  }
  void vpunpcklqdq_mr(const void* addr, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpunpcklqdq", VEX_PD, OP2_PUNPCKLQDQ_VdqWdq, addr, src0,
                  dst);
  }
  void vpunpckhdq_rr(XMMRegisterID src1, XMMRegisterID src0,
                     XMMRegisterID dst) {
    twoByteOpSimd("vpunpckhdq", VEX_PD, OP2_PUNPCKHDQ_VdqWdq, src1, src0, dst);
  }
  void vpunpckhqdq_rr(XMMRegisterID src1, XMMRegisterID src0,
                      XMMRegisterID dst) {
    twoByteOpSimd("vpunpckhqdq", VEX_PD, OP2_PUNPCKHQDQ_VdqWdq, src1, src0,
                  dst);
  }
  void vpunpcklwd_rr(XMMRegisterID src1, XMMRegisterID src0,
                     XMMRegisterID dst) {
    twoByteOpSimd("vpunpcklwd", VEX_PD, OP2_PUNPCKLWD_VdqWdq, src1, src0, dst);
  }
  void vpunpckhwd_rr(XMMRegisterID src1, XMMRegisterID src0,
                     XMMRegisterID dst) {
    twoByteOpSimd("vpunpckhwd", VEX_PD, OP2_PUNPCKHWD_VdqWdq, src1, src0, dst);
  }

  void vpaddq_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpaddq", VEX_PD, OP2_PADDQ_VdqWdq, src1, src0, dst);
  }
  void vpsubq_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpSimd("vpsubq", VEX_PD, OP2_PSUBQ_VdqWdq, src1, src0, dst);
  }

  void vbroadcastb_rr(XMMRegisterID src, XMMRegisterID dst) {
    threeByteOpSimd("vbroadcastb", VEX_PD, OP3_VBROADCASTB_VxWx, ESCAPE_38, src,
                    invalid_xmm, dst);
  }
  void vbroadcastb_mr(int32_t offset, RegisterID base, XMMRegisterID dst) {
    threeByteOpSimd("vbroadcastb", VEX_PD, OP3_VBROADCASTB_VxWx, ESCAPE_38,
                    offset, base, invalid_xmm, dst);
  }
  void vbroadcastb_mr(int32_t offset, RegisterID base, RegisterID index,
                      int32_t scale, XMMRegisterID dst) {
    threeByteOpSimd("vbroadcastb", VEX_PD, OP3_VBROADCASTB_VxWx, ESCAPE_38,
                    offset, base, index, scale, invalid_xmm, dst);
  }
  void vbroadcastw_rr(XMMRegisterID src, XMMRegisterID dst) {
    threeByteOpSimd("vbroadcastw", VEX_PD, OP3_VBROADCASTW_VxWx, ESCAPE_38, src,
                    invalid_xmm, dst);
  }
  void vbroadcastw_mr(int32_t offset, RegisterID base, XMMRegisterID dst) {
    threeByteOpSimd("vbroadcastw", VEX_PD, OP3_VBROADCASTW_VxWx, ESCAPE_38,
                    offset, base, invalid_xmm, dst);
  }
  void vbroadcastw_mr(int32_t offset, RegisterID base, RegisterID index,
                      int32_t scale, XMMRegisterID dst) {
    threeByteOpSimd("vbroadcastw", VEX_PD, OP3_VBROADCASTW_VxWx, ESCAPE_38,
                    offset, base, index, scale, invalid_xmm, dst);
  }
  void vbroadcastd_rr(XMMRegisterID src, XMMRegisterID dst) {
    threeByteOpSimd("vbroadcastd", VEX_PD, OP3_VBROADCASTD_VxWx, ESCAPE_38, src,
                    invalid_xmm, dst);
  }
  void vbroadcastd_mr(int32_t offset, RegisterID base, XMMRegisterID dst) {
    threeByteOpSimd("vbroadcastd", VEX_PD, OP3_VBROADCASTD_VxWx, ESCAPE_38,
                    offset, base, invalid_xmm, dst);
  }
  void vbroadcastd_mr(int32_t offset, RegisterID base, RegisterID index,
                      int32_t scale, XMMRegisterID dst) {
    threeByteOpSimd("vbroadcastd", VEX_PD, OP3_VBROADCASTD_VxWx, ESCAPE_38,
                    offset, base, index, scale, invalid_xmm, dst);
  }
  void vbroadcastq_rr(XMMRegisterID src, XMMRegisterID dst) {
    threeByteOpSimd("vbroadcastq", VEX_PD, OP3_VBROADCASTQ_VxWx, ESCAPE_38, src,
                    invalid_xmm, dst);
  }
  void vbroadcastq_mr(int32_t offset, RegisterID base, XMMRegisterID dst) {
    threeByteOpSimd("vbroadcastq", VEX_PD, OP3_VBROADCASTQ_VxWx, ESCAPE_38,
                    offset, base, invalid_xmm, dst);
  }
  void vbroadcastq_mr(int32_t offset, RegisterID base, RegisterID index,
                      int32_t scale, XMMRegisterID dst) {
    threeByteOpSimd("vbroadcastq", VEX_PD, OP3_VBROADCASTQ_VxWx, ESCAPE_38,
                    offset, base, index, scale, invalid_xmm, dst);
  }
  void vbroadcastss_rr(XMMRegisterID src, XMMRegisterID dst) {
    threeByteOpSimd("vbroadcastss", VEX_PD, OP3_VBROADCASTSS_VxWd, ESCAPE_38,
                    src, invalid_xmm, dst);
  }
  void vbroadcastss_mr(int32_t offset, RegisterID base, XMMRegisterID dst) {
    threeByteOpSimd("vbroadcastss", VEX_PD, OP3_VBROADCASTSS_VxWd, ESCAPE_38,
                    offset, base, invalid_xmm, dst);
  }
  void vbroadcastss_mr(int32_t offset, RegisterID base, RegisterID index,
                       int32_t scale, XMMRegisterID dst) {
    threeByteOpSimd("vbroadcastss", VEX_PD, OP3_VBROADCASTSS_VxWd, ESCAPE_38,
                    offset, base, index, scale, invalid_xmm, dst);
  }

  // BMI instructions:

  void sarxl_rrr(RegisterID src, RegisterID shift, RegisterID dst) {
    spew("sarxl      %s, %s, %s", GPReg32Name(src), GPReg32Name(shift),
         GPReg32Name(dst));

    RegisterID rm = src;
    XMMRegisterID src0 = static_cast<XMMRegisterID>(shift);
    int reg = dst;
    m_formatter.threeByteOpVex(VEX_SS /* = F3 */, OP3_SARX_GyEyBy, ESCAPE_38,
                               rm, src0, reg);
  }

  void shlxl_rrr(RegisterID src, RegisterID shift, RegisterID dst) {
    spew("shlxl      %s, %s, %s", GPReg32Name(src), GPReg32Name(shift),
         GPReg32Name(dst));

    RegisterID rm = src;
    XMMRegisterID src0 = static_cast<XMMRegisterID>(shift);
    int reg = dst;
    m_formatter.threeByteOpVex(VEX_PD /* = 66 */, OP3_SHLX_GyEyBy, ESCAPE_38,
                               rm, src0, reg);
  }

  void shrxl_rrr(RegisterID src, RegisterID shift, RegisterID dst) {
    spew("shrxl      %s, %s, %s", GPReg32Name(src), GPReg32Name(shift),
         GPReg32Name(dst));

    RegisterID rm = src;
    XMMRegisterID src0 = static_cast<XMMRegisterID>(shift);
    int reg = dst;
    m_formatter.threeByteOpVex(VEX_SD /* = F2 */, OP3_SHRX_GyEyBy, ESCAPE_38,
                               rm, src0, reg);
  }

  // FMA instructions:

  void vfmadd231ps_rrr(XMMRegisterID src1, XMMRegisterID src0,
                       XMMRegisterID dst) {
    spew("vfmadd213ps %s, %s, %s", XMMRegName(src1), XMMRegName(src0),
         XMMRegName(dst));

    m_formatter.threeByteOpVex(VEX_PD, OP3_VFMADD231PS_VxHxWx, ESCAPE_38,
                               (RegisterID)src1, src0, (RegisterID)dst);
  }

  void vfnmadd231ps_rrr(XMMRegisterID src1, XMMRegisterID src0,
                        XMMRegisterID dst) {
    spew("vfnmadd213ps %s, %s, %s", XMMRegName(src1), XMMRegName(src0),
         XMMRegName(dst));

    m_formatter.threeByteOpVex(VEX_PD, OP3_VFNMADD231PS_VxHxWx, ESCAPE_38,
                               (RegisterID)src1, src0, (RegisterID)dst);
  }

  void vfmadd231pd_rrr(XMMRegisterID src1, XMMRegisterID src0,
                       XMMRegisterID dst) {
    spew("vfmadd213pd %s, %s, %s", XMMRegName(src1), XMMRegName(src0),
         XMMRegName(dst));

    m_formatter.threeByteOpVex64(VEX_PD, OP3_VFMADD231PD_VxHxWx, ESCAPE_38,
                                 (RegisterID)src1, src0, (RegisterID)dst);
  }

  void vfnmadd231pd_rrr(XMMRegisterID src1, XMMRegisterID src0,
                        XMMRegisterID dst) {
    spew("vfnmadd213pd %s, %s, %s", XMMRegName(src1), XMMRegName(src0),
         XMMRegName(dst));

    m_formatter.threeByteOpVex64(VEX_PD, OP3_VFNMADD231PD_VxHxWx, ESCAPE_38,
                                 (RegisterID)src1, src0, (RegisterID)dst);
  }

  // Misc instructions:

  void int3() {
    spew("int3");
    m_formatter.oneByteOp(OP_INT3);
  }

  void ud2() {
    spew("ud2");
    m_formatter.twoByteOp(OP2_UD2);
  }

  void ret() {
    spew("ret");
    m_formatter.oneByteOp(OP_RET);
  }

  void ret_i(int32_t imm) {
    spew("ret        $%d", imm);
    m_formatter.oneByteOp(OP_RET_Iz);
    m_formatter.immediate16u(imm);
  }

  void lfence() {
    spew("lfence");
    m_formatter.twoByteOp(OP_FENCE, (RegisterID)0, 0b101);
  }
  void mfence() {
    spew("mfence");
    m_formatter.twoByteOp(OP_FENCE, (RegisterID)0, 0b110);
  }

  // Assembler admin methods:

  JmpDst label() {
    JmpDst r = JmpDst(m_formatter.size());
    spew(".set .Llabel%d, .", r.offset());
    return r;
  }

  size_t currentOffset() const { return m_formatter.size(); }

  static JmpDst labelFor(JmpSrc jump, intptr_t offset = 0) {
    return JmpDst(jump.offset() + offset);
  }

  void haltingAlign(int alignment) {
    spew(".balign %d, 0x%x   # hlt", alignment, unsigned(OP_HLT));
    while (!m_formatter.isAligned(alignment)) {
      m_formatter.oneByteOp(OP_HLT);
    }
  }

  void nopAlign(int alignment) {
    spew(".balign %d", alignment);

    int remainder = m_formatter.size() % alignment;
    if (remainder > 0) {
      insert_nop(alignment - remainder);
    }
  }

  void jumpTablePointer(uintptr_t ptr) {
#ifdef JS_CODEGEN_X64
    spew(".quad 0x%" PRIxPTR, ptr);
#else
    spew(".int 0x%" PRIxPTR, ptr);
#endif
    m_formatter.jumpTablePointer(ptr);
  }

  void doubleConstant(double d) {
    spew(".double %.16g", d);
    m_formatter.doubleConstant(d);
  }
  void floatConstant(float f) {
    spew(".float %.16g", f);
    m_formatter.floatConstant(f);
  }

  void simd128Constant(const void* data) {
    const uint32_t* dw = reinterpret_cast<const uint32_t*>(data);
    spew(".int 0x%08x,0x%08x,0x%08x,0x%08x", dw[0], dw[1], dw[2], dw[3]);
    MOZ_ASSERT(m_formatter.isAligned(16));
    m_formatter.simd128Constant(data);
  }

  void int32Constant(int32_t i) {
    spew(".int %d", i);
    m_formatter.int32Constant(i);
  }
  void int64Constant(int64_t i) {
    spew(".quad %lld", (long long)i);
    m_formatter.int64Constant(i);
  }

  // Linking & patching:

  void assertValidJmpSrc(JmpSrc src) {
    // The target offset is stored at offset - 4.
    MOZ_RELEASE_ASSERT(src.offset() > int32_t(sizeof(int32_t)));
    MOZ_RELEASE_ASSERT(size_t(src.offset()) <= size());
  }

  bool nextJump(const JmpSrc& from, JmpSrc* next) {
    // Sanity check - if the assembler has OOM'd, it will start overwriting
    // its internal buffer and thus our links could be garbage.
    if (oom()) {
      return false;
    }

    assertValidJmpSrc(from);
    MOZ_ASSERT(from.trailing() == 0);

    const unsigned char* code = m_formatter.data();
    int32_t offset = GetInt32(code + from.offset());
    if (offset == -1) {
      return false;
    }

    MOZ_RELEASE_ASSERT(size_t(offset) < size(), "nextJump bogus offset");

    *next = JmpSrc(offset);
    return true;
  }
  void setNextJump(const JmpSrc& from, const JmpSrc& to) {
    // Sanity check - if the assembler has OOM'd, it will start overwriting
    // its internal buffer and thus our links could be garbage.
    if (oom()) {
      return;
    }

    assertValidJmpSrc(from);
    MOZ_ASSERT(from.trailing() == 0);
    MOZ_RELEASE_ASSERT(to.offset() == -1 || size_t(to.offset()) <= size());

    unsigned char* code = m_formatter.data();
    SetInt32(code + from.offset(), to.offset());
  }

  void linkJump(JmpSrc from, JmpDst to) {
    MOZ_ASSERT(from.offset() != -1);
    MOZ_ASSERT(to.offset() != -1);

    // Sanity check - if the assembler has OOM'd, it will start overwriting
    // its internal buffer and thus our links could be garbage.
    if (oom()) {
      return;
    }

    assertValidJmpSrc(from);
    MOZ_RELEASE_ASSERT(size_t(to.offset()) <= size());

    spew(".set .Lfrom%d, .Llabel%d", from.offset(), to.offset());
    unsigned char* code = m_formatter.data();
    SetRel32(code + from.offset(), code + to.offset(), from.trailing());
  }

  void executableCopy(void* dst) {
    const unsigned char* src = m_formatter.buffer();
    memcpy(dst, src, size());
  }
  [[nodiscard]] bool appendRawCode(const uint8_t* code, size_t numBytes) {
    return m_formatter.append(code, numBytes);
  }

  // `offset` is the instruction offset at the end of the instruction.
  void addToPCRel4(uint32_t offset, int32_t bias) {
    unsigned char* code = m_formatter.data();
    SetInt32(code + offset, GetInt32(code + offset) + bias);
  }

 protected:
  static bool CAN_SIGN_EXTEND_8_32(int32_t value) {
    return value == (int32_t)(int8_t)value;
  }
  static bool CAN_SIGN_EXTEND_16_32(int32_t value) {
    return value == (int32_t)(int16_t)value;
  }
  static bool CAN_ZERO_EXTEND_8_32(int32_t value) {
    return value == (int32_t)(uint8_t)value;
  }
  static bool CAN_ZERO_EXTEND_8H_32(int32_t value) {
    return value == (value & 0xff00);
  }
  static bool CAN_ZERO_EXTEND_16_32(int32_t value) {
    return value == (int32_t)(uint16_t)value;
  }
  static bool CAN_ZERO_EXTEND_32_64(int32_t value) { return value >= 0; }

  // Methods for encoding SIMD instructions via either legacy SSE encoding or
  // VEX encoding.

  bool useLegacySSEEncoding(XMMRegisterID src0, XMMRegisterID dst) {
    // If we don't have AVX or it's disabled, use the legacy SSE encoding.
    if (!useVEX_) {
      MOZ_ASSERT(
          src0 == invalid_xmm || src0 == dst,
          "Legacy SSE (pre-AVX) encoding requires the output register to be "
          "the same as the src0 input register");
      return true;
    }

    // If src0 is the same as the output register, we might as well use
    // the legacy SSE encoding, since it is smaller. However, this is only
    // beneficial as long as we're not using ymm registers anywhere.
    return src0 == dst;
  }

  bool useLegacySSEEncodingForVblendv(XMMRegisterID mask, XMMRegisterID src0,
                                      XMMRegisterID dst) {
    // Similar to useLegacySSEEncoding, but for vblendv the Legacy SSE
    // encoding also requires the mask to be in xmm0.

    if (!useVEX_) {
      MOZ_ASSERT(
          src0 == dst,
          "Legacy SSE (pre-AVX) encoding requires the output register to be "
          "the same as the src0 input register");
      MOZ_ASSERT(
          mask == xmm0,
          "Legacy SSE (pre-AVX) encoding for blendv requires the mask to be "
          "in xmm0");
      return true;
    }

    return src0 == dst && mask == xmm0;
  }

  bool useLegacySSEEncodingAlways() { return !useVEX_; }

  const char* legacySSEOpName(const char* name) {
    MOZ_ASSERT(name[0] == 'v');
    return name + 1;
  }

  void twoByteOpSimd(const char* name, VexOperandType ty,
                     TwoByteOpcodeID opcode, XMMRegisterID rm,
                     XMMRegisterID src0, XMMRegisterID dst) {
    if (useLegacySSEEncoding(src0, dst)) {
      if (IsXMMReversedOperands(opcode)) {
        spew("%-11s%s, %s", legacySSEOpName(name), XMMRegName(dst),
             XMMRegName(rm));
      } else {
        spew("%-11s%s, %s", legacySSEOpName(name), XMMRegName(rm),
             XMMRegName(dst));
      }
      m_formatter.legacySSEPrefix(ty);
      m_formatter.twoByteOp(opcode, (RegisterID)rm, dst);
      return;
    }

    if (src0 == invalid_xmm) {
      if (IsXMMReversedOperands(opcode)) {
        spew("%-11s%s, %s", name, XMMRegName(dst), XMMRegName(rm));
      } else {
        spew("%-11s%s, %s", name, XMMRegName(rm), XMMRegName(dst));
      }
    } else {
      spew("%-11s%s, %s, %s", name, XMMRegName(rm), XMMRegName(src0),
           XMMRegName(dst));
    }
    m_formatter.twoByteOpVex(ty, opcode, (RegisterID)rm, src0, dst);
  }

  void twoByteOpImmSimd(const char* name, VexOperandType ty,
                        TwoByteOpcodeID opcode, uint32_t imm, XMMRegisterID rm,
                        XMMRegisterID src0, XMMRegisterID dst) {
    if (useLegacySSEEncoding(src0, dst)) {
      spew("%-11s$0x%x, %s, %s", legacySSEOpName(name), imm, XMMRegName(rm),
           XMMRegName(dst));
      m_formatter.legacySSEPrefix(ty);
      m_formatter.twoByteOp(opcode, (RegisterID)rm, dst);
      m_formatter.immediate8u(imm);
      return;
    }

    if (src0 == invalid_xmm) {
      spew("%-11s$0x%x, %s, %s", name, imm, XMMRegName(rm), XMMRegName(dst));
    } else {
      spew("%-11s$0x%x, %s, %s, %s", name, imm, XMMRegName(rm),
           XMMRegName(src0), XMMRegName(dst));
    }
    m_formatter.twoByteOpVex(ty, opcode, (RegisterID)rm, src0, dst);
    m_formatter.immediate8u(imm);
  }

  void twoByteOpSimd(const char* name, VexOperandType ty,
                     TwoByteOpcodeID opcode, int32_t offset, RegisterID base,
                     XMMRegisterID src0, XMMRegisterID dst) {
    if (useLegacySSEEncoding(src0, dst)) {
      if (IsXMMReversedOperands(opcode)) {
        spew("%-11s%s, " MEM_ob, legacySSEOpName(name), XMMRegName(dst),
             ADDR_ob(offset, base));
      } else {
        spew("%-11s" MEM_ob ", %s", legacySSEOpName(name),
             ADDR_ob(offset, base), XMMRegName(dst));
      }
      m_formatter.legacySSEPrefix(ty);
      m_formatter.twoByteOp(opcode, offset, base, dst);
      return;
    }

    if (src0 == invalid_xmm) {
      if (IsXMMReversedOperands(opcode)) {
        spew("%-11s%s, " MEM_ob, name, XMMRegName(dst), ADDR_ob(offset, base));
      } else {
        spew("%-11s" MEM_ob ", %s", name, ADDR_ob(offset, base),
             XMMRegName(dst));
      }
    } else {
      spew("%-11s" MEM_ob ", %s, %s", name, ADDR_ob(offset, base),
           XMMRegName(src0), XMMRegName(dst));
    }
    m_formatter.twoByteOpVex(ty, opcode, offset, base, src0, dst);
  }

  void twoByteOpSimd_disp32(const char* name, VexOperandType ty,
                            TwoByteOpcodeID opcode, int32_t offset,
                            RegisterID base, XMMRegisterID src0,
                            XMMRegisterID dst) {
    if (useLegacySSEEncoding(src0, dst)) {
      if (IsXMMReversedOperands(opcode)) {
        spew("%-11s%s, " MEM_o32b, legacySSEOpName(name), XMMRegName(dst),
             ADDR_o32b(offset, base));
      } else {
        spew("%-11s" MEM_o32b ", %s", legacySSEOpName(name),
             ADDR_o32b(offset, base), XMMRegName(dst));
      }
      m_formatter.legacySSEPrefix(ty);
      m_formatter.twoByteOp_disp32(opcode, offset, base, dst);
      return;
    }

    if (src0 == invalid_xmm) {
      if (IsXMMReversedOperands(opcode)) {
        spew("%-11s%s, " MEM_o32b, name, XMMRegName(dst),
             ADDR_o32b(offset, base));
      } else {
        spew("%-11s" MEM_o32b ", %s", name, ADDR_o32b(offset, base),
             XMMRegName(dst));
      }
    } else {
      spew("%-11s" MEM_o32b ", %s, %s", name, ADDR_o32b(offset, base),
           XMMRegName(src0), XMMRegName(dst));
    }
    m_formatter.twoByteOpVex_disp32(ty, opcode, offset, base, src0, dst);
  }

  void twoByteOpImmSimd(const char* name, VexOperandType ty,
                        TwoByteOpcodeID opcode, uint32_t imm, int32_t offset,
                        RegisterID base, XMMRegisterID src0,
                        XMMRegisterID dst) {
    if (useLegacySSEEncoding(src0, dst)) {
      spew("%-11s$0x%x, " MEM_ob ", %s", legacySSEOpName(name), imm,
           ADDR_ob(offset, base), XMMRegName(dst));
      m_formatter.legacySSEPrefix(ty);
      m_formatter.twoByteOp(opcode, offset, base, dst);
      m_formatter.immediate8u(imm);
      return;
    }

    spew("%-11s$0x%x, " MEM_ob ", %s, %s", name, imm, ADDR_ob(offset, base),
         XMMRegName(src0), XMMRegName(dst));
    m_formatter.twoByteOpVex(ty, opcode, offset, base, src0, dst);
    m_formatter.immediate8u(imm);
  }

  void twoByteOpSimd(const char* name, VexOperandType ty,
                     TwoByteOpcodeID opcode, int32_t offset, RegisterID base,
                     RegisterID index, int scale, XMMRegisterID src0,
                     XMMRegisterID dst) {
    if (useLegacySSEEncoding(src0, dst)) {
      if (IsXMMReversedOperands(opcode)) {
        spew("%-11s%s, " MEM_obs, legacySSEOpName(name), XMMRegName(dst),
             ADDR_obs(offset, base, index, scale));
      } else {
        spew("%-11s" MEM_obs ", %s", legacySSEOpName(name),
             ADDR_obs(offset, base, index, scale), XMMRegName(dst));
      }
      m_formatter.legacySSEPrefix(ty);
      m_formatter.twoByteOp(opcode, offset, base, index, scale, dst);
      return;
    }

    if (src0 == invalid_xmm) {
      if (IsXMMReversedOperands(opcode)) {
        spew("%-11s%s, " MEM_obs, name, XMMRegName(dst),
             ADDR_obs(offset, base, index, scale));
      } else {
        spew("%-11s" MEM_obs ", %s", name, ADDR_obs(offset, base, index, scale),
             XMMRegName(dst));
      }
    } else {
      spew("%-11s" MEM_obs ", %s, %s", name,
           ADDR_obs(offset, base, index, scale), XMMRegName(src0),
           XMMRegName(dst));
    }
    m_formatter.twoByteOpVex(ty, opcode, offset, base, index, scale, src0, dst);
  }

  void twoByteOpSimd(const char* name, VexOperandType ty,
                     TwoByteOpcodeID opcode, const void* address,
                     XMMRegisterID src0, XMMRegisterID dst) {
    if (useLegacySSEEncoding(src0, dst)) {
      if (IsXMMReversedOperands(opcode)) {
        spew("%-11s%s, %p", legacySSEOpName(name), XMMRegName(dst), address);
      } else {
        spew("%-11s%p, %s", legacySSEOpName(name), address, XMMRegName(dst));
      }
      m_formatter.legacySSEPrefix(ty);
      m_formatter.twoByteOp(opcode, address, dst);
      return;
    }

    if (src0 == invalid_xmm) {
      if (IsXMMReversedOperands(opcode)) {
        spew("%-11s%s, %p", name, XMMRegName(dst), address);
      } else {
        spew("%-11s%p, %s", name, address, XMMRegName(dst));
      }
    } else {
      spew("%-11s%p, %s, %s", name, address, XMMRegName(src0), XMMRegName(dst));
    }
    m_formatter.twoByteOpVex(ty, opcode, address, src0, dst);
  }

  void twoByteOpImmSimd(const char* name, VexOperandType ty,
                        TwoByteOpcodeID opcode, uint32_t imm,
                        const void* address, XMMRegisterID src0,
                        XMMRegisterID dst) {
    if (useLegacySSEEncoding(src0, dst)) {
      spew("%-11s$0x%x, %p, %s", legacySSEOpName(name), imm, address,
           XMMRegName(dst));
      m_formatter.legacySSEPrefix(ty);
      m_formatter.twoByteOp(opcode, address, dst);
      m_formatter.immediate8u(imm);
      return;
    }

    spew("%-11s$0x%x, %p, %s, %s", name, imm, address, XMMRegName(src0),
         XMMRegName(dst));
    m_formatter.twoByteOpVex(ty, opcode, address, src0, dst);
    m_formatter.immediate8u(imm);
  }

  void twoByteOpInt32Simd(const char* name, VexOperandType ty,
                          TwoByteOpcodeID opcode, RegisterID rm,
                          XMMRegisterID src0, XMMRegisterID dst) {
    if (useLegacySSEEncoding(src0, dst)) {
      if (IsXMMReversedOperands(opcode)) {
        spew("%-11s%s, %s", legacySSEOpName(name), XMMRegName(dst),
             GPReg32Name(rm));
      } else {
        spew("%-11s%s, %s", legacySSEOpName(name), GPReg32Name(rm),
             XMMRegName(dst));
      }
      m_formatter.legacySSEPrefix(ty);
      m_formatter.twoByteOp(opcode, rm, dst);
      return;
    }

    if (src0 == invalid_xmm) {
      if (IsXMMReversedOperands(opcode)) {
        spew("%-11s%s, %s", name, XMMRegName(dst), GPReg32Name(rm));
      } else {
        spew("%-11s%s, %s", name, GPReg32Name(rm), XMMRegName(dst));
      }
    } else {
      spew("%-11s%s, %s, %s", name, GPReg32Name(rm), XMMRegName(src0),
           XMMRegName(dst));
    }
    m_formatter.twoByteOpVex(ty, opcode, rm, src0, dst);
  }

  void twoByteOpSimdInt32(const char* name, VexOperandType ty,
                          TwoByteOpcodeID opcode, XMMRegisterID rm,
                          RegisterID dst) {
    if (useLegacySSEEncodingAlways()) {
      if (IsXMMReversedOperands(opcode)) {
        spew("%-11s%s, %s", legacySSEOpName(name), GPReg32Name(dst),
             XMMRegName(rm));
      } else if (opcode == OP2_MOVD_EdVd) {
        spew("%-11s%s, %s", legacySSEOpName(name),
             XMMRegName((XMMRegisterID)dst), GPReg32Name((RegisterID)rm));
      } else {
        spew("%-11s%s, %s", legacySSEOpName(name), XMMRegName(rm),
             GPReg32Name(dst));
      }
      m_formatter.legacySSEPrefix(ty);
      m_formatter.twoByteOp(opcode, (RegisterID)rm, dst);
      return;
    }

    if (IsXMMReversedOperands(opcode)) {
      spew("%-11s%s, %s", name, GPReg32Name(dst), XMMRegName(rm));
    } else if (opcode == OP2_MOVD_EdVd) {
      spew("%-11s%s, %s", name, XMMRegName((XMMRegisterID)dst),
           GPReg32Name((RegisterID)rm));
    } else {
      spew("%-11s%s, %s", name, XMMRegName(rm), GPReg32Name(dst));
    }
    m_formatter.twoByteOpVex(ty, opcode, (RegisterID)rm, invalid_xmm, dst);
  }

  void twoByteOpImmSimdInt32(const char* name, VexOperandType ty,
                             TwoByteOpcodeID opcode, uint32_t imm,
                             XMMRegisterID rm, RegisterID dst) {
    if (useLegacySSEEncodingAlways()) {
      spew("%-11s$0x%x, %s, %s", legacySSEOpName(name), imm, XMMRegName(rm),
           GPReg32Name(dst));
      m_formatter.legacySSEPrefix(ty);
      m_formatter.twoByteOp(opcode, (RegisterID)rm, dst);
      m_formatter.immediate8u(imm);
      return;
    }

    spew("%-11s$0x%x, %s, %s", name, imm, XMMRegName(rm), GPReg32Name(dst));
    m_formatter.twoByteOpVex(ty, opcode, (RegisterID)rm, invalid_xmm, dst);
    m_formatter.immediate8u(imm);
  }

  void twoByteOpImmInt32Simd(const char* name, VexOperandType ty,
                             TwoByteOpcodeID opcode, uint32_t imm,
                             RegisterID rm, XMMRegisterID src0,
                             XMMRegisterID dst) {
    if (useLegacySSEEncodingAlways()) {
      spew("%-11s$0x%x, %s, %s", legacySSEOpName(name), imm, GPReg32Name(rm),
           XMMRegName(dst));
      m_formatter.legacySSEPrefix(ty);
      m_formatter.twoByteOp(opcode, rm, dst);
      m_formatter.immediate8u(imm);
      return;
    }

    spew("%-11s$0x%x, %s, %s", name, imm, GPReg32Name(rm), XMMRegName(dst));
    m_formatter.twoByteOpVex(ty, opcode, rm, src0, dst);
    m_formatter.immediate8u(imm);
  }

  void twoByteOpImmInt32Simd(const char* name, VexOperandType ty,
                             TwoByteOpcodeID opcode, uint32_t imm,
                             int32_t offset, RegisterID base,
                             XMMRegisterID src0, XMMRegisterID dst) {
    if (useLegacySSEEncodingAlways()) {
      spew("%-11s$0x%x, " MEM_ob ", %s", legacySSEOpName(name), imm,
           ADDR_ob(offset, base), XMMRegName(dst));
      m_formatter.legacySSEPrefix(ty);
      m_formatter.twoByteOp(opcode, offset, base, dst);
      m_formatter.immediate8u(imm);
      return;
    }

    spew("%-11s$0x%x, " MEM_ob ", %s, %s", name, imm, ADDR_ob(offset, base),
         XMMRegName(src0), XMMRegName(dst));
    m_formatter.twoByteOpVex(ty, opcode, offset, base, src0, dst);
    m_formatter.immediate8u(imm);
  }

  void twoByteOpImmInt32Simd(const char* name, VexOperandType ty,
                             TwoByteOpcodeID opcode, uint32_t imm,
                             int32_t offset, RegisterID base, RegisterID index,
                             int scale, XMMRegisterID src0, XMMRegisterID dst) {
    if (useLegacySSEEncodingAlways()) {
      spew("%-11s$0x%x, " MEM_obs ", %s", legacySSEOpName(name), imm,
           ADDR_obs(offset, base, index, scale), XMMRegName(dst));
      m_formatter.legacySSEPrefix(ty);
      m_formatter.twoByteOp(opcode, offset, base, index, scale, dst);
      m_formatter.immediate8u(imm);
      return;
    }

    spew("%-11s$0x%x, " MEM_obs ", %s, %s", name, imm,
         ADDR_obs(offset, base, index, scale), XMMRegName(src0),
         XMMRegName(dst));
    m_formatter.twoByteOpVex(ty, opcode, offset, base, index, scale, src0, dst);
    m_formatter.immediate8u(imm);
  }

  void twoByteOpSimdFlags(const char* name, VexOperandType ty,
                          TwoByteOpcodeID opcode, XMMRegisterID rm,
                          XMMRegisterID reg) {
    if (useLegacySSEEncodingAlways()) {
      spew("%-11s%s, %s", legacySSEOpName(name), XMMRegName(rm),
           XMMRegName(reg));
      m_formatter.legacySSEPrefix(ty);
      m_formatter.twoByteOp(opcode, (RegisterID)rm, reg);
      return;
    }

    spew("%-11s%s, %s", name, XMMRegName(rm), XMMRegName(reg));
    m_formatter.twoByteOpVex(ty, opcode, (RegisterID)rm, invalid_xmm,
                             (XMMRegisterID)reg);
  }

  void twoByteOpSimdFlags(const char* name, VexOperandType ty,
                          TwoByteOpcodeID opcode, int32_t offset,
                          RegisterID base, XMMRegisterID reg) {
    if (useLegacySSEEncodingAlways()) {
      spew("%-11s" MEM_ob ", %s", legacySSEOpName(name), ADDR_ob(offset, base),
           XMMRegName(reg));
      m_formatter.legacySSEPrefix(ty);
      m_formatter.twoByteOp(opcode, offset, base, reg);
      return;
    }

    spew("%-11s" MEM_ob ", %s", name, ADDR_ob(offset, base), XMMRegName(reg));
    m_formatter.twoByteOpVex(ty, opcode, offset, base, invalid_xmm,
                             (XMMRegisterID)reg);
  }

  void threeByteOpSimd(const char* name, VexOperandType ty,
                       ThreeByteOpcodeID opcode, ThreeByteEscape escape,
                       XMMRegisterID rm, XMMRegisterID src0,
                       XMMRegisterID dst) {
    if (useLegacySSEEncoding(src0, dst)) {
      spew("%-11s%s, %s", legacySSEOpName(name), XMMRegName(rm),
           XMMRegName(dst));
      m_formatter.legacySSEPrefix(ty);
      m_formatter.threeByteOp(opcode, escape, (RegisterID)rm, dst);
      return;
    }

    if (src0 == invalid_xmm) {
      spew("%-11s%s, %s", name, XMMRegName(rm), XMMRegName(dst));
    } else {
      spew("%-11s%s, %s, %s", name, XMMRegName(rm), XMMRegName(src0),
           XMMRegName(dst));
    }
    m_formatter.threeByteOpVex(ty, opcode, escape, (RegisterID)rm, src0, dst);
  }

  void threeByteOpImmSimd(const char* name, VexOperandType ty,
                          ThreeByteOpcodeID opcode, ThreeByteEscape escape,
                          uint32_t imm, XMMRegisterID rm, XMMRegisterID src0,
                          XMMRegisterID dst) {
    if (useLegacySSEEncoding(src0, dst)) {
      spew("%-11s$0x%x, %s, %s", legacySSEOpName(name), imm, XMMRegName(rm),
           XMMRegName(dst));
      m_formatter.legacySSEPrefix(ty);
      m_formatter.threeByteOp(opcode, escape, (RegisterID)rm, dst);
      m_formatter.immediate8u(imm);
      return;
    }

    if (src0 == invalid_xmm) {
      spew("%-11s$0x%x, %s, %s", name, imm, XMMRegName(rm), XMMRegName(dst));
    } else {
      spew("%-11s$0x%x, %s, %s, %s", name, imm, XMMRegName(rm),
           XMMRegName(src0), XMMRegName(dst));
    }
    m_formatter.threeByteOpVex(ty, opcode, escape, (RegisterID)rm, src0, dst);
    m_formatter.immediate8u(imm);
  }

  void threeByteOpSimd(const char* name, VexOperandType ty,
                       ThreeByteOpcodeID opcode, ThreeByteEscape escape,
                       int32_t offset, RegisterID base, XMMRegisterID src0,
                       XMMRegisterID dst) {
    if (useLegacySSEEncoding(src0, dst)) {
      spew("%-11s" MEM_ob ", %s", legacySSEOpName(name), ADDR_ob(offset, base),
           XMMRegName(dst));
      m_formatter.legacySSEPrefix(ty);
      m_formatter.threeByteOp(opcode, escape, offset, base, dst);
      return;
    }

    if (src0 == invalid_xmm) {
      spew("%-11s" MEM_ob ", %s", name, ADDR_ob(offset, base), XMMRegName(dst));
    } else {
      spew("%-11s" MEM_ob ", %s, %s", name, ADDR_ob(offset, base),
           XMMRegName(src0), XMMRegName(dst));
    }
    m_formatter.threeByteOpVex(ty, opcode, escape, offset, base, src0, dst);
  }

  void threeByteOpSimd(const char* name, VexOperandType ty,
                       ThreeByteOpcodeID opcode, ThreeByteEscape escape,
                       int32_t offset, RegisterID base, RegisterID index,
                       int32_t scale, XMMRegisterID src0, XMMRegisterID dst) {
    if (useLegacySSEEncoding(src0, dst)) {
      spew("%-11s" MEM_obs ", %s", legacySSEOpName(name),
           ADDR_obs(offset, base, index, scale), XMMRegName(dst));
      m_formatter.legacySSEPrefix(ty);
      m_formatter.threeByteOp(opcode, escape, offset, base, index, scale, dst);
      return;
    }

    if (src0 == invalid_xmm) {
      spew("%-11s" MEM_obs ", %s", name, ADDR_obs(offset, base, index, scale),
           XMMRegName(dst));
    } else {
      spew("%-11s" MEM_obs ", %s, %s", name,
           ADDR_obs(offset, base, index, scale), XMMRegName(src0),
           XMMRegName(dst));
    }
    m_formatter.threeByteOpVex(ty, opcode, escape, offset, base, index, scale,
                               src0, dst);
  }

  void threeByteOpImmSimd(const char* name, VexOperandType ty,
                          ThreeByteOpcodeID opcode, ThreeByteEscape escape,
                          uint32_t imm, int32_t offset, RegisterID base,
                          XMMRegisterID src0, XMMRegisterID dst) {
    if (useLegacySSEEncoding(src0, dst)) {
      spew("%-11s$0x%x, " MEM_ob ", %s", legacySSEOpName(name), imm,
           ADDR_ob(offset, base), XMMRegName(dst));
      m_formatter.legacySSEPrefix(ty);
      m_formatter.threeByteOp(opcode, escape, offset, base, dst);
      m_formatter.immediate8u(imm);
      return;
    }

    if (src0 == invalid_xmm) {
      spew("%-11s$0x%x, " MEM_ob ", %s", name, imm, ADDR_ob(offset, base),
           XMMRegName(dst));
    } else {
      spew("%-11s$0x%x, " MEM_ob ", %s, %s", name, imm, ADDR_ob(offset, base),
           XMMRegName(src0), XMMRegName(dst));
    }
    m_formatter.threeByteOpVex(ty, opcode, escape, offset, base, src0, dst);
    m_formatter.immediate8u(imm);
  }

  void threeByteOpImmSimd(const char* name, VexOperandType ty,
                          ThreeByteOpcodeID opcode, ThreeByteEscape escape,
                          uint32_t imm, int32_t offset, RegisterID base,
                          RegisterID index, int scale, XMMRegisterID src0,
                          XMMRegisterID dst) {
    if (useLegacySSEEncoding(src0, dst)) {
      spew("%-11s$0x%x, " MEM_obs ", %s", legacySSEOpName(name), imm,
           ADDR_obs(offset, base, index, scale), XMMRegName(dst));
      m_formatter.legacySSEPrefix(ty);
      m_formatter.threeByteOp(opcode, escape, offset, base, index, scale, dst);
      m_formatter.immediate8u(imm);
      return;
    }

    if (src0 == invalid_xmm) {
      spew("%-11s$0x%x, " MEM_obs ", %s", name, imm,
           ADDR_obs(offset, base, index, scale), XMMRegName(dst));
    } else {
      spew("%-11s$0x%x, " MEM_obs ", %s, %s", name, imm,
           ADDR_obs(offset, base, index, scale), XMMRegName(src0),
           XMMRegName(dst));
    }
    m_formatter.threeByteOpVex(ty, opcode, escape, offset, base, index, scale,
                               src0, dst);
    m_formatter.immediate8u(imm);
  }

  void threeByteOpSimd(const char* name, VexOperandType ty,
                       ThreeByteOpcodeID opcode, ThreeByteEscape escape,
                       const void* address, XMMRegisterID src0,
                       XMMRegisterID dst) {
    if (useLegacySSEEncoding(src0, dst)) {
      spew("%-11s%p, %s", legacySSEOpName(name), address, XMMRegName(dst));
      m_formatter.legacySSEPrefix(ty);
      m_formatter.threeByteOp(opcode, escape, address, dst);
      return;
    }

    if (src0 == invalid_xmm) {
      spew("%-11s%p, %s", name, address, XMMRegName(dst));
    } else {
      spew("%-11s%p, %s, %s", name, address, XMMRegName(src0), XMMRegName(dst));
    }
    m_formatter.threeByteOpVex(ty, opcode, escape, address, src0, dst);
  }

  void threeByteOpImmInt32Simd(const char* name, VexOperandType ty,
                               ThreeByteOpcodeID opcode, ThreeByteEscape escape,
                               uint32_t imm, RegisterID src1,
                               XMMRegisterID src0, XMMRegisterID dst) {
    if (useLegacySSEEncoding(src0, dst)) {
      spew("%-11s$0x%x, %s, %s", legacySSEOpName(name), imm, GPReg32Name(src1),
           XMMRegName(dst));
      m_formatter.legacySSEPrefix(ty);
      m_formatter.threeByteOp(opcode, escape, src1, dst);
      m_formatter.immediate8u(imm);
      return;
    }

    spew("%-11s$0x%x, %s, %s, %s", name, imm, GPReg32Name(src1),
         XMMRegName(src0), XMMRegName(dst));
    m_formatter.threeByteOpVex(ty, opcode, escape, src1, src0, dst);
    m_formatter.immediate8u(imm);
  }

  void threeByteOpImmInt32Simd(const char* name, VexOperandType ty,
                               ThreeByteOpcodeID opcode, ThreeByteEscape escape,
                               uint32_t imm, int32_t offset, RegisterID base,
                               XMMRegisterID src0, XMMRegisterID dst) {
    if (useLegacySSEEncoding(src0, dst)) {
      spew("%-11s$0x%x, " MEM_ob ", %s", legacySSEOpName(name), imm,
           ADDR_ob(offset, base), XMMRegName(dst));
      m_formatter.legacySSEPrefix(ty);
      m_formatter.threeByteOp(opcode, escape, offset, base, dst);
      m_formatter.immediate8u(imm);
      return;
    }

    spew("%-11s$0x%x, " MEM_ob ", %s, %s", name, imm, ADDR_ob(offset, base),
         XMMRegName(src0), XMMRegName(dst));
    m_formatter.threeByteOpVex(ty, opcode, escape, offset, base, src0, dst);
    m_formatter.immediate8u(imm);
  }

  void threeByteOpImmInt32Simd(const char* name, VexOperandType ty,
                               ThreeByteOpcodeID opcode, ThreeByteEscape escape,
                               uint32_t imm, int32_t offset, RegisterID base,
                               RegisterID index, int scale, XMMRegisterID src0,
                               XMMRegisterID dst) {
    if (useLegacySSEEncoding(src0, dst)) {
      spew("%-11s$0x%x, " MEM_obs ", %s", legacySSEOpName(name), imm,
           ADDR_obs(offset, base, index, scale), XMMRegName(dst));
      m_formatter.legacySSEPrefix(ty);
      m_formatter.threeByteOp(opcode, escape, offset, base, index, scale, dst);
      m_formatter.immediate8u(imm);
      return;
    }

    spew("%-11s$0x%x, " MEM_obs ", %s, %s", name, imm,
         ADDR_obs(offset, base, index, scale), XMMRegName(src0),
         XMMRegName(dst));
    m_formatter.threeByteOpVex(ty, opcode, escape, offset, base, index, scale,
                               src0, dst);
    m_formatter.immediate8u(imm);
  }

  void threeByteOpImmSimdInt32(const char* name, VexOperandType ty,
                               ThreeByteOpcodeID opcode, ThreeByteEscape escape,
                               uint32_t imm, XMMRegisterID src,
                               RegisterID dst) {
    if (useLegacySSEEncodingAlways()) {
      spew("%-11s$0x%x, %s, %s", legacySSEOpName(name), imm, XMMRegName(src),
           GPReg32Name(dst));
      m_formatter.legacySSEPrefix(ty);
      m_formatter.threeByteOp(opcode, escape, (RegisterID)src, dst);
      m_formatter.immediate8u(imm);
      return;
    }

    if (opcode == OP3_PEXTRD_EvVdqIb) {
      spew("%-11s$0x%x, %s, %s", name, imm, XMMRegName((XMMRegisterID)dst),
           GPReg32Name((RegisterID)src));
    } else {
      spew("%-11s$0x%x, %s, %s", name, imm, XMMRegName(src), GPReg32Name(dst));
    }
    m_formatter.threeByteOpVex(ty, opcode, escape, (RegisterID)src, invalid_xmm,
                               dst);
    m_formatter.immediate8u(imm);
  }

  void threeByteOpImmSimdInt32(const char* name, VexOperandType ty,
                               ThreeByteOpcodeID opcode, ThreeByteEscape escape,
                               uint32_t imm, int32_t offset, RegisterID base,
                               RegisterID dst) {
    if (useLegacySSEEncodingAlways()) {
      spew("%-11s$0x%x, " MEM_ob ", %s", legacySSEOpName(name), imm,
           ADDR_ob(offset, base), GPReg32Name(dst));
      m_formatter.legacySSEPrefix(ty);
      m_formatter.threeByteOp(opcode, escape, offset, base, dst);
      m_formatter.immediate8u(imm);
      return;
    }

    spew("%-11s$0x%x, " MEM_ob ", %s", name, imm, ADDR_ob(offset, base),
         GPReg32Name(dst));
    m_formatter.threeByteOpVex(ty, opcode, escape, offset, base, invalid_xmm,
                               dst);
    m_formatter.immediate8u(imm);
  }

  void threeByteOpImmSimdInt32(const char* name, VexOperandType ty,
                               ThreeByteOpcodeID opcode, ThreeByteEscape escape,
                               uint32_t imm, int32_t offset, RegisterID base,
                               RegisterID index, int scale, RegisterID dst) {
    if (useLegacySSEEncodingAlways()) {
      spew("%-11s$0x%x, " MEM_obs ", %s", legacySSEOpName(name), imm,
           ADDR_obs(offset, base, index, scale), GPReg32Name(dst));
      m_formatter.legacySSEPrefix(ty);
      m_formatter.threeByteOp(opcode, escape, offset, base, index, scale, dst);
      m_formatter.immediate8u(imm);
      return;
    }

    spew("%-11s$0x%x, " MEM_obs ", %s", name, imm,
         ADDR_obs(offset, base, index, scale), GPReg32Name(dst));
    m_formatter.threeByteOpVex(ty, opcode, escape, offset, base, index, scale,
                               invalid_xmm, dst);
    m_formatter.immediate8u(imm);
  }

  // Blendv is a three-byte op, but the VEX encoding has a different opcode
  // than the SSE encoding, so we handle it specially.
  void vblendvOpSimd(const char* name, ThreeByteOpcodeID opcode,
                     ThreeByteOpcodeID vexOpcode, XMMRegisterID mask,
                     XMMRegisterID rm, XMMRegisterID src0, XMMRegisterID dst) {
    if (useLegacySSEEncodingForVblendv(mask, src0, dst)) {
      spew("%-11s%s, %s", legacySSEOpName(name), XMMRegName(rm),
           XMMRegName(dst));
      // Even though a "ps" instruction, vblendv is encoded with the "pd"
      // prefix.
      m_formatter.legacySSEPrefix(VEX_PD);
      m_formatter.threeByteOp(opcode, ESCAPE_38, (RegisterID)rm, dst);
      return;
    }

    spew("%-11s%s, %s, %s, %s", name, XMMRegName(mask), XMMRegName(rm),
         XMMRegName(src0), XMMRegName(dst));
    // Even though a "ps" instruction, vblendv is encoded with the "pd" prefix.
    m_formatter.vblendvOpVex(VEX_PD, vexOpcode, ESCAPE_3A, mask, (RegisterID)rm,
                             src0, dst);
  }

  void vblendvOpSimd(const char* name, ThreeByteOpcodeID opcode,
                     ThreeByteOpcodeID vexOpcode, XMMRegisterID mask,
                     int32_t offset, RegisterID base, XMMRegisterID src0,
                     XMMRegisterID dst) {
    if (useLegacySSEEncodingForVblendv(mask, src0, dst)) {
      spew("%-11s" MEM_ob ", %s", legacySSEOpName(name), ADDR_ob(offset, base),
           XMMRegName(dst));
      // Even though a "ps" instruction, vblendv is encoded with the "pd"
      // prefix.
      m_formatter.legacySSEPrefix(VEX_PD);
      m_formatter.threeByteOp(opcode, ESCAPE_38, offset, base, dst);
      return;
    }

    spew("%-11s%s, " MEM_ob ", %s, %s", name, XMMRegName(mask),
         ADDR_ob(offset, base), XMMRegName(src0), XMMRegName(dst));
    // Even though a "ps" instruction, vblendv is encoded with the "pd" prefix.
    m_formatter.vblendvOpVex(VEX_PD, vexOpcode, ESCAPE_3A, mask, offset, base,
                             src0, dst);
  }

  void shiftOpImmSimd(const char* name, TwoByteOpcodeID opcode,
                      ShiftID shiftKind, uint32_t imm, XMMRegisterID src,
                      XMMRegisterID dst) {
    if (useLegacySSEEncoding(src, dst)) {
      spew("%-11s$%d, %s", legacySSEOpName(name), int32_t(imm),
           XMMRegName(dst));
      m_formatter.legacySSEPrefix(VEX_PD);
      m_formatter.twoByteOp(opcode, (RegisterID)dst, (int)shiftKind);
      m_formatter.immediate8u(imm);
      return;
    }

    spew("%-11s$%d, %s, %s", name, int32_t(imm), XMMRegName(src),
         XMMRegName(dst));
    // For shift instructions, destination is stored in vvvv field.
    m_formatter.twoByteOpVex(VEX_PD, opcode, (RegisterID)src, dst,
                             (int)shiftKind);
    m_formatter.immediate8u(imm);
  }

  class X86InstructionFormatter {
   public:
    // Legacy prefix bytes:
    //
    // These are emmitted prior to the instruction.

    void prefix(OneByteOpcodeID pre) { m_buffer.putByte(pre); }

    void legacySSEPrefix(VexOperandType ty) {
      switch (ty) {
        case VEX_PS:
          break;
        case VEX_PD:
          prefix(PRE_SSE_66);
          break;
        case VEX_SS:
          prefix(PRE_SSE_F3);
          break;
        case VEX_SD:
          prefix(PRE_SSE_F2);
          break;
      }
    }

    /* clang-format off */
        //
        // Word-sized operands / no operand instruction formatters.
        //
        // In addition to the opcode, the following operand permutations are supported:
        //   * None - instruction takes no operands.
        //   * One register - the low three bits of the RegisterID are added into the opcode.
        //   * Two registers - encode a register form ModRm (for all ModRm formats, the reg field is passed first, and a GroupOpcodeID may be passed in its place).
        //   * Three argument ModRM - a register, and a register and an offset describing a memory operand.
        //   * Five argument ModRM - a register, and a base register, an index, scale, and offset describing a memory operand.
        //
        // For 32-bit x86 targets, the address operand may also be provided as a
        // void*.  On 64-bit targets REX prefixes will be planted as necessary,
        // where high numbered registers are used.
        //
        // The twoByteOp methods plant two-byte Intel instructions sequences
        // (first opcode byte 0x0F).
        //
    /* clang-format on */

    void oneByteOp(OneByteOpcodeID opcode) {
      m_buffer.ensureSpace(MaxInstructionSize);
      m_buffer.putByteUnchecked(opcode);
    }

    void oneByteOp(OneByteOpcodeID opcode, RegisterID reg) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexIfNeeded(0, 0, reg);
      m_buffer.putByteUnchecked(opcode + (reg & 7));
    }

    void oneByteOp(OneByteOpcodeID opcode, RegisterID rm, int reg) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexIfNeeded(reg, 0, rm);
      m_buffer.putByteUnchecked(opcode);
      registerModRM(rm, reg);
    }

    void oneByteOp(OneByteOpcodeID opcode, int32_t offset, RegisterID base,
                   int reg) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexIfNeeded(reg, 0, base);
      m_buffer.putByteUnchecked(opcode);
      memoryModRM(offset, base, reg);
    }

    void oneByteOp_disp32(OneByteOpcodeID opcode, int32_t offset,
                          RegisterID base, int reg) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexIfNeeded(reg, 0, base);
      m_buffer.putByteUnchecked(opcode);
      memoryModRM_disp32(offset, base, reg);
    }

    void oneByteOp(OneByteOpcodeID opcode, int32_t offset, RegisterID base,
                   RegisterID index, int scale, int reg) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexIfNeeded(reg, index, base);
      m_buffer.putByteUnchecked(opcode);
      memoryModRM(offset, base, index, scale, reg);
    }

    void oneByteOp_disp32(OneByteOpcodeID opcode, int32_t offset,
                          RegisterID index, int scale, int reg) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexIfNeeded(reg, index, 0);
      m_buffer.putByteUnchecked(opcode);
      memoryModRM_disp32(offset, index, scale, reg);
    }

    void oneByteOp(OneByteOpcodeID opcode, const void* address, int reg) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexIfNeeded(reg, 0, 0);
      m_buffer.putByteUnchecked(opcode);
      memoryModRM_disp32(address, reg);
    }

    void oneByteOp_disp32(OneByteOpcodeID opcode, const void* address,
                          int reg) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexIfNeeded(reg, 0, 0);
      m_buffer.putByteUnchecked(opcode);
      memoryModRM_disp32(address, reg);
    }
#ifdef JS_CODEGEN_X64
    void oneByteRipOp(OneByteOpcodeID opcode, int ripOffset, int reg) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexIfNeeded(reg, 0, 0);
      m_buffer.putByteUnchecked(opcode);
      putModRm(ModRmMemoryNoDisp, noBase, reg);
      m_buffer.putIntUnchecked(ripOffset);
    }

    void oneByteRipOp64(OneByteOpcodeID opcode, int ripOffset, int reg) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexW(reg, 0, 0);
      m_buffer.putByteUnchecked(opcode);
      putModRm(ModRmMemoryNoDisp, noBase, reg);
      m_buffer.putIntUnchecked(ripOffset);
    }

    void twoByteRipOp(TwoByteOpcodeID opcode, int ripOffset, int reg) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexIfNeeded(reg, 0, 0);
      m_buffer.putByteUnchecked(OP_2BYTE_ESCAPE);
      m_buffer.putByteUnchecked(opcode);
      putModRm(ModRmMemoryNoDisp, noBase, reg);
      m_buffer.putIntUnchecked(ripOffset);
    }

    void twoByteRipOpVex(VexOperandType ty, TwoByteOpcodeID opcode,
                         int ripOffset, XMMRegisterID src0, XMMRegisterID reg) {
      int r = (reg >> 3), x = 0, b = 0;
      int m = 1;  // 0x0F
      int w = 0, v = src0, l = 0;
      threeOpVex(ty, r, x, b, m, w, v, l, opcode);
      putModRm(ModRmMemoryNoDisp, noBase, reg);
      m_buffer.putIntUnchecked(ripOffset);
    }
#endif

    void twoByteOp(TwoByteOpcodeID opcode) {
      m_buffer.ensureSpace(MaxInstructionSize);
      m_buffer.putByteUnchecked(OP_2BYTE_ESCAPE);
      m_buffer.putByteUnchecked(opcode);
    }

    void twoByteOp(TwoByteOpcodeID opcode, int reg) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexIfNeeded(0, 0, reg);
      m_buffer.putByteUnchecked(OP_2BYTE_ESCAPE);
      m_buffer.putByteUnchecked(opcode + (reg & 7));
    }

    void twoByteOp(TwoByteOpcodeID opcode, RegisterID rm, int reg) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexIfNeeded(reg, 0, rm);
      m_buffer.putByteUnchecked(OP_2BYTE_ESCAPE);
      m_buffer.putByteUnchecked(opcode);
      registerModRM(rm, reg);
    }

    void twoByteOpVex(VexOperandType ty, TwoByteOpcodeID opcode, RegisterID rm,
                      XMMRegisterID src0, int reg) {
      int r = (reg >> 3), x = 0, b = (rm >> 3);
      int m = 1;  // 0x0F
      int w = 0, v = src0, l = 0;
      threeOpVex(ty, r, x, b, m, w, v, l, opcode);
      registerModRM(rm, reg);
    }

    void twoByteOp(TwoByteOpcodeID opcode, int32_t offset, RegisterID base,
                   int reg) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexIfNeeded(reg, 0, base);
      m_buffer.putByteUnchecked(OP_2BYTE_ESCAPE);
      m_buffer.putByteUnchecked(opcode);
      memoryModRM(offset, base, reg);
    }

    void twoByteOpVex(VexOperandType ty, TwoByteOpcodeID opcode, int32_t offset,
                      RegisterID base, XMMRegisterID src0, int reg) {
      int r = (reg >> 3), x = 0, b = (base >> 3);
      int m = 1;  // 0x0F
      int w = 0, v = src0, l = 0;
      threeOpVex(ty, r, x, b, m, w, v, l, opcode);
      memoryModRM(offset, base, reg);
    }

    void twoByteOp_disp32(TwoByteOpcodeID opcode, int32_t offset,
                          RegisterID base, int reg) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexIfNeeded(reg, 0, base);
      m_buffer.putByteUnchecked(OP_2BYTE_ESCAPE);
      m_buffer.putByteUnchecked(opcode);
      memoryModRM_disp32(offset, base, reg);
    }

    void twoByteOpVex_disp32(VexOperandType ty, TwoByteOpcodeID opcode,
                             int32_t offset, RegisterID base,
                             XMMRegisterID src0, int reg) {
      int r = (reg >> 3), x = 0, b = (base >> 3);
      int m = 1;  // 0x0F
      int w = 0, v = src0, l = 0;
      threeOpVex(ty, r, x, b, m, w, v, l, opcode);
      memoryModRM_disp32(offset, base, reg);
    }

    void twoByteOp(TwoByteOpcodeID opcode, int32_t offset, RegisterID base,
                   RegisterID index, int scale, int reg) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexIfNeeded(reg, index, base);
      m_buffer.putByteUnchecked(OP_2BYTE_ESCAPE);
      m_buffer.putByteUnchecked(opcode);
      memoryModRM(offset, base, index, scale, reg);
    }

    void twoByteOpVex(VexOperandType ty, TwoByteOpcodeID opcode, int32_t offset,
                      RegisterID base, RegisterID index, int scale,
                      XMMRegisterID src0, int reg) {
      int r = (reg >> 3), x = (index >> 3), b = (base >> 3);
      int m = 1;  // 0x0F
      int w = 0, v = src0, l = 0;
      threeOpVex(ty, r, x, b, m, w, v, l, opcode);
      memoryModRM(offset, base, index, scale, reg);
    }

    void twoByteOp(TwoByteOpcodeID opcode, const void* address, int reg) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexIfNeeded(reg, 0, 0);
      m_buffer.putByteUnchecked(OP_2BYTE_ESCAPE);
      m_buffer.putByteUnchecked(opcode);
      memoryModRM(address, reg);
    }

    void twoByteOpVex(VexOperandType ty, TwoByteOpcodeID opcode,
                      const void* address, XMMRegisterID src0, int reg) {
      int r = (reg >> 3), x = 0, b = 0;
      int m = 1;  // 0x0F
      int w = 0, v = src0, l = 0;
      threeOpVex(ty, r, x, b, m, w, v, l, opcode);
      memoryModRM(address, reg);
    }

    void threeByteOp(ThreeByteOpcodeID opcode, ThreeByteEscape escape,
                     RegisterID rm, int reg) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexIfNeeded(reg, 0, rm);
      m_buffer.putByteUnchecked(OP_2BYTE_ESCAPE);
      m_buffer.putByteUnchecked(escape);
      m_buffer.putByteUnchecked(opcode);
      registerModRM(rm, reg);
    }

    void threeByteOpVex(VexOperandType ty, ThreeByteOpcodeID opcode,
                        ThreeByteEscape escape, RegisterID rm,
                        XMMRegisterID src0, int reg) {
      int r = (reg >> 3), x = 0, b = (rm >> 3);
      int m = 0, w = 0, v = src0, l = 0;
      switch (escape) {
        case ESCAPE_38:
          m = 2;
          break;
        case ESCAPE_3A:
          m = 3;
          break;
        default:
          MOZ_CRASH("unexpected escape");
      }
      threeOpVex(ty, r, x, b, m, w, v, l, opcode);
      registerModRM(rm, reg);
    }

    void threeByteOp(ThreeByteOpcodeID opcode, ThreeByteEscape escape,
                     int32_t offset, RegisterID base, int reg) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexIfNeeded(reg, 0, base);
      m_buffer.putByteUnchecked(OP_2BYTE_ESCAPE);
      m_buffer.putByteUnchecked(escape);
      m_buffer.putByteUnchecked(opcode);
      memoryModRM(offset, base, reg);
    }

    void threeByteOp(ThreeByteOpcodeID opcode, ThreeByteEscape escape,
                     int32_t offset, RegisterID base, RegisterID index,
                     int32_t scale, int reg) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexIfNeeded(reg, index, base);
      m_buffer.putByteUnchecked(OP_2BYTE_ESCAPE);
      m_buffer.putByteUnchecked(escape);
      m_buffer.putByteUnchecked(opcode);
      memoryModRM(offset, base, index, scale, reg);
    }

    void threeByteOpVex(VexOperandType ty, ThreeByteOpcodeID opcode,
                        ThreeByteEscape escape, int32_t offset, RegisterID base,
                        XMMRegisterID src0, int reg) {
      int r = (reg >> 3), x = 0, b = (base >> 3);
      int m = 0, w = 0, v = src0, l = 0;
      switch (escape) {
        case ESCAPE_38:
          m = 2;
          break;
        case ESCAPE_3A:
          m = 3;
          break;
        default:
          MOZ_CRASH("unexpected escape");
      }
      threeOpVex(ty, r, x, b, m, w, v, l, opcode);
      memoryModRM(offset, base, reg);
    }

    void threeByteOpVex(VexOperandType ty, ThreeByteOpcodeID opcode,
                        ThreeByteEscape escape, int32_t offset, RegisterID base,
                        RegisterID index, int scale, XMMRegisterID src0,
                        int reg) {
      int r = (reg >> 3), x = (index >> 3), b = (base >> 3);
      int m = 0, w = 0, v = src0, l = 0;
      switch (escape) {
        case ESCAPE_38:
          m = 2;
          break;
        case ESCAPE_3A:
          m = 3;
          break;
        default:
          MOZ_CRASH("unexpected escape");
      }
      threeOpVex(ty, r, x, b, m, w, v, l, opcode);
      memoryModRM(offset, base, index, scale, reg);
    }

    void threeByteOp(ThreeByteOpcodeID opcode, ThreeByteEscape escape,
                     const void* address, int reg) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexIfNeeded(reg, 0, 0);
      m_buffer.putByteUnchecked(OP_2BYTE_ESCAPE);
      m_buffer.putByteUnchecked(escape);
      m_buffer.putByteUnchecked(opcode);
      memoryModRM(address, reg);
    }

    void threeByteRipOp(ThreeByteOpcodeID opcode, ThreeByteEscape escape,
                        int ripOffset, int reg) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexIfNeeded(reg, 0, 0);
      m_buffer.putByteUnchecked(OP_2BYTE_ESCAPE);
      m_buffer.putByteUnchecked(escape);
      m_buffer.putByteUnchecked(opcode);
      putModRm(ModRmMemoryNoDisp, noBase, reg);
      m_buffer.putIntUnchecked(ripOffset);
    }

    void threeByteOpVex(VexOperandType ty, ThreeByteOpcodeID opcode,
                        ThreeByteEscape escape, const void* address,
                        XMMRegisterID src0, int reg) {
      int r = (reg >> 3), x = 0, b = 0;
      int m = 0, w = 0, v = src0, l = 0;
      switch (escape) {
        case ESCAPE_38:
          m = 2;
          break;
        case ESCAPE_3A:
          m = 3;
          break;
        default:
          MOZ_CRASH("unexpected escape");
      }
      threeOpVex(ty, r, x, b, m, w, v, l, opcode);
      memoryModRM(address, reg);
    }

    void threeByteRipOpVex(VexOperandType ty, ThreeByteOpcodeID opcode,
                           ThreeByteEscape escape, int ripOffset,
                           XMMRegisterID src0, int reg) {
      int r = (reg >> 3), x = 0, b = 0;
      int m = 0;
      switch (escape) {
        case ESCAPE_38:
          m = 2;
          break;
        case ESCAPE_3A:
          m = 3;
          break;
        default:
          MOZ_CRASH("unexpected escape");
      }
      int w = 0, v = src0, l = 0;
      threeOpVex(ty, r, x, b, m, w, v, l, opcode);
      putModRm(ModRmMemoryNoDisp, noBase, reg);
      m_buffer.putIntUnchecked(ripOffset);
    }

    void vblendvOpVex(VexOperandType ty, ThreeByteOpcodeID opcode,
                      ThreeByteEscape escape, XMMRegisterID mask, RegisterID rm,
                      XMMRegisterID src0, int reg) {
      int r = (reg >> 3), x = 0, b = (rm >> 3);
      int m = 0, w = 0, v = src0, l = 0;
      switch (escape) {
        case ESCAPE_38:
          m = 2;
          break;
        case ESCAPE_3A:
          m = 3;
          break;
        default:
          MOZ_CRASH("unexpected escape");
      }
      threeOpVex(ty, r, x, b, m, w, v, l, opcode);
      registerModRM(rm, reg);
      immediate8u(mask << 4);
    }

    void vblendvOpVex(VexOperandType ty, ThreeByteOpcodeID opcode,
                      ThreeByteEscape escape, XMMRegisterID mask,
                      int32_t offset, RegisterID base, XMMRegisterID src0,
                      int reg) {
      int r = (reg >> 3), x = 0, b = (base >> 3);
      int m = 0, w = 0, v = src0, l = 0;
      switch (escape) {
        case ESCAPE_38:
          m = 2;
          break;
        case ESCAPE_3A:
          m = 3;
          break;
        default:
          MOZ_CRASH("unexpected escape");
      }
      threeOpVex(ty, r, x, b, m, w, v, l, opcode);
      memoryModRM(offset, base, reg);
      immediate8u(mask << 4);
    }

#ifdef JS_CODEGEN_X64
    // Quad-word-sized operands:
    //
    // Used to format 64-bit operantions, planting a REX.w prefix.  When
    // planting d64 or f64 instructions, not requiring a REX.w prefix, the
    // normal (non-'64'-postfixed) formatters should be used.

    void oneByteOp64(OneByteOpcodeID opcode) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexW(0, 0, 0);
      m_buffer.putByteUnchecked(opcode);
    }

    void oneByteOp64(OneByteOpcodeID opcode, RegisterID reg) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexW(0, 0, reg);
      m_buffer.putByteUnchecked(opcode + (reg & 7));
    }

    void oneByteOp64(OneByteOpcodeID opcode, RegisterID rm, int reg) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexW(reg, 0, rm);
      m_buffer.putByteUnchecked(opcode);
      registerModRM(rm, reg);
    }

    void oneByteOp64(OneByteOpcodeID opcode, int32_t offset, RegisterID base,
                     int reg) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexW(reg, 0, base);
      m_buffer.putByteUnchecked(opcode);
      memoryModRM(offset, base, reg);
    }

    void oneByteOp64_disp32(OneByteOpcodeID opcode, int32_t offset,
                            RegisterID base, int reg) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexW(reg, 0, base);
      m_buffer.putByteUnchecked(opcode);
      memoryModRM_disp32(offset, base, reg);
    }

    void oneByteOp64(OneByteOpcodeID opcode, int32_t offset, RegisterID base,
                     RegisterID index, int scale, int reg) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexW(reg, index, base);
      m_buffer.putByteUnchecked(opcode);
      memoryModRM(offset, base, index, scale, reg);
    }

    void oneByteOp64(OneByteOpcodeID opcode, const void* address, int reg) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexW(reg, 0, 0);
      m_buffer.putByteUnchecked(opcode);
      memoryModRM(address, reg);
    }

    void twoByteOp64(TwoByteOpcodeID opcode, int reg) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexW(0, 0, reg);
      m_buffer.putByteUnchecked(OP_2BYTE_ESCAPE);
      m_buffer.putByteUnchecked(opcode + (reg & 7));
    }

    void twoByteOp64(TwoByteOpcodeID opcode, RegisterID rm, int reg) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexW(reg, 0, rm);
      m_buffer.putByteUnchecked(OP_2BYTE_ESCAPE);
      m_buffer.putByteUnchecked(opcode);
      registerModRM(rm, reg);
    }

    void twoByteOp64(TwoByteOpcodeID opcode, int offset, RegisterID base,
                     int reg) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexW(reg, 0, base);
      m_buffer.putByteUnchecked(OP_2BYTE_ESCAPE);
      m_buffer.putByteUnchecked(opcode);
      memoryModRM(offset, base, reg);
    }

    void twoByteOp64(TwoByteOpcodeID opcode, int offset, RegisterID base,
                     RegisterID index, int scale, int reg) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexW(reg, index, base);
      m_buffer.putByteUnchecked(OP_2BYTE_ESCAPE);
      m_buffer.putByteUnchecked(opcode);
      memoryModRM(offset, base, index, scale, reg);
    }

    void twoByteOp64(TwoByteOpcodeID opcode, const void* address, int reg) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexW(reg, 0, 0);
      m_buffer.putByteUnchecked(OP_2BYTE_ESCAPE);
      m_buffer.putByteUnchecked(opcode);
      memoryModRM(address, reg);
    }

    void twoByteOpVex64(VexOperandType ty, TwoByteOpcodeID opcode,
                        RegisterID rm, XMMRegisterID src0, XMMRegisterID reg) {
      int r = (reg >> 3), x = 0, b = (rm >> 3);
      int m = 1;  // 0x0F
      int w = 1, v = src0, l = 0;
      threeOpVex(ty, r, x, b, m, w, v, l, opcode);
      registerModRM(rm, reg);
    }

    void threeByteOp64(ThreeByteOpcodeID opcode, ThreeByteEscape escape,
                       RegisterID rm, int reg) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexW(reg, 0, rm);
      m_buffer.putByteUnchecked(OP_2BYTE_ESCAPE);
      m_buffer.putByteUnchecked(escape);
      m_buffer.putByteUnchecked(opcode);
      registerModRM(rm, reg);
    }
#endif  // JS_CODEGEN_X64

    void threeByteOpVex64(VexOperandType ty, ThreeByteOpcodeID opcode,
                          ThreeByteEscape escape, RegisterID rm,
                          XMMRegisterID src0, int reg) {
      int r = (reg >> 3), x = 0, b = (rm >> 3);
      int m = 0, w = 1, v = src0, l = 0;
      switch (escape) {
        case ESCAPE_38:
          m = 2;
          break;
        case ESCAPE_3A:
          m = 3;
          break;
        default:
          MOZ_CRASH("unexpected escape");
      }
      threeOpVex(ty, r, x, b, m, w, v, l, opcode);
      registerModRM(rm, reg);
    }

    // Byte-operands:
    //
    // These methods format byte operations.  Byte operations differ from
    // the normal formatters in the circumstances under which they will
    // decide to emit REX prefixes.  These should be used where any register
    // operand signifies a byte register.
    //
    // The disctinction is due to the handling of register numbers in the
    // range 4..7 on x86-64.  These register numbers may either represent
    // the second byte of the first four registers (ah..bh) or the first
    // byte of the second four registers (spl..dil).
    //
    // Address operands should still be checked using regRequiresRex(),
    // while byteRegRequiresRex() is provided to check byte register
    // operands.

    void oneByteOp8(OneByteOpcodeID opcode) {
      m_buffer.ensureSpace(MaxInstructionSize);
      m_buffer.putByteUnchecked(opcode);
    }

    void oneByteOp8(OneByteOpcodeID opcode, RegisterID r) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexIf(byteRegRequiresRex(r), 0, 0, r);
      m_buffer.putByteUnchecked(opcode + (r & 7));
    }

    void oneByteOp8(OneByteOpcodeID opcode, RegisterID rm,
                    GroupOpcodeID groupOp) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexIf(byteRegRequiresRex(rm), 0, 0, rm);
      m_buffer.putByteUnchecked(opcode);
      registerModRM(rm, groupOp);
    }

    // Like oneByteOp8, but never emits a REX prefix.
    void oneByteOp8_norex(OneByteOpcodeID opcode, HRegisterID rm,
                          GroupOpcodeID groupOp) {
      MOZ_ASSERT(!regRequiresRex(RegisterID(rm)));
      m_buffer.ensureSpace(MaxInstructionSize);
      m_buffer.putByteUnchecked(opcode);
      registerModRM(RegisterID(rm), groupOp);
    }

    void oneByteOp8(OneByteOpcodeID opcode, int32_t offset, RegisterID base,
                    RegisterID reg) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexIf(byteRegRequiresRex(reg), reg, 0, base);
      m_buffer.putByteUnchecked(opcode);
      memoryModRM(offset, base, reg);
    }

    void oneByteOp8_disp32(OneByteOpcodeID opcode, int32_t offset,
                           RegisterID base, RegisterID reg) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexIf(byteRegRequiresRex(reg), reg, 0, base);
      m_buffer.putByteUnchecked(opcode);
      memoryModRM_disp32(offset, base, reg);
    }

    void oneByteOp8(OneByteOpcodeID opcode, int32_t offset, RegisterID base,
                    RegisterID index, int scale, RegisterID reg) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexIf(byteRegRequiresRex(reg), reg, index, base);
      m_buffer.putByteUnchecked(opcode);
      memoryModRM(offset, base, index, scale, reg);
    }

    void oneByteOp8(OneByteOpcodeID opcode, const void* address,
                    RegisterID reg) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexIf(byteRegRequiresRex(reg), reg, 0, 0);
      m_buffer.putByteUnchecked(opcode);
      memoryModRM_disp32(address, reg);
    }

    void twoByteOp8(TwoByteOpcodeID opcode, RegisterID rm, RegisterID reg) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexIf(byteRegRequiresRex(reg) || byteRegRequiresRex(rm), reg, 0, rm);
      m_buffer.putByteUnchecked(OP_2BYTE_ESCAPE);
      m_buffer.putByteUnchecked(opcode);
      registerModRM(rm, reg);
    }

    void twoByteOp8(TwoByteOpcodeID opcode, int32_t offset, RegisterID base,
                    RegisterID reg) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexIf(byteRegRequiresRex(reg) || regRequiresRex(base), reg, 0, base);
      m_buffer.putByteUnchecked(OP_2BYTE_ESCAPE);
      m_buffer.putByteUnchecked(opcode);
      memoryModRM(offset, base, reg);
    }

    void twoByteOp8(TwoByteOpcodeID opcode, int32_t offset, RegisterID base,
                    RegisterID index, int scale, RegisterID reg) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexIf(byteRegRequiresRex(reg) || regRequiresRex(base) ||
                    regRequiresRex(index),
                reg, index, base);
      m_buffer.putByteUnchecked(OP_2BYTE_ESCAPE);
      m_buffer.putByteUnchecked(opcode);
      memoryModRM(offset, base, index, scale, reg);
    }

    // Like twoByteOp8 but doesn't add a REX prefix if the destination reg
    // is in esp..edi. This may be used when the destination is not an 8-bit
    // register (as in a movzbl instruction), so it doesn't need a REX
    // prefix to disambiguate it from ah..bh.
    void twoByteOp8_movx(TwoByteOpcodeID opcode, RegisterID rm,
                         RegisterID reg) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexIf(regRequiresRex(reg) || byteRegRequiresRex(rm), reg, 0, rm);
      m_buffer.putByteUnchecked(OP_2BYTE_ESCAPE);
      m_buffer.putByteUnchecked(opcode);
      registerModRM(rm, reg);
    }

    void twoByteOp8(TwoByteOpcodeID opcode, RegisterID rm,
                    GroupOpcodeID groupOp) {
      m_buffer.ensureSpace(MaxInstructionSize);
      emitRexIf(byteRegRequiresRex(rm), 0, 0, rm);
      m_buffer.putByteUnchecked(OP_2BYTE_ESCAPE);
      m_buffer.putByteUnchecked(opcode);
      registerModRM(rm, groupOp);
    }

    // Immediates:
    //
    // An immedaite should be appended where appropriate after an op has
    // been emitted.  The writes are unchecked since the opcode formatters
    // above will have ensured space.

    // A signed 8-bit immediate.
    MOZ_ALWAYS_INLINE void immediate8s(int32_t imm) {
      MOZ_ASSERT(CAN_SIGN_EXTEND_8_32(imm));
      m_buffer.putByteUnchecked(imm);
    }

    // An unsigned 8-bit immediate.
    MOZ_ALWAYS_INLINE void immediate8u(uint32_t imm) {
      MOZ_ASSERT(CAN_ZERO_EXTEND_8_32(imm));
      m_buffer.putByteUnchecked(int32_t(imm));
    }

    // An 8-bit immediate with is either signed or unsigned, for use in
    // instructions which actually only operate on 8 bits.
    MOZ_ALWAYS_INLINE void immediate8(int32_t imm) {
      m_buffer.putByteUnchecked(imm);
    }

    // A signed 16-bit immediate.
    MOZ_ALWAYS_INLINE void immediate16s(int32_t imm) {
      MOZ_ASSERT(CAN_SIGN_EXTEND_16_32(imm));
      m_buffer.putShortUnchecked(imm);
    }

    // An unsigned 16-bit immediate.
    MOZ_ALWAYS_INLINE void immediate16u(int32_t imm) {
      MOZ_ASSERT(CAN_ZERO_EXTEND_16_32(imm));
      m_buffer.putShortUnchecked(imm);
    }

    // A 16-bit immediate with is either signed or unsigned, for use in
    // instructions which actually only operate on 16 bits.
    MOZ_ALWAYS_INLINE void immediate16(int32_t imm) {
      m_buffer.putShortUnchecked(imm);
    }

    MOZ_ALWAYS_INLINE void immediate32(int32_t imm) {
      m_buffer.putIntUnchecked(imm);
    }

    MOZ_ALWAYS_INLINE void immediate64(int64_t imm) {
      m_buffer.putInt64Unchecked(imm);
    }

    [[nodiscard]] MOZ_ALWAYS_INLINE JmpSrc immediateRel32() {
      m_buffer.putIntUnchecked(0);
      return JmpSrc(m_buffer.size());
    }

    // Data:

    void jumpTablePointer(uintptr_t ptr) {
      m_buffer.ensureSpace(sizeof(uintptr_t));
#ifdef JS_CODEGEN_X64
      m_buffer.putInt64Unchecked(ptr);
#else
      m_buffer.putIntUnchecked(ptr);
#endif
    }

    void doubleConstant(double d) {
      m_buffer.ensureSpace(sizeof(double));
      m_buffer.putInt64Unchecked(mozilla::BitwiseCast<uint64_t>(d));
    }

    void floatConstant(float f) {
      m_buffer.ensureSpace(sizeof(float));
      m_buffer.putIntUnchecked(mozilla::BitwiseCast<uint32_t>(f));
    }

    void simd128Constant(const void* data) {
      const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
      m_buffer.ensureSpace(16);
      for (size_t i = 0; i < 16; ++i) {
        m_buffer.putByteUnchecked(bytes[i]);
      }
    }

    void int64Constant(int64_t i) {
      m_buffer.ensureSpace(sizeof(int64_t));
      m_buffer.putInt64Unchecked(i);
    }

    void int32Constant(int32_t i) {
      m_buffer.ensureSpace(sizeof(int32_t));
      m_buffer.putIntUnchecked(i);
    }

    // Administrative methods:

    size_t size() const { return m_buffer.size(); }
    const unsigned char* buffer() const { return m_buffer.buffer(); }
    unsigned char* data() { return m_buffer.data(); }
    bool oom() const { return m_buffer.oom(); }
    bool reserve(size_t size) { return m_buffer.reserve(size); }
    bool swapBuffer(wasm::Bytes& other) { return m_buffer.swap(other); }
    bool isAligned(int alignment) const {
      return m_buffer.isAligned(alignment);
    }

    [[nodiscard]] bool append(const unsigned char* values, size_t size) {
      return m_buffer.append(values, size);
    }

   private:
    // Internals; ModRm and REX formatters.

    // Byte operand register spl & above requir a REX prefix, which precludes
    // use of the h registers in the same instruction.
    static bool byteRegRequiresRex(RegisterID reg) {
#ifdef JS_CODEGEN_X64
      return reg >= rsp;
#else
      return false;
#endif
    }

    // For non-byte sizes, registers r8 & above always require a REX prefix.
    static bool regRequiresRex(RegisterID reg) {
#ifdef JS_CODEGEN_X64
      return reg >= r8;
#else
      return false;
#endif
    }

#ifdef JS_CODEGEN_X64
    // Format a REX prefix byte.
    void emitRex(bool w, int r, int x, int b) {
      m_buffer.putByteUnchecked(PRE_REX | ((int)w << 3) | ((r >> 3) << 2) |
                                ((x >> 3) << 1) | (b >> 3));
    }

    // Used to plant a REX byte with REX.w set (for 64-bit operations).
    void emitRexW(int r, int x, int b) { emitRex(true, r, x, b); }

    // Used for operations with byte operands - use byteRegRequiresRex() to
    // check register operands, regRequiresRex() to check other registers
    // (i.e. address base & index).
    //
    // NB: WebKit's use of emitRexIf() is limited such that the
    // reqRequiresRex() checks are not needed. SpiderMonkey extends
    // oneByteOp8 and twoByteOp8 functionality such that r, x, and b
    // can all be used.
    void emitRexIf(bool condition, int r, int x, int b) {
      if (condition || regRequiresRex(RegisterID(r)) ||
          regRequiresRex(RegisterID(x)) || regRequiresRex(RegisterID(b))) {
        emitRex(false, r, x, b);
      }
    }

    // Used for word sized operations, will plant a REX prefix if necessary
    // (if any register is r8 or above).
    void emitRexIfNeeded(int r, int x, int b) { emitRexIf(false, r, x, b); }
#else
    // No REX prefix bytes on 32-bit x86.
    void emitRexIf(bool condition, int, int, int) {
      MOZ_ASSERT(!condition, "32-bit x86 should never use a REX prefix");
    }
    void emitRexIfNeeded(int, int, int) {}
#endif

    void putModRm(ModRmMode mode, RegisterID rm, int reg) {
      m_buffer.putByteUnchecked((mode << 6) | ((reg & 7) << 3) | (rm & 7));
    }

    void putModRmSib(ModRmMode mode, RegisterID base, RegisterID index,
                     int scale, int reg) {
      MOZ_ASSERT(mode != ModRmRegister);

      putModRm(mode, hasSib, reg);
      m_buffer.putByteUnchecked((scale << 6) | ((index & 7) << 3) | (base & 7));
    }

    void registerModRM(RegisterID rm, int reg) {
      putModRm(ModRmRegister, rm, reg);
    }

    void memoryModRM(int32_t offset, RegisterID base, int reg) {
      // A base of esp or r12 would be interpreted as a sib, so force a
      // sib with no index & put the base in there.
#ifdef JS_CODEGEN_X64
      if ((base == hasSib) || (base == hasSib2)) {
#else
      if (base == hasSib) {
#endif
        if (!offset) {  // No need to check if the base is noBase, since we know
                        // it is hasSib!
          putModRmSib(ModRmMemoryNoDisp, base, noIndex, 0, reg);
        } else if (CAN_SIGN_EXTEND_8_32(offset)) {
          putModRmSib(ModRmMemoryDisp8, base, noIndex, 0, reg);
          m_buffer.putByteUnchecked(offset);
        } else {
          putModRmSib(ModRmMemoryDisp32, base, noIndex, 0, reg);
          m_buffer.putIntUnchecked(offset);
        }
      } else {
#ifdef JS_CODEGEN_X64
        if (!offset && (base != noBase) && (base != noBase2)) {
#else
        if (!offset && (base != noBase)) {
#endif
          putModRm(ModRmMemoryNoDisp, base, reg);
        } else if (CAN_SIGN_EXTEND_8_32(offset)) {
          putModRm(ModRmMemoryDisp8, base, reg);
          m_buffer.putByteUnchecked(offset);
        } else {
          putModRm(ModRmMemoryDisp32, base, reg);
          m_buffer.putIntUnchecked(offset);
        }
      }
    }

    void memoryModRM_disp32(int32_t offset, RegisterID base, int reg) {
      // A base of esp or r12 would be interpreted as a sib, so force a
      // sib with no index & put the base in there.
#ifdef JS_CODEGEN_X64
      if ((base == hasSib) || (base == hasSib2)) {
#else
      if (base == hasSib) {
#endif
        putModRmSib(ModRmMemoryDisp32, base, noIndex, 0, reg);
        m_buffer.putIntUnchecked(offset);
      } else {
        putModRm(ModRmMemoryDisp32, base, reg);
        m_buffer.putIntUnchecked(offset);
      }
    }

    void memoryModRM(int32_t offset, RegisterID base, RegisterID index,
                     int scale, int reg) {
      MOZ_ASSERT(index != noIndex);

#ifdef JS_CODEGEN_X64
      if (!offset && (base != noBase) && (base != noBase2)) {
#else
      if (!offset && (base != noBase)) {
#endif
        putModRmSib(ModRmMemoryNoDisp, base, index, scale, reg);
      } else if (CAN_SIGN_EXTEND_8_32(offset)) {
        putModRmSib(ModRmMemoryDisp8, base, index, scale, reg);
        m_buffer.putByteUnchecked(offset);
      } else {
        putModRmSib(ModRmMemoryDisp32, base, index, scale, reg);
        m_buffer.putIntUnchecked(offset);
      }
    }

    void memoryModRM_disp32(int32_t offset, RegisterID index, int scale,
                            int reg) {
      MOZ_ASSERT(index != noIndex);

      // NB: the base-less memoryModRM overloads generate different code
      // then the base-full memoryModRM overloads in the base == noBase
      // case. The base-less overloads assume that the desired effective
      // address is:
      //
      //   reg := [scaled index] + disp32
      //
      // which means the mod needs to be ModRmMemoryNoDisp. The base-full
      // overloads pass ModRmMemoryDisp32 in all cases and thus, when
      // base == noBase (== ebp), the effective address is:
      //
      //   reg := [scaled index] + disp32 + [ebp]
      //
      // See Intel developer manual, Vol 2, 2.1.5, Table 2-3.
      putModRmSib(ModRmMemoryNoDisp, noBase, index, scale, reg);
      m_buffer.putIntUnchecked(offset);
    }

    void memoryModRM_disp32(const void* address, int reg) {
      int32_t disp = AddressImmediate(address);

#ifdef JS_CODEGEN_X64
      // On x64-64, non-RIP-relative absolute mode requires a SIB.
      putModRmSib(ModRmMemoryNoDisp, noBase, noIndex, 0, reg);
#else
      // noBase + ModRmMemoryNoDisp means noBase + ModRmMemoryDisp32!
      putModRm(ModRmMemoryNoDisp, noBase, reg);
#endif
      m_buffer.putIntUnchecked(disp);
    }

    void memoryModRM(const void* address, int reg) {
      memoryModRM_disp32(address, reg);
    }

    void threeOpVex(VexOperandType p, int r, int x, int b, int m, int w, int v,
                    int l, int opcode) {
      m_buffer.ensureSpace(MaxInstructionSize);

      if (v == invalid_xmm) {
        v = XMMRegisterID(0);
      }

      if (x == 0 && b == 0 && m == 1 && w == 0) {
        // Two byte VEX.
        m_buffer.putByteUnchecked(PRE_VEX_C5);
        m_buffer.putByteUnchecked(((r << 7) | (v << 3) | (l << 2) | p) ^ 0xf8);
      } else {
        // Three byte VEX.
        m_buffer.putByteUnchecked(PRE_VEX_C4);
        m_buffer.putByteUnchecked(((r << 7) | (x << 6) | (b << 5) | m) ^ 0xe0);
        m_buffer.putByteUnchecked(((w << 7) | (v << 3) | (l << 2) | p) ^ 0x78);
      }

      m_buffer.putByteUnchecked(opcode);
    }

    AssemblerBuffer m_buffer;
  } m_formatter;

  bool useVEX_;
};

}  // namespace X86Encoding

}  // namespace jit
}  // namespace js

#endif /* jit_x86_shared_BaseAssembler_x86_shared_h */
