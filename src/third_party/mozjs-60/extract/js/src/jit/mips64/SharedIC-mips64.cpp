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
#include "vm/Iteration.h"

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

    // Add R0 and R1. Don't need to explicitly unbox, just use R2's valueReg.
    Register scratchReg = R2.valueReg();

    Label goodMul, divTest1, divTest2;
    switch(op_) {
      case JSOP_ADD:
        masm.unboxInt32(R0, ExtractTemp0);
        masm.unboxInt32(R1, ExtractTemp1);
        masm.ma_addTestOverflow(scratchReg, ExtractTemp0, ExtractTemp1, &failure);
        masm.boxValue(JSVAL_TYPE_INT32, scratchReg, R0.valueReg());
        break;
      case JSOP_SUB:
        masm.unboxInt32(R0, ExtractTemp0);
        masm.unboxInt32(R1, ExtractTemp1);
        masm.ma_subTestOverflow(scratchReg, ExtractTemp0, ExtractTemp1, &failure);
        masm.boxValue(JSVAL_TYPE_INT32, scratchReg, R0.valueReg());
        break;
      case JSOP_MUL: {
        masm.unboxInt32(R0, ExtractTemp0);
        masm.unboxInt32(R1, ExtractTemp1);
        masm.ma_mul_branch_overflow(scratchReg, ExtractTemp0, ExtractTemp1, &failure);

        masm.ma_b(scratchReg, Imm32(0), &goodMul, Assembler::NotEqual, ShortJump);

        // Result is -0 if operands have different signs.
        masm.as_xor(t8, ExtractTemp0, ExtractTemp1);
        masm.ma_b(t8, Imm32(0), &failure, Assembler::LessThan, ShortJump);

        masm.bind(&goodMul);
        masm.boxValue(JSVAL_TYPE_INT32, scratchReg, R0.valueReg());
        break;
      }
      case JSOP_DIV:
      case JSOP_MOD: {
        masm.unboxInt32(R0, ExtractTemp0);
        masm.unboxInt32(R1, ExtractTemp1);
        // Check for INT_MIN / -1, it results in a double.
        masm.ma_b(ExtractTemp0, Imm32(INT_MIN), &divTest1, Assembler::NotEqual, ShortJump);
        masm.ma_b(ExtractTemp1, Imm32(-1), &failure, Assembler::Equal, ShortJump);
        masm.bind(&divTest1);

        // Check for division by zero
        masm.ma_b(ExtractTemp1, Imm32(0), &failure, Assembler::Equal, ShortJump);

        // Check for 0 / X with X < 0 (results in -0).
        masm.ma_b(ExtractTemp0, Imm32(0), &divTest2, Assembler::NotEqual, ShortJump);
        masm.ma_b(ExtractTemp1, Imm32(0), &failure, Assembler::LessThan, ShortJump);
        masm.bind(&divTest2);

        masm.as_div(ExtractTemp0, ExtractTemp1);

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
            masm.ma_b(ExtractTemp0, Imm32(0), &failure, Assembler::LessThan, ShortJump);
            masm.bind(&done);
            masm.tagValue(JSVAL_TYPE_INT32, scratchReg, R0);
        }
        break;
      }
      case JSOP_BITOR:
        masm.as_or(R0.valueReg() , R0.valueReg(), R1.valueReg());
        break;
      case JSOP_BITXOR:
        masm.as_xor(scratchReg, R0.valueReg(), R1.valueReg());
        masm.tagValue(JSVAL_TYPE_INT32, scratchReg, R0);
        break;
      case JSOP_BITAND:
        masm.as_and(R0.valueReg() , R0.valueReg(), R1.valueReg());
        break;
      case JSOP_LSH:
        masm.unboxInt32(R0, ExtractTemp0);
        masm.unboxInt32(R1, ExtractTemp1);
        // MIPS will only use 5 lowest bits in R1 as shift offset.
        masm.ma_sll(scratchReg, ExtractTemp0, ExtractTemp1);
        masm.tagValue(JSVAL_TYPE_INT32, scratchReg, R0);
        break;
      case JSOP_RSH:
        masm.unboxInt32(R0, ExtractTemp0);
        masm.unboxInt32(R1, ExtractTemp1);
        masm.ma_sra(scratchReg, ExtractTemp0, ExtractTemp1);
        masm.tagValue(JSVAL_TYPE_INT32, scratchReg, R0);
        break;
      case JSOP_URSH:
        masm.unboxInt32(R0, ExtractTemp0);
        masm.unboxInt32(R1, ExtractTemp1);
        masm.ma_srl(scratchReg, ExtractTemp0, ExtractTemp1);
        if (allowDouble_) {
            Label toUint;
            masm.ma_b(scratchReg, Imm32(0), &toUint, Assembler::LessThan, ShortJump);

            // Move result and box for return.
            masm.tagValue(JSVAL_TYPE_INT32, scratchReg, R0);
            EmitReturnFromIC(masm);

            masm.bind(&toUint);
            masm.convertUInt32ToDouble(scratchReg, FloatReg1);
            masm.boxDouble(FloatReg1, R0, ScratchDoubleReg);
        } else {
            masm.ma_b(scratchReg, Imm32(0), &failure, Assembler::LessThan, ShortJump);
            // Move result for return.
            masm.tagValue(JSVAL_TYPE_INT32, scratchReg, R0);
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
        masm.not32(R0.valueReg());
        masm.tagValue(JSVAL_TYPE_INT32, R0.valueReg(), R0);
        break;
      case JSOP_NEG:
        masm.unboxInt32(R0, ExtractTemp0);
        // Guard against 0 and MIN_INT, both result in a double.
        masm.branchTest32(Assembler::Zero, ExtractTemp0, Imm32(INT32_MAX), &failure);

        masm.neg32(ExtractTemp0);
        masm.tagValue(JSVAL_TYPE_INT32, ExtractTemp0, R0);
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
