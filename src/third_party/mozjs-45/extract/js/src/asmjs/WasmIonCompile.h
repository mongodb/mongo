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

#ifndef asmjs_wasm_ion_compile_h
#define asmjs_wasm_ion_compile_h

#include "asmjs/AsmJSFrameIterator.h"
#include "asmjs/WasmCompileArgs.h"
#include "asmjs/WasmIR.h"
#include "jit/MacroAssembler.h"

namespace js {
namespace wasm {

class FunctionCompileResults
{
    jit::TempAllocator alloc_;
    jit::MacroAssembler masm_;
    AsmJSFunctionOffsets offsets_;
    unsigned compileTime_;

    FunctionCompileResults(const FunctionCompileResults&) = delete;
    FunctionCompileResults& operator=(const FunctionCompileResults&) = delete;

  public:
    explicit FunctionCompileResults(LifoAlloc& lifo)
      : alloc_(&lifo),
        masm_(jit::MacroAssembler::AsmJSToken(), &alloc_),
        compileTime_(0)
    {}

    jit::TempAllocator& alloc() { return alloc_; }
    jit::MacroAssembler& masm() { return masm_; }

    AsmJSFunctionOffsets& offsets() { return offsets_; }
    const AsmJSFunctionOffsets& offsets() const { return offsets_; }

    void setCompileTime(unsigned t) { MOZ_ASSERT(!compileTime_); compileTime_ = t; }
    unsigned compileTime() const { return compileTime_; }
};

class CompileTask
{
    LifoAlloc lifo_;
    const CompileArgs args_;
    const FuncIR* func_;
    mozilla::Maybe<FunctionCompileResults> results_;

    CompileTask(const CompileTask&) = delete;
    CompileTask& operator=(const CompileTask&) = delete;

  public:
    CompileTask(size_t defaultChunkSize, CompileArgs args)
      : lifo_(defaultChunkSize),
        args_(args),
        func_(nullptr)
    {}
    LifoAlloc& lifo() {
        return lifo_;
    }
    CompileArgs args() const {
        return args_;
    }
    void init(const FuncIR& func) {
        func_ = &func;
        results_.emplace(lifo_);
    }
    const FuncIR& func() const {
        MOZ_ASSERT(func_);
        return *func_;
    }
    FunctionCompileResults& results() {
        return *results_;
    }
    void reset() {
        func_ = nullptr;
        results_.reset();
        lifo_.releaseAll();
    }
};

bool
CompileFunction(CompileTask* task);

} // namespace wasm
} // namespace js

#endif // asmjs_wasm_ion_compile_h
