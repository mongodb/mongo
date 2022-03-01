/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_CalleeToken_h
#define jit_CalleeToken_h

#include "mozilla/Assertions.h"

#include <stdint.h>

#include "js/TypeDecls.h"

class JS_PUBLIC_API JSTracer;

namespace js::jit {

using CalleeToken = void*;

enum CalleeTokenTag {
  CalleeToken_Function = 0x0,  // untagged
  CalleeToken_FunctionConstructing = 0x1,
  CalleeToken_Script = 0x2
};

// Any CalleeToken with this bit set must be CalleeToken_Script.
static const uintptr_t CalleeTokenScriptBit = CalleeToken_Script;

static const uintptr_t CalleeTokenMask = ~uintptr_t(0x3);

static inline CalleeTokenTag GetCalleeTokenTag(CalleeToken token) {
  CalleeTokenTag tag = CalleeTokenTag(uintptr_t(token) & 0x3);
  MOZ_ASSERT(tag <= CalleeToken_Script);
  return tag;
}
static inline CalleeToken CalleeToToken(JSFunction* fun, bool constructing) {
  CalleeTokenTag tag =
      constructing ? CalleeToken_FunctionConstructing : CalleeToken_Function;
  return CalleeToken(uintptr_t(fun) | uintptr_t(tag));
}
static inline CalleeToken CalleeToToken(JSScript* script) {
  return CalleeToken(uintptr_t(script) | uintptr_t(CalleeToken_Script));
}
static inline bool CalleeTokenIsFunction(CalleeToken token) {
  CalleeTokenTag tag = GetCalleeTokenTag(token);
  return tag == CalleeToken_Function || tag == CalleeToken_FunctionConstructing;
}
static inline bool CalleeTokenIsConstructing(CalleeToken token) {
  return GetCalleeTokenTag(token) == CalleeToken_FunctionConstructing;
}
static inline JSFunction* CalleeTokenToFunction(CalleeToken token) {
  MOZ_ASSERT(CalleeTokenIsFunction(token));
  return (JSFunction*)(uintptr_t(token) & CalleeTokenMask);
}
static inline JSScript* CalleeTokenToScript(CalleeToken token) {
  MOZ_ASSERT(GetCalleeTokenTag(token) == CalleeToken_Script);
  return (JSScript*)(uintptr_t(token) & CalleeTokenMask);
}

CalleeToken TraceCalleeToken(JSTracer* trc, CalleeToken token);

} /* namespace js::jit */

#endif /* jit_CalleeToken_h */
