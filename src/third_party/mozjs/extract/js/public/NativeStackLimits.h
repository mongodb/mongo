/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_public_NativeStackLimit_h
#define js_public_NativeStackLimit_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include <stddef.h>  // size_t
#include <stdint.h>  // uint32_t, uintptr_t, UINTPTR_MAX
#include <utility>   // std::move

namespace JS {

using NativeStackSize = size_t;

using NativeStackBase = uintptr_t;

using NativeStackLimit = uintptr_t;

#if JS_STACK_GROWTH_DIRECTION > 0
constexpr NativeStackLimit NativeStackLimitMin = 0;
constexpr NativeStackLimit NativeStackLimitMax = UINTPTR_MAX;
#else
constexpr NativeStackLimit NativeStackLimitMin = UINTPTR_MAX;
constexpr NativeStackLimit NativeStackLimitMax = 0;
#endif

#ifdef __wasi__
// We build with the "stack-first" wasm-ld option, so the stack grows downward
// toward zero. Let's set a limit just a bit above this so that we catch an
// overflow before a Wasm trap occurs.
constexpr NativeStackLimit WASINativeStackLimit = 1024;
#endif  // __wasi__

inline NativeStackLimit GetNativeStackLimit(NativeStackBase base,
                                            NativeStackSize size) {
#if JS_STACK_GROWTH_DIRECTION > 0
  MOZ_ASSERT(base <= size_t(-1) - size);
  return base + size - 1;
#else   // stack grows up
  MOZ_ASSERT(base >= size);
  return base - (size - 1);
#endif  // stack grows down
}

}  // namespace JS

#endif /* js_public_NativeStackLimit_h */
