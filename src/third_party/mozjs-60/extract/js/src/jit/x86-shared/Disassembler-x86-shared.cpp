/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/Disassembler.h"

#include "jit/x86-shared/Encoding-x86-shared.h"

using namespace js;
using namespace js::jit;
using namespace js::jit::X86Encoding;
using namespace js::jit::Disassembler;

MOZ_COLD static bool REX_W(uint8_t rex) { return (rex >> 3) & 0x1; }
MOZ_COLD static bool REX_R(uint8_t rex) { return (rex >> 2) & 0x1; }
MOZ_COLD static bool REX_X(uint8_t rex) { return (rex >> 1) & 0x1; }
MOZ_COLD static bool REX_B(uint8_t rex) { return (rex >> 0) & 0x1; }

MOZ_COLD static uint8_t
MakeREXFlags(bool w, bool r, bool x, bool b)
{
    uint8_t rex = (w << 3) | (r << 2) | (x << 1) | (b << 0);
    MOZ_RELEASE_ASSERT(REX_W(rex) == w);
    MOZ_RELEASE_ASSERT(REX_R(rex) == r);
    MOZ_RELEASE_ASSERT(REX_X(rex) == x);
    MOZ_RELEASE_ASSERT(REX_B(rex) == b);
    return rex;
}

MOZ_COLD static ModRmMode
ModRM_Mode(uint8_t modrm)
{
    return ModRmMode((modrm >> 6) & 0x3);
}

MOZ_COLD static uint8_t
ModRM_Reg(uint8_t modrm)
{
    return (modrm >> 3) & 0x7;
}

MOZ_COLD static uint8_t
ModRM_RM(uint8_t modrm)
{
    return (modrm >> 0) & 0x7;
}

MOZ_COLD static bool
ModRM_hasSIB(uint8_t modrm)
{
    return ModRM_Mode(modrm) != ModRmRegister && ModRM_RM(modrm) == hasSib;
}
MOZ_COLD static bool
ModRM_hasDisp8(uint8_t modrm)
{
    return ModRM_Mode(modrm) == ModRmMemoryDisp8;
}
MOZ_COLD static bool
ModRM_hasRIP(uint8_t modrm)
{
#ifdef JS_CODEGEN_X64
    return ModRM_Mode(modrm) == ModRmMemoryNoDisp && ModRM_RM(modrm) == noBase;
#else
    return false;
#endif
}
MOZ_COLD static bool
ModRM_hasDisp32(uint8_t modrm)
{
    return ModRM_Mode(modrm) == ModRmMemoryDisp32 ||
           ModRM_hasRIP(modrm);
}

MOZ_COLD static uint8_t
SIB_SS(uint8_t sib)
{
    return (sib >> 6) & 0x3;
}

MOZ_COLD static uint8_t
SIB_Index(uint8_t sib)
{
    return (sib >> 3) & 0x7;
}

MOZ_COLD static uint8_t
SIB_Base(uint8_t sib)
{
    return (sib >> 0) & 0x7;
}

MOZ_COLD static bool
SIB_hasRIP(uint8_t sib)
{
    return SIB_Base(sib) == noBase && SIB_Index(sib) == noIndex;
}

MOZ_COLD static bool
HasRIP(uint8_t modrm, uint8_t sib, uint8_t rex)
{
    return ModRM_hasRIP(modrm) && SIB_hasRIP(sib);
}

MOZ_COLD static bool
HasDisp8(uint8_t modrm)
{
    return ModRM_hasDisp8(modrm);
}

MOZ_COLD static bool
HasDisp32(uint8_t modrm, uint8_t sib)
{
    return ModRM_hasDisp32(modrm) ||
           (SIB_Base(sib) == noBase &&
            SIB_Index(sib) == noIndex &&
            ModRM_Mode(modrm) == ModRmMemoryNoDisp);
}

MOZ_COLD static uint32_t
Reg(uint8_t modrm, uint8_t sib, uint8_t rex)
{
    return ModRM_Reg(modrm) | (REX_R(rex) << 3);
}

MOZ_COLD static bool
HasBase(uint8_t modrm, uint8_t sib)
{
    return !ModRM_hasSIB(modrm) ||
           SIB_Base(sib) != noBase ||
           SIB_Index(sib) != noIndex ||
           ModRM_Mode(modrm) != ModRmMemoryNoDisp;
}

MOZ_COLD static RegisterID
DecodeBase(uint8_t modrm, uint8_t sib, uint8_t rex)
{
    return HasBase(modrm, sib)
           ? RegisterID((ModRM_hasSIB(modrm) ? SIB_Base(sib) : ModRM_RM(modrm)) | (REX_B(rex) << 3))
           : invalid_reg;
}

MOZ_COLD static RegisterID
DecodeIndex(uint8_t modrm, uint8_t sib, uint8_t rex)
{
    RegisterID index = RegisterID(SIB_Index(sib) | (REX_X(rex) << 3));
    return ModRM_hasSIB(modrm) && index != noIndex ? index : invalid_reg;
}

MOZ_COLD static uint32_t
DecodeScale(uint8_t modrm, uint8_t sib, uint8_t rex)
{
    return ModRM_hasSIB(modrm) ? SIB_SS(sib) : 0;
}

#define PackOpcode(op0, op1, op2) ((op0) | ((op1) << 8) | ((op2) << 16))
#define Pack2ByteOpcode(op1) PackOpcode(OP_2BYTE_ESCAPE, op1, 0)
#define Pack3ByteOpcode(op1, op2) PackOpcode(OP_2BYTE_ESCAPE, op1, op2)

uint8_t*
js::jit::Disassembler::DisassembleHeapAccess(uint8_t* ptr, HeapAccess* access)
{
    VexOperandType type = VEX_PS;
    uint32_t opcode = OP_HLT;
    uint8_t modrm = 0;
    uint8_t sib = 0;
    uint8_t rex = 0;
    int32_t disp = 0;
    int32_t imm = 0;
    bool haveImm = false;
    int opsize = 4;

    // Legacy prefixes
    switch (*ptr) {
      case PRE_LOCK:
      case PRE_PREDICT_BRANCH_NOT_TAKEN: // (obsolete), aka %cs
      case 0x3E: // aka predict-branch-taken (obsolete)
      case 0x36: // %ss
      case 0x26: // %es
      case 0x64: // %fs
      case 0x65: // %gs
      case 0x67: // address-size override
        MOZ_CRASH("Unable to disassemble instruction");
      case PRE_SSE_F2: // aka REPNZ/REPNE
        type = VEX_SD;
        ptr++;
        break;
      case PRE_SSE_F3: // aka REP/REPE/REPZ
        type = VEX_SS;
        ptr++;
        break;
      case PRE_SSE_66: // aka PRE_OPERAND_SIZE
        type = VEX_PD;
        opsize = 2;
        ptr++;
        break;
      default:
        break;
    }

    // REX and VEX prefixes
    {
        int x = 0, b = 0, m = 1, w = 0;
        int r, l, p;
        switch (*ptr) {
#ifdef JS_CODEGEN_X64
          case PRE_REX | 0x0: case PRE_REX | 0x1: case PRE_REX | 0x2: case PRE_REX | 0x3:
          case PRE_REX | 0x4: case PRE_REX | 0x5: case PRE_REX | 0x6: case PRE_REX | 0x7:
          case PRE_REX | 0x8: case PRE_REX | 0x9: case PRE_REX | 0xa: case PRE_REX | 0xb:
          case PRE_REX | 0xc: case PRE_REX | 0xd: case PRE_REX | 0xe: case PRE_REX | 0xf:
            rex = *ptr++ & 0xf;
            goto rex_done;
#endif
          case PRE_VEX_C4: {
            if (type != VEX_PS)
                MOZ_CRASH("Unable to disassemble instruction");
            ++ptr;
            uint8_t c4a = *ptr++ ^ 0xe0;
            uint8_t c4b = *ptr++ ^ 0x78;
            r = (c4a >> 7) & 0x1;
            x = (c4a >> 6) & 0x1;
            b = (c4a >> 5) & 0x1;
            m = (c4a >> 0) & 0x1f;
            w = (c4b >> 7) & 0x1;
            l = (c4b >> 2) & 0x1;
            p = (c4b >> 0) & 0x3;
            break;
          }
          case PRE_VEX_C5: {
            if (type != VEX_PS)
              MOZ_CRASH("Unable to disassemble instruction");
            ++ptr;
            uint8_t c5 = *ptr++ ^ 0xf8;
            r = (c5 >> 7) & 0x1;
            l = (c5 >> 2) & 0x1;
            p = (c5 >> 0) & 0x3;
            break;
          }
          default:
            goto rex_done;
        }
        if (l != 0) // 256-bit SIMD
            MOZ_CRASH("Unable to disassemble instruction");
        type = VexOperandType(p);
        rex = MakeREXFlags(w, r, x, b);
        switch (m) {
          case 0x1:
            opcode = Pack2ByteOpcode(*ptr++);
            goto opcode_done;
          case 0x2:
            opcode = Pack3ByteOpcode(ESCAPE_38, *ptr++);
            goto opcode_done;
          case 0x3:
            opcode = Pack3ByteOpcode(ESCAPE_3A, *ptr++);
            goto opcode_done;
          default:
            MOZ_CRASH("Unable to disassemble instruction");
        }
    }
  rex_done:;
    if (REX_W(rex))
        opsize = 8;

    // Opcode.
    opcode = *ptr++;
    switch (opcode) {
#ifdef JS_CODEGEN_X64
      case OP_PUSH_EAX + 0: case OP_PUSH_EAX + 1: case OP_PUSH_EAX + 2: case OP_PUSH_EAX + 3:
      case OP_PUSH_EAX + 4: case OP_PUSH_EAX + 5: case OP_PUSH_EAX + 6: case OP_PUSH_EAX + 7:
      case OP_POP_EAX + 0: case OP_POP_EAX + 1: case OP_POP_EAX + 2: case OP_POP_EAX + 3:
      case OP_POP_EAX + 4: case OP_POP_EAX + 5: case OP_POP_EAX + 6: case OP_POP_EAX + 7:
      case OP_PUSH_Iz:
      case OP_PUSH_Ib:
        opsize = 8;
        break;
#endif
      case OP_2BYTE_ESCAPE:
        opcode |= *ptr << 8;
        switch (*ptr++) {
          case ESCAPE_38:
          case ESCAPE_3A:
            opcode |= *ptr++ << 16;
            break;
          default:
            break;
        }
        break;
      default:
        break;
    }
  opcode_done:;

    // ModR/M
    modrm = *ptr++;

    // SIB
    if (ModRM_hasSIB(modrm))
        sib = *ptr++;

    // Address Displacement
    if (HasDisp8(modrm)) {
        disp = int8_t(*ptr++);
    } else if (HasDisp32(modrm, sib)) {
        memcpy(&disp, ptr, sizeof(int32_t));
        ptr += sizeof(int32_t);
    }

    // Immediate operand
    switch (opcode) {
      case OP_PUSH_Ib:
      case OP_IMUL_GvEvIb:
      case OP_GROUP1_EbIb:
      case OP_GROUP1_EvIb:
      case OP_TEST_EAXIb:
      case OP_GROUP2_EvIb:
      case OP_GROUP11_EvIb:
      case OP_GROUP3_EbIb:
      case Pack2ByteOpcode(OP2_PSHUFD_VdqWdqIb):
      case Pack2ByteOpcode(OP2_PSLLD_UdqIb): // aka OP2_PSRAD_UdqIb, aka OP2_PSRLD_UdqIb
      case Pack2ByteOpcode(OP2_PEXTRW_GdUdIb):
      case Pack2ByteOpcode(OP2_SHUFPS_VpsWpsIb):
      case Pack3ByteOpcode(ESCAPE_3A, OP3_PEXTRD_EdVdqIb):
      case Pack3ByteOpcode(ESCAPE_3A, OP3_BLENDPS_VpsWpsIb):
      case Pack3ByteOpcode(ESCAPE_3A, OP3_PINSRD_VdqEdIb):
        // 8-bit signed immediate
        imm = int8_t(*ptr++);
        haveImm = true;
        break;
      case OP_RET_Iz:
        // 16-bit unsigned immediate
        memcpy(&imm, ptr, sizeof(int16_t));
        ptr += sizeof(int16_t);
        haveImm = true;
        break;
      case OP_ADD_EAXIv:
      case OP_OR_EAXIv:
      case OP_AND_EAXIv:
      case OP_SUB_EAXIv:
      case OP_XOR_EAXIv:
      case OP_CMP_EAXIv:
      case OP_PUSH_Iz:
      case OP_IMUL_GvEvIz:
      case OP_GROUP1_EvIz:
      case OP_TEST_EAXIv:
      case OP_MOV_EAXIv:
      case OP_GROUP3_EvIz:
        // 32-bit signed immediate
        memcpy(&imm, ptr, sizeof(int32_t));
        ptr += sizeof(int32_t);
        haveImm = true;
        break;
      case OP_GROUP11_EvIz:
        // opsize-sized signed immediate
        memcpy(&imm, ptr, opsize);
        imm = int32_t(uint32_t(imm) << (32 - opsize * 8)) >> (32 - opsize * 8);
        ptr += opsize;
        haveImm = true;
        break;
      default:
        break;
    }

    // Interpret the opcode.
    if (HasRIP(modrm, sib, rex))
        MOZ_CRASH("Unable to disassemble instruction");

    size_t memSize = 0;
    OtherOperand otherOperand(imm);
    HeapAccess::Kind kind = HeapAccess::Unknown;
    RegisterID gpr(RegisterID(Reg(modrm, sib, rex)));
    XMMRegisterID xmm(XMMRegisterID(Reg(modrm, sib, rex)));
    ComplexAddress addr(disp,
                        DecodeBase(modrm, sib, rex),
                        DecodeIndex(modrm, sib, rex),
                        DecodeScale(modrm, sib, rex));
    switch (opcode) {
      case OP_GROUP11_EvIb:
        if (gpr != RegisterID(GROUP11_MOV))
            MOZ_CRASH("Unable to disassemble instruction");
        MOZ_RELEASE_ASSERT(haveImm);
        memSize = 1;
        kind = HeapAccess::Store;
        break;
      case OP_GROUP11_EvIz:
        if (gpr != RegisterID(GROUP11_MOV))
            MOZ_CRASH("Unable to disassemble instruction");
        MOZ_RELEASE_ASSERT(haveImm);
        memSize = opsize;
        kind = HeapAccess::Store;
        break;
      case OP_MOV_GvEv:
        MOZ_RELEASE_ASSERT(!haveImm);
        otherOperand = OtherOperand(gpr);
        memSize = opsize;
        kind = HeapAccess::Load;
        break;
      case OP_MOV_GvEb:
        MOZ_RELEASE_ASSERT(!haveImm);
        otherOperand = OtherOperand(gpr);
        memSize = 1;
        kind = HeapAccess::Load;
        break;
      case OP_MOV_EvGv:
        if (!haveImm)
            otherOperand = OtherOperand(gpr);
        memSize = opsize;
        kind = HeapAccess::Store;
        break;
      case OP_MOV_EbGv:
        if (!haveImm)
            otherOperand = OtherOperand(gpr);
        memSize = 1;
        kind = HeapAccess::Store;
        break;
      case Pack2ByteOpcode(OP2_MOVZX_GvEb):
        MOZ_RELEASE_ASSERT(!haveImm);
        otherOperand = OtherOperand(gpr);
        memSize = 1;
        kind = HeapAccess::Load;
        break;
      case Pack2ByteOpcode(OP2_MOVZX_GvEw):
        MOZ_RELEASE_ASSERT(!haveImm);
        otherOperand = OtherOperand(gpr);
        memSize = 2;
        kind = HeapAccess::Load;
        break;
      case Pack2ByteOpcode(OP2_MOVSX_GvEb):
        MOZ_RELEASE_ASSERT(!haveImm);
        otherOperand = OtherOperand(gpr);
        memSize = 1;
        kind = opsize == 8 ? HeapAccess::LoadSext64 : HeapAccess::LoadSext32;
        break;
      case Pack2ByteOpcode(OP2_MOVSX_GvEw):
        MOZ_RELEASE_ASSERT(!haveImm);
        otherOperand = OtherOperand(gpr);
        memSize = 2;
        kind = opsize == 8 ? HeapAccess::LoadSext64 : HeapAccess::LoadSext32;
        break;
#ifdef JS_CODEGEN_X64
      case OP_MOVSXD_GvEv:
        MOZ_RELEASE_ASSERT(!haveImm);
        otherOperand = OtherOperand(gpr);
        memSize = 4;
        kind = HeapAccess::LoadSext64;
        break;
#endif // JS_CODEGEN_X64
      case Pack2ByteOpcode(OP2_MOVDQ_VdqWdq): // aka OP2_MOVDQ_VsdWsd
      case Pack2ByteOpcode(OP2_MOVAPS_VsdWsd):
        MOZ_RELEASE_ASSERT(!haveImm);
        otherOperand = OtherOperand(xmm);
        memSize = 16;
        kind = HeapAccess::Load;
        break;
      case Pack2ByteOpcode(OP2_MOVSD_VsdWsd): // aka OP2_MOVPS_VpsWps
        MOZ_RELEASE_ASSERT(!haveImm);
        otherOperand = OtherOperand(xmm);
        switch (type) {
          case VEX_SS: memSize = 4; break;
          case VEX_SD: memSize = 8; break;
          case VEX_PS:
          case VEX_PD: memSize = 16; break;
          default: MOZ_CRASH("Unexpected VEX type");
        }
        kind = HeapAccess::Load;
        break;
      case Pack2ByteOpcode(OP2_MOVDQ_WdqVdq):
        MOZ_RELEASE_ASSERT(!haveImm);
        otherOperand = OtherOperand(xmm);
        memSize = 16;
        kind = HeapAccess::Store;
        break;
      case Pack2ByteOpcode(OP2_MOVSD_WsdVsd): // aka OP2_MOVPS_WpsVps
        MOZ_RELEASE_ASSERT(!haveImm);
        otherOperand = OtherOperand(xmm);
        switch (type) {
          case VEX_SS: memSize = 4; break;
          case VEX_SD: memSize = 8; break;
          case VEX_PS:
          case VEX_PD: memSize = 16; break;
          default: MOZ_CRASH("Unexpected VEX type");
        }
        kind = HeapAccess::Store;
        break;
      case Pack2ByteOpcode(OP2_MOVD_VdEd):
        MOZ_RELEASE_ASSERT(!haveImm);
        otherOperand = OtherOperand(xmm);
        switch (type) {
          case VEX_PD: memSize = 4; break;
          default: MOZ_CRASH("Unexpected VEX type");
        }
        kind = HeapAccess::Load;
        break;
      case Pack2ByteOpcode(OP2_MOVQ_WdVd):
        MOZ_RELEASE_ASSERT(!haveImm);
        otherOperand = OtherOperand(xmm);
        switch (type) {
          case VEX_PD: memSize = 8; break;
          default: MOZ_CRASH("Unexpected VEX type");
        }
        kind = HeapAccess::Store;
        break;
      case Pack2ByteOpcode(OP2_MOVD_EdVd): // aka OP2_MOVQ_VdWd
        MOZ_RELEASE_ASSERT(!haveImm);
        otherOperand = OtherOperand(xmm);
        switch (type) {
          case VEX_SS: memSize = 8; kind = HeapAccess::Load; break;
          case VEX_PD: memSize = 4; kind = HeapAccess::Store; break;
          default: MOZ_CRASH("Unexpected VEX type");
        }
        break;
      default:
        MOZ_CRASH("Unable to disassemble instruction");
    }

    *access = HeapAccess(kind, memSize, addr, otherOperand);
    return ptr;
}

#ifdef DEBUG
void
js::jit::Disassembler::DumpHeapAccess(const HeapAccess& access)
{
    switch (access.kind()) {
      case HeapAccess::Store:      fprintf(stderr, "store"); break;
      case HeapAccess::Load:       fprintf(stderr, "load"); break;
      case HeapAccess::LoadSext32: fprintf(stderr, "loadSext32"); break;
      case HeapAccess::LoadSext64: fprintf(stderr, "loadSext64"); break;
      default:                     fprintf(stderr, "unknown"); break;
    }
    fprintf(stderr, "%u ", unsigned(access.size()));

    switch (access.otherOperand().kind()) {
      case OtherOperand::Imm:
        fprintf(stderr, "imm %d", access.otherOperand().imm());
        break;
      case OtherOperand::GPR:
        fprintf(stderr, "gpr %s", X86Encoding::GPRegName(access.otherOperand().gpr()));
        break;
      case OtherOperand::FPR:
        fprintf(stderr, "fpr %s", X86Encoding::XMMRegName(access.otherOperand().fpr()));
        break;
      default: fprintf(stderr, "unknown");
    }

    fprintf(stderr, " @ ");

    if (access.address().isPCRelative()) {
        fprintf(stderr, MEM_o32r " ", ADDR_o32r(access.address().disp()));
    } else if (access.address().hasIndex()) {
        if (access.address().hasBase()) {
            fprintf(stderr, MEM_obs " ",
                    ADDR_obs(access.address().disp(), access.address().base(),
                             access.address().index(), access.address().scale()));
        } else {
            fprintf(stderr, MEM_os " ",
                    ADDR_os(access.address().disp(),
                            access.address().index(), access.address().scale()));
        }
    } else if (access.address().hasBase()) {
        fprintf(stderr, MEM_ob " ", ADDR_ob(access.address().disp(), access.address().base()));
    } else {
        fprintf(stderr, MEM_o " ", ADDR_o(access.address().disp()));
    }

    fprintf(stderr, "\n");
}
#endif
