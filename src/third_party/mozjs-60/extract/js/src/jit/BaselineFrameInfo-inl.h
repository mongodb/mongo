/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BaselineFrameInfo_inl_h
#define jit_BaselineFrameInfo_inl_h

namespace js {
namespace jit {

void
FrameInfo::pop(StackAdjustment adjust)
{
    spIndex--;
    StackValue* popped = &stack[spIndex];

    if (adjust == AdjustStack && popped->kind() == StackValue::Stack)
        masm.addToStackPtr(Imm32(sizeof(Value)));
    // Assert when anything uses this value.
    popped->reset();
}

void
FrameInfo::popn(uint32_t n, StackAdjustment adjust)
{
    uint32_t poppedStack = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (peek(-1)->kind() == StackValue::Stack)
            poppedStack++;
        pop(DontAdjustStack);
    }
    if (adjust == AdjustStack && poppedStack > 0)
        masm.addToStackPtr(Imm32(sizeof(Value) * poppedStack));
}

} // namespace jit
} // namespace js

#endif /* jit_BaselineFrameInfo_inl_h */
