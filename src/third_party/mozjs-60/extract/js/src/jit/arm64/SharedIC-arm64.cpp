/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/SharedIC.h"
#include "jit/SharedICHelpers.h"

#ifdef JS_SIMULATOR_ARM64
#include "jit/arm64/Assembler-arm64.h"
#include "jit/arm64/BaselineCompiler-arm64.h"
#include "jit/arm64/vixl/Debugger-vixl.h"
#endif

#include "jit/arm64/MacroAssembler-arm64-inl.h"

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

    // Add R0 and R1. Don't need to explicitly unbox, just use R2.
    Register Rscratch = R2_;
    ARMRegister Wscratch = ARMRegister(Rscratch, 32);
#ifdef MERGE
    // DIV and MOD need an extra non-volatile ValueOperand to hold R0.
    AllocatableGeneralRegisterSet savedRegs(availableGeneralRegs(2));
    savedRegs.set() = GeneralRegisterSet::Intersect(GeneralRegisterSet::NonVolatile(), savedRegs);
#endif
    // get some more ARM-y names for the registers
    ARMRegister W0(R0_, 32);
    ARMRegister X0(R0_, 64);
    ARMRegister W1(R1_, 32);
    ARMRegister X1(R1_, 64);
    ARMRegister WTemp(ExtractTemp0, 32);
    ARMRegister XTemp(ExtractTemp0, 64);
    Label maybeNegZero, revertRegister;
    switch(op_) {
      case JSOP_ADD:
        masm.Adds(WTemp, W0, Operand(W1));

        // Just jump to failure on overflow. R0 and R1 are preserved, so we can
        // just jump to the next stub.
        masm.j(Assembler::Overflow, &failure);

        // Box the result and return. We know R0 already contains the
        // integer tag, so we just need to move the payload into place.
        masm.movePayload(ExtractTemp0, R0_);
        break;

      case JSOP_SUB:
        masm.Subs(WTemp, W0, Operand(W1));
        masm.j(Assembler::Overflow, &failure);
        masm.movePayload(ExtractTemp0, R0_);
        break;

      case JSOP_MUL:
        masm.mul32(R0.valueReg(), R1.valueReg(), Rscratch, &failure, &maybeNegZero);
        masm.movePayload(Rscratch, R0_);
        break;

      case JSOP_DIV:
      case JSOP_MOD: {

        // Check for INT_MIN / -1, it results in a double.
        Label check2;
        masm.Cmp(W0, Operand(INT_MIN));
        masm.B(&check2, Assembler::NotEqual);
        masm.Cmp(W1, Operand(-1));
        masm.j(Assembler::Equal, &failure);
        masm.bind(&check2);
        Label no_fail;
        // Check for both division by zero and 0 / X with X < 0 (results in -0).
        masm.Cmp(W1, Operand(0));
        // If x > 0, then it can't be bad.
        masm.B(&no_fail, Assembler::GreaterThan);
        // if x == 0, then ignore any comparison, and force
        // it to fail, if x < 0 (the only other case)
        // then do the comparison, and fail if y == 0
        masm.Ccmp(W0, Operand(0), vixl::ZFlag, Assembler::NotEqual);
        masm.B(&failure, Assembler::Equal);
        masm.bind(&no_fail);
        masm.Sdiv(Wscratch, W0, W1);
        // Start calculating the remainder, x - (x / y) * y.
        masm.mul(WTemp, W1, Wscratch);
        if (op_ == JSOP_DIV) {
            // Result is a double if the remainder != 0, which happens
            // when (x/y)*y != x.
            masm.branch32(Assembler::NotEqual, R0.valueReg(), ExtractTemp0, &revertRegister);
            masm.movePayload(Rscratch, R0_);
        } else {
            // Calculate the actual mod. Set the condition code, so we can see if it is non-zero.
            masm.Subs(WTemp, W0, WTemp);

            // If X % Y == 0 and X < 0, the result is -0.
            masm.Ccmp(W0, Operand(0), vixl::NoFlag, Assembler::Equal);
            masm.branch(Assembler::LessThan, &revertRegister);
            masm.movePayload(ExtractTemp0, R0_);
        }
        break;
      }
        // ORR, EOR, AND can trivially be coerced int
        // working without affecting the tag of the dest..
      case JSOP_BITOR:
        masm.Orr(X0, X0, Operand(X1));
        break;
      case JSOP_BITXOR:
        masm.Eor(X0, X0, Operand(W1, vixl::UXTW));
        break;
      case JSOP_BITAND:
        masm.And(X0, X0, Operand(X1));
        break;
        // LSH, RSH and URSH can not.
      case JSOP_LSH:
        // ARM will happily try to shift by more than 0x1f.
        masm.Lsl(Wscratch, W0, W1);
        masm.movePayload(Rscratch, R0.valueReg());
        break;
      case JSOP_RSH:
        masm.Asr(Wscratch, W0, W1);
        masm.movePayload(Rscratch, R0.valueReg());
        break;
      case JSOP_URSH:
        masm.Lsr(Wscratch, W0, W1);
        if (allowDouble_) {
            Label toUint;
            // Testing for negative is equivalent to testing bit 31
            masm.Tbnz(Wscratch, 31, &toUint);
            // Move result and box for return.
            masm.movePayload(Rscratch, R0_);
            EmitReturnFromIC(masm);

            masm.bind(&toUint);
            masm.convertUInt32ToDouble(Rscratch, ScratchDoubleReg);
            masm.boxDouble(ScratchDoubleReg, R0, ScratchDoubleReg);
        } else {
            // Testing for negative is equivalent to testing bit 31
            masm.Tbnz(Wscratch, 31, &failure);
            // Move result for return.
            masm.movePayload(Rscratch, R0_);
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
        masm.Cmn(W0, W1);
        masm.j(Assembler::Signed, &failure);

        // Result is +0, so use the zero register.
        masm.movePayload(rzr, R0_);
        EmitReturnFromIC(masm);
        break;
      case JSOP_DIV:
      case JSOP_MOD:
        masm.bind(&revertRegister);
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
        masm.Mvn(ARMRegister(R1.valueReg(), 32), ARMRegister(R0.valueReg(), 32));
        masm.movePayload(R1.valueReg(), R0.valueReg());
        break;
      case JSOP_NEG:
        // Guard against 0 and MIN_INT, both result in a double.
        masm.branchTest32(Assembler::Zero, R0.valueReg(), Imm32(0x7fffffff), &failure);

        // Compile -x as 0 - x.
        masm.Sub(ARMRegister(R1.valueReg(), 32), wzr, ARMRegister(R0.valueReg(), 32));
        masm.movePayload(R1.valueReg(), R0.valueReg());
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
