/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
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

// Return whether BaselineCompileFunction can generate code on the current
// device.  Usually you do *not* want to call this, you want
// BaselineAvailable().
[[nodiscard]] bool BaselinePlatformSupport();

// Generate adequate code quickly.
[[nodiscard]] bool BaselineCompileFunctions(
    const ModuleEnvironment& moduleEnv, const CompilerEnvironment& compilerEnv,
    LifoAlloc& lifo, const FuncCompileInputVector& inputs, CompiledCode* code,
    UniqueChars* error);

class BaseLocalIter {
 private:
  using ConstValTypeRange = mozilla::Range<const ValType>;

  const ValTypeVector& locals_;
  const ArgTypeVector& args_;
  jit::WasmABIArgIter<ArgTypeVector> argsIter_;
  size_t index_;
  int32_t frameSize_;
  int32_t nextFrameSize_;
  int32_t frameOffset_;
  int32_t stackResultPointerOffset_;
  jit::MIRType mirType_;
  bool done_;

  void settle();
  int32_t pushLocal(size_t nbytes);

 public:
  BaseLocalIter(const ValTypeVector& locals, const ArgTypeVector& args,
                bool debugEnabled);
  void operator++(int);
  bool done() const { return done_; }

  jit::MIRType mirType() const {
    MOZ_ASSERT(!done_);
    return mirType_;
  }
  int32_t frameOffset() const {
    MOZ_ASSERT(!done_);
    MOZ_ASSERT(frameOffset_ != INT32_MAX);
    return frameOffset_;
  }
  size_t index() const {
    MOZ_ASSERT(!done_);
    return index_;
  }
  // The size in bytes taken up by the previous `index_` locals, also including
  // fixed allocations like the DebugFrame and "hidden" locals like a spilled
  // stack results pointer.
  int32_t frameSize() const { return frameSize_; }

  int32_t stackResultPointerOffset() const {
    MOZ_ASSERT(args_.hasSyntheticStackResultPointerArg());
    MOZ_ASSERT(stackResultPointerOffset_ != INT32_MAX);
    return stackResultPointerOffset_;
  }

#ifdef DEBUG
  bool isArg() const {
    MOZ_ASSERT(!done_);
    return !argsIter_.done();
  }
#endif
};

}  // namespace wasm
}  // namespace js

#endif  // asmjs_wasm_baseline_compile_h
