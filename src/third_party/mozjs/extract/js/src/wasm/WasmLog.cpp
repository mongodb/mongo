/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2021 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "wasm/WasmLog.h"

#include <stdio.h>

#include "jit/JitOptions.h"
#include "js/Printf.h"
#include "js/Utility.h"
#include "vm/JSContext.h"
#include "vm/Warnings.h"

using namespace js;
using namespace js::wasm;

void wasm::Log(JSContext* cx, const char* fmt, ...) {
  MOZ_ASSERT(!cx->isExceptionPending());

  if (!cx->options().wasmVerbose()) {
    return;
  }

  va_list args;
  va_start(args, fmt);

  if (UniqueChars chars = JS_vsmprintf(fmt, args)) {
    WarnNumberASCII(cx, JSMSG_WASM_VERBOSE, chars.get());
    if (cx->isExceptionPending()) {
      cx->clearPendingException();
    }
  }

  va_end(args);
}

void wasm::LogOffThread(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
}

#ifdef WASM_CODEGEN_DEBUG
bool wasm::IsCodegenDebugEnabled(DebugChannel channel) {
  switch (channel) {
    case DebugChannel::Function:
      return jit::JitOptions.enableWasmFuncCallSpew;
    case DebugChannel::Import:
      return jit::JitOptions.enableWasmImportCallSpew;
  }
  return false;
}
#endif

void wasm::DebugCodegen(DebugChannel channel, const char* fmt, ...) {
#ifdef WASM_CODEGEN_DEBUG
  if (!IsCodegenDebugEnabled(channel)) {
    return;
  }
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
#endif
}
