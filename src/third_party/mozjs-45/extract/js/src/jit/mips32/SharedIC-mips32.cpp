/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jsiter.h"

#include "jit/BaselineCompiler.h"
#include "jit/BaselineIC.h"
#include "jit/BaselineJIT.h"
#include "jit/Linker.h"
#include "jit/SharedICHelpers.h"

#include "jsboolinlines.h"

using namespace js;
using namespace js::jit;

namespace js {
namespace jit {

// ICBinaryArith_Int32

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

    Label goodMul, divTest1, divTest2;
    switch(op_) {
      case JSOP_ADD:
        // We know R0.typeReg() already contains the integer tag. No boxing
        // required.
        masm.ma_addTestOverflow(scratchReg, R0.payloadReg(), R1.payloadReg(), &failure);
        masm.move32(scratchReg, R0.payloadReg());
        break;
      case JSOP_SUB:
        masm.ma_subTestOverflow(scratchReg, R0.payloadReg(), R1.payloadReg(), &failure);
        masm.move32(scratchReg, R0.payloadReg());
        break;
      case JSOP_MUL: {
        masm.ma_mul_branch_overflow(scratchReg, R0.payloadReg(), R1.payloadReg(), &failure);

        masm.ma_b(scratchReg, Imm32(0), &goodMul, Assembler::NotEqual, ShortJump);

        // Result is -0 if operands have different signs.
        masm.as_xor(t8, R0.payloadReg(), R1.payloadReg());
        masm.ma_b(t8, Imm32(0), &failure, Assembler::LessThan, ShortJump);

        masm.bind(&goodMul);
        masm.move32(scratchReg, R0.payloadReg());
        break;
      }
      case JSOP_DIV:
      case JSOP_MOD: {
        // Check for INT_MIN / -1, it results in a double.
        masm.ma_b(R0.payloadReg(), Imm32(INT_MIN), &divTest1, Assembler::NotEqual, ShortJump);
        masm.ma_b(R1.payloadReg(), Imm32(-1), &failure, Assembler::Equal, ShortJump);
        masm.bind(&divTest1);

        // Check for division by zero
        masm.ma_b(R1.payloadReg(), Imm32(0), &failure, Assembler::Equal, ShortJump);

        // Check for 0 / X with X < 0 (results in -0).
        masm.ma_b(R0.payloadReg(), Imm32(0), &divTest2, Assembler::NotEqual, ShortJump);
        masm.ma_b(R1.payloadReg(), Imm32(0), &failure, Assembler::LessThan, ShortJump);
        masm.bind(&divTest2);

        masm.as_div(R0.payloadReg(), R1.payloadReg());

        if (op_ == JSOP_DIV) {
            // Result is a double if the remainder != 0.
            masm.as_mfhi(scratchReg);
            masm.ma_b(scratchReg, Imm32(0), &failure, Assembler::NotEqual, ShortJump);
            masm.as_mflo(scratchReg);
            masm.tagValue(JSVAL_TYPE_INT32, scratchReg, R0);
        } else {
            Label done;
            // If X % Y == 0 and X < 0, the result is -0.
            masm.as_mfhi(scratchReg);
            masm.ma_b(scratchReg, Imm32(0), &done, Assembler::NotEqual, ShortJump);
            masm.ma_b(R0.payloadReg(), Imm32(0), &failure, Assembler::LessThan, ShortJump);
            masm.bind(&done);
            masm.tagValue(JSVAL_TYPE_INT32, scratchReg, R0);
        }
        break;
      }
      case JSOP_BITOR:
        masm.as_or(R0.payloadReg() , R0.payloadReg(), R1.payloadReg());
        break;
      case JSOP_BITXOR:
        masm.as_xor(R0.payloadReg() , R0.payloadReg(), R1.payloadReg());
        break;
      case JSOP_BITAND:
        masm.as_and(R0.payloadReg() , R0.payloadReg(), R1.payloadReg());
        break;
      case JSOP_LSH:
        // MIPS will only use 5 lowest bits in R1 as shift offset.
        masm.ma_sll(R0.payloadReg(), R0.payloadReg(), R1.payloadReg());
        break;
      case JSOP_RSH:
        masm.ma_sra(R0.payloadReg(), R0.payloadReg(), R1.payloadReg());
        break;
      case JSOP_URSH:
        masm.ma_srl(scratchReg, R0.payloadReg(), R1.payloadReg());
        if (allowDouble_) {
            Label toUint;
            masm.ma_b(scratchReg, Imm32(0), &toUint, Assembler::LessThan, ShortJump);

            // Move result and box for return.
            masm.move32(scratchReg, R0.payloadReg());
            EmitReturnFromIC(masm);

            masm.bind(&toUint);
            masm.convertUInt32ToDouble(scratchReg, FloatReg1);
            masm.boxDouble(FloatReg1, R0);
        } else {
            masm.ma_b(scratchReg, Imm32(0), &failure, Assembler::LessThan, ShortJump);
            // Move result for return.
            masm.move32(scratchReg, R0.payloadReg());
        }
        break;
      default:
        MOZ_CRASH("Unhandled op for BinaryArith_Int32.");
    }

    EmitReturnFromIC(masm);

    // Failure case - jump to next stub
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
        masm.not32(R0.payloadReg());
        break;
      case JSOP_NEG:
        // Guard against 0 and MIN_INT, both result in a double.
        masm.branchTest32(Assembler::Zero, R0.payloadReg(), Imm32(INT32_MAX), &failure);

        masm.neg32(R0.payloadReg());
        break;
      default:
        MOZ_CRASH("Unexpected op");
        return false;
    }

    EmitReturnFromIC(masm);

    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}


} // namespace jit
} // namespace js
