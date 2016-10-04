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

#ifndef asmjs_AsmJSValidate_h
#define asmjs_AsmJSValidate_h

#include "mozilla/MathAlgorithms.h"

#include <stddef.h>

#include "jsutil.h"

#include "jit/Registers.h"
#include "js/TypeDecls.h"
#include "vm/NativeObject.h"

namespace js {

class ExclusiveContext;
namespace frontend {
    template <typename ParseHandler> class Parser;
    template <typename ParseHandler> struct ParseContext;
    class FullParseHandler;
    class ParseNode;
} // namespace frontend

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

// The minimum heap length for asm.js.
const size_t AsmJSMinHeapLength = 64 * 1024;

// The assumed page size; dynamically checked in ValidateAsmJS.
#ifdef _MIPS_ARCH_LOONGSON3A
const size_t AsmJSPageSize = 16384;
#else
const size_t AsmJSPageSize = 4096;
#endif

static_assert(AsmJSMinHeapLength % AsmJSPageSize == 0, "Invalid page size");

#if defined(ASMJS_MAY_USE_SIGNAL_HANDLERS_FOR_OOB)

// Targets define AsmJSImmediateRange to be the size of an address immediate,
// and AsmJSCheckedImmediateRange, to be the size of an address immediate that
// can be supported by signal-handler OOB handling.
static_assert(jit::AsmJSCheckedImmediateRange <= jit::AsmJSImmediateRange,
              "AsmJSImmediateRange should be the size of an unconstrained "
              "address immediate");

// To support the use of signal handlers for catching Out Of Bounds accesses,
// the internal ArrayBuffer data array is inflated to 4GiB (only the
// byteLength portion of which is accessible) so that out-of-bounds accesses
// (made using a uint32 index) are guaranteed to raise a SIGSEGV.
// Then, an additional extent is added to permit folding of immediate
// values into addresses. And finally, unaligned accesses and mask optimizations
// might also try to access a few bytes after this limit, so just inflate it by
// AsmJSPageSize.
static const size_t AsmJSMappedSize = 4 * 1024ULL * 1024ULL * 1024ULL +
                                      jit::AsmJSImmediateRange +
                                      AsmJSPageSize;

#endif // defined(ASMJS_MAY_USE_SIGNAL_HANDLERS_FOR_OOB)

// From the asm.js spec Linking section:
//  the heap object's byteLength must be either
//    2^n for n in [12, 24)
//  or
//    2^24 * n for n >= 1.

inline uint32_t
RoundUpToNextValidAsmJSHeapLength(uint32_t length)
{
    if (length <= AsmJSMinHeapLength)
        return AsmJSMinHeapLength;

    if (length <= 16 * 1024 * 1024)
        return mozilla::RoundUpPow2(length);

    MOZ_ASSERT(length <= 0xff000000);
    return (length + 0x00ffffff) & ~0x00ffffff;
}

inline bool
IsValidAsmJSHeapLength(uint32_t length)
{
    bool valid = length >= AsmJSMinHeapLength &&
                 (IsPowerOfTwo(length) ||
                  (length & 0x00ffffff) == 0);

    MOZ_ASSERT_IF(valid, length % AsmJSPageSize == 0);
    MOZ_ASSERT_IF(valid, length == RoundUpToNextValidAsmJSHeapLength(length));

    return valid;
}

// Return whether asm.js optimization is inhibited by the platform or
// dynamically disabled:
extern bool
IsAsmJSCompilationAvailable(JSContext* cx, unsigned argc, JS::Value* vp);

} // namespace js

#endif // asmjs_AsmJSValidate_h
