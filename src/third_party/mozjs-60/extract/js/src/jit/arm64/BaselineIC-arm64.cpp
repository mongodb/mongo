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
    masm.cmp32(R0.valueReg(), R1.valueReg());
    masm.Cset(ARMRegister(R0.valueReg(), 32), cond);

    // Result is implicitly boxed already.
    masm.tagValue(JSVAL_TYPE_BOOLEAN, R0.valueReg(), R0);
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

    Register dest = R0.valueReg();

    Assembler::DoubleCondition doubleCond = JSOpToDoubleCondition(op);
    Assembler::Condition cond = Assembler::ConditionFromDoubleCondition(doubleCond);

    masm.compareDouble(doubleCond, FloatReg0, FloatReg1);
    masm.Cset(ARMRegister(dest, 32), cond);

    masm.tagValue(JSVAL_TYPE_BOOLEAN, dest, R0);
    EmitReturnFromIC(masm);

    // Failure case - jump to next stub.
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

} // namespace jit
} // namespace js
