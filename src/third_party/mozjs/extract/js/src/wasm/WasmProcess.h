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

#ifndef wasm_process_h
#define wasm_process_h

#include "mozilla/Atomics.h"

namespace js {
namespace wasm {

class Code;
class CodeRange;
class CodeSegment;

// These methods return the wasm::CodeSegment (resp. wasm::Code) containing
// the given pc, if any exist in the process. These methods do not take a lock,
// and thus are safe to use in a profiling or async interrupt context.

const CodeSegment*
LookupCodeSegment(const void* pc, const CodeRange** codeRange = nullptr);

const Code*
LookupCode(const void* pc, const CodeRange** codeRange = nullptr);

// A bool member that can be used as a very fast lookup to know if there is any
// code segment at all.

extern mozilla::Atomic<bool> CodeExists;

// These methods allow to (un)register CodeSegments so they can be looked up
// via pc in the methods described above.

bool
RegisterCodeSegment(const CodeSegment* cs);

void
UnregisterCodeSegment(const CodeSegment* cs);

void
ShutDownProcessStaticData();

} // namespace wasm
} // namespace js

#endif // wasm_process_h
