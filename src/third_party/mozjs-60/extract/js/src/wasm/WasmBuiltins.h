/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 *
 * Copyright 2017 Mozilla Foundation
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

#ifndef wasm_builtins_h
#define wasm_builtins_h

#include "wasm/WasmTypes.h"

namespace js {
namespace wasm {

class WasmFrameIter;

// A SymbolicAddress that NeedsBuiltinThunk() will call through a thunk to the
// C++ function. This will be true for all normal calls from normal wasm
// function code. Only calls to C++ from other exits/thunks do not need a thunk.

bool
NeedsBuiltinThunk(SymbolicAddress sym);

// This function queries whether pc is in one of the process's builtin thunks
// and, if so, returns the CodeRange and pointer to the code segment that the
// CodeRange is relative to.

bool
LookupBuiltinThunk(void* pc, const CodeRange** codeRange, uint8_t** codeBase);

// EnsureBuiltinThunksInitialized() must be called, and must succeed, before
// SymbolicAddressTarget() or MaybeGetBuiltinThunk(). This function creates all
// thunks for the process. ReleaseBuiltinThunks() should be called before
// ReleaseProcessExecutableMemory() so that the latter can assert that all
// executable code has been released.

bool
EnsureBuiltinThunksInitialized();

void*
HandleThrow(JSContext* cx, WasmFrameIter& iter);

void*
SymbolicAddressTarget(SymbolicAddress sym);

void*
MaybeGetBuiltinThunk(HandleFunction f, const Sig& sig);

void
ReleaseBuiltinThunks();

} // namespace wasm
} // namespace js

#endif // wasm_builtins_h
