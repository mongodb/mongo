/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 *
 * Copyright 2014 Mozilla Foundation
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

#ifndef jit_AsmJS_h
#define jit_AsmJS_h

#include "mozilla/MathAlgorithms.h"

#include <stddef.h>

#include "jsutil.h"

#include "js/TypeDecls.h"
#include "vm/NativeObject.h"

namespace js {

class ExclusiveContext;
namespace frontend {
    template <typename ParseHandler> class Parser;
    template <typename ParseHandler> struct ParseContext;
    class FullParseHandler;
    class ParseNode;
}

typedef frontend::Parser<frontend::FullParseHandler> AsmJSParser;
typedef frontend::ParseContext<frontend::FullParseHandler> AsmJSParseContext;

// Takes over parsing of a function starting with "use asm". The return value
// indicates whether an error was reported which the caller should propagate.
// If no error was reported, the function may still fail to validate as asm.js.
// In this case, the parser.tokenStream has been advanced an indeterminate
// amount and the entire function should be reparsed from the beginning.
extern bool
ValidateAsmJS(ExclusiveContext* cx, AsmJSParser& parser, frontend::ParseNode* stmtList,
             bool* validated);

// The assumed page size; dynamically checked in ValidateAsmJS.
const size_t AsmJSPageSize = 4096;

#ifdef JS_CPU_X64
// On x64, the internal ArrayBuffer data array is inflated to 4GiB (only the
// byteLength portion of which is accessible) so that out-of-bounds accesses
// (made using a uint32 index) are guaranteed to raise a SIGSEGV.
// Unaligned accesses and mask optimizations might also try to access a few
// bytes after this limit, so just inflate it by AsmJSPageSize.
static const size_t AsmJSMappedSize = 4 * 1024ULL * 1024ULL * 1024ULL + AsmJSPageSize;
#endif

// From the asm.js spec Linking section:
//  the heap object's byteLength must be either
//    2^n for n in [12, 24)
//  or
//    2^24 * n for n >= 1.

inline uint32_t
RoundUpToNextValidAsmJSHeapLength(uint32_t length)
{
    if (length <= 4 * 1024)
        return 4 * 1024;

    if (length <= 16 * 1024 * 1024)
        return mozilla::RoundUpPow2(length);

    MOZ_ASSERT(length <= 0xff000000);
    return (length + 0x00ffffff) & ~0x00ffffff;
}

inline bool
IsValidAsmJSHeapLength(uint32_t length)
{
    bool valid = length >= 4 * 1024 &&
                 (IsPowerOfTwo(length) ||
                  (length & 0x00ffffff) == 0);

    MOZ_ASSERT_IF(valid, length % AsmJSPageSize == 0);
    MOZ_ASSERT_IF(valid, length == RoundUpToNextValidAsmJSHeapLength(length));

    return valid;
}

// For now, power-of-2 lengths in this range are accepted, but in the future
// we'll change this to cause link-time failure.
inline bool
IsDeprecatedAsmJSHeapLength(uint32_t length)
{
    return length >= 4 * 1024 && length < 64 * 1024 && IsPowerOfTwo(length);
}

// Return whether asm.js optimization is inhibited by the platform or
// dynamically disabled:
extern bool
IsAsmJSCompilationAvailable(JSContext* cx, unsigned argc, JS::Value* vp);

} // namespace js

#endif // jit_AsmJS_h
