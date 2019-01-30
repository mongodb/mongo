/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 *
 * Copyright 2015 Mozilla Foundation
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

#ifndef wasm_compile_h
#define wasm_compile_h

#include "wasm/WasmModule.h"

namespace js {
namespace wasm {

// Describes the JS scripted caller of a request to compile a wasm module.

struct ScriptedCaller
{
    UniqueChars filename;
    unsigned line;
    unsigned column;
};

struct ResponseURLs
{
    UniqueChars baseURL;
    UniqueChars sourceMapURL;
};

// Describes all the parameters that control wasm compilation.

struct CompileArgs : ShareableBase<CompileArgs>
{
    Assumptions assumptions;
    ScriptedCaller scriptedCaller;
    ResponseURLs responseURLs;
    bool baselineEnabled;
    bool debugEnabled;
    bool ionEnabled;
    bool sharedMemoryEnabled;
    bool testTiering;

    CompileArgs(Assumptions&& assumptions, ScriptedCaller&& scriptedCaller)
      : assumptions(Move(assumptions)),
        scriptedCaller(Move(scriptedCaller)),
        baselineEnabled(false),
        debugEnabled(false),
        ionEnabled(false),
        sharedMemoryEnabled(false),
        testTiering(false)
    {}

    // If CompileArgs is constructed without arguments, initFromContext() must
    // be called to complete initialization.
    CompileArgs() = default;
    bool initFromContext(JSContext* cx, ScriptedCaller&& scriptedCaller);
};

typedef RefPtr<CompileArgs> MutableCompileArgs;
typedef RefPtr<const CompileArgs> SharedCompileArgs;

// Return the estimated compiled (machine) code size for the given bytecode size
// compiled at the given tier.

double
EstimateCompiledCodeSize(Tier tier, size_t bytecodeSize);

// Compile the given WebAssembly bytecode with the given arguments into a
// wasm::Module. On success, the Module is returned. On failure, the returned
// SharedModule pointer is null and either:
//  - *error points to a string description of the error
//  - *error is null and the caller should report out-of-memory.

SharedModule
CompileBuffer(const CompileArgs& args, const ShareableBytes& bytecode, UniqueChars* error);

// Attempt to compile the second tier of the given wasm::Module, returning whether
// tier-2 compilation succeeded and Module::finishTier2 was called.

bool
CompileTier2(const CompileArgs& args, Module& module, Atomic<bool>* cancelled);

// Compile the given WebAssembly module which has been broken into three
// partitions:
//  - envBytes contains a complete ModuleEnvironment that has already been
//    copied in from the stream.
//  - codeBytes is pre-sized to hold the complete code section when the stream
//    completes.
//  - The range [codeBytes.begin(), codeStreamEnd) contains the bytes currently
//    read from the stream and codeStreamEnd will advance until either
//    the stream is cancelled or codeStreamEnd == codeBytes.end().
//  - tailBytesPtr is null until the module has finished streaming at which
//    point tailBytesPtr will point to the complete tail bytes.
// The ExclusiveWaitableData are notified when CompileStreaming() can make
// progress (i.e., codeStreamEnd advances or tailBytes is set to non-null).
// If cancelled is set to true, compilation aborts and returns null. After
// cancellation is set, both ExclusiveWaitableData will be notified and so every
// wait() loop must check cancelled.

typedef ExclusiveWaitableData<const uint8_t*> ExclusiveStreamEnd;
typedef ExclusiveWaitableData<const Bytes*> ExclusiveTailBytesPtr;

SharedModule
CompileStreaming(const CompileArgs& args,
                 const Bytes& envBytes,
                 const Bytes& codeBytes,
                 const ExclusiveStreamEnd& codeStreamEnd,
                 const ExclusiveTailBytesPtr& tailBytesPtr,
                 const Atomic<bool>& cancelled,
                 UniqueChars* error);

}  // namespace wasm
}  // namespace js

#endif // namespace wasm_compile_h
