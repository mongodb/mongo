/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/BaselineIC.h"
#include "jit/SharedICHelpers.h"

using namespace js;
using namespace js::jit;

namespace js {
namespace jit {

bool
ICCompare_Double::Compiler::generateStubCode(MacroAssembler& masm)
{
    Label failure, isNaN;
    masm.ensureDouble(R0, FloatReg0, &failure);
    masm.ensureDouble(R1, FloatReg1, &failure);

    Register dest = R0.scratchReg();

    Assembler::DoubleCondition doubleCond = JSOpToDoubleCondition(op);

    masm.ma_cmp_set_double(dest, FloatReg0, FloatReg1, doubleCond);

    masm.tagValue(JSVAL_TYPE_BOOLEAN, dest, R0);
    EmitReturnFromIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);
    return true;
}

} // namespace jit
} // namespace js
