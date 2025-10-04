/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_TrampolineNatives_h
#define jit_TrampolineNatives_h

#include <stdint.h>

#include "js/TypeDecls.h"

// [SMDOC] Trampoline Natives
//
// Trampoline natives are JS builtin functions that use the NATIVE_JIT_ENTRY
// mechanism. This means they have two implementations: the usual native C++
// implementation and a generated JIT trampoline that JIT callers can call
// directly using the JIT ABI calling convention. (This is very similar to how
// calls from JS to WebAssembly are optimized in the JITs.)
//
// The JIT trampoline lets us implement some natives in a more efficient way. In
// particular, it's much faster to call (other) JS functions with JIT code from
// a JIT trampoline than from C++ code.
//
// Trampoline frames use FrameType::TrampolineNative.

class JSJitInfo;

namespace JS {
class CallArgs;
}  // namespace JS

// List of all trampoline natives.
#define TRAMPOLINE_NATIVE_LIST(_) \
  _(ArraySort)                    \
  _(TypedArraySort)

namespace js {
namespace jit {

enum class TrampolineNative : uint16_t {
#define ADD_NATIVE(native) native,
  TRAMPOLINE_NATIVE_LIST(ADD_NATIVE)
#undef ADD_NATIVE
      Count
};

#define ADD_NATIVE(native) extern const JSJitInfo JitInfo_##native;
TRAMPOLINE_NATIVE_LIST(ADD_NATIVE)
#undef ADD_NATIVE

void SetTrampolineNativeJitEntry(JSContext* cx, JSFunction* fun,
                                 TrampolineNative native);

bool CallTrampolineNativeJitCode(JSContext* cx, TrampolineNative native,
                                 JS::CallArgs& args);

}  // namespace jit
}  // namespace js

#endif /* jit_TrampolineNatives_h */
