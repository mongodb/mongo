/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x86_shared_Encoding_x86_shared_h
#define jit_x86_shared_Encoding_x86_shared_h

#include <type_traits>

#include "jit/x86-shared/Constants-x86-shared.h"

namespace js {
namespace jit {

namespace X86Encoding {

static const size_t MaxInstructionSize = 16;

// These enumerated values are following the Intel documentation Volume 2C [1],
// Appendix A.2 and Appendix A.3.
//
// Operand size/types as listed in the Appendix A.2.  Tables of the instructions
// and their operands can be found in the Appendix A.3.
//
// B = reg (VEX.vvvv of VEX prefix)
// E = reg/mem
// G = reg (reg field of ModR/M)
// U = xmm (R/M field of ModR/M)
// V = xmm (reg field of ModR/M)
// W = xmm/mem64
// I = immediate
// O = offset
//
// b = byte (8-bit)
// w = word (16-bit)
// v = register size
// d = double (32-bit)
// dq = double-quad (128-bit) (xmm)
// ss = scalar float 32 (xmm)
// ps = packed float 32 (xmm)
// sd = scalar double (xmm)
// pd = packed double (xmm)
// y = 32/64-bit
// z = 16/32/64-bit
// vqp = (*)
//
// (*) Some website [2] provides a convenient list of all instructions, but be
// aware that they do not follow the Intel documentation naming, as the
// following enumeration does. Do not use these names as a reference for adding
// new instructions.
//
// [1]
// http://www.intel.com/content/www/us/en/architecture-and-technology/64-ia-32-architectures-software-developer-manual-325462.html
// [2] http://ref.x86asm.net/geek.html
//
// OPn_NAME_DstSrc
enum OneByteOpcodeID {
  OP_NOP_00 = 0x00,
  OP_ADD_EbGb = 0x00,
  OP_ADD_EvGv = 0x01,
  OP_ADD_GvEv = 0x03,
  OP_ADD_EAXIv = 0x05,
  OP_OR_EbGb = 0x08,
  OP_OR_EvGv = 0x09,
  OP_OR_GvEv = 0x0B,
  OP_OR_EAXIv = 0x0D,
  OP_2BYTE_ESCAPE = 0x0F,
  OP_NOP_0F = 0x0F,
  OP_ADC_GvEv = 0x13,
  OP_SBB_GvEv = 0x1B,
  OP_NOP_1F = 0x1F,
  OP_AND_EbGb = 0x20,
  OP_AND_EvGv = 0x21,
  OP_AND_GvEv = 0x23,
  OP_AND_EAXIv = 0x25,
  OP_SUB_EbGb = 0x28,
  OP_SUB_EvGv = 0x29,
  OP_SUB_GvEv = 0x2B,
  OP_SUB_EAXIv = 0x2D,
  PRE_PREDICT_BRANCH_NOT_TAKEN = 0x2E,
  OP_XOR_EbGb = 0x30,
  OP_XOR_EvGv = 0x31,
  OP_XOR_GvEv = 0x33,
  OP_XOR_EAXIv = 0x35,
  OP_CMP_EbGb = 0x38,
  OP_CMP_EvGv = 0x39,
  OP_CMP_GbEb = 0x3A,
  OP_CMP_GvEv = 0x3B,
  OP_CMP_EAXIb = 0x3C,
  OP_CMP_EAXIv = 0x3D,
#ifdef JS_CODEGEN_X64
  PRE_REX = 0x40,
#endif
  OP_NOP_40 = 0x40,
  OP_NOP_44 = 0x44,
  OP_PUSH_EAX = 0x50,
  OP_POP_EAX = 0x58,
#ifdef JS_CODEGEN_X86
  OP_PUSHA = 0x60,
  OP_POPA = 0x61,
#endif
#ifdef JS_CODEGEN_X64
  OP_MOVSXD_GvEv = 0x63,
#endif
  PRE_OPERAND_SIZE = 0x66,
  PRE_SSE_66 = 0x66,
  OP_NOP_66 = 0x66,
  OP_PUSH_Iz = 0x68,
  OP_IMUL_GvEvIz = 0x69,
  OP_PUSH_Ib = 0x6a,
  OP_IMUL_GvEvIb = 0x6b,
  OP_JCC_rel8 = 0x70,
  OP_GROUP1_EbIb = 0x80,
  OP_NOP_80 = 0x80,
  OP_GROUP1_EvIz = 0x81,
  OP_GROUP1_EvIb = 0x83,
  OP_TEST_EbGb = 0x84,
  OP_NOP_84 = 0x84,
  OP_TEST_EvGv = 0x85,
  OP_XCHG_GbEb = 0x86,
  OP_XCHG_GvEv = 0x87,
  OP_MOV_EbGv = 0x88,
  OP_MOV_EvGv = 0x89,
  OP_MOV_GvEb = 0x8A,
  OP_MOV_GvEv = 0x8B,
  OP_LEA = 0x8D,
  OP_GROUP1A_Ev = 0x8F,
  OP_NOP = 0x90,
  OP_PUSHFLAGS = 0x9C,
  OP_POPFLAGS = 0x9D,
  OP_CDQ = 0x99,
  OP_MOV_EAXOv = 0xA1,
  OP_MOV_OvEAX = 0xA3,
  OP_TEST_EAXIb = 0xA8,
  OP_TEST_EAXIv = 0xA9,
  OP_MOV_EbIb = 0xB0,
  OP_MOV_EAXIv = 0xB8,
  OP_GROUP2_EvIb = 0xC1,
  OP_ADDP_ST0_ST1 = 0xC1,
  OP_RET_Iz = 0xC2,
  PRE_VEX_C4 = 0xC4,
  PRE_VEX_C5 = 0xC5,
  OP_RET = 0xC3,
  OP_GROUP11_EvIb = 0xC6,
  OP_GROUP11_EvIz = 0xC7,
  OP_INT3 = 0xCC,
  OP_GROUP2_Ev1 = 0xD1,
  OP_GROUP2_EvCL = 0xD3,
  OP_FPU6 = 0xDD,
  OP_FPU6_F32 = 0xD9,
  OP_FPU6_ADDP = 0xDE,
  OP_FILD = 0xDF,
  OP_CALL_rel32 = 0xE8,
  OP_JMP_rel32 = 0xE9,
  OP_JMP_rel8 = 0xEB,
  PRE_LOCK = 0xF0,
  PRE_SSE_F2 = 0xF2,
  PRE_SSE_F3 = 0xF3,
  PRE_REP = 0xF3,
  OP_HLT = 0xF4,
  OP_GROUP3_EbIb = 0xF6,
  OP_GROUP3_Ev = 0xF7,
  OP_GROUP3_EvIz =
      0xF7,  // OP_GROUP3_Ev has an immediate, when instruction is a test.
  OP_GROUP5_Ev = 0xFF
};

enum class ShiftID {
  vpsrlx = 2,
  vpsrldq = 3,
  vpsrad = 4,
  vpsllx = 6,
  vpslldq = 7
};

enum TwoByteOpcodeID {
  OP2_UD2 = 0x0B,
  OP2_MOVSD_VsdWsd = 0x10,
  OP2_MOVPS_VpsWps = 0x10,
  OP2_MOVSD_WsdVsd = 0x11,
  OP2_MOVPS_WpsVps = 0x11,
  OP2_MOVDDUP_VqWq = 0x12,
  OP2_MOVHLPS_VqUq = 0x12,
  OP2_MOVSLDUP_VpsWps = 0x12,
  OP2_MOVLPS_VqEq = 0x12,
  OP2_MOVLPS_EqVq = 0x13,
  OP2_UNPCKLPS_VsdWsd = 0x14,
  OP2_UNPCKHPS_VsdWsd = 0x15,
  OP2_MOVLHPS_VqUq = 0x16,
  OP2_MOVSHDUP_VpsWps = 0x16,
  OP2_MOVHPS_VqEq = 0x16,
  OP2_MOVHPS_EqVq = 0x17,
  OP2_MOVAPD_VsdWsd = 0x28,
  OP2_MOVAPS_VsdWsd = 0x28,
  OP2_MOVAPS_WsdVsd = 0x29,
  OP2_CVTSI2SD_VsdEd = 0x2A,
  OP2_CVTTSD2SI_GdWsd = 0x2C,
  OP2_UCOMISD_VsdWsd = 0x2E,
  OP2_CMOVCC_GvEv = 0x40,
  OP2_MOVMSKPD_EdVd = 0x50,
  OP2_ANDPS_VpsWps = 0x54,
  OP2_ANDNPS_VpsWps = 0x55,
  OP2_ORPS_VpsWps = 0x56,
  OP2_XORPS_VpsWps = 0x57,
  OP2_ADDSD_VsdWsd = 0x58,
  OP2_ADDPS_VpsWps = 0x58,
  OP2_ADDPD_VpdWpd = 0x58,
  OP2_MULSD_VsdWsd = 0x59,
  OP2_MULPD_VpdWpd = 0x59,
  OP2_MULPS_VpsWps = 0x59,
  OP2_CVTSS2SD_VsdEd = 0x5A,
  OP2_CVTSD2SS_VsdEd = 0x5A,
  OP2_CVTPS2PD_VpdWps = 0x5A,
  OP2_CVTPD2PS_VpsWpd = 0x5A,
  OP2_CVTTPS2DQ_VdqWps = 0x5B,
  OP2_CVTDQ2PS_VpsWdq = 0x5B,
  OP2_SUBSD_VsdWsd = 0x5C,
  OP2_SUBPS_VpsWps = 0x5C,
  OP2_SUBPD_VpdWpd = 0x5C,
  OP2_MINSD_VsdWsd = 0x5D,
  OP2_MINSS_VssWss = 0x5D,
  OP2_MINPS_VpsWps = 0x5D,
  OP2_MINPD_VpdWpd = 0x5D,
  OP2_DIVSD_VsdWsd = 0x5E,
  OP2_DIVPS_VpsWps = 0x5E,
  OP2_DIVPD_VpdWpd = 0x5E,
  OP2_MAXSD_VsdWsd = 0x5F,
  OP2_MAXSS_VssWss = 0x5F,
  OP2_MAXPS_VpsWps = 0x5F,
  OP2_MAXPD_VpdWpd = 0x5F,
  OP2_SQRTSD_VsdWsd = 0x51,
  OP2_SQRTSS_VssWss = 0x51,
  OP2_SQRTPS_VpsWps = 0x51,
  OP2_SQRTPD_VpdWpd = 0x51,
  OP2_RSQRTPS_VpsWps = 0x52,
  OP2_RCPPS_VpsWps = 0x53,
  OP2_ANDPD_VpdWpd = 0x54,
  OP2_ORPD_VpdWpd = 0x56,
  OP2_XORPD_VpdWpd = 0x57,
  OP2_PUNPCKLBW_VdqWdq = 0x60,
  OP2_PUNPCKLWD_VdqWdq = 0x61,
  OP2_PUNPCKLDQ_VdqWdq = 0x62,
  OP2_PACKSSWB_VdqWdq = 0x63,
  OP2_PCMPGTB_VdqWdq = 0x64,
  OP2_PCMPGTW_VdqWdq = 0x65,
  OP2_PCMPGTD_VdqWdq = 0x66,
  OP2_PACKUSWB_VdqWdq = 0x67,
  OP2_PUNPCKHBW_VdqWdq = 0x68,
  OP2_PUNPCKHWD_VdqWdq = 0x69,
  OP2_PUNPCKHDQ_VdqWdq = 0x6A,
  OP2_PACKSSDW_VdqWdq = 0x6B,
  OP2_PUNPCKLQDQ_VdqWdq = 0x6C,
  OP2_PUNPCKHQDQ_VdqWdq = 0x6D,
  OP2_MOVD_VdEd = 0x6E,
  OP2_MOVDQ_VsdWsd = 0x6F,
  OP2_MOVDQ_VdqWdq = 0x6F,
  OP2_PSHUFD_VdqWdqIb = 0x70,
  OP2_PSHUFLW_VdqWdqIb = 0x70,
  OP2_PSHUFHW_VdqWdqIb = 0x70,
  OP2_PSLLW_UdqIb = 0x71,
  OP2_PSRAW_UdqIb = 0x71,
  OP2_PSRLW_UdqIb = 0x71,
  OP2_PSLLD_UdqIb = 0x72,
  OP2_PSRAD_UdqIb = 0x72,
  OP2_PSRLD_UdqIb = 0x72,
  OP2_PSRLDQ_Vd = 0x73,
  OP2_PCMPEQB_VdqWdq = 0x74,
  OP2_PCMPEQW_VdqWdq = 0x75,
  OP2_PCMPEQD_VdqWdq = 0x76,
  OP2_HADDPD = 0x7C,
  OP2_MOVD_EdVd = 0x7E,
  OP2_MOVQ_VdWd = 0x7E,
  OP2_MOVDQ_WdqVdq = 0x7F,
  OP2_JCC_rel32 = 0x80,
  OP_SETCC = 0x90,
  OP2_SHLD = 0xA4,
  OP2_SHLD_GvEv = 0xA5,
  OP2_SHRD = 0xAC,
  OP2_SHRD_GvEv = 0xAD,
  OP_FENCE = 0xAE,
  OP2_IMUL_GvEv = 0xAF,
  OP2_CMPXCHG_GvEb = 0xB0,
  OP2_CMPXCHG_GvEw = 0xB1,
  OP2_POPCNT_GvEv = 0xB8,
  OP2_BSF_GvEv = 0xBC,
  OP2_TZCNT_GvEv = 0xBC,
  OP2_BSR_GvEv = 0xBD,
  OP2_LZCNT_GvEv = 0xBD,
  OP2_MOVSX_GvEb = 0xBE,
  OP2_MOVSX_GvEw = 0xBF,
  OP2_MOVZX_GvEb = 0xB6,
  OP2_MOVZX_GvEw = 0xB7,
  OP2_XADD_EbGb = 0xC0,
  OP2_XADD_EvGv = 0xC1,
  OP2_CMPPS_VpsWps = 0xC2,
  OP2_CMPPD_VpdWpd = 0xC2,
  OP2_PINSRW = 0xC4,
  OP2_PEXTRW_GdUdIb = 0xC5,
  OP2_SHUFPS_VpsWpsIb = 0xC6,
  OP2_SHUFPD_VpdWpdIb = 0xC6,
  OP2_CMPXCHGNB = 0xC7,  // CMPXCHG8B; CMPXCHG16B with REX
  OP2_BSWAP = 0xC8,
  OP2_PSRLW_VdqWdq = 0xD1,
  OP2_PSRLD_VdqWdq = 0xD2,
  OP2_PSRLQ_VdqWdq = 0xD3,
  OP2_PADDQ_VdqWdq = 0xD4,
  OP2_PMULLW_VdqWdq = 0xD5,
  OP2_MOVQ_WdVd = 0xD6,
  OP2_PMOVMSKB_EdVd = 0xD7,
  OP2_PSUBUSB_VdqWdq = 0xD8,
  OP2_PSUBUSW_VdqWdq = 0xD9,
  OP2_PMINUB_VdqWdq = 0xDA,
  OP2_PANDDQ_VdqWdq = 0xDB,
  OP2_PADDUSB_VdqWdq = 0xDC,
  OP2_PADDUSW_VdqWdq = 0xDD,
  OP2_PMAXUB_VdqWdq = 0xDE,
  OP2_PANDNDQ_VdqWdq = 0xDF,
  OP2_PAVGB_VdqWdq = 0xE0,
  OP2_PSRAW_VdqWdq = 0xE1,
  OP2_PSRAD_VdqWdq = 0xE2,
  OP2_PAVGW_VdqWdq = 0xE3,
  OP2_PMULHUW_VdqWdq = 0xE4,
  OP2_PMULHW_VdqWdq = 0xE5,
  OP2_CVTDQ2PD_VpdWdq = 0xE6,
  OP2_CVTTPD2DQ_VdqWpd = 0xE6,
  OP2_PSUBSB_VdqWdq = 0xE8,
  OP2_PSUBSW_VdqWdq = 0xE9,
  OP2_PMINSW_VdqWdq = 0xEA,
  OP2_PORDQ_VdqWdq = 0xEB,
  OP2_PADDSB_VdqWdq = 0xEC,
  OP2_PADDSW_VdqWdq = 0xED,
  OP2_PMAXSW_VdqWdq = 0xEE,
  OP2_PXORDQ_VdqWdq = 0xEF,
  OP2_PSLLW_VdqWdq = 0xF1,
  OP2_PSLLD_VdqWdq = 0xF2,
  OP2_PSLLQ_VdqWdq = 0xF3,
  OP2_PMULUDQ_VdqWdq = 0xF4,
  OP2_PMADDWD_VdqWdq = 0xF5,
  OP2_PSUBB_VdqWdq = 0xF8,
  OP2_PSUBW_VdqWdq = 0xF9,
  OP2_PSUBD_VdqWdq = 0xFA,
  OP2_PSUBQ_VdqWdq = 0xFB,
  OP2_PADDB_VdqWdq = 0xFC,
  OP2_PADDW_VdqWdq = 0xFD,
  OP2_PADDD_VdqWdq = 0xFE
};

enum ThreeByteOpcodeID {
  OP3_PSHUFB_VdqWdq = 0x00,
  OP3_PHADDD_VdqWdq = 0x02,
  OP3_PMADDUBSW_VdqWdq = 0x04,
  OP3_ROUNDPS_VpsWps = 0x08,
  OP3_ROUNDPD_VpdWpd = 0x09,
  OP3_ROUNDSS_VsdWsd = 0x0A,
  OP3_PSIGND_PdqQdq = 0x0A,
  OP3_ROUNDSD_VsdWsd = 0x0B,
  OP3_PMULHRSW_VdqWdq = 0x0B,
  OP3_BLENDPS_VpsWpsIb = 0x0C,
  OP3_PBLENDW_VdqWdqIb = 0x0E,
  OP3_PALIGNR_VdqWdqIb = 0x0F,
  OP3_PBLENDVB_VdqWdq = 0x10,
  OP3_VCVTPH2PS_VxWxIb = 0x13,
  OP3_BLENDVPS_VdqWdq = 0x14,
  OP3_PEXTRB_EvVdqIb = 0x14,
  OP3_PEXTRW_EwVdqIb = 0x15,
  OP3_BLENDVPD_VdqWdq = 0x15,
  OP3_PEXTRD_EvVdqIb = 0x16,
  OP3_PEXTRQ_EvVdqIb = 0x16,
  OP3_PTEST_VdVd = 0x17,
  OP3_EXTRACTPS_EdVdqIb = 0x17,
  OP3_VBROADCASTSS_VxWd = 0x18,
  OP3_PABSB_VdqWdq = 0x1C,
  OP3_PABSW_VdqWdq = 0x1D,
  OP3_VCVTPS2PH_WxVxIb = 0x1D,
  OP3_PABSD_VdqWdq = 0x1E,
  OP3_PINSRB_VdqEvIb = 0x20,
  OP3_PMOVSXBW_VdqWdq = 0x20,
  OP3_INSERTPS_VpsUps = 0x21,
  OP3_PINSRD_VdqEvIb = 0x22,
  OP3_PINSRQ_VdqEvIb = 0x22,
  OP3_PMOVSXWD_VdqWdq = 0x23,
  OP3_PMOVSXDQ_VdqWdq = 0x25,
  OP3_PMULDQ_VdqWdq = 0x28,
  OP3_PCMPEQQ_VdqWdq = 0x29,
  OP3_PACKUSDW_VdqWdq = 0x2B,
  OP3_PMOVZXBW_VdqWdq = 0x30,
  OP3_PMOVZXBD_VdqWdq = 0x31,
  OP3_PMOVZXBQ_VdqWdq = 0x32,
  OP3_PMOVZXWD_VdqWdq = 0x33,
  OP3_PMOVZXWQ_VdqWdq = 0x34,
  OP3_PMOVZXDQ_VdqWdq = 0x35,
  OP3_PCMPGTQ_VdqWdq = 0x37,
  OP3_PMINSB_VdqWdq = 0x38,
  OP3_PMINSD_VdqWdq = 0x39,
  OP3_PMINUW_VdqWdq = 0x3A,
  OP3_PMINUD_VdqWdq = 0x3B,
  OP3_PMAXSB_VdqWdq = 0x3C,
  OP3_PMAXSD_VdqWdq = 0x3D,
  OP3_PMAXUW_VdqWdq = 0x3E,
  OP3_PMAXUD_VdqWdq = 0x3F,
  OP3_PMULLD_VdqWdq = 0x40,
  OP3_VBLENDVPS_VdqWdq = 0x4A,
  OP3_VBLENDVPD_VdqWdq = 0x4B,
  OP3_VPBLENDVB_VdqWdq = 0x4C,
  OP3_VBROADCASTD_VxWx = 0x58,
  OP3_VBROADCASTQ_VxWx = 0x59,
  OP3_VBROADCASTB_VxWx = 0x78,
  OP3_VBROADCASTW_VxWx = 0x79,
  OP3_VFMADD231PS_VxHxWx = 0xB8,
  OP3_VFMADD231PD_VxHxWx = 0xB8,
  OP3_VFNMADD231PS_VxHxWx = 0xBC,
  OP3_VFNMADD231PD_VxHxWx = 0xBC,
  OP3_SHLX_GyEyBy = 0xF7,
  OP3_SARX_GyEyBy = 0xF7,
  OP3_SHRX_GyEyBy = 0xF7,
};

// Test whether the given opcode should be printed with its operands reversed.
inline bool IsXMMReversedOperands(TwoByteOpcodeID opcode) {
  switch (opcode) {
    case OP2_MOVSD_WsdVsd:  // also OP2_MOVPS_WpsVps
    case OP2_MOVAPS_WsdVsd:
    case OP2_MOVDQ_WdqVdq:
      return true;
    default:
      break;
  }
  return false;
}

enum ThreeByteEscape { ESCAPE_38 = 0x38, ESCAPE_3A = 0x3A };

enum VexOperandType { VEX_PS = 0, VEX_PD = 1, VEX_SS = 2, VEX_SD = 3 };

inline OneByteOpcodeID jccRel8(Condition cond) {
  return OneByteOpcodeID(OP_JCC_rel8 + std::underlying_type_t<Condition>(cond));
}
inline TwoByteOpcodeID jccRel32(Condition cond) {
  return TwoByteOpcodeID(OP2_JCC_rel32 +
                         std::underlying_type_t<Condition>(cond));
}
inline TwoByteOpcodeID setccOpcode(Condition cond) {
  return TwoByteOpcodeID(OP_SETCC + std::underlying_type_t<Condition>(cond));
}
inline TwoByteOpcodeID cmovccOpcode(Condition cond) {
  return TwoByteOpcodeID(OP2_CMOVCC_GvEv +
                         std::underlying_type_t<Condition>(cond));
}

enum GroupOpcodeID {
  GROUP1_OP_ADD = 0,
  GROUP1_OP_OR = 1,
  GROUP1_OP_ADC = 2,
  GROUP1_OP_SBB = 3,
  GROUP1_OP_AND = 4,
  GROUP1_OP_SUB = 5,
  GROUP1_OP_XOR = 6,
  GROUP1_OP_CMP = 7,

  GROUP1A_OP_POP = 0,

  GROUP2_OP_ROL = 0,
  GROUP2_OP_ROR = 1,
  GROUP2_OP_SHL = 4,
  GROUP2_OP_SHR = 5,
  GROUP2_OP_SAR = 7,

  GROUP3_OP_TEST = 0,
  GROUP3_OP_NOT = 2,
  GROUP3_OP_NEG = 3,
  GROUP3_OP_MUL = 4,
  GROUP3_OP_IMUL = 5,
  GROUP3_OP_DIV = 6,
  GROUP3_OP_IDIV = 7,

  GROUP5_OP_INC = 0,
  GROUP5_OP_DEC = 1,
  GROUP5_OP_CALLN = 2,
  GROUP5_OP_JMPN = 4,
  GROUP5_OP_PUSH = 6,

  FILD_OP_64 = 5,

  FPU6_OP_FLD = 0,
  FPU6_OP_FISTTP = 1,
  FPU6_OP_FSTP = 3,
  FPU6_OP_FLDCW = 5,
  FPU6_OP_FISTP = 7,

  GROUP11_MOV = 0
};

static const RegisterID noBase = rbp;
static const RegisterID hasSib = rsp;
static const RegisterID noIndex = rsp;
#ifdef JS_CODEGEN_X64
static const RegisterID noBase2 = r13;
static const RegisterID hasSib2 = r12;
#endif

enum ModRmMode {
  ModRmMemoryNoDisp,
  ModRmMemoryDisp8,
  ModRmMemoryDisp32,
  ModRmRegister
};

}  // namespace X86Encoding

}  // namespace jit
}  // namespace js

#endif /* jit_x86_shared_Encoding_x86_shared_h */
