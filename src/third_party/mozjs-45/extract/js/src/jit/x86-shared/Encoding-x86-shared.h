/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x86_shared_Encoding_x86_shared_h
#define jit_x86_shared_Encoding_x86_shared_h

#include "jit/x86-shared/Constants-x86-shared.h"

namespace js {
namespace jit {

namespace X86Encoding {

static const size_t MaxInstructionSize = 16;

enum OneByteOpcodeID {
    OP_NOP_00                       = 0x00,
    OP_ADD_EbGb                     = 0x00,
    OP_ADD_EvGv                     = 0x01,
    OP_ADD_GvEv                     = 0x03,
    OP_ADD_EAXIv                    = 0x05,
    OP_OR_EbGb                      = 0x08,
    OP_OR_EvGv                      = 0x09,
    OP_OR_GvEv                      = 0x0B,
    OP_OR_EAXIv                     = 0x0D,
    OP_2BYTE_ESCAPE                 = 0x0F,
    OP_NOP_0F                       = 0x0F,
    OP_ADC_GvEv                     = 0x13,
    OP_NOP_1F                       = 0x1F,
    OP_AND_EbGb                     = 0x20,
    OP_AND_EvGv                     = 0x21,
    OP_AND_GvEv                     = 0x23,
    OP_AND_EAXIv                    = 0x25,
    OP_SUB_EbGb                     = 0x28,
    OP_SUB_EvGv                     = 0x29,
    OP_SUB_GvEv                     = 0x2B,
    OP_SUB_EAXIv                    = 0x2D,
    PRE_PREDICT_BRANCH_NOT_TAKEN    = 0x2E,
    OP_XOR_EbGb                     = 0x30,
    OP_XOR_EvGv                     = 0x31,
    OP_XOR_GvEv                     = 0x33,
    OP_XOR_EAXIv                    = 0x35,
    OP_CMP_EvGv                     = 0x39,
    OP_CMP_GvEv                     = 0x3B,
    OP_CMP_EAXIv                    = 0x3D,
#ifdef JS_CODEGEN_X64
    PRE_REX                         = 0x40,
#endif
    OP_NOP_40                       = 0x40,
    OP_NOP_44                       = 0x44,
    OP_PUSH_EAX                     = 0x50,
    OP_POP_EAX                      = 0x58,
#ifdef JS_CODEGEN_X86
    OP_PUSHA                        = 0x60,
    OP_POPA                         = 0x61,
#endif
#ifdef JS_CODEGEN_X64
    OP_MOVSXD_GvEv                  = 0x63,
#endif
    PRE_OPERAND_SIZE                = 0x66,
    PRE_SSE_66                      = 0x66,
    OP_NOP_66                       = 0x66,
    OP_PUSH_Iz                      = 0x68,
    OP_IMUL_GvEvIz                  = 0x69,
    OP_PUSH_Ib                      = 0x6a,
    OP_IMUL_GvEvIb                  = 0x6b,
    OP_JCC_rel8                     = 0x70,
    OP_GROUP1_EbIb                  = 0x80,
    OP_NOP_80                       = 0x80,
    OP_GROUP1_EvIz                  = 0x81,
    OP_GROUP1_EvIb                  = 0x83,
    OP_TEST_EbGb                    = 0x84,
    OP_NOP_84                       = 0x84,
    OP_TEST_EvGv                    = 0x85,
    OP_XCHG_GbEb                    = 0x86,
    OP_XCHG_GvEv                    = 0x87,
    OP_MOV_EbGv                     = 0x88,
    OP_MOV_EvGv                     = 0x89,
    OP_MOV_GvEb                     = 0x8A,
    OP_MOV_GvEv                     = 0x8B,
    OP_LEA                          = 0x8D,
    OP_GROUP1A_Ev                   = 0x8F,
    OP_NOP                          = 0x90,
    OP_PUSHFLAGS                    = 0x9C,
    OP_POPFLAGS                     = 0x9D,
    OP_CDQ                          = 0x99,
    OP_MOV_EAXOv                    = 0xA1,
    OP_MOV_OvEAX                    = 0xA3,
    OP_TEST_EAXIb                   = 0xA8,
    OP_TEST_EAXIv                   = 0xA9,
    OP_MOV_EbIb                     = 0xB0,
    OP_MOV_EAXIv                    = 0xB8,
    OP_GROUP2_EvIb                  = 0xC1,
    OP_RET_Iz                       = 0xC2,
    PRE_VEX_C4                      = 0xC4,
    PRE_VEX_C5                      = 0xC5,
    OP_RET                          = 0xC3,
    OP_GROUP11_EvIb                 = 0xC6,
    OP_GROUP11_EvIz                 = 0xC7,
    OP_INT3                         = 0xCC,
    OP_GROUP2_Ev1                   = 0xD1,
    OP_GROUP2_EvCL                  = 0xD3,
    OP_FPU6                         = 0xDD,
    OP_FPU6_F32                     = 0xD9,
    OP_CALL_rel32                   = 0xE8,
    OP_JMP_rel32                    = 0xE9,
    OP_JMP_rel8                     = 0xEB,
    PRE_LOCK                        = 0xF0,
    PRE_SSE_F2                      = 0xF2,
    PRE_SSE_F3                      = 0xF3,
    OP_HLT                          = 0xF4,
    OP_GROUP3_EbIb                  = 0xF6,
    OP_GROUP3_Ev                    = 0xF7,
    OP_GROUP3_EvIz                  = 0xF7, // OP_GROUP3_Ev has an immediate, when instruction is a test.
    OP_GROUP5_Ev                    = 0xFF
};

enum class ShiftID {
    vpsrld = 2,
    vpsrlq = 2,
    vpsrldq = 3,
    vpsrad = 4,
    vpslld = 6,
    vpsllq = 6
};

enum TwoByteOpcodeID {
    OP2_UD2             = 0x0B,
    OP2_MOVSD_VsdWsd    = 0x10,
    OP2_MOVPS_VpsWps    = 0x10,
    OP2_MOVSD_WsdVsd    = 0x11,
    OP2_MOVPS_WpsVps    = 0x11,
    OP2_MOVDDUP_VqWq    = 0x12,
    OP2_MOVHLPS_VqUq    = 0x12,
    OP2_MOVSLDUP_VpsWps = 0x12,
    OP2_UNPCKLPS_VsdWsd = 0x14,
    OP2_UNPCKHPS_VsdWsd = 0x15,
    OP2_MOVLHPS_VqUq    = 0x16,
    OP2_MOVSHDUP_VpsWps = 0x16,
    OP2_MOVAPD_VsdWsd   = 0x28,
    OP2_MOVAPS_VsdWsd   = 0x28,
    OP2_MOVAPS_WsdVsd   = 0x29,
    OP2_CVTSI2SD_VsdEd  = 0x2A,
    OP2_CVTTSD2SI_GdWsd = 0x2C,
    OP2_UCOMISD_VsdWsd  = 0x2E,
    OP2_MOVMSKPD_EdVd   = 0x50,
    OP2_ANDPS_VpsWps    = 0x54,
    OP2_ANDNPS_VpsWps   = 0x55,
    OP2_ORPS_VpsWps     = 0x56,
    OP2_XORPS_VpsWps    = 0x57,
    OP2_ADDSD_VsdWsd    = 0x58,
    OP2_ADDPS_VpsWps    = 0x58,
    OP2_MULSD_VsdWsd    = 0x59,
    OP2_MULPS_VpsWps    = 0x59,
    OP2_CVTSS2SD_VsdEd  = 0x5A,
    OP2_CVTSD2SS_VsdEd  = 0x5A,
    OP2_CVTTPS2DQ_VdqWps = 0x5B,
    OP2_CVTDQ2PS_VpsWdq = 0x5B,
    OP2_SUBSD_VsdWsd    = 0x5C,
    OP2_SUBPS_VpsWps    = 0x5C,
    OP2_MINSD_VsdWsd    = 0x5D,
    OP2_MINSS_VssWss    = 0x5D,
    OP2_MINPS_VpsWps    = 0x5D,
    OP2_DIVSD_VsdWsd    = 0x5E,
    OP2_DIVPS_VpsWps    = 0x5E,
    OP2_MAXSD_VsdWsd    = 0x5F,
    OP2_MAXSS_VssWss    = 0x5F,
    OP2_MAXPS_VpsWps    = 0x5F,
    OP2_SQRTSD_VsdWsd   = 0x51,
    OP2_SQRTSS_VssWss   = 0x51,
    OP2_SQRTPS_VpsWps   = 0x51,
    OP2_RSQRTPS_VpsWps  = 0x52,
    OP2_RCPPS_VpsWps    = 0x53,
    OP2_ANDPD_VpdWpd    = 0x54,
    OP2_ORPD_VpdWpd     = 0x56,
    OP2_XORPD_VpdWpd    = 0x57,
    OP2_PUNPCKLDQ       = 0x62,
    OP2_PCMPGTD_VdqWdq  = 0x66,
    OP2_MOVD_VdEd       = 0x6E,
    OP2_MOVDQ_VsdWsd    = 0x6F,
    OP2_MOVDQ_VdqWdq    = 0x6F,
    OP2_PSHUFD_VdqWdqIb = 0x70,
    OP2_PSLLD_UdqIb     = 0x72,
    OP2_PSRAD_UdqIb     = 0x72,
    OP2_PSRLD_UdqIb     = 0x72,
    OP2_PSRLDQ_Vd       = 0x73,
    OP2_PCMPEQW         = 0x75,
    OP2_PCMPEQD_VdqWdq  = 0x76,
    OP2_HADDPD          = 0x7C,
    OP2_MOVD_EdVd       = 0x7E,
    OP2_MOVQ_VdWd       = 0x7E,
    OP2_MOVDQ_WdqVdq    = 0x7F,
    OP2_JCC_rel32       = 0x80,
    OP_SETCC            = 0x90,
    OP2_SHLD            = 0xA4,
    OP2_SHRD            = 0xAC,
    OP_FENCE            = 0xAE,
    OP2_IMUL_GvEv       = 0xAF,
    OP2_CMPXCHG_GvEb    = 0xB0,
    OP2_CMPXCHG_GvEw    = 0xB1,
    OP2_BSR_GvEv        = 0xBD,
    OP2_MOVSX_GvEb      = 0xBE,
    OP2_MOVSX_GvEw      = 0xBF,
    OP2_MOVZX_GvEb      = 0xB6,
    OP2_MOVZX_GvEw      = 0xB7,
    OP2_XADD_EbGb       = 0xC0,
    OP2_XADD_EvGv       = 0xC1,
    OP2_CMPPS_VpsWps    = 0xC2,
    OP2_PEXTRW_GdUdIb   = 0xC5,
    OP2_SHUFPS_VpsWpsIb = 0xC6,
    OP2_PSRLD_VdqWdq    = 0xD2,
    OP2_MOVQ_WdVd       = 0xD6,
    OP2_PANDDQ_VdqWdq   = 0xDB,
    OP2_PANDNDQ_VdqWdq  = 0xDF,
    OP2_PSRAD_VdqWdq    = 0xE2,
    OP2_PORDQ_VdqWdq    = 0xEB,
    OP2_PXORDQ_VdqWdq   = 0xEF,
    OP2_PSLLD_VdqWdq    = 0xF2,
    OP2_PMULUDQ_VdqWdq  = 0xF4,
    OP2_PSUBD_VdqWdq    = 0xFA,
    OP2_PADDD_VdqWdq    = 0xFE
};

enum ThreeByteOpcodeID {
    OP3_ROUNDSS_VsdWsd  = 0x0A,
    OP3_ROUNDSD_VsdWsd  = 0x0B,
    OP3_BLENDVPS_VdqWdq = 0x14,
    OP3_PEXTRD_EdVdqIb  = 0x16,
    OP3_BLENDPS_VpsWpsIb = 0x0C,
    OP3_PTEST_VdVd      = 0x17,
    OP3_INSERTPS_VpsUps = 0x21,
    OP3_PINSRD_VdqEdIb  = 0x22,
    OP3_PMULLD_VdqWdq   = 0x40,
    OP3_VBLENDVPS_VdqWdq = 0x4A
};

// Test whether the given opcode should be printed with its operands reversed.
inline bool IsXMMReversedOperands(TwoByteOpcodeID opcode)
{
    switch (opcode) {
      case OP2_MOVSD_WsdVsd: // also OP2_MOVPS_WpsVps
      case OP2_MOVAPS_WsdVsd:
      case OP2_MOVDQ_WdqVdq:
      case OP3_PEXTRD_EdVdqIb:
        return true;
      default:
        break;
    }
    return false;
}

enum ThreeByteEscape {
    ESCAPE_38     = 0x38,
    ESCAPE_3A     = 0x3A
};

enum VexOperandType {
    VEX_PS = 0,
    VEX_PD = 1,
    VEX_SS = 2,
    VEX_SD = 3
};

inline OneByteOpcodeID jccRel8(Condition cond)
{
    return OneByteOpcodeID(OP_JCC_rel8 + cond);
}
inline TwoByteOpcodeID jccRel32(Condition cond)
{
    return TwoByteOpcodeID(OP2_JCC_rel32 + cond);
}
inline TwoByteOpcodeID setccOpcode(Condition cond)
{
    return TwoByteOpcodeID(OP_SETCC + cond);
}

enum GroupOpcodeID {
    GROUP1_OP_ADD = 0,
    GROUP1_OP_OR  = 1,
    GROUP1_OP_ADC = 2,
    GROUP1_OP_AND = 4,
    GROUP1_OP_SUB = 5,
    GROUP1_OP_XOR = 6,
    GROUP1_OP_CMP = 7,

    GROUP1A_OP_POP = 0,

    GROUP2_OP_SHL = 4,
    GROUP2_OP_SHR = 5,
    GROUP2_OP_SAR = 7,

    GROUP3_OP_TEST = 0,
    GROUP3_OP_NOT  = 2,
    GROUP3_OP_NEG  = 3,
    GROUP3_OP_MUL  = 4,
    GROUP3_OP_IMUL = 5,
    GROUP3_OP_DIV  = 6,
    GROUP3_OP_IDIV = 7,

    GROUP5_OP_INC   = 0,
    GROUP5_OP_DEC   = 1,
    GROUP5_OP_CALLN = 2,
    GROUP5_OP_JMPN  = 4,
    GROUP5_OP_PUSH  = 6,

    FPU6_OP_FLD     = 0,
    FPU6_OP_FISTTP  = 1,
    FPU6_OP_FSTP    = 3,

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

} // namespace X86Encoding

} // namespace jit
} // namespace js

#endif /* jit_x86_shared_Encoding_x86_shared_h */
