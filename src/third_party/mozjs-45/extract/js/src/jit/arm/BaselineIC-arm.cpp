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

// ICCompare_Int32

bool
ICCompare_Int32::Compiler::generateStubCode(MacroAssembler& masm)
{
    // Guard that R0 is an integer and R1 is an integer.
    Label failure;
    masm.branchTestInt32(Assembler::NotEqual, R0, &failure);
    masm.branchTestInt32(Assembler::NotEqual, R1, &failure);

    // Compare payload regs of R0 and R1.
    Assembler::Condition cond = JSOpToCondition(op, /* signed = */true);
    masm.cmp32(R0.payloadReg(), R1.payloadReg());
    masm.ma_mov(Imm32(1), R0.payloadReg(), LeaveCC, cond);
    masm.ma_mov(Imm32(0), R0.payloadReg(), LeaveCC, Assembler::InvertCondition(cond));

    // Result is implicitly boxed already.
    masm.tagValue(JSVAL_TYPE_BOOLEAN, R0.payloadReg(), R0);
    EmitReturnFromIC(masm);

    // Failure case - jump to next stub.
    masm.bind(&failure);
    EmitStubGuardFailure(masm);

    return true;
}

bool
ICCompare_Double::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure, isNaN;
    masm.ensureDouble(R0, FloatReg0, &failure);
    masm.ensureDouble(R1, FloatReg1, &failure);

    Register dest = R0.scratchReg();

    Assembler::DoubleCondition doubleCond = JSOpToDoubleCondition(op);
    Assembler::Condition cond = Assembler::ConditionFromDoubleCondition(doubleCond);

    masm.compareDouble(FloatReg0, FloatReg1);
    masm.ma_mov(Imm32(0), dest);
    masm.ma_mov(Imm32(1), dest, LeaveCC, cond);

    masm.tagValue(JSVAL_TYPE_BOOLEAN, dest, R0);
    EmitReturnFromIC(masm);

    // Failure case - jump to next stub.
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

} // namespace jit
} // namespace js
