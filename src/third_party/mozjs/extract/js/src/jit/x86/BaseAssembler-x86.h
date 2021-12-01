/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_x86_BaseAssembler_x86_h
#define jit_x86_BaseAssembler_x86_h

#include "jit/x86-shared/BaseAssembler-x86-shared.h"

namespace js {
namespace jit {

namespace X86Encoding {

class BaseAssemblerX86 : public BaseAssembler
{
  public:

    // Arithmetic operations:

    void adcl_ir(int32_t imm, RegisterID dst)
    {
        spew("adcl       $%d, %s", imm, GPReg32Name(dst));
        if (CAN_SIGN_EXTEND_8_32(imm)) {
            m_formatter.oneByteOp(OP_GROUP1_EvIb, dst, GROUP1_OP_ADC);
            m_formatter.immediate8s(imm);
        } else {
            m_formatter.oneByteOp(OP_GROUP1_EvIz, dst, GROUP1_OP_ADC);
            m_formatter.immediate32(imm);
        }
    }

    void adcl_im(int32_t imm, const void* addr)
    {
        spew("adcl       %d, %p", imm, addr);
        if (CAN_SIGN_EXTEND_8_32(imm)) {
            m_formatter.oneByteOp(OP_GROUP1_EvIb, addr, GROUP1_OP_ADC);
            m_formatter.immediate8s(imm);
        } else {
            m_formatter.oneByteOp(OP_GROUP1_EvIz, addr, GROUP1_OP_ADC);
            m_formatter.immediate32(imm);
        }
    }

    void adcl_rr(RegisterID src, RegisterID dst)
    {
        spew("adcl       %s, %s", GPReg32Name(src), GPReg32Name(dst));
        m_formatter.oneByteOp(OP_ADC_GvEv, src, dst);
    }

    void adcl_mr(int32_t offset, RegisterID base, RegisterID dst)
    {
        spew("adcl       " MEM_ob ", %s", ADDR_ob(offset, base), GPReg32Name(dst));
        m_formatter.oneByteOp(OP_ADC_GvEv, offset, base, dst);
    }

    void adcl_mr(int32_t offset, RegisterID base, RegisterID index, int scale, RegisterID dst)
    {
        spew("adcl       " MEM_obs ", %s", ADDR_obs(offset, base, index, scale), GPReg32Name(dst));
        m_formatter.oneByteOp(OP_ADC_GvEv, offset, base, index, scale, dst);
    }

    void sbbl_ir(int32_t imm, RegisterID dst)
    {
        spew("sbbl       $%d, %s", imm, GPReg32Name(dst));
        if (CAN_SIGN_EXTEND_8_32(imm)) {
            m_formatter.oneByteOp(OP_GROUP1_EvIb, dst, GROUP1_OP_SBB);
            m_formatter.immediate8s(imm);
        } else {
            m_formatter.oneByteOp(OP_GROUP1_EvIz, dst, GROUP1_OP_SBB);
            m_formatter.immediate32(imm);
        }
    }

    void sbbl_rr(RegisterID src, RegisterID dst)
    {
        spew("sbbl       %s, %s", GPReg32Name(src), GPReg32Name(dst));
        m_formatter.oneByteOp(OP_SBB_GvEv, src, dst);
    }

    void sbbl_mr(int32_t offset, RegisterID base, RegisterID dst)
    {
        spew("sbbl       " MEM_ob ", %s", ADDR_ob(offset, base), GPReg32Name(dst));
        m_formatter.oneByteOp(OP_SBB_GvEv, offset, base, dst);
    }

    void sbbl_mr(int32_t offset, RegisterID base, RegisterID index, int scale, RegisterID dst)
    {
        spew("sbbl       " MEM_obs ", %s", ADDR_obs(offset, base, index, scale), GPReg32Name(dst));
        m_formatter.oneByteOp(OP_SBB_GvEv, offset, base, index, scale, dst);
    }

    using BaseAssembler::andl_im;
    void andl_im(int32_t imm, const void* addr)
    {
        spew("andl       $0x%x, %p", imm, addr);
        if (CAN_SIGN_EXTEND_8_32(imm)) {
            m_formatter.oneByteOp(OP_GROUP1_EvIb, addr, GROUP1_OP_AND);
            m_formatter.immediate8s(imm);
        } else {
            m_formatter.oneByteOp(OP_GROUP1_EvIz, addr, GROUP1_OP_AND);
            m_formatter.immediate32(imm);
        }
    }

    using BaseAssembler::orl_im;
    void orl_im(int32_t imm, const void* addr)
    {
        spew("orl        $0x%x, %p", imm, addr);
        if (CAN_SIGN_EXTEND_8_32(imm)) {
            m_formatter.oneByteOp(OP_GROUP1_EvIb, addr, GROUP1_OP_OR);
            m_formatter.immediate8s(imm);
        } else {
            m_formatter.oneByteOp(OP_GROUP1_EvIz, addr, GROUP1_OP_OR);
            m_formatter.immediate32(imm);
        }
    }

    using BaseAssembler::subl_im;
    void subl_im(int32_t imm, const void* addr)
    {
        spew("subl       $%d, %p", imm, addr);
        if (CAN_SIGN_EXTEND_8_32(imm)) {
            m_formatter.oneByteOp(OP_GROUP1_EvIb, addr, GROUP1_OP_SUB);
            m_formatter.immediate8s(imm);
        } else {
            m_formatter.oneByteOp(OP_GROUP1_EvIz, addr, GROUP1_OP_SUB);
            m_formatter.immediate32(imm);
        }
    }

    void shldl_irr(int32_t imm, RegisterID src, RegisterID dst)
    {
        MOZ_ASSERT(imm < 32);
        spew("shldl      $%d, %s, %s", imm, GPReg32Name(src), GPReg32Name(dst));
        m_formatter.twoByteOp8(OP2_SHLD, dst, src);
        m_formatter.immediate8u(imm);
    }

    void shrdl_irr(int32_t imm, RegisterID src, RegisterID dst)
    {
        MOZ_ASSERT(imm < 32);
        spew("shrdl      $%d, %s, %s", imm, GPReg32Name(src), GPReg32Name(dst));
        m_formatter.twoByteOp8(OP2_SHRD, dst, src);
        m_formatter.immediate8u(imm);
    }

    // SSE operations:

    using BaseAssembler::vcvtsi2sd_mr;
    void vcvtsi2sd_mr(const void* address, XMMRegisterID src0, XMMRegisterID dst)
    {
        twoByteOpSimd("vcvtsi2sd", VEX_SD, OP2_CVTSI2SD_VsdEd, address, src0, dst);
    }

    using BaseAssembler::vmovaps_mr;
    void vmovaps_mr(const void* address, XMMRegisterID dst)
    {
        twoByteOpSimd("vmovaps", VEX_PS, OP2_MOVAPS_VsdWsd, address, invalid_xmm, dst);
    }

    using BaseAssembler::vmovdqa_mr;
    void vmovdqa_mr(const void* address, XMMRegisterID dst)
    {
        twoByteOpSimd("vmovdqa", VEX_PD, OP2_MOVDQ_VdqWdq, address, invalid_xmm, dst);
    }

    void vhaddpd_rr(XMMRegisterID src, XMMRegisterID dst)
    {
        twoByteOpSimdFlags("vhaddpd", VEX_PD, OP2_HADDPD, src, dst);
    }

    void vsubpd_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst)
    {
        twoByteOpSimd("vsubpd", VEX_PD, OP2_SUBPS_VpsWps, src1, src0, dst);
    }

    void vpunpckldq_rr(XMMRegisterID src1, XMMRegisterID src0, XMMRegisterID dst) {
        twoByteOpSimd("vpunpckldq", VEX_PD, OP2_PUNPCKLDQ, src1, src0, dst);
    }
    void vpunpckldq_mr(int32_t offset, RegisterID base, XMMRegisterID src0, XMMRegisterID dst)
    {
        twoByteOpSimd("vpunpckldq", VEX_PD, OP2_PUNPCKLDQ, offset, base, src0, dst);
    }
    void vpunpckldq_mr(const void* addr, XMMRegisterID src0, XMMRegisterID dst)
    {
        twoByteOpSimd("vpunpckldq", VEX_PD, OP2_PUNPCKLDQ, addr, src0, dst);
    }

    void fild_m(int32_t offset, RegisterID base)
    {
        m_formatter.oneByteOp(OP_FILD, offset, base, FILD_OP_64);
    }

    // Misc instructions:

    void pusha()
    {
        spew("pusha");
        m_formatter.oneByteOp(OP_PUSHA);
    }

    void popa()
    {
        spew("popa");
        m_formatter.oneByteOp(OP_POPA);
    }
};

typedef BaseAssemblerX86 BaseAssemblerSpecific;

} // namespace X86Encoding

} // namespace jit
} // namespace js

#endif /* jit_x86_BaseAssembler_x86_h */
