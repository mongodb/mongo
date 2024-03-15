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

#ifndef wasm_codegen_constants_h
#define wasm_codegen_constants_h

#include <stdint.h>

namespace js {
namespace wasm {

static const unsigned MaxArgsForJitInlineCall = 8;
static const unsigned MaxResultsForJitEntry = 1;
static const unsigned MaxResultsForJitExit = 1;
static const unsigned MaxResultsForJitInlineCall = MaxResultsForJitEntry;
// The maximum number of results of a function call or block that may be
// returned in registers.
static const unsigned MaxRegisterResults = 1;

// A magic value of the InstanceReg to indicate after a return to the entry
// stub that an exception has been caught and that we should throw.

static const unsigned FailInstanceReg = 0xbad;

// The following thresholds were derived from a microbenchmark. If we begin to
// ship this optimization for more platforms, we will need to extend this list.

#if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_ARM64)
static const uint32_t MaxInlineMemoryCopyLength = 64;
static const uint32_t MaxInlineMemoryFillLength = 64;
#elif defined(JS_CODEGEN_X86)
static const uint32_t MaxInlineMemoryCopyLength = 32;
static const uint32_t MaxInlineMemoryFillLength = 32;
#else
static const uint32_t MaxInlineMemoryCopyLength = 0;
static const uint32_t MaxInlineMemoryFillLength = 0;
#endif

// The size we round all super type vectors to. All accesses below this length
// can avoid bounds checks. The value of 8 was chosen after a bit of profiling
// with the Dart Barista benchmark.
//
// Keep jit-tests/tests/wasm/gc/casting.js in sync with this constant.
static const uint32_t MinSuperTypeVectorLength = 8;

// An exported wasm function may have a 'jit entry' stub attached that can be
// called using the JS JIT ABI. This relies on the pointer we store in the
// `NativeJitInfoOrInterpretedScriptSlot` slot of JSFunction to have a
// compatible representation with BaseScript/SelfHostedLazyScript so that
// `masm.loadJitCodeRaw` works.
//
// We store jit entry pointers in an array (see wasm::JumpTable) and store the
// pointer to a function's jit entry in the JSFunction slot. We rely on the
// below offset of each entry in the jump table to be compatible with
// BaseScript/SelfHostedLazyScript.
static const uint32_t JumpTableJitEntryOffset = 0;

}  // namespace wasm
}  // namespace js

#endif  // wasm_codegen_constants_h
