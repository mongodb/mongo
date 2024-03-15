/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* JavaScript API for compiling scripts to stencil without depending on
 * JSContext. */

#ifndef js_experimental_CompileScript_h
#define js_experimental_CompileScript_h

#include "jspubtd.h"
#include "js/experimental/JSStencil.h"
#include "js/GCAnnotations.h"
#include "js/Modules.h"
#include "js/Stack.h"
#include "js/UniquePtr.h"

namespace js {
class FrontendContext;
namespace frontend {
struct CompilationInput;
}  // namespace frontend
}  // namespace js

namespace JS {
using FrontendContext = js::FrontendContext;

// Create a new front-end context.
JS_PUBLIC_API JS::FrontendContext* NewFrontendContext();

// Destroy a front-end context allocated with NewFrontendContext.
JS_PUBLIC_API void DestroyFrontendContext(JS::FrontendContext* fc);

JS_PUBLIC_API void SetNativeStackQuota(JS::FrontendContext* fc,
                                       JS::NativeStackSize stackSize);

/*
 * Set supported import assertions on a FrontendContext to be used with
 * CompileModuleScriptToStencil. May only be set once for each FrontendContext.
 * The default list of supported import assertions is empty.
 */
JS_PUBLIC_API bool SetSupportedImportAssertions(
    JS::FrontendContext* fc,
    const JS::ImportAssertionVector& supportedImportAssertions);

// Temporary storage used during compiling and preparing to instantiate a
// Stencil.
//
// Off-thread consumers can allocate this instance off main thread, and pass it
// back to the main thread, in order to reduce the main thread allocation.
struct CompilationStorage {
 private:
  // Owned CompilationInput.
  //
  // This uses raw pointer instead of UniquePtr because CompilationInput
  // is opaque.
  JS_HAZ_NON_GC_POINTER js::frontend::CompilationInput* input_ = nullptr;
  bool isBorrowed_ = false;

 public:
  CompilationStorage() = default;
  explicit CompilationStorage(js::frontend::CompilationInput* input)
      : input_(input), isBorrowed_(true) {}
  CompilationStorage(CompilationStorage&& other)
      : input_(other.input_), isBorrowed_(other.isBorrowed_) {
    other.input_ = nullptr;
  }

  ~CompilationStorage();

 private:
  CompilationStorage(const CompilationStorage& other) = delete;
  void operator=(const CompilationStorage& aOther) = delete;

 public:
  bool hasInput() { return !!input_; }

  // Internal function that initializes the CompilationInput. It should only be
  // called once.
  bool allocateInput(FrontendContext* fc,
                     const JS::ReadOnlyCompileOptions& options);

  js::frontend::CompilationInput& getInput() {
    MOZ_ASSERT(hasInput());
    return *input_;
  }

  // Size of dynamic data. Note that GC data is counted by GC and not here.
  size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const;

  void trace(JSTracer* trc);
};

extern JS_PUBLIC_API already_AddRefed<JS::Stencil> CompileGlobalScriptToStencil(
    JS::FrontendContext* fc, const JS::ReadOnlyCompileOptions& options,
    JS::SourceText<mozilla::Utf8Unit>& srcBuf,
    JS::CompilationStorage& compileStorage);

extern JS_PUBLIC_API already_AddRefed<JS::Stencil> CompileGlobalScriptToStencil(
    JS::FrontendContext* fc, const JS::ReadOnlyCompileOptions& options,
    JS::SourceText<char16_t>& srcBuf, JS::CompilationStorage& compileStorage);

extern JS_PUBLIC_API already_AddRefed<JS::Stencil> CompileModuleScriptToStencil(
    JS::FrontendContext* fc, const JS::ReadOnlyCompileOptions& options,
    JS::SourceText<mozilla::Utf8Unit>& srcBuf,
    JS::CompilationStorage& compileStorage);

extern JS_PUBLIC_API already_AddRefed<JS::Stencil> CompileModuleScriptToStencil(
    JS::FrontendContext* fc, const JS::ReadOnlyCompileOptions& options,
    JS::SourceText<char16_t>& srcBuf, JS::CompilationStorage& compileStorage);

extern JS_PUBLIC_API bool PrepareForInstantiate(
    JS::FrontendContext* fc, JS::CompilationStorage& compileStorage,
    JS::Stencil& stencil, JS::InstantiationStorage& storage);

}  // namespace JS

#endif  // js_experimental_CompileScript_h
