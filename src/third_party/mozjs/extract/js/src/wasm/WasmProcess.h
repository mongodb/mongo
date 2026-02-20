/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
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

#ifndef wasm_process_h
#define wasm_process_h

#include "mozilla/Atomics.h"

#include "wasm/WasmMemory.h"

namespace js {
namespace wasm {

class Code;
class CodeRange;
class CodeBlock;
class TagType;

extern const TagType* sWrappedJSValueTagType;
static constexpr uint32_t WrappedJSValueTagType_ValueOffset = 0;

// These methods return the wasm::CodeBlock (resp. wasm::Code) containing
// the given pc, if any exist in the process. These methods do not take a lock,
// and thus are safe to use in a profiling context.

const CodeBlock* LookupCodeBlock(const void* pc,
                                 const CodeRange** codeRange = nullptr);

const Code* LookupCode(const void* pc, const CodeRange** codeRange = nullptr);

// Return whether the given PC is in any type of wasm code (module or builtin).

bool InCompiledCode(void* pc);

// A bool member that can be used as a very fast lookup to know if there is any
// code segment at all.

extern mozilla::Atomic<bool> CodeExists;

// These methods allow to (un)register CodeBlocks so they can be looked up
// via pc in the methods described above.

bool RegisterCodeBlock(const CodeBlock* cs);

void UnregisterCodeBlock(const CodeBlock* cs);

// Whether this process is configured to use huge memory or not.  Note that this
// is not precise enough to tell whether a particular memory uses huge memory,
// there are additional conditions for that.

bool IsHugeMemoryEnabled(AddressType t);

// Called once before/after the last VM execution which could execute or compile
// wasm.

bool Init();

void ShutDown();

}  // namespace wasm
}  // namespace js

#endif  // wasm_process_h
