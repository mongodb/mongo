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

#ifndef wasm_log_h
#define wasm_log_h

#include "js/TypeDecls.h"

namespace js {
namespace wasm {

// Verbose logging support.

extern void Log(JSContext* cx, const char* fmt, ...) MOZ_FORMAT_PRINTF(2, 3);
extern void LogOffThread(const char* fmt, ...) MOZ_FORMAT_PRINTF(1, 2);

// Codegen debug support.

enum class DebugChannel {
  Function,
  Import,
};

#ifdef WASM_CODEGEN_DEBUG
bool IsCodegenDebugEnabled(DebugChannel channel);
#endif

void DebugCodegen(DebugChannel channel, const char* fmt, ...)
    MOZ_FORMAT_PRINTF(2, 3);

using PrintCallback = void (*)(const char*);

}  // namespace wasm
}  // namespace js

#endif  // wasm_log_h
