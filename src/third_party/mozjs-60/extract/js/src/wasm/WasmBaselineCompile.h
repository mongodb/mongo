/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 *
 * Copyright 2016 Mozilla Foundation
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

#ifndef asmjs_wasm_baseline_compile_h
#define asmjs_wasm_baseline_compile_h

#include "wasm/WasmGenerator.h"

namespace js {
namespace wasm {

// Return whether BaselineCompileFunction can generate code on the current device.
// Note: asm.js is also currently not supported due to Atomics and SIMD.
bool
BaselineCanCompile();

// Generate adequate code quickly.
MOZ_MUST_USE bool
BaselineCompileFunctions(const ModuleEnvironment& env, LifoAlloc& lifo,
                         const FuncCompileInputVector& inputs, CompiledCode* code,
                         UniqueChars* error);

class BaseLocalIter
{
  private:
    using ConstValTypeRange = mozilla::Range<const ValType>;

    const ValTypeVector&               locals_;
    size_t                             argsLength_;
    ConstValTypeRange                  argsRange_; // range struct cache for ABIArgIter
    jit::ABIArgIter<ConstValTypeRange> argsIter_;
    size_t                             index_;
    int32_t                            localSize_;
    int32_t                            reservedSize_;
    int32_t                            frameOffset_;
    jit::MIRType                       mirType_;
    bool                               done_;

    void settle();
    int32_t pushLocal(size_t nbytes);

  public:
    BaseLocalIter(const ValTypeVector& locals, size_t argsLength, bool debugEnabled);
    void operator++(int);
    bool done() const { return done_; }

    jit::MIRType mirType() const { MOZ_ASSERT(!done_); return mirType_; }
    int32_t frameOffset() const { MOZ_ASSERT(!done_); return frameOffset_; }
    size_t index() const { MOZ_ASSERT(!done_); return index_; }
    int32_t currentLocalSize() const { return localSize_; }
    int32_t reservedSize() const { return reservedSize_; }

#ifdef DEBUG
    bool isArg() const { MOZ_ASSERT(!done_); return !argsIter_.done(); }
#endif
};

} // namespace wasm
} // namespace js

#endif // asmjs_wasm_baseline_compile_h
