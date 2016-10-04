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

// ICCompare_Int32

bool
ICCompare_Int32::Compiler::generateStubCode(MacroAssembler& masm)
{
    // Guard that R0 is an integer and R1 is an integer.
    Label failure;
    masm.branchTestInt32(Assembler::NotEqual, R0, &failure);
    masm.branchTestInt32(Assembler::NotEqual, R1, &failure);

    // Directly compare the int32 payload of R0 and R1.
    ScratchRegisterScope scratch(masm);
    Assembler::Condition cond = JSOpToCondition(op, /* signed = */true);
    masm.mov(ImmWord(0), scratch);
    masm.cmp32(R0.valueReg(), R1.valueReg());
    masm.setCC(cond, scratch);

    // Box the result and return
    masm.boxValue(JSVAL_TYPE_BOOLEAN, scratch, R0.valueReg());
    EmitReturnFromIC(masm);

    // Failure case - jump to next stub
    masm.bind(&failure);
    EmitStubGuardFailure(masm);

    return true;
}

} // namespace jit
} // namespace js
