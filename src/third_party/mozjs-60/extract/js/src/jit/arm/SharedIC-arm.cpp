/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/BaselineCompiler.h"
#include "jit/BaselineIC.h"
#include "jit/BaselineJIT.h"
#include "jit/Linker.h"
#include "jit/SharedICHelpers.h"

#include "jit/MacroAssembler-inl.h"

using namespace js;
using namespace js::jit;

namespace js {
namespace jit {

// ICBinaryArith_Int32

extern "C" {
    extern MOZ_EXPORT int64_t __aeabi_idivmod(int,int);
}

bool
ICBinaryArith_Int32::Compiler::generateStubCode(MacroAssembler& masm)
{
    // Guard that R0 is an integer and R1 is an integer.
    Label failure;
    masm.branchTestInt32(Assembler::NotEqual, R0, &failure);
    masm.branchTestInt32(Assembler::NotEqual, R1, &failure);

    // Add R0 and R1. Don't need to explicitly unbox, just use R2's payloadReg.
    Register scratchReg = R2.payloadReg();

    // DIV and MOD need an extra non-volatile ValueOperand to hold R0.
    AllocatableGeneralRegisterSet savedRegs(availableGeneralRegs(2));
    savedRegs.set() = GeneralRegisterSet::Intersect(GeneralRegisterSet::NonVolatile(), savedRegs.set());
    ValueOperand savedValue = savedRegs.takeAnyValue();

    Label maybeNegZero, revertRegister;
    switch(op_) {
      case JSOP_ADD:
        masm.ma_add(R0.payloadReg(), R1.payloadReg(), scratchReg, SetCC);

        // Just jump to failure on overflow. R0 and R1 are preserved, so we can
        // just jump to the next stub.
        masm.j(Assembler::Overflow, &failure);

        // Box the result and return. We know R0.typeReg() already contains the
        // integer tag, so we just need to move the result value into place.
        masm.mov(scratchReg, R0.payloadReg());
        break;
      case JSOP_SUB:
        masm.ma_sub(R0.payloadReg(), R1.payloadReg(), scratchReg, SetCC);
        masm.j(Assembler::Overflow, &failure);
        masm.mov(scratchReg, R0.payloadReg());
        break;
      case JSOP_MUL: {
        ScratchRegisterScope scratch(masm);
        Assembler::Condition cond = masm.ma_check_mul(R0.payloadReg(), R1.payloadReg(), scratchReg,
                                                      scratch, Assembler::Overflow);
        masm.j(cond, &failure);

        masm.as_cmp(scratchReg, Imm8(0));
        masm.j(Assembler::Equal, &maybeNegZero);

        masm.mov(scratchReg, R0.payloadReg());
        break;
      }
      case JSOP_DIV:
      case JSOP_MOD: {
        // Check for INT_MIN / -1, it results in a double.
        {
            ScratchRegisterScope scratch(masm);
            masm.ma_cmp(R0.payloadReg(), Imm32(INT_MIN), scratch);
            masm.ma_cmp(R1.payloadReg(), Imm32(-1), scratch, Assembler::Equal);
            masm.j(Assembler::Equal, &failure);
        }

        // Check for both division by zero and 0 / X with X < 0 (results in -0).
        masm.as_cmp(R1.payloadReg(), Imm8(0));
        masm.as_cmp(R0.payloadReg(), Imm8(0), Assembler::LessThan);
        masm.j(Assembler::Equal, &failure);

        // The call will preserve registers r4-r11. Save R0 and the link
        // register.
        MOZ_ASSERT(R1 == ValueOperand(r5, r4));
        MOZ_ASSERT(R0 == ValueOperand(r3, r2));
        masm.moveValue(R0, savedValue);

        masm.setupAlignedABICall();
        masm.passABIArg(R0.payloadReg());
        masm.passABIArg(R1.payloadReg());
        masm.callWithABI(JS_FUNC_TO_DATA_PTR(void*, __aeabi_idivmod), MoveOp::GENERAL,
                         CheckUnsafeCallWithABI::DontCheckOther);

        // idivmod returns the quotient in r0, and the remainder in r1.
        if (op_ == JSOP_DIV) {
            // Result is a double if the remainder != 0.
            masm.branch32(Assembler::NotEqual, r1, Imm32(0), &revertRegister);
            masm.tagValue(JSVAL_TYPE_INT32, r0, R0);
        } else {
            // If X % Y == 0 and X < 0, the result is -0.
            Label done;
            masm.branch32(Assembler::NotEqual, r1, Imm32(0), &done);
            masm.branch32(Assembler::LessThan, savedValue.payloadReg(), Imm32(0), &revertRegister);
            masm.bind(&done);
            masm.tagValue(JSVAL_TYPE_INT32, r1, R0);
        }
        break;
      }
      case JSOP_BITOR:
        masm.ma_orr(R1.payloadReg(), R0.payloadReg(), R0.payloadReg());
        break;
      case JSOP_BITXOR:
        masm.ma_eor(R1.payloadReg(), R0.payloadReg(), R0.payloadReg());
        break;
      case JSOP_BITAND:
        masm.ma_and(R1.payloadReg(), R0.payloadReg(), R0.payloadReg());
        break;
      case JSOP_LSH:
        // ARM will happily try to shift by more than 0x1f.
        masm.as_and(R1.payloadReg(), R1.payloadReg(), Imm8(0x1F));
        masm.ma_lsl(R1.payloadReg(), R0.payloadReg(), R0.payloadReg());
        break;
      case JSOP_RSH:
        masm.as_and(R1.payloadReg(), R1.payloadReg(), Imm8(0x1F));
        masm.ma_asr(R1.payloadReg(), R0.payloadReg(), R0.payloadReg());
        break;
      case JSOP_URSH:
        masm.as_and(scratchReg, R1.payloadReg(), Imm8(0x1F));
        masm.ma_lsr(scratchReg, R0.payloadReg(), scratchReg);
        masm.as_cmp(scratchReg, Imm8(0));
        if (allowDouble_) {
            Label toUint;
            masm.j(Assembler::LessThan, &toUint);

            // Move result and box for return.
            masm.mov(scratchReg, R0.payloadReg());
            EmitReturnFromIC(masm);

            masm.bind(&toUint);
            ScratchDoubleScope scratchDouble(masm);
            masm.convertUInt32ToDouble(scratchReg, scratchDouble);
            masm.boxDouble(scratchDouble, R0, scratchDouble);
        } else {
            masm.j(Assembler::LessThan, &failure);
            // Move result for return.
            masm.mov(scratchReg, R0.payloadReg());
        }
        break;
      default:
        MOZ_CRASH("Unhandled op for BinaryArith_Int32.");
    }

    EmitReturnFromIC(masm);

    switch (op_) {
      case JSOP_MUL:
        masm.bind(&maybeNegZero);

        // Result is -0 if exactly one of lhs or rhs is negative.
        masm.ma_cmn(R0.payloadReg(), R1.payloadReg());
        masm.j(Assembler::Signed, &failure);

        // Result is +0.
        masm.ma_mov(Imm32(0), R0.payloadReg());
        EmitReturnFromIC(masm);
        break;
      case JSOP_DIV:
      case JSOP_MOD:
        masm.bind(&revertRegister);
        masm.moveValue(savedValue, R0);
        break;
      default:
        break;
    }

    // Failure case - jump to next stub.
    masm.bind(&failure);
    EmitStubGuardFailure(masm);

    return true;
}

bool
ICUnaryArith_Int32::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure;
    masm.branchTestInt32(Assembler::NotEqual, R0, &failure);

    switch (op) {
      case JSOP_BITNOT:
        masm.ma_mvn(R0.payloadReg(), R0.payloadReg());
        break;
      case JSOP_NEG:
        // Guard against 0 and MIN_INT, both result in a double.
        masm.branchTest32(Assembler::Zero, R0.payloadReg(), Imm32(0x7fffffff), &failure);

        // Compile -x as 0 - x.
        masm.as_rsb(R0.payloadReg(), R0.payloadReg(), Imm8(0));
        break;
      default:
        MOZ_CRASH("Unexpected op");
    }

    EmitReturnFromIC(masm);

    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

} // namespace jit
} // namespace js
