/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/BaselineIC.h"
#include "jit/SharedICHelpers.h"

#include "jit/MacroAssembler-inl.h"

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

    // The scratch register is only used in the case of JSOP_URSH.
    mozilla::Maybe<ScratchRegisterScope> scratch;

    Label revertRegister, maybeNegZero;
    switch(op_) {
      case JSOP_ADD:
        masm.unboxInt32(R0, ExtractTemp0);
        // Just jump to failure on overflow. R0 and R1 are preserved, so we can just jump to
        // the next stub.
        masm.addl(R1.valueReg(), ExtractTemp0);
        masm.j(Assembler::Overflow, &failure);

        // Box the result
        masm.boxValue(JSVAL_TYPE_INT32, ExtractTemp0, R0.valueReg());
        break;
      case JSOP_SUB:
        masm.unboxInt32(R0, ExtractTemp0);
        masm.subl(R1.valueReg(), ExtractTemp0);
        masm.j(Assembler::Overflow, &failure);
        masm.boxValue(JSVAL_TYPE_INT32, ExtractTemp0, R0.valueReg());
        break;
      case JSOP_MUL:
        masm.unboxInt32(R0, ExtractTemp0);
        masm.imull(R1.valueReg(), ExtractTemp0);
        masm.j(Assembler::Overflow, &failure);

        masm.branchTest32(Assembler::Zero, ExtractTemp0, ExtractTemp0, &maybeNegZero);

        masm.boxValue(JSVAL_TYPE_INT32, ExtractTemp0, R0.valueReg());
        break;
      case JSOP_DIV:
      {
        MOZ_ASSERT(R2.scratchReg() == rax);
        MOZ_ASSERT(R0.valueReg() != rdx);
        MOZ_ASSERT(R1.valueReg() != rdx);
        masm.unboxInt32(R0, eax);
        masm.unboxInt32(R1, ExtractTemp0);

        // Prevent division by 0.
        masm.branchTest32(Assembler::Zero, ExtractTemp0, ExtractTemp0, &failure);

        // Prevent negative 0 and -2147483648 / -1.
        masm.branch32(Assembler::Equal, eax, Imm32(INT32_MIN), &failure);

        Label notZero;
        masm.branch32(Assembler::NotEqual, eax, Imm32(0), &notZero);
        masm.branchTest32(Assembler::Signed, ExtractTemp0, ExtractTemp0, &failure);
        masm.bind(&notZero);

        // Sign extend eax into edx to make (edx:eax), since idiv is 64-bit.
        masm.cdq();
        masm.idiv(ExtractTemp0);

        // A remainder implies a double result.
        masm.branchTest32(Assembler::NonZero, edx, edx, &failure);

        masm.boxValue(JSVAL_TYPE_INT32, eax, R0.valueReg());
        break;
      }
      case JSOP_MOD:
      {
        MOZ_ASSERT(R2.scratchReg() == rax);
        MOZ_ASSERT(R0.valueReg() != rdx);
        MOZ_ASSERT(R1.valueReg() != rdx);
        masm.unboxInt32(R0, eax);
        masm.unboxInt32(R1, ExtractTemp0);

        // x % 0 always results in NaN.
        masm.branchTest32(Assembler::Zero, ExtractTemp0, ExtractTemp0, &failure);

        // Prevent negative 0 and -2147483648 % -1.
        masm.branchTest32(Assembler::Zero, eax, Imm32(0x7fffffff), &failure);

        // Sign extend eax into edx to make (edx:eax), since idiv is 64-bit.
        masm.cdq();
        masm.idiv(ExtractTemp0);

        // Fail when we would need a negative remainder.
        Label done;
        masm.branchTest32(Assembler::NonZero, edx, edx, &done);
        masm.orl(ExtractTemp0, eax);
        masm.branchTest32(Assembler::Signed, eax, eax, &failure);

        masm.bind(&done);
        masm.boxValue(JSVAL_TYPE_INT32, edx, R0.valueReg());
        break;
      }
      case JSOP_BITOR:
        // We can overide R0, because the instruction is unfailable.
        // Because the tag bits are the same, we don't need to retag.
        masm.orq(R1.valueReg(), R0.valueReg());
        break;
      case JSOP_BITXOR:
        masm.xorl(R1.valueReg(), R0.valueReg());
        masm.tagValue(JSVAL_TYPE_INT32, R0.valueReg(), R0);
        break;
      case JSOP_BITAND:
        masm.andq(R1.valueReg(), R0.valueReg());
        break;
      case JSOP_LSH:
        masm.unboxInt32(R0, ExtractTemp0);
        masm.unboxInt32(R1, ecx); // Unboxing R1 to ecx, clobbers R0.
        masm.shll_cl(ExtractTemp0);
        masm.boxValue(JSVAL_TYPE_INT32, ExtractTemp0, R0.valueReg());
        break;
      case JSOP_RSH:
        masm.unboxInt32(R0, ExtractTemp0);
        masm.unboxInt32(R1, ecx);
        masm.sarl_cl(ExtractTemp0);
        masm.boxValue(JSVAL_TYPE_INT32, ExtractTemp0, R0.valueReg());
        break;
      case JSOP_URSH:
        if (!allowDouble_) {
            scratch.emplace(masm);
            masm.movq(R0.valueReg(), *scratch);
        }

        masm.unboxInt32(R0, ExtractTemp0);
        masm.unboxInt32(R1, ecx); // This clobbers R0

        masm.shrl_cl(ExtractTemp0);
        masm.test32(ExtractTemp0, ExtractTemp0);
        if (allowDouble_) {
            Label toUint;
            masm.j(Assembler::Signed, &toUint);

            // Box and return.
            masm.boxValue(JSVAL_TYPE_INT32, ExtractTemp0, R0.valueReg());
            EmitReturnFromIC(masm);

            masm.bind(&toUint);
            ScratchDoubleScope scratchDouble(masm);
            masm.convertUInt32ToDouble(ExtractTemp0, scratchDouble);
            masm.boxDouble(scratchDouble, R0);
        } else {
            masm.j(Assembler::Signed, &revertRegister);
            masm.boxValue(JSVAL_TYPE_INT32, ExtractTemp0, R0.valueReg());
        }
        break;
      default:
        MOZ_CRASH("Unhandled op in BinaryArith_Int32");
    }

    // Return from stub.
    EmitReturnFromIC(masm);

    if (op_ == JSOP_MUL) {
        masm.bind(&maybeNegZero);

        // Result is -0 if exactly one of lhs or rhs is negative.
        {
            ScratchRegisterScope scratch(masm);
            masm.movl(R0.valueReg(), scratch);
            masm.orl(R1.valueReg(), scratch);
            masm.j(Assembler::Signed, &failure);
        }

        // Result is +0.
        masm.moveValue(Int32Value(0), R0);
        EmitReturnFromIC(masm);
    }

    // Revert the content of R0 in the fallible >>> case.
    if (op_ == JSOP_URSH && !allowDouble_) {
        // Scope continuation from JSOP_URSH case above.
        masm.bind(&revertRegister);
        // Restore tag and payload.
        masm.movq(*scratch, R0.valueReg());
        // Fall through to failure.
    }
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
        masm.notl(R0.valueReg());
        break;
      case JSOP_NEG:
        // Guard against 0 and MIN_INT, both result in a double.
        masm.branchTest32(Assembler::Zero, R0.valueReg(), Imm32(0x7fffffff), &failure);
        masm.negl(R0.valueReg());
        break;
      default:
        MOZ_CRASH("Unexpected op");
    }

    masm.tagValue(JSVAL_TYPE_INT32, R0.valueReg(), R0);

    EmitReturnFromIC(masm);

    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

} // namespace jit
} // namespace js
