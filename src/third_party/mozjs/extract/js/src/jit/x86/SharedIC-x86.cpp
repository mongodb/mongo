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

bool
ICBinaryArith_Int32::Compiler::generateStubCode(MacroAssembler& masm)
{
    // Guard that R0 is an integer and R1 is an integer.
    Label failure;
    masm.branchTestInt32(Assembler::NotEqual, R0, &failure);
    masm.branchTestInt32(Assembler::NotEqual, R1, &failure);

    // Add R0 and R1.  Don't need to explicitly unbox, just use the TailCallReg which
    // should be available.
    Register scratchReg = ICTailCallReg;

    Label revertRegister, maybeNegZero;
    switch(op_) {
      case JSOP_ADD:
        // Add R0 and R1.  Don't need to explicitly unbox.
        masm.movl(R0.payloadReg(), scratchReg);
        masm.addl(R1.payloadReg(), scratchReg);

        // Just jump to failure on overflow.  R0 and R1 are preserved, so we can just jump to
        // the next stub.
        masm.j(Assembler::Overflow, &failure);

        // Just overwrite the payload, the tag is still fine.
        masm.movl(scratchReg, R0.payloadReg());
        break;
      case JSOP_SUB:
        masm.movl(R0.payloadReg(), scratchReg);
        masm.subl(R1.payloadReg(), scratchReg);
        masm.j(Assembler::Overflow, &failure);
        masm.movl(scratchReg, R0.payloadReg());
        break;
      case JSOP_MUL:
        masm.movl(R0.payloadReg(), scratchReg);
        masm.imull(R1.payloadReg(), scratchReg);
        masm.j(Assembler::Overflow, &failure);

        masm.test32(scratchReg, scratchReg);
        masm.j(Assembler::Zero, &maybeNegZero);

        masm.movl(scratchReg, R0.payloadReg());
        break;
      case JSOP_DIV:
      {
        // Prevent division by 0.
        masm.branchTest32(Assembler::Zero, R1.payloadReg(), R1.payloadReg(), &failure);

        // Prevent negative 0 and -2147483648 / -1.
        masm.branch32(Assembler::Equal, R0.payloadReg(), Imm32(INT32_MIN), &failure);

        Label notZero;
        masm.branch32(Assembler::NotEqual, R0.payloadReg(), Imm32(0), &notZero);
        masm.branchTest32(Assembler::Signed, R1.payloadReg(), R1.payloadReg(), &failure);
        masm.bind(&notZero);

        // For idiv we need eax.
        MOZ_ASSERT(R1.typeReg() == eax);
        masm.movl(R0.payloadReg(), eax);
        // Preserve R0.payloadReg()/edx, eax is JSVAL_TYPE_INT32.
        masm.movl(R0.payloadReg(), scratchReg);
        // Sign extend eax into edx to make (edx:eax), since idiv is 64-bit.
        masm.cdq();
        masm.idiv(R1.payloadReg());

        // A remainder implies a double result.
        masm.branchTest32(Assembler::NonZero, edx, edx, &revertRegister);

        masm.movl(eax, R0.payloadReg());
        break;
      }
      case JSOP_MOD:
      {
        // x % 0 always results in NaN.
        masm.branchTest32(Assembler::Zero, R1.payloadReg(), R1.payloadReg(), &failure);

        // Prevent negative 0 and -2147483648 % -1.
        masm.branchTest32(Assembler::Zero, R0.payloadReg(), Imm32(0x7fffffff), &failure);

        // For idiv we need eax.
        MOZ_ASSERT(R1.typeReg() == eax);
        masm.movl(R0.payloadReg(), eax);
        // Preserve R0.payloadReg()/edx, eax is JSVAL_TYPE_INT32.
        masm.movl(R0.payloadReg(), scratchReg);
        // Sign extend eax into edx to make (edx:eax), since idiv is 64-bit.
        masm.cdq();
        masm.idiv(R1.payloadReg());

        // Fail when we would need a negative remainder.
        Label done;
        masm.branchTest32(Assembler::NonZero, edx, edx, &done);
        masm.branchTest32(Assembler::Signed, scratchReg, scratchReg, &revertRegister);
        masm.branchTest32(Assembler::Signed, R1.payloadReg(), R1.payloadReg(), &revertRegister);

        masm.bind(&done);
        // Result is in edx, tag in ecx remains untouched.
        MOZ_ASSERT(R0.payloadReg() == edx);
        MOZ_ASSERT(R0.typeReg() == ecx);
        break;
      }
      case JSOP_BITOR:
        // We can overide R0, because the instruction is unfailable.
        // The R0.typeReg() is also still intact.
        masm.orl(R1.payloadReg(), R0.payloadReg());
        break;
      case JSOP_BITXOR:
        masm.xorl(R1.payloadReg(), R0.payloadReg());
        break;
      case JSOP_BITAND:
        masm.andl(R1.payloadReg(), R0.payloadReg());
        break;
      case JSOP_LSH:
        // RHS needs to be in ecx for shift operations.
        MOZ_ASSERT(R0.typeReg() == ecx);
        masm.movl(R1.payloadReg(), ecx);
        masm.shll_cl(R0.payloadReg());
        // We need to tag again, because we overwrote it.
        masm.tagValue(JSVAL_TYPE_INT32, R0.payloadReg(), R0);
        break;
      case JSOP_RSH:
        masm.movl(R1.payloadReg(), ecx);
        masm.sarl_cl(R0.payloadReg());
        masm.tagValue(JSVAL_TYPE_INT32, R0.payloadReg(), R0);
        break;
      case JSOP_URSH:
        if (!allowDouble_)
            masm.movl(R0.payloadReg(), scratchReg);

        masm.movl(R1.payloadReg(), ecx);
        masm.shrl_cl(R0.payloadReg());
        masm.test32(R0.payloadReg(), R0.payloadReg());
        if (allowDouble_) {
            Label toUint;
            masm.j(Assembler::Signed, &toUint);

            // Box and return.
            masm.tagValue(JSVAL_TYPE_INT32, R0.payloadReg(), R0);
            EmitReturnFromIC(masm);

            masm.bind(&toUint);
            masm.convertUInt32ToDouble(R0.payloadReg(), ScratchDoubleReg);
            masm.boxDouble(ScratchDoubleReg, R0, ScratchDoubleReg);
        } else {
            masm.j(Assembler::Signed, &revertRegister);
            masm.tagValue(JSVAL_TYPE_INT32, R0.payloadReg(), R0);
        }
        break;
      default:
       MOZ_CRASH("Unhandled op for BinaryArith_Int32.");
    }

    // Return.
    EmitReturnFromIC(masm);

    switch(op_) {
      case JSOP_MUL:
        masm.bind(&maybeNegZero);

        // Result is -0 if exactly one of lhs or rhs is negative.
        masm.movl(R0.payloadReg(), scratchReg);
        masm.orl(R1.payloadReg(), scratchReg);
        masm.j(Assembler::Signed, &failure);

        // Result is +0.
        masm.mov(ImmWord(0), R0.payloadReg());
        EmitReturnFromIC(masm);
        break;
      case JSOP_DIV:
      case JSOP_MOD:
        masm.bind(&revertRegister);
        masm.movl(scratchReg, R0.payloadReg());
        masm.movl(ImmType(JSVAL_TYPE_INT32), R1.typeReg());
        break;
      case JSOP_URSH:
        // Revert the content of R0 in the fallible >>> case.
        if (!allowDouble_) {
            masm.bind(&revertRegister);
            masm.tagValue(JSVAL_TYPE_INT32, scratchReg, R0);
        }
        break;
      default:
        // No special failure handling required.
        // Fall through to failure.
        break;
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
        masm.notl(R0.payloadReg());
        break;
      case JSOP_NEG:
        // Guard against 0 and MIN_INT, both result in a double.
        masm.branchTest32(Assembler::Zero, R0.payloadReg(), Imm32(0x7fffffff), &failure);
        masm.negl(R0.payloadReg());
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
