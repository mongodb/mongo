/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x64_BaseAssembler_x64_h
#define jit_x64_BaseAssembler_x64_h

#include "jit/x86-shared/BaseAssembler-x86-shared.h"

namespace js {
namespace jit {

namespace X86Encoding {

class BaseAssemblerX64 : public BaseAssembler {
 public:
  // Arithmetic operations:

  void addq_rr(RegisterID src, RegisterID dst) {
    spew("addq       %s, %s", GPReg64Name(src), GPReg64Name(dst));
    m_formatter.oneByteOp64(OP_ADD_GvEv, src, dst);
  }

  void addq_mr(int32_t offset, RegisterID base, RegisterID dst) {
    spew("addq       " MEM_ob ", %s", ADDR_ob(offset, base), GPReg64Name(dst));
    m_formatter.oneByteOp64(OP_ADD_GvEv, offset, base, dst);
  }

  void addq_mr(const void* addr, RegisterID dst) {
    spew("addq       %p, %s", addr, GPReg64Name(dst));
    m_formatter.oneByteOp64(OP_ADD_GvEv, addr, dst);
  }

  void addq_mr(int32_t offset, RegisterID base, RegisterID index, int scale,
               RegisterID dst) {
    spew("addq       " MEM_obs ", %s", ADDR_obs(offset, base, index, scale),
         GPReg64Name(dst));
    m_formatter.oneByteOp64(OP_ADD_GvEv, offset, base, index, scale, dst);
  }

  void addq_rm(RegisterID src, int32_t offset, RegisterID base) {
    spew("addq       %s, " MEM_ob, GPReg64Name(src), ADDR_ob(offset, base));
    m_formatter.oneByteOp64(OP_ADD_EvGv, offset, base, src);
  }

  void addq_rm(RegisterID src, int32_t offset, RegisterID base,
               RegisterID index, int scale) {
    spew("addq       %s, " MEM_obs, GPReg64Name(src),
         ADDR_obs(offset, base, index, scale));
    m_formatter.oneByteOp64(OP_ADD_EvGv, offset, base, index, scale, src);
  }

  void addq_ir(int32_t imm, RegisterID dst) {
    spew("addq       $%d, %s", imm, GPReg64Name(dst));
    if (CAN_SIGN_EXTEND_8_32(imm)) {
      m_formatter.oneByteOp64(OP_GROUP1_EvIb, dst, GROUP1_OP_ADD);
      m_formatter.immediate8s(imm);
    } else {
      if (dst == rax) {
        m_formatter.oneByteOp64(OP_ADD_EAXIv);
      } else {
        m_formatter.oneByteOp64(OP_GROUP1_EvIz, dst, GROUP1_OP_ADD);
      }
      m_formatter.immediate32(imm);
    }
  }

  void addq_i32r(int32_t imm, RegisterID dst) {
    // 32-bit immediate always, for patching.
    spew("addq       $0x%04x, %s", uint32_t(imm), GPReg64Name(dst));
    if (dst == rax) {
      m_formatter.oneByteOp64(OP_ADD_EAXIv);
    } else {
      m_formatter.oneByteOp64(OP_GROUP1_EvIz, dst, GROUP1_OP_ADD);
    }
    m_formatter.immediate32(imm);
  }

  void addq_im(int32_t imm, int32_t offset, RegisterID base) {
    spew("addq       $%d, " MEM_ob, imm, ADDR_ob(offset, base));
    if (CAN_SIGN_EXTEND_8_32(imm)) {
      m_formatter.oneByteOp64(OP_GROUP1_EvIb, offset, base, GROUP1_OP_ADD);
      m_formatter.immediate8s(imm);
    } else {
      m_formatter.oneByteOp64(OP_GROUP1_EvIz, offset, base, GROUP1_OP_ADD);
      m_formatter.immediate32(imm);
    }
  }

  void addq_im(int32_t imm, const void* addr) {
    spew("addq       $%d, %p", imm, addr);
    if (CAN_SIGN_EXTEND_8_32(imm)) {
      m_formatter.oneByteOp64(OP_GROUP1_EvIb, addr, GROUP1_OP_ADD);
      m_formatter.immediate8s(imm);
    } else {
      m_formatter.oneByteOp64(OP_GROUP1_EvIz, addr, GROUP1_OP_ADD);
      m_formatter.immediate32(imm);
    }
  }

  void andq_rr(RegisterID src, RegisterID dst) {
    spew("andq       %s, %s", GPReg64Name(src), GPReg64Name(dst));
    m_formatter.oneByteOp64(OP_AND_GvEv, src, dst);
  }

  void andq_mr(int32_t offset, RegisterID base, RegisterID dst) {
    spew("andq       " MEM_ob ", %s", ADDR_ob(offset, base), GPReg64Name(dst));
    m_formatter.oneByteOp64(OP_AND_GvEv, offset, base, dst);
  }

  void andq_mr(int32_t offset, RegisterID base, RegisterID index, int scale,
               RegisterID dst) {
    spew("andq       " MEM_obs ", %s", ADDR_obs(offset, base, index, scale),
         GPReg64Name(dst));
    m_formatter.oneByteOp64(OP_AND_GvEv, offset, base, index, scale, dst);
  }

  void andq_mr(const void* addr, RegisterID dst) {
    spew("andq       %p, %s", addr, GPReg64Name(dst));
    m_formatter.oneByteOp64(OP_AND_GvEv, addr, dst);
  }

  void andq_rm(RegisterID src, int32_t offset, RegisterID base) {
    spew("andq       %s, " MEM_ob, GPReg64Name(src), ADDR_ob(offset, base));
    m_formatter.oneByteOp64(OP_AND_EvGv, offset, base, src);
  }

  void andq_rm(RegisterID src, int32_t offset, RegisterID base,
               RegisterID index, int scale) {
    spew("andq       %s, " MEM_obs, GPReg64Name(src),
         ADDR_obs(offset, base, index, scale));
    m_formatter.oneByteOp64(OP_AND_EvGv, offset, base, index, scale, src);
  }

  void orq_mr(int32_t offset, RegisterID base, RegisterID dst) {
    spew("orq        " MEM_ob ", %s", ADDR_ob(offset, base), GPReg64Name(dst));
    m_formatter.oneByteOp64(OP_OR_GvEv, offset, base, dst);
  }

  void orq_mr(const void* addr, RegisterID dst) {
    spew("orq        %p, %s", addr, GPReg64Name(dst));
    m_formatter.oneByteOp64(OP_OR_GvEv, addr, dst);
  }

  void orq_rm(RegisterID src, int32_t offset, RegisterID base) {
    spew("orq       %s, " MEM_ob, GPReg64Name(src), ADDR_ob(offset, base));
    m_formatter.oneByteOp64(OP_OR_EvGv, offset, base, src);
  }

  void orq_rm(RegisterID src, int32_t offset, RegisterID base, RegisterID index,
              int scale) {
    spew("orq       %s, " MEM_obs, GPReg64Name(src),
         ADDR_obs(offset, base, index, scale));
    m_formatter.oneByteOp64(OP_OR_EvGv, offset, base, index, scale, src);
  }

  void xorq_mr(int32_t offset, RegisterID base, RegisterID dst) {
    spew("xorq       " MEM_ob ", %s", ADDR_ob(offset, base), GPReg64Name(dst));
    m_formatter.oneByteOp64(OP_XOR_GvEv, offset, base, dst);
  }

  void xorq_mr(int32_t offset, RegisterID base, RegisterID index, int scale,
               RegisterID dst) {
    spew("xorq       " MEM_obs ", %s", ADDR_obs(offset, base, index, scale),
         GPReg64Name(dst));
    m_formatter.oneByteOp64(OP_XOR_GvEv, offset, base, index, scale, dst);
  }

  void xorq_mr(const void* addr, RegisterID dst) {
    spew("xorq       %p, %s", addr, GPReg64Name(dst));
    m_formatter.oneByteOp64(OP_XOR_GvEv, addr, dst);
  }

  void xorq_rm(RegisterID src, int32_t offset, RegisterID base) {
    spew("xorq       %s, " MEM_ob, GPReg64Name(src), ADDR_ob(offset, base));
    m_formatter.oneByteOp64(OP_XOR_EvGv, offset, base, src);
  }

  void xorq_rm(RegisterID src, int32_t offset, RegisterID base,
               RegisterID index, int scale) {
    spew("xorq       %s, " MEM_obs, GPReg64Name(src),
         ADDR_obs(offset, base, index, scale));
    m_formatter.oneByteOp64(OP_XOR_EvGv, offset, base, index, scale, src);
  }

  void bswapq_r(RegisterID dst) {
    spew("bswapq     %s", GPReg64Name(dst));
    m_formatter.twoByteOp64(OP2_BSWAP, dst);
  }

  void bsrq_rr(RegisterID src, RegisterID dst) {
    spew("bsrq       %s, %s", GPReg64Name(src), GPReg64Name(dst));
    m_formatter.twoByteOp64(OP2_BSR_GvEv, src, dst);
  }

  void bsfq_rr(RegisterID src, RegisterID dst) {
    spew("bsfq       %s, %s", GPReg64Name(src), GPReg64Name(dst));
    m_formatter.twoByteOp64(OP2_BSF_GvEv, src, dst);
  }

  void lzcntq_rr(RegisterID src, RegisterID dst) {
    spew("lzcntq     %s, %s", GPReg64Name(src), GPReg64Name(dst));
    m_formatter.legacySSEPrefix(VEX_SS);
    m_formatter.twoByteOp64(OP2_LZCNT_GvEv, src, dst);
  }

  void tzcntq_rr(RegisterID src, RegisterID dst) {
    spew("tzcntq     %s, %s", GPReg64Name(src), GPReg64Name(dst));
    m_formatter.legacySSEPrefix(VEX_SS);
    m_formatter.twoByteOp64(OP2_TZCNT_GvEv, src, dst);
  }

  void popcntq_rr(RegisterID src, RegisterID dst) {
    spew("popcntq    %s, %s", GPReg64Name(src), GPReg64Name(dst));
    m_formatter.legacySSEPrefix(VEX_SS);
    m_formatter.twoByteOp64(OP2_POPCNT_GvEv, src, dst);
  }

  void andq_ir(int32_t imm, RegisterID dst) {
    spew("andq       $0x%" PRIx64 ", %s", uint64_t(imm), GPReg64Name(dst));
    if (CAN_SIGN_EXTEND_8_32(imm)) {
      m_formatter.oneByteOp64(OP_GROUP1_EvIb, dst, GROUP1_OP_AND);
      m_formatter.immediate8s(imm);
    } else {
      if (dst == rax) {
        m_formatter.oneByteOp64(OP_AND_EAXIv);
      } else {
        m_formatter.oneByteOp64(OP_GROUP1_EvIz, dst, GROUP1_OP_AND);
      }
      m_formatter.immediate32(imm);
    }
  }

  void negq_r(RegisterID dst) {
    spew("negq       %s", GPReg64Name(dst));
    m_formatter.oneByteOp64(OP_GROUP3_Ev, dst, GROUP3_OP_NEG);
  }

  void orq_rr(RegisterID src, RegisterID dst) {
    spew("orq        %s, %s", GPReg64Name(src), GPReg64Name(dst));
    m_formatter.oneByteOp64(OP_OR_GvEv, src, dst);
  }

  void orq_ir(int32_t imm, RegisterID dst) {
    spew("orq        $0x%" PRIx64 ", %s", uint64_t(imm), GPReg64Name(dst));
    if (CAN_SIGN_EXTEND_8_32(imm)) {
      m_formatter.oneByteOp64(OP_GROUP1_EvIb, dst, GROUP1_OP_OR);
      m_formatter.immediate8s(imm);
    } else {
      if (dst == rax) {
        m_formatter.oneByteOp64(OP_OR_EAXIv);
      } else {
        m_formatter.oneByteOp64(OP_GROUP1_EvIz, dst, GROUP1_OP_OR);
      }
      m_formatter.immediate32(imm);
    }
  }

  void notq_r(RegisterID dst) {
    spew("notq       %s", GPReg64Name(dst));
    m_formatter.oneByteOp64(OP_GROUP3_Ev, dst, GROUP3_OP_NOT);
  }

  void subq_rr(RegisterID src, RegisterID dst) {
    spew("subq       %s, %s", GPReg64Name(src), GPReg64Name(dst));
    m_formatter.oneByteOp64(OP_SUB_GvEv, src, dst);
  }

  void subq_rm(RegisterID src, int32_t offset, RegisterID base) {
    spew("subq       %s, " MEM_ob, GPReg64Name(src), ADDR_ob(offset, base));
    m_formatter.oneByteOp64(OP_SUB_EvGv, offset, base, src);
  }

  void subq_rm(RegisterID src, int32_t offset, RegisterID base,
               RegisterID index, int scale) {
    spew("subq       %s, " MEM_obs, GPReg64Name(src),
         ADDR_obs(offset, base, index, scale));
    m_formatter.oneByteOp64(OP_SUB_EvGv, offset, base, index, scale, src);
  }

  void subq_mr(int32_t offset, RegisterID base, RegisterID dst) {
    spew("subq       " MEM_ob ", %s", ADDR_ob(offset, base), GPReg64Name(dst));
    m_formatter.oneByteOp64(OP_SUB_GvEv, offset, base, dst);
  }

  void subq_mr(const void* addr, RegisterID dst) {
    spew("subq       %p, %s", addr, GPReg64Name(dst));
    m_formatter.oneByteOp64(OP_SUB_GvEv, addr, dst);
  }

  void subq_ir(int32_t imm, RegisterID dst) {
    spew("subq       $%d, %s", imm, GPReg64Name(dst));
    if (CAN_SIGN_EXTEND_8_32(imm)) {
      m_formatter.oneByteOp64(OP_GROUP1_EvIb, dst, GROUP1_OP_SUB);
      m_formatter.immediate8s(imm);
    } else {
      if (dst == rax) {
        m_formatter.oneByteOp64(OP_SUB_EAXIv);
      } else {
        m_formatter.oneByteOp64(OP_GROUP1_EvIz, dst, GROUP1_OP_SUB);
      }
      m_formatter.immediate32(imm);
    }
  }

  void xorq_rr(RegisterID src, RegisterID dst) {
    spew("xorq       %s, %s", GPReg64Name(src), GPReg64Name(dst));
    m_formatter.oneByteOp64(OP_XOR_GvEv, src, dst);
  }

  void xorq_ir(int32_t imm, RegisterID dst) {
    spew("xorq       $0x%" PRIx64 ", %s", uint64_t(imm), GPReg64Name(dst));
    if (CAN_SIGN_EXTEND_8_32(imm)) {
      m_formatter.oneByteOp64(OP_GROUP1_EvIb, dst, GROUP1_OP_XOR);
      m_formatter.immediate8s(imm);
    } else {
      if (dst == rax) {
        m_formatter.oneByteOp64(OP_XOR_EAXIv);
      } else {
        m_formatter.oneByteOp64(OP_GROUP1_EvIz, dst, GROUP1_OP_XOR);
      }
      m_formatter.immediate32(imm);
    }
  }

  void sarq_CLr(RegisterID dst) {
    spew("sarq       %%cl, %s", GPReg64Name(dst));
    m_formatter.oneByteOp64(OP_GROUP2_EvCL, dst, GROUP2_OP_SAR);
  }

  void shlq_CLr(RegisterID dst) {
    spew("shlq       %%cl, %s", GPReg64Name(dst));
    m_formatter.oneByteOp64(OP_GROUP2_EvCL, dst, GROUP2_OP_SHL);
  }

  void shrq_CLr(RegisterID dst) {
    spew("shrq       %%cl, %s", GPReg64Name(dst));
    m_formatter.oneByteOp64(OP_GROUP2_EvCL, dst, GROUP2_OP_SHR);
  }

  void sarq_ir(int32_t imm, RegisterID dst) {
    MOZ_ASSERT(imm < 64);
    spew("sarq       $%d, %s", imm, GPReg64Name(dst));
    if (imm == 1) {
      m_formatter.oneByteOp64(OP_GROUP2_Ev1, dst, GROUP2_OP_SAR);
    } else {
      m_formatter.oneByteOp64(OP_GROUP2_EvIb, dst, GROUP2_OP_SAR);
      m_formatter.immediate8u(imm);
    }
  }

  void shlq_ir(int32_t imm, RegisterID dst) {
    MOZ_ASSERT(imm < 64);
    spew("shlq       $%d, %s", imm, GPReg64Name(dst));
    if (imm == 1) {
      m_formatter.oneByteOp64(OP_GROUP2_Ev1, dst, GROUP2_OP_SHL);
    } else {
      m_formatter.oneByteOp64(OP_GROUP2_EvIb, dst, GROUP2_OP_SHL);
      m_formatter.immediate8u(imm);
    }
  }

  void shrq_ir(int32_t imm, RegisterID dst) {
    MOZ_ASSERT(imm < 64);
    spew("shrq       $%d, %s", imm, GPReg64Name(dst));
    if (imm == 1) {
      m_formatter.oneByteOp64(OP_GROUP2_Ev1, dst, GROUP2_OP_SHR);
    } else {
      m_formatter.oneByteOp64(OP_GROUP2_EvIb, dst, GROUP2_OP_SHR);
      m_formatter.immediate8u(imm);
    }
  }

  void rolq_ir(int32_t imm, RegisterID dst) {
    MOZ_ASSERT(imm < 64);
    spew("rolq       $%d, %s", imm, GPReg64Name(dst));
    if (imm == 1) {
      m_formatter.oneByteOp64(OP_GROUP2_Ev1, dst, GROUP2_OP_ROL);
    } else {
      m_formatter.oneByteOp64(OP_GROUP2_EvIb, dst, GROUP2_OP_ROL);
      m_formatter.immediate8u(imm);
    }
  }
  void rolq_CLr(RegisterID dst) {
    spew("rolq       %%cl, %s", GPReg64Name(dst));
    m_formatter.oneByteOp64(OP_GROUP2_EvCL, dst, GROUP2_OP_ROL);
  }

  void rorq_ir(int32_t imm, RegisterID dst) {
    MOZ_ASSERT(imm < 64);
    spew("rorq       $%d, %s", imm, GPReg64Name(dst));
    if (imm == 1) {
      m_formatter.oneByteOp64(OP_GROUP2_Ev1, dst, GROUP2_OP_ROR);
    } else {
      m_formatter.oneByteOp64(OP_GROUP2_EvIb, dst, GROUP2_OP_ROR);
      m_formatter.immediate8u(imm);
    }
  }
  void rorq_CLr(RegisterID dst) {
    spew("rorq       %%cl, %s", GPReg64Name(dst));
    m_formatter.oneByteOp64(OP_GROUP2_EvCL, dst, GROUP2_OP_ROR);
  }

  void imulq_rr(RegisterID src, RegisterID dst) {
    spew("imulq      %s, %s", GPReg64Name(src), GPReg64Name(dst));
    m_formatter.twoByteOp64(OP2_IMUL_GvEv, src, dst);
  }

  void imulq_mr(int32_t offset, RegisterID base, RegisterID dst) {
    spew("imulq      " MEM_ob ", %s", ADDR_ob(offset, base), GPReg64Name(dst));
    m_formatter.twoByteOp64(OP2_IMUL_GvEv, offset, base, dst);
  }

  void imulq_ir(int32_t value, RegisterID src, RegisterID dst) {
    spew("imulq      $%d, %s, %s", value, GPReg64Name(src), GPReg64Name(dst));
    if (CAN_SIGN_EXTEND_8_32(value)) {
      m_formatter.oneByteOp64(OP_IMUL_GvEvIb, src, dst);
      m_formatter.immediate8s(value);
    } else {
      m_formatter.oneByteOp64(OP_IMUL_GvEvIz, src, dst);
      m_formatter.immediate32(value);
    }
  }

  void cqo() {
    spew("cqo        ");
    m_formatter.oneByteOp64(OP_CDQ);
  }

  void idivq_r(RegisterID divisor) {
    spew("idivq      %s", GPReg64Name(divisor));
    m_formatter.oneByteOp64(OP_GROUP3_Ev, divisor, GROUP3_OP_IDIV);
  }

  void divq_r(RegisterID divisor) {
    spew("divq       %s", GPReg64Name(divisor));
    m_formatter.oneByteOp64(OP_GROUP3_Ev, divisor, GROUP3_OP_DIV);
  }

  // Comparisons:

  void cmpq_rr(RegisterID rhs, RegisterID lhs) {
    spew("cmpq       %s, %s", GPReg64Name(rhs), GPReg64Name(lhs));
    m_formatter.oneByteOp64(OP_CMP_GvEv, rhs, lhs);
  }

  void cmpq_rm(RegisterID rhs, int32_t offset, RegisterID base) {
    spew("cmpq       %s, " MEM_ob, GPReg64Name(rhs), ADDR_ob(offset, base));
    m_formatter.oneByteOp64(OP_CMP_EvGv, offset, base, rhs);
  }

  void cmpq_rm(RegisterID rhs, int32_t offset, RegisterID base,
               RegisterID index, int scale) {
    spew("cmpq       %s, " MEM_obs, GPReg64Name(rhs),
         ADDR_obs(offset, base, index, scale));
    m_formatter.oneByteOp64(OP_CMP_EvGv, offset, base, index, scale, rhs);
  }

  void cmpq_mr(int32_t offset, RegisterID base, RegisterID lhs) {
    spew("cmpq       " MEM_ob ", %s", ADDR_ob(offset, base), GPReg64Name(lhs));
    m_formatter.oneByteOp64(OP_CMP_GvEv, offset, base, lhs);
  }

  void cmpq_ir(int32_t rhs, RegisterID lhs) {
    if (rhs == 0) {
      testq_rr(lhs, lhs);
      return;
    }

    spew("cmpq       $0x%" PRIx64 ", %s", uint64_t(rhs), GPReg64Name(lhs));
    if (CAN_SIGN_EXTEND_8_32(rhs)) {
      m_formatter.oneByteOp64(OP_GROUP1_EvIb, lhs, GROUP1_OP_CMP);
      m_formatter.immediate8s(rhs);
    } else {
      if (lhs == rax) {
        m_formatter.oneByteOp64(OP_CMP_EAXIv);
      } else {
        m_formatter.oneByteOp64(OP_GROUP1_EvIz, lhs, GROUP1_OP_CMP);
      }
      m_formatter.immediate32(rhs);
    }
  }

  void cmpq_im(int32_t rhs, int32_t offset, RegisterID base) {
    spew("cmpq       $0x%" PRIx64 ", " MEM_ob, uint64_t(rhs),
         ADDR_ob(offset, base));
    if (CAN_SIGN_EXTEND_8_32(rhs)) {
      m_formatter.oneByteOp64(OP_GROUP1_EvIb, offset, base, GROUP1_OP_CMP);
      m_formatter.immediate8s(rhs);
    } else {
      m_formatter.oneByteOp64(OP_GROUP1_EvIz, offset, base, GROUP1_OP_CMP);
      m_formatter.immediate32(rhs);
    }
  }

  void cmpq_im(int32_t rhs, int32_t offset, RegisterID base, RegisterID index,
               int scale) {
    spew("cmpq       $0x%x, " MEM_obs, uint32_t(rhs),
         ADDR_obs(offset, base, index, scale));
    if (CAN_SIGN_EXTEND_8_32(rhs)) {
      m_formatter.oneByteOp64(OP_GROUP1_EvIb, offset, base, index, scale,
                              GROUP1_OP_CMP);
      m_formatter.immediate8s(rhs);
    } else {
      m_formatter.oneByteOp64(OP_GROUP1_EvIz, offset, base, index, scale,
                              GROUP1_OP_CMP);
      m_formatter.immediate32(rhs);
    }
  }
  void cmpq_im(int32_t rhs, const void* addr) {
    spew("cmpq       $0x%" PRIx64 ", %p", uint64_t(rhs), addr);
    if (CAN_SIGN_EXTEND_8_32(rhs)) {
      m_formatter.oneByteOp64(OP_GROUP1_EvIb, addr, GROUP1_OP_CMP);
      m_formatter.immediate8s(rhs);
    } else {
      m_formatter.oneByteOp64(OP_GROUP1_EvIz, addr, GROUP1_OP_CMP);
      m_formatter.immediate32(rhs);
    }
  }
  void cmpq_rm(RegisterID rhs, const void* addr) {
    spew("cmpq       %s, %p", GPReg64Name(rhs), addr);
    m_formatter.oneByteOp64(OP_CMP_EvGv, addr, rhs);
  }

  void testq_rr(RegisterID rhs, RegisterID lhs) {
    spew("testq      %s, %s", GPReg64Name(rhs), GPReg64Name(lhs));
    m_formatter.oneByteOp64(OP_TEST_EvGv, lhs, rhs);
  }

  void testq_ir(int32_t rhs, RegisterID lhs) {
    // If the mask fits in a 32-bit immediate, we can use testl with a
    // 32-bit subreg.
    if (CAN_ZERO_EXTEND_32_64(rhs)) {
      testl_ir(rhs, lhs);
      return;
    }
    spew("testq      $0x%" PRIx64 ", %s", uint64_t(rhs), GPReg64Name(lhs));
    if (lhs == rax) {
      m_formatter.oneByteOp64(OP_TEST_EAXIv);
    } else {
      m_formatter.oneByteOp64(OP_GROUP3_EvIz, lhs, GROUP3_OP_TEST);
    }
    m_formatter.immediate32(rhs);
  }

  void testq_i32m(int32_t rhs, int32_t offset, RegisterID base) {
    spew("testq      $0x%" PRIx64 ", " MEM_ob, uint64_t(rhs),
         ADDR_ob(offset, base));
    m_formatter.oneByteOp64(OP_GROUP3_EvIz, offset, base, GROUP3_OP_TEST);
    m_formatter.immediate32(rhs);
  }

  void testq_i32m(int32_t rhs, int32_t offset, RegisterID base,
                  RegisterID index, int scale) {
    spew("testq      $0x%4x, " MEM_obs, uint32_t(rhs),
         ADDR_obs(offset, base, index, scale));
    m_formatter.oneByteOp64(OP_GROUP3_EvIz, offset, base, index, scale,
                            GROUP3_OP_TEST);
    m_formatter.immediate32(rhs);
  }

  // Various move ops:

  void cmovCCq_rr(Condition cond, RegisterID src, RegisterID dst) {
    spew("cmov%s     %s, %s", CCName(cond), GPReg64Name(src), GPReg64Name(dst));
    m_formatter.twoByteOp64(cmovccOpcode(cond), src, dst);
  }
  void cmovCCq_mr(Condition cond, int32_t offset, RegisterID base,
                  RegisterID dst) {
    spew("cmov%s     " MEM_ob ", %s", CCName(cond), ADDR_ob(offset, base),
         GPReg64Name(dst));
    m_formatter.twoByteOp64(cmovccOpcode(cond), offset, base, dst);
  }
  void cmovCCq_mr(Condition cond, int32_t offset, RegisterID base,
                  RegisterID index, int scale, RegisterID dst) {
    spew("cmov%s     " MEM_obs ", %s", CCName(cond),
         ADDR_obs(offset, base, index, scale), GPReg64Name(dst));
    m_formatter.twoByteOp64(cmovccOpcode(cond), offset, base, index, scale,
                            dst);
  }

  void cmpxchgq(RegisterID src, int32_t offset, RegisterID base) {
    spew("cmpxchgq   %s, " MEM_ob, GPReg64Name(src), ADDR_ob(offset, base));
    m_formatter.twoByteOp64(OP2_CMPXCHG_GvEw, offset, base, src);
  }

  void cmpxchgq(RegisterID src, int32_t offset, RegisterID base,
                RegisterID index, int scale) {
    spew("cmpxchgq   %s, " MEM_obs, GPReg64Name(src),
         ADDR_obs(offset, base, index, scale));
    m_formatter.twoByteOp64(OP2_CMPXCHG_GvEw, offset, base, index, scale, src);
  }

  void lock_xaddq_rm(RegisterID srcdest, int32_t offset, RegisterID base) {
    spew("lock xaddq %s, " MEM_ob, GPReg64Name(srcdest), ADDR_ob(offset, base));
    m_formatter.oneByteOp(PRE_LOCK);
    m_formatter.twoByteOp64(OP2_XADD_EvGv, offset, base, srcdest);
  }

  void lock_xaddq_rm(RegisterID srcdest, int32_t offset, RegisterID base,
                     RegisterID index, int scale) {
    spew("lock xaddq %s, " MEM_obs, GPReg64Name(srcdest),
         ADDR_obs(offset, base, index, scale));
    m_formatter.oneByteOp(PRE_LOCK);
    m_formatter.twoByteOp64(OP2_XADD_EvGv, offset, base, index, scale, srcdest);
  }

  void xchgq_rr(RegisterID src, RegisterID dst) {
    spew("xchgq      %s, %s", GPReg64Name(src), GPReg64Name(dst));
    m_formatter.oneByteOp64(OP_XCHG_GvEv, src, dst);
  }
  void xchgq_rm(RegisterID src, int32_t offset, RegisterID base) {
    spew("xchgq      %s, " MEM_ob, GPReg64Name(src), ADDR_ob(offset, base));
    m_formatter.oneByteOp64(OP_XCHG_GvEv, offset, base, src);
  }
  void xchgq_rm(RegisterID src, int32_t offset, RegisterID base,
                RegisterID index, int scale) {
    spew("xchgq      %s, " MEM_obs, GPReg64Name(src),
         ADDR_obs(offset, base, index, scale));
    m_formatter.oneByteOp64(OP_XCHG_GvEv, offset, base, index, scale, src);
  }

  void movq_rr(RegisterID src, RegisterID dst) {
    spew("movq       %s, %s", GPReg64Name(src), GPReg64Name(dst));
    m_formatter.oneByteOp64(OP_MOV_EvGv, dst, src);
  }

  void movq_rm(RegisterID src, int32_t offset, RegisterID base) {
    spew("movq       %s, " MEM_ob, GPReg64Name(src), ADDR_ob(offset, base));
    m_formatter.oneByteOp64(OP_MOV_EvGv, offset, base, src);
  }

  void movq_rm_disp32(RegisterID src, int32_t offset, RegisterID base) {
    spew("movq       %s, " MEM_o32b, GPReg64Name(src), ADDR_o32b(offset, base));
    m_formatter.oneByteOp64_disp32(OP_MOV_EvGv, offset, base, src);
  }

  void movq_rm(RegisterID src, int32_t offset, RegisterID base,
               RegisterID index, int scale) {
    spew("movq       %s, " MEM_obs, GPReg64Name(src),
         ADDR_obs(offset, base, index, scale));
    m_formatter.oneByteOp64(OP_MOV_EvGv, offset, base, index, scale, src);
  }

  void movq_rm(RegisterID src, const void* addr) {
    if (src == rax && !IsAddressImmediate(addr)) {
      movq_EAXm(addr);
      return;
    }

    spew("movq       %s, %p", GPReg64Name(src), addr);
    m_formatter.oneByteOp64(OP_MOV_EvGv, addr, src);
  }

  void movq_mEAX(const void* addr) {
    if (IsAddressImmediate(addr)) {
      movq_mr(addr, rax);
      return;
    }

    spew("movq       %p, %%rax", addr);
    m_formatter.oneByteOp64(OP_MOV_EAXOv);
    m_formatter.immediate64(reinterpret_cast<int64_t>(addr));
  }

  void movq_EAXm(const void* addr) {
    if (IsAddressImmediate(addr)) {
      movq_rm(rax, addr);
      return;
    }

    spew("movq       %%rax, %p", addr);
    m_formatter.oneByteOp64(OP_MOV_OvEAX);
    m_formatter.immediate64(reinterpret_cast<int64_t>(addr));
  }

  void movq_mr(int32_t offset, RegisterID base, RegisterID dst) {
    spew("movq       " MEM_ob ", %s", ADDR_ob(offset, base), GPReg64Name(dst));
    m_formatter.oneByteOp64(OP_MOV_GvEv, offset, base, dst);
  }

  void movq_mr_disp32(int32_t offset, RegisterID base, RegisterID dst) {
    spew("movq       " MEM_o32b ", %s", ADDR_o32b(offset, base),
         GPReg64Name(dst));
    m_formatter.oneByteOp64_disp32(OP_MOV_GvEv, offset, base, dst);
  }

  void movq_mr(int32_t offset, RegisterID base, RegisterID index, int scale,
               RegisterID dst) {
    spew("movq       " MEM_obs ", %s", ADDR_obs(offset, base, index, scale),
         GPReg64Name(dst));
    m_formatter.oneByteOp64(OP_MOV_GvEv, offset, base, index, scale, dst);
  }

  void movq_mr(const void* addr, RegisterID dst) {
    if (dst == rax && !IsAddressImmediate(addr)) {
      movq_mEAX(addr);
      return;
    }

    spew("movq       %p, %s", addr, GPReg64Name(dst));
    m_formatter.oneByteOp64(OP_MOV_GvEv, addr, dst);
  }

  void leaq_mr(int32_t offset, RegisterID base, RegisterID index, int scale,
               RegisterID dst) {
    spew("leaq       " MEM_obs ", %s", ADDR_obs(offset, base, index, scale),
         GPReg64Name(dst));
    m_formatter.oneByteOp64(OP_LEA, offset, base, index, scale, dst);
  }

  void movq_i32m(int32_t imm, int32_t offset, RegisterID base) {
    spew("movq       $%d, " MEM_ob, imm, ADDR_ob(offset, base));
    m_formatter.oneByteOp64(OP_GROUP11_EvIz, offset, base, GROUP11_MOV);
    m_formatter.immediate32(imm);
  }
  void movq_i32m(int32_t imm, int32_t offset, RegisterID base, RegisterID index,
                 int scale) {
    spew("movq       $%d, " MEM_obs, imm, ADDR_obs(offset, base, index, scale));
    m_formatter.oneByteOp64(OP_GROUP11_EvIz, offset, base, index, scale,
                            GROUP11_MOV);
    m_formatter.immediate32(imm);
  }
  void movq_i32m(int32_t imm, const void* addr) {
    spew("movq       $%d, %p", imm, addr);
    m_formatter.oneByteOp64(OP_GROUP11_EvIz, addr, GROUP11_MOV);
    m_formatter.immediate32(imm);
  }

  // Note that this instruction sign-extends its 32-bit immediate field to 64
  // bits and loads the 64-bit value into a 64-bit register.
  //
  // Note also that this is similar to the movl_i32r instruction, except that
  // movl_i32r *zero*-extends its 32-bit immediate, and it has smaller code
  // size, so it's preferred for values which could use either.
  void movq_i32r(int32_t imm, RegisterID dst) {
    spew("movq       $%d, %s", imm, GPRegName(dst));
    m_formatter.oneByteOp64(OP_GROUP11_EvIz, dst, GROUP11_MOV);
    m_formatter.immediate32(imm);
  }

  void movq_i64r(int64_t imm, RegisterID dst) {
    spew("movabsq    $0x%" PRIx64 ", %s", uint64_t(imm), GPReg64Name(dst));
    m_formatter.oneByteOp64(OP_MOV_EAXIv, dst);
    m_formatter.immediate64(imm);
  }

  void movsbq_rr(RegisterID src, RegisterID dst) {
    spew("movsbq     %s, %s", GPReg32Name(src), GPReg64Name(dst));
    m_formatter.twoByteOp64(OP2_MOVSX_GvEb, src, dst);
  }
  void movsbq_mr(int32_t offset, RegisterID base, RegisterID dst) {
    spew("movsbq     " MEM_ob ", %s", ADDR_ob(offset, base), GPReg64Name(dst));
    m_formatter.twoByteOp64(OP2_MOVSX_GvEb, offset, base, dst);
  }
  void movsbq_mr(int32_t offset, RegisterID base, RegisterID index, int scale,
                 RegisterID dst) {
    spew("movsbq     " MEM_obs ", %s", ADDR_obs(offset, base, index, scale),
         GPReg64Name(dst));
    m_formatter.twoByteOp64(OP2_MOVSX_GvEb, offset, base, index, scale, dst);
  }

  void movswq_rr(RegisterID src, RegisterID dst) {
    spew("movswq     %s, %s", GPReg32Name(src), GPReg64Name(dst));
    m_formatter.twoByteOp64(OP2_MOVSX_GvEw, src, dst);
  }
  void movswq_mr(int32_t offset, RegisterID base, RegisterID dst) {
    spew("movswq     " MEM_ob ", %s", ADDR_ob(offset, base), GPReg64Name(dst));
    m_formatter.twoByteOp64(OP2_MOVSX_GvEw, offset, base, dst);
  }
  void movswq_mr(int32_t offset, RegisterID base, RegisterID index, int scale,
                 RegisterID dst) {
    spew("movswq     " MEM_obs ", %s", ADDR_obs(offset, base, index, scale),
         GPReg64Name(dst));
    m_formatter.twoByteOp64(OP2_MOVSX_GvEw, offset, base, index, scale, dst);
  }

  void movslq_rr(RegisterID src, RegisterID dst) {
    spew("movslq     %s, %s", GPReg32Name(src), GPReg64Name(dst));
    m_formatter.oneByteOp64(OP_MOVSXD_GvEv, src, dst);
  }
  void movslq_mr(int32_t offset, RegisterID base, RegisterID dst) {
    spew("movslq     " MEM_ob ", %s", ADDR_ob(offset, base), GPReg64Name(dst));
    m_formatter.oneByteOp64(OP_MOVSXD_GvEv, offset, base, dst);
  }
  void movslq_mr(int32_t offset, RegisterID base, RegisterID index, int scale,
                 RegisterID dst) {
    spew("movslq     " MEM_obs ", %s", ADDR_obs(offset, base, index, scale),
         GPReg64Name(dst));
    m_formatter.oneByteOp64(OP_MOVSXD_GvEv, offset, base, index, scale, dst);
  }

  [[nodiscard]] JmpSrc movl_ripr(RegisterID dst) {
    m_formatter.oneByteRipOp(OP_MOV_GvEv, 0, (RegisterID)dst);
    JmpSrc label(m_formatter.size());
    spew("movl       " MEM_o32r ", %s", ADDR_o32r(label.offset()),
         GPReg32Name(dst));
    return label;
  }

  [[nodiscard]] JmpSrc movl_rrip(RegisterID src) {
    m_formatter.oneByteRipOp(OP_MOV_EvGv, 0, (RegisterID)src);
    JmpSrc label(m_formatter.size());
    spew("movl       %s, " MEM_o32r "", GPReg32Name(src),
         ADDR_o32r(label.offset()));
    return label;
  }

  [[nodiscard]] JmpSrc movq_ripr(RegisterID dst) {
    m_formatter.oneByteRipOp64(OP_MOV_GvEv, 0, dst);
    JmpSrc label(m_formatter.size());
    spew("movq       " MEM_o32r ", %s", ADDR_o32r(label.offset()),
         GPRegName(dst));
    return label;
  }

  [[nodiscard]] JmpSrc movq_rrip(RegisterID src) {
    m_formatter.oneByteRipOp64(OP_MOV_EvGv, 0, (RegisterID)src);
    JmpSrc label(m_formatter.size());
    spew("movq       %s, " MEM_o32r "", GPRegName(src),
         ADDR_o32r(label.offset()));
    return label;
  }

  void leaq_mr(int32_t offset, RegisterID base, RegisterID dst) {
    spew("leaq       " MEM_ob ", %s", ADDR_ob(offset, base), GPReg64Name(dst));
    m_formatter.oneByteOp64(OP_LEA, offset, base, dst);
  }

  [[nodiscard]] JmpSrc leaq_rip(RegisterID dst) {
    m_formatter.oneByteRipOp64(OP_LEA, 0, dst);
    JmpSrc label(m_formatter.size());
    spew("leaq       " MEM_o32r ", %s", ADDR_o32r(label.offset()),
         GPRegName(dst));
    return label;
  }

  // Flow control:

  void jmp_rip(int ripOffset) {
    // rip-relative addressing.
    spew("jmp        *%d(%%rip)", ripOffset);
    m_formatter.oneByteRipOp(OP_GROUP5_Ev, ripOffset, GROUP5_OP_JMPN);
  }

  void immediate64(int64_t imm) {
    spew(".quad      %lld", (long long)imm);
    m_formatter.immediate64(imm);
  }

  // SSE operations:

  void vcvtsq2sd_rr(RegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpInt64Simd("vcvtsi2sd", VEX_SD, OP2_CVTSI2SD_VsdEd, src1, src0,
                       dst);
  }
  void vcvtsq2ss_rr(RegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
    twoByteOpInt64Simd("vcvtsi2ss", VEX_SS, OP2_CVTSI2SD_VsdEd, src1, src0,
                       dst);
  }

  void vcvtsi2sdq_rr(RegisterID src, XMMRegisterID dst) {
    twoByteOpInt64Simd("vcvtsi2sdq", VEX_SD, OP2_CVTSI2SD_VsdEd, src,
                       invalid_xmm, dst);
  }

  void vcvttsd2sq_rr(XMMRegisterID src, RegisterID dst) {
    twoByteOpSimdInt64("vcvttsd2si", VEX_SD, OP2_CVTTSD2SI_GdWsd, src, dst);
  }

  void vcvttss2sq_rr(XMMRegisterID src, RegisterID dst) {
    twoByteOpSimdInt64("vcvttss2si", VEX_SS, OP2_CVTTSD2SI_GdWsd, src, dst);
  }

  void vmovq_rr(XMMRegisterID src, RegisterID dst) {
    // While this is called "vmovq", it actually uses the vmovd encoding
    // with a REX prefix modifying it to be 64-bit.
    twoByteOpSimdInt64("vmovq", VEX_PD, OP2_MOVD_EdVd, (XMMRegisterID)dst,
                       (RegisterID)src);
  }

  void vpextrq_irr(unsigned lane, XMMRegisterID src, RegisterID dst) {
    MOZ_ASSERT(lane < 2);
    threeByteOpImmSimdInt64("vpextrq", VEX_PD, OP3_PEXTRQ_EvVdqIb, ESCAPE_3A,
                            lane, src, dst);
  }

  void vpinsrq_irr(unsigned lane, RegisterID src1, XMMRegisterID src0,
                   XMMRegisterID dst) {
    MOZ_ASSERT(lane < 2);
    threeByteOpImmInt64Simd("vpinsrq", VEX_PD, OP3_PINSRQ_VdqEvIb, ESCAPE_3A,
                            lane, src1, src0, dst);
  }

  void vmovq_rr(RegisterID src, XMMRegisterID dst) {
    // While this is called "vmovq", it actually uses the vmovd encoding
    // with a REX prefix modifying it to be 64-bit.
    twoByteOpInt64Simd("vmovq", VEX_PD, OP2_MOVD_VdEd, src, invalid_xmm, dst);
  }

  [[nodiscard]] JmpSrc vmovsd_ripr(XMMRegisterID dst) {
    return twoByteRipOpSimd("vmovsd", VEX_SD, OP2_MOVSD_VsdWsd, dst);
  }
  [[nodiscard]] JmpSrc vmovss_ripr(XMMRegisterID dst) {
    return twoByteRipOpSimd("vmovss", VEX_SS, OP2_MOVSD_VsdWsd, dst);
  }
  [[nodiscard]] JmpSrc vmovaps_ripr(XMMRegisterID dst) {
    return twoByteRipOpSimd("vmovaps", VEX_PS, OP2_MOVAPS_VsdWsd, dst);
  }
  [[nodiscard]] JmpSrc vmovdqa_ripr(XMMRegisterID dst) {
    return twoByteRipOpSimd("vmovdqa", VEX_PD, OP2_MOVDQ_VdqWdq, dst);
  }

  [[nodiscard]] JmpSrc vpaddb_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vpaddb", VEX_PD, OP2_PADDB_VdqWdq, src, dst);
  }
  [[nodiscard]] JmpSrc vpaddw_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vpaddw", VEX_PD, OP2_PADDW_VdqWdq, src, dst);
  }
  [[nodiscard]] JmpSrc vpaddd_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vpaddd", VEX_PD, OP2_PADDD_VdqWdq, src, dst);
  }
  [[nodiscard]] JmpSrc vpaddq_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vpaddq", VEX_PD, OP2_PADDQ_VdqWdq, src, dst);
  }
  [[nodiscard]] JmpSrc vpsubb_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vpsubb", VEX_PD, OP2_PSUBB_VdqWdq, src, dst);
  }
  [[nodiscard]] JmpSrc vpsubw_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vpsubw", VEX_PD, OP2_PSUBW_VdqWdq, src, dst);
  }
  [[nodiscard]] JmpSrc vpsubd_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vpsubd", VEX_PD, OP2_PSUBD_VdqWdq, src, dst);
  }
  [[nodiscard]] JmpSrc vpsubq_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vpsubq", VEX_PD, OP2_PSUBQ_VdqWdq, src, dst);
  }
  [[nodiscard]] JmpSrc vpmullw_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vpmullw", VEX_PD, OP2_PMULLW_VdqWdq, src, dst);
  }
  [[nodiscard]] JmpSrc vpmulld_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return threeByteRipOpSimd("vpmulld", VEX_PD, OP3_PMULLD_VdqWdq, ESCAPE_38,
                              src, dst);
  }
  [[nodiscard]] JmpSrc vpaddsb_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vpaddsb", VEX_PD, OP2_PADDSB_VdqWdq, src, dst);
  }
  [[nodiscard]] JmpSrc vpaddusb_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vpaddusb", VEX_PD, OP2_PADDUSB_VdqWdq, src, dst);
  }
  [[nodiscard]] JmpSrc vpaddsw_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vpaddsw", VEX_PD, OP2_PADDSW_VdqWdq, src, dst);
  }
  [[nodiscard]] JmpSrc vpaddusw_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vpaddusw", VEX_PD, OP2_PADDUSW_VdqWdq, src, dst);
  }
  [[nodiscard]] JmpSrc vpsubsb_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vpsubsb", VEX_PD, OP2_PSUBSB_VdqWdq, src, dst);
  }
  [[nodiscard]] JmpSrc vpsubusb_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vpsubusb", VEX_PD, OP2_PSUBUSB_VdqWdq, src, dst);
  }
  [[nodiscard]] JmpSrc vpsubsw_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vpsubsw", VEX_PD, OP2_PSUBSW_VdqWdq, src, dst);
  }
  [[nodiscard]] JmpSrc vpsubusw_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vpsubusw", VEX_PD, OP2_PSUBUSW_VdqWdq, src, dst);
  }
  [[nodiscard]] JmpSrc vpminsb_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return threeByteRipOpSimd("vpminsb", VEX_PD, OP3_PMINSB_VdqWdq, ESCAPE_38,
                              src, dst);
  }
  [[nodiscard]] JmpSrc vpminub_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vpminub", VEX_PD, OP2_PMINUB_VdqWdq, src, dst);
  }
  [[nodiscard]] JmpSrc vpminsw_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vpminsw", VEX_PD, OP2_PMINSW_VdqWdq, src, dst);
  }
  [[nodiscard]] JmpSrc vpminuw_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return threeByteRipOpSimd("vpminuw", VEX_PD, OP3_PMINUW_VdqWdq, ESCAPE_38,
                              src, dst);
  }
  [[nodiscard]] JmpSrc vpminsd_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return threeByteRipOpSimd("vpminsd", VEX_PD, OP3_PMINSD_VdqWdq, ESCAPE_38,
                              src, dst);
  }
  [[nodiscard]] JmpSrc vpminud_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return threeByteRipOpSimd("vpminud", VEX_PD, OP3_PMINUD_VdqWdq, ESCAPE_38,
                              src, dst);
  }
  [[nodiscard]] JmpSrc vpmaxsb_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return threeByteRipOpSimd("vpmaxsb", VEX_PD, OP3_PMAXSB_VdqWdq, ESCAPE_38,
                              src, dst);
  }
  [[nodiscard]] JmpSrc vpmaxub_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vpmaxub", VEX_PD, OP2_PMAXUB_VdqWdq, src, dst);
  }
  [[nodiscard]] JmpSrc vpmaxsw_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vpmaxsw", VEX_PD, OP2_PMAXSW_VdqWdq, src, dst);
  }
  [[nodiscard]] JmpSrc vpmaxuw_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return threeByteRipOpSimd("vpmaxuw", VEX_PD, OP3_PMAXUW_VdqWdq, ESCAPE_38,
                              src, dst);
  }
  [[nodiscard]] JmpSrc vpmaxsd_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return threeByteRipOpSimd("vpmaxsd", VEX_PD, OP3_PMAXSD_VdqWdq, ESCAPE_38,
                              src, dst);
  }
  [[nodiscard]] JmpSrc vpmaxud_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return threeByteRipOpSimd("vpmaxud", VEX_PD, OP3_PMAXUD_VdqWdq, ESCAPE_38,
                              src, dst);
  }
  [[nodiscard]] JmpSrc vpand_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vpand", VEX_PD, OP2_PANDDQ_VdqWdq, src, dst);
  }
  [[nodiscard]] JmpSrc vpxor_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vpxor", VEX_PD, OP2_PXORDQ_VdqWdq, src, dst);
  }
  [[nodiscard]] JmpSrc vpor_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vpor", VEX_PD, OP2_PORDQ_VdqWdq, src, dst);
  }
  [[nodiscard]] JmpSrc vaddps_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vaddps", VEX_PS, OP2_ADDPS_VpsWps, src, dst);
  }
  [[nodiscard]] JmpSrc vaddpd_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vaddpd", VEX_PD, OP2_ADDPD_VpdWpd, src, dst);
  }
  [[nodiscard]] JmpSrc vsubps_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vsubps", VEX_PS, OP2_SUBPS_VpsWps, src, dst);
  }
  [[nodiscard]] JmpSrc vsubpd_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vsubpd", VEX_PD, OP2_SUBPD_VpdWpd, src, dst);
  }
  [[nodiscard]] JmpSrc vdivps_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vdivps", VEX_PS, OP2_DIVPS_VpsWps, src, dst);
  }
  [[nodiscard]] JmpSrc vdivpd_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vdivpd", VEX_PD, OP2_DIVPD_VpdWpd, src, dst);
  }
  [[nodiscard]] JmpSrc vmulps_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vmulps", VEX_PS, OP2_MULPS_VpsWps, src, dst);
  }
  [[nodiscard]] JmpSrc vmulpd_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vmulpd", VEX_PD, OP2_MULPD_VpdWpd, src, dst);
  }
  [[nodiscard]] JmpSrc vandpd_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vandpd", VEX_PD, OP2_ANDPD_VpdWpd, src, dst);
  }
  [[nodiscard]] JmpSrc vminpd_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vminpd", VEX_PD, OP2_MINPD_VpdWpd, src, dst);
  }
  [[nodiscard]] JmpSrc vpacksswb_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vpacksswb", VEX_PD, OP2_PACKSSWB_VdqWdq, src, dst);
  }
  [[nodiscard]] JmpSrc vpackuswb_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vpackuswb", VEX_PD, OP2_PACKUSWB_VdqWdq, src, dst);
  }
  [[nodiscard]] JmpSrc vpackssdw_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vpackssdw", VEX_PD, OP2_PACKSSDW_VdqWdq, src, dst);
  }
  [[nodiscard]] JmpSrc vpackusdw_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return threeByteRipOpSimd("vpackusdw", VEX_PD, OP3_PACKUSDW_VdqWdq,
                              ESCAPE_38, src, dst);
  }
  [[nodiscard]] JmpSrc vpunpckldq_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vpunpckldq", VEX_PD, OP2_PUNPCKLDQ_VdqWdq, src,
                            dst);
  }
  [[nodiscard]] JmpSrc vunpcklps_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vunpcklps", VEX_PS, OP2_UNPCKLPS_VsdWsd, src, dst);
  }
  [[nodiscard]] JmpSrc vptest_ripr(XMMRegisterID lhs) {
    return threeByteRipOpSimd("vptest", VEX_PD, OP3_PTEST_VdVd, ESCAPE_38, lhs);
  }
  [[nodiscard]] JmpSrc vpshufb_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return threeByteRipOpSimd("vpshufb", VEX_PD, OP3_PSHUFB_VdqWdq, ESCAPE_38,
                              src, dst);
  }
  [[nodiscard]] JmpSrc vpmaddwd_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vpmaddwd", VEX_PD, OP2_PMADDWD_VdqWdq, src, dst);
  }
  [[nodiscard]] JmpSrc vpcmpeqb_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vpcmpeqb", VEX_PD, OP2_PCMPEQB_VdqWdq, src, dst);
  }
  [[nodiscard]] JmpSrc vpcmpgtb_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vpcmpgtb", VEX_PD, OP2_PCMPGTB_VdqWdq, src, dst);
  }
  [[nodiscard]] JmpSrc vpcmpeqw_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vpcmpeqw", VEX_PD, OP2_PCMPEQW_VdqWdq, src, dst);
  }
  [[nodiscard]] JmpSrc vpcmpgtw_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vpcmpgtw", VEX_PD, OP2_PCMPGTW_VdqWdq, src, dst);
  }
  [[nodiscard]] JmpSrc vpcmpeqd_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vpcmpeqd", VEX_PD, OP2_PCMPEQD_VdqWdq, src, dst);
  }
  [[nodiscard]] JmpSrc vpcmpgtd_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vpcmpgtd", VEX_PD, OP2_PCMPGTD_VdqWdq, src, dst);
  }
  [[nodiscard]] JmpSrc vcmpeqps_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpImmSimd("vcmpps", VEX_PS, OP2_CMPPS_VpsWps,
                               X86Encoding::ConditionCmp_EQ, src, dst);
  }
  [[nodiscard]] JmpSrc vcmpneqps_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpImmSimd("vcmpps", VEX_PS, OP2_CMPPS_VpsWps,
                               X86Encoding::ConditionCmp_NEQ, src, dst);
  }
  [[nodiscard]] JmpSrc vcmpltps_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpImmSimd("vcmpps", VEX_PS, OP2_CMPPS_VpsWps,
                               X86Encoding::ConditionCmp_LT, src, dst);
  }
  [[nodiscard]] JmpSrc vcmpleps_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpImmSimd("vcmpps", VEX_PS, OP2_CMPPS_VpsWps,
                               X86Encoding::ConditionCmp_LE, src, dst);
  }
  [[nodiscard]] JmpSrc vcmpgeps_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpImmSimd("vcmpps", VEX_PS, OP2_CMPPS_VpsWps,
                               X86Encoding::ConditionCmp_GE, src, dst);
  }
  [[nodiscard]] JmpSrc vcmpeqpd_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpImmSimd("vcmppd", VEX_PD, OP2_CMPPD_VpdWpd,
                               X86Encoding::ConditionCmp_EQ, src, dst);
  }
  [[nodiscard]] JmpSrc vcmpneqpd_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpImmSimd("vcmppd", VEX_PD, OP2_CMPPD_VpdWpd,
                               X86Encoding::ConditionCmp_NEQ, src, dst);
  }
  [[nodiscard]] JmpSrc vcmpltpd_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpImmSimd("vcmppd", VEX_PD, OP2_CMPPD_VpdWpd,
                               X86Encoding::ConditionCmp_LT, src, dst);
  }
  [[nodiscard]] JmpSrc vcmplepd_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpImmSimd("vcmppd", VEX_PD, OP2_CMPPD_VpdWpd,
                               X86Encoding::ConditionCmp_LE, src, dst);
  }
  [[nodiscard]] JmpSrc vpmaddubsw_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return threeByteRipOpSimd("vpmaddubsw", VEX_PD, OP3_PMADDUBSW_VdqWdq,
                              ESCAPE_38, src, dst);
  }
  [[nodiscard]] JmpSrc vpmuludq_ripr(XMMRegisterID src, XMMRegisterID dst) {
    return twoByteRipOpSimd("vpmuludq", VEX_PD, OP2_PMULUDQ_VdqWdq, src, dst);
  }

  // BMI instructions:

  void sarxq_rrr(RegisterID src, RegisterID shift, RegisterID dst) {
    spew("sarxq      %s, %s, %s", GPReg64Name(src), GPReg64Name(shift),
         GPReg64Name(dst));

    RegisterID rm = src;
    XMMRegisterID src0 = static_cast<XMMRegisterID>(shift);
    int reg = dst;
    m_formatter.threeByteOpVex64(VEX_SS /* = F3 */, OP3_SARX_GyEyBy, ESCAPE_38,
                                 rm, src0, reg);
  }

  void shlxq_rrr(RegisterID src, RegisterID shift, RegisterID dst) {
    spew("shlxq      %s, %s, %s", GPReg64Name(src), GPReg64Name(shift),
         GPReg64Name(dst));

    RegisterID rm = src;
    XMMRegisterID src0 = static_cast<XMMRegisterID>(shift);
    int reg = dst;
    m_formatter.threeByteOpVex64(VEX_PD /* = 66 */, OP3_SHLX_GyEyBy, ESCAPE_38,
                                 rm, src0, reg);
  }

  void shrxq_rrr(RegisterID src, RegisterID shift, RegisterID dst) {
    spew("shrxq      %s, %s, %s", GPReg64Name(src), GPReg64Name(shift),
         GPReg64Name(dst));

    RegisterID rm = src;
    XMMRegisterID src0 = static_cast<XMMRegisterID>(shift);
    int reg = dst;
    m_formatter.threeByteOpVex64(VEX_SD /* = F2 */, OP3_SHRX_GyEyBy, ESCAPE_38,
                                 rm, src0, reg);
  }

 private:
  [[nodiscard]] JmpSrc twoByteRipOpSimd(const char* name, VexOperandType ty,
                                        TwoByteOpcodeID opcode,
                                        XMMRegisterID reg) {
    MOZ_ASSERT(!IsXMMReversedOperands(opcode));
    m_formatter.legacySSEPrefix(ty);
    m_formatter.twoByteRipOp(opcode, 0, reg);
    JmpSrc label(m_formatter.size());
    spew("%-11s " MEM_o32r ", %s", legacySSEOpName(name),
         ADDR_o32r(label.offset()), XMMRegName(reg));
    return label;
  }

  [[nodiscard]] JmpSrc twoByteRipOpSimd(const char* name, VexOperandType ty,
                                        TwoByteOpcodeID opcode,
                                        XMMRegisterID src0, XMMRegisterID dst) {
    MOZ_ASSERT(src0 != invalid_xmm && !IsXMMReversedOperands(opcode));
    if (useLegacySSEEncoding(src0, dst)) {
      m_formatter.legacySSEPrefix(ty);
      m_formatter.twoByteRipOp(opcode, 0, dst);
      JmpSrc label(m_formatter.size());
      spew("%-11s" MEM_o32r ", %s", legacySSEOpName(name),
           ADDR_o32r(label.offset()), XMMRegName(dst));
      return label;
    }

    m_formatter.twoByteRipOpVex(ty, opcode, 0, src0, dst);
    JmpSrc label(m_formatter.size());
    spew("%-11s, " MEM_o32r ", %s, %s", name, ADDR_o32r(label.offset()),
         XMMRegName(src0), XMMRegName(dst));
    return label;
  }

  [[nodiscard]] JmpSrc twoByteRipOpImmSimd(const char* name, VexOperandType ty,
                                           TwoByteOpcodeID opcode, uint32_t imm,
                                           XMMRegisterID src0,
                                           XMMRegisterID dst) {
    MOZ_ASSERT(src0 != invalid_xmm && !IsXMMReversedOperands(opcode));
    if (useLegacySSEEncoding(src0, dst)) {
      m_formatter.legacySSEPrefix(ty);
      m_formatter.twoByteRipOp(opcode, 0, dst);
      m_formatter.immediate8u(imm);
      JmpSrc label(m_formatter.size(),
                   /* bytes trailing the patch field = */ 1);
      spew("%-11s$0x%x, " MEM_o32r ", %s", legacySSEOpName(name), imm,
           ADDR_o32r(label.offset()), XMMRegName(dst));
      return label;
    }

    m_formatter.twoByteRipOpVex(ty, opcode, 0, src0, dst);
    m_formatter.immediate8u(imm);
    JmpSrc label(m_formatter.size(),
                 /* bytes trailing the patch field = */ 1);
    spew("%-11s$0x%x, " MEM_o32r ", %s, %s", name, imm,
         ADDR_o32r(label.offset()), XMMRegName(src0), XMMRegName(dst));
    return label;
  }

  void twoByteOpInt64Simd(const char* name, VexOperandType ty,
                          TwoByteOpcodeID opcode, RegisterID rm,
                          XMMRegisterID src0, XMMRegisterID dst) {
    if (useLegacySSEEncoding(src0, dst)) {
      if (IsXMMReversedOperands(opcode)) {
        spew("%-11s%s, %s", legacySSEOpName(name), XMMRegName(dst),
             GPRegName(rm));
      } else {
        spew("%-11s%s, %s", legacySSEOpName(name), GPRegName(rm),
             XMMRegName(dst));
      }
      m_formatter.legacySSEPrefix(ty);
      m_formatter.twoByteOp64(opcode, rm, dst);
      return;
    }

    if (src0 == invalid_xmm) {
      if (IsXMMReversedOperands(opcode)) {
        spew("%-11s%s, %s", name, XMMRegName(dst), GPRegName(rm));
      } else {
        spew("%-11s%s, %s", name, GPRegName(rm), XMMRegName(dst));
      }
    } else {
      spew("%-11s%s, %s, %s", name, GPRegName(rm), XMMRegName(src0),
           XMMRegName(dst));
    }
    m_formatter.twoByteOpVex64(ty, opcode, rm, src0, dst);
  }

  void twoByteOpSimdInt64(const char* name, VexOperandType ty,
                          TwoByteOpcodeID opcode, XMMRegisterID rm,
                          RegisterID dst) {
    if (useLegacySSEEncodingAlways()) {
      if (IsXMMReversedOperands(opcode)) {
        spew("%-11s%s, %s", legacySSEOpName(name), GPRegName(dst),
             XMMRegName(rm));
      } else if (opcode == OP2_MOVD_EdVd) {
        spew("%-11s%s, %s", legacySSEOpName(name),
             XMMRegName((XMMRegisterID)dst), GPRegName((RegisterID)rm));
      } else {
        spew("%-11s%s, %s", legacySSEOpName(name), XMMRegName(rm),
             GPRegName(dst));
      }
      m_formatter.legacySSEPrefix(ty);
      m_formatter.twoByteOp64(opcode, (RegisterID)rm, dst);
      return;
    }

    if (IsXMMReversedOperands(opcode)) {
      spew("%-11s%s, %s", name, GPRegName(dst), XMMRegName(rm));
    } else if (opcode == OP2_MOVD_EdVd) {
      spew("%-11s%s, %s", name, XMMRegName((XMMRegisterID)dst),
           GPRegName((RegisterID)rm));
    } else {
      spew("%-11s%s, %s", name, XMMRegName(rm), GPRegName(dst));
    }
    m_formatter.twoByteOpVex64(ty, opcode, (RegisterID)rm, invalid_xmm,
                               (XMMRegisterID)dst);
  }

  [[nodiscard]] JmpSrc threeByteRipOpSimd(const char* name, VexOperandType ty,
                                          ThreeByteOpcodeID opcode,
                                          ThreeByteEscape escape,
                                          XMMRegisterID dst) {
    m_formatter.legacySSEPrefix(ty);
    m_formatter.threeByteRipOp(opcode, escape, 0, dst);
    JmpSrc label(m_formatter.size());
    spew("%-11s" MEM_o32r ", %s", legacySSEOpName(name),
         ADDR_o32r(label.offset()), XMMRegName(dst));
    return label;
  }

  [[nodiscard]] JmpSrc threeByteRipOpSimd(const char* name, VexOperandType ty,
                                          ThreeByteOpcodeID opcode,
                                          ThreeByteEscape escape,
                                          XMMRegisterID src0,
                                          XMMRegisterID dst) {
    MOZ_ASSERT(src0 != invalid_xmm);
    if (useLegacySSEEncoding(src0, dst)) {
      m_formatter.legacySSEPrefix(ty);
      m_formatter.threeByteRipOp(opcode, escape, 0, dst);
      JmpSrc label(m_formatter.size());
      spew("%-11s" MEM_o32r ", %s", legacySSEOpName(name),
           ADDR_o32r(label.offset()), XMMRegName(dst));
      return label;
    }

    m_formatter.threeByteRipOpVex(ty, opcode, escape, 0, src0, dst);
    JmpSrc label(m_formatter.size());
    spew("%-11s" MEM_o32r ", %s, %s", name, ADDR_o32r(label.offset()),
         XMMRegName(src0), XMMRegName(dst));
    return label;
  }

  void threeByteOpImmSimdInt64(const char* name, VexOperandType ty,
                               ThreeByteOpcodeID opcode, ThreeByteEscape escape,
                               uint32_t imm, XMMRegisterID src,
                               RegisterID dst) {
    spew("%-11s$0x%x, %s, %s", legacySSEOpName(name), imm, GPReg64Name(dst),
         XMMRegName(src));
    m_formatter.legacySSEPrefix(ty);
    m_formatter.threeByteOp64(opcode, escape, dst, (RegisterID)src);
    m_formatter.immediate8u(imm);
  }

  void threeByteOpImmInt64Simd(const char* name, VexOperandType ty,
                               ThreeByteOpcodeID opcode, ThreeByteEscape escape,
                               uint32_t imm, RegisterID src1,
                               XMMRegisterID src0, XMMRegisterID dst) {
    if (useLegacySSEEncoding(src0, dst)) {
      spew("%-11s$0x%x, %s, %s", legacySSEOpName(name), imm, GPReg64Name(src1),
           XMMRegName(dst));
      m_formatter.legacySSEPrefix(ty);
      m_formatter.threeByteOp64(opcode, escape, src1, (RegisterID)dst);
      m_formatter.immediate8u(imm);
      return;
    }

    MOZ_ASSERT(src0 != invalid_xmm);
    spew("%-11s$0x%x, %s, %s, %s", name, imm, GPReg64Name(src1),
         XMMRegName(src0), XMMRegName(dst));
    m_formatter.threeByteOpVex64(ty, opcode, escape, src1, src0,
                                 (RegisterID)dst);
    m_formatter.immediate8u(imm);
  }
};

using BaseAssemblerSpecific = BaseAssemblerX64;

}  // namespace X86Encoding

}  // namespace jit
}  // namespace js

#endif /* jit_x64_BaseAssembler_x64_h */
