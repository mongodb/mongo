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

bool
ICCompare_Double::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure, notNaN;
    masm.ensureDouble(R0, FloatReg0, &failure);
    masm.ensureDouble(R1, FloatReg1, &failure);

    Register dest = R0.scratchReg();

    Assembler::DoubleCondition cond = JSOpToDoubleCondition(op);
    masm.mov(ImmWord(0), dest);
    masm.compareDouble(cond, FloatReg0, FloatReg1);
    masm.setCC(Assembler::ConditionFromDoubleCondition(cond), dest);

    // Check for NaN, if needed.
    Assembler::NaNCond nanCond = Assembler::NaNCondFromDoubleCondition(cond);
    if (nanCond != Assembler::NaN_HandledByCond) {
      masm.j(Assembler::NoParity, &notNaN);
      masm.mov(ImmWord(nanCond == Assembler::NaN_IsTrue), dest);
      masm.bind(&notNaN);
    }

    masm.tagValue(JSVAL_TYPE_BOOLEAN, dest, R0);
    EmitReturnFromIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}
