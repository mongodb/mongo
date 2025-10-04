/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_Jit_h
#define jit_Jit_h

#include "jstypes.h"

struct JS_PUBLIC_API JSContext;

namespace js {

class RunState;

namespace jit {

enum class EnterJitStatus {
  // An error occurred, either before we entered JIT code or the script threw
  // an exception. Usually the context will have a pending exception, except
  // for uncatchable exceptions (interrupts).
  Error,

  // Entered and returned from JIT code.
  Ok,

  // We didn't enter JIT code, for instance because the script still has to
  // warm up or cannot be compiled.
  NotEntered,
};

extern bool EnterInterpreterEntryTrampoline(uint8_t* code, JSContext* cx,
                                            RunState* state);
extern EnterJitStatus MaybeEnterJit(JSContext* cx, RunState& state);

}  // namespace jit
}  // namespace js

#endif /* jit_Jit_h */
