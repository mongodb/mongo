/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BaselineFrameInfo_inl_h
#define jit_BaselineFrameInfo_inl_h

#include "jit/BaselineFrameInfo.h"

#include "jit/MacroAssembler-inl.h"

namespace js {
namespace jit {

void CompilerFrameInfo::pop(StackAdjustment adjust) {
  spIndex--;
  StackValue* popped = &stack[spIndex];

  if (adjust == AdjustStack && popped->kind() == StackValue::Stack) {
    masm.addToStackPtr(Imm32(sizeof(Value)));
  }
  // Assert when anything uses this value.
  popped->reset();
}

void CompilerFrameInfo::popn(uint32_t n, StackAdjustment adjust) {
  uint32_t poppedStack = 0;
  for (uint32_t i = 0; i < n; i++) {
    if (peek(-1)->kind() == StackValue::Stack) {
      poppedStack++;
    }
    pop(DontAdjustStack);
  }
  if (adjust == AdjustStack && poppedStack > 0) {
    masm.addToStackPtr(Imm32(sizeof(Value) * poppedStack));
  }
}

void InterpreterFrameInfo::pop() { popn(1); }

void InterpreterFrameInfo::popn(uint32_t n) {
  masm.addToStackPtr(Imm32(n * sizeof(Value)));
}

}  // namespace jit
}  // namespace js

#endif /* jit_BaselineFrameInfo_inl_h */
