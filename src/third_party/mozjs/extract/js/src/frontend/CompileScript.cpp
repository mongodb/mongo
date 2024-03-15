/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/experimental/CompileScript.h"

#include "frontend/BytecodeCompilation.h"  // frontend::CompileGlobalScriptToStencil
#include "frontend/BytecodeCompiler.h"  // frontend::ParseModuleToStencil
#include "frontend/CompilationStencil.h"  // frontend::{CompilationStencil,CompilationInput}
#include "frontend/FrontendContext.h"    // frontend::FrontendContext
#include "frontend/ScopeBindingCache.h"  // frontend::NoScopeBindingCache
#include "js/SourceText.h"               // JS::SourceText

using namespace js;
using namespace js::frontend;

JS_PUBLIC_API FrontendContext* JS::NewFrontendContext() {
  MOZ_ASSERT(JS::detail::libraryInitState == JS::detail::InitState::Running,
             "must call JS_Init prior to creating any FrontendContexts");

  return js::NewFrontendContext();
}

JS_PUBLIC_API void JS::DestroyFrontendContext(FrontendContext* fc) {
  return js::DestroyFrontendContext(fc);
}

JS_PUBLIC_API void JS::SetNativeStackQuota(JS::FrontendContext* fc,
                                           JS::NativeStackSize stackSize) {
  fc->setStackQuota(stackSize);
}

JS_PUBLIC_API bool JS::SetSupportedImportAssertions(
    FrontendContext* fc,
    const JS::ImportAssertionVector& supportedImportAssertions) {
  return fc->setSupportedImportAssertions(supportedImportAssertions);
}

JS::CompilationStorage::~CompilationStorage() {
  if (input_ && !isBorrowed_) {
    js_delete(input_);
    input_ = nullptr;
  }
}

size_t JS::CompilationStorage::sizeOfIncludingThis(
    mozilla::MallocSizeOf mallocSizeOf) const {
  size_t sizeOfCompilationInput =
      input_ ? input_->sizeOfExcludingThis(mallocSizeOf) : 0;
  return mallocSizeOf(this) + sizeOfCompilationInput;
}

bool JS::CompilationStorage::allocateInput(
    FrontendContext* fc, const JS::ReadOnlyCompileOptions& options) {
  MOZ_ASSERT(!input_);
  input_ = fc->getAllocator()->new_<frontend::CompilationInput>(options);
  return !!input_;
}

void JS::CompilationStorage::trace(JSTracer* trc) {
  if (input_) {
    input_->trace(trc);
  }
}

template <typename CharT>
static already_AddRefed<JS::Stencil> CompileGlobalScriptToStencilImpl(
    JS::FrontendContext* fc, const JS::ReadOnlyCompileOptions& options,
    JS::SourceText<CharT>& srcBuf, JS::CompilationStorage& compilationStorage) {
  ScopeKind scopeKind =
      options.nonSyntacticScope ? ScopeKind::NonSyntactic : ScopeKind::Global;

  JS::SourceText<CharT> data(std::move(srcBuf));

  compilationStorage.allocateInput(fc, options);
  if (!compilationStorage.hasInput()) {
    return nullptr;
  }

  frontend::NoScopeBindingCache scopeCache;
  LifoAlloc tempLifoAlloc(JSContext::TEMP_LIFO_ALLOC_PRIMARY_CHUNK_SIZE);
  RefPtr<frontend::CompilationStencil> stencil_ =
      frontend::CompileGlobalScriptToStencil(nullptr, fc, tempLifoAlloc,
                                             compilationStorage.getInput(),
                                             &scopeCache, data, scopeKind);
  return stencil_.forget();
}

template <typename CharT>
static already_AddRefed<JS::Stencil> CompileModuleScriptToStencilImpl(
    JS::FrontendContext* fc, const JS::ReadOnlyCompileOptions& optionsInput,
    JS::SourceText<CharT>& srcBuf, JS::CompilationStorage& compilationStorage) {
  JS::CompileOptions options(nullptr, optionsInput);
  options.setModule();

  compilationStorage.allocateInput(fc, options);
  if (!compilationStorage.hasInput()) {
    return nullptr;
  }

  NoScopeBindingCache scopeCache;
  js::LifoAlloc tempLifoAlloc(JSContext::TEMP_LIFO_ALLOC_PRIMARY_CHUNK_SIZE);
  RefPtr<JS::Stencil> stencil =
      ParseModuleToStencil(nullptr, fc, tempLifoAlloc,
                           compilationStorage.getInput(), &scopeCache, srcBuf);
  if (!stencil) {
    return nullptr;
  }

  // Convert the UniquePtr to a RefPtr and increment the count (to 1).
  return stencil.forget();
}

already_AddRefed<JS::Stencil> JS::CompileGlobalScriptToStencil(
    JS::FrontendContext* fc, const JS::ReadOnlyCompileOptions& options,
    JS::SourceText<mozilla::Utf8Unit>& srcBuf,
    JS::CompilationStorage& compileStorage) {
  return CompileGlobalScriptToStencilImpl(fc, options, srcBuf, compileStorage);
}

already_AddRefed<JS::Stencil> JS::CompileGlobalScriptToStencil(
    JS::FrontendContext* fc, const JS::ReadOnlyCompileOptions& options,
    JS::SourceText<char16_t>& srcBuf, JS::CompilationStorage& compileStorage) {
  return CompileGlobalScriptToStencilImpl(fc, options, srcBuf, compileStorage);
}

already_AddRefed<JS::Stencil> JS::CompileModuleScriptToStencil(
    JS::FrontendContext* fc, const JS::ReadOnlyCompileOptions& optionsInput,
    JS::SourceText<mozilla::Utf8Unit>& srcBuf,
    JS::CompilationStorage& compileStorage) {
  return CompileModuleScriptToStencilImpl(fc, optionsInput, srcBuf,
                                          compileStorage);
}

already_AddRefed<JS::Stencil> JS::CompileModuleScriptToStencil(
    JS::FrontendContext* fc, const JS::ReadOnlyCompileOptions& optionsInput,
    JS::SourceText<char16_t>& srcBuf, JS::CompilationStorage& compileStorage) {
  return CompileModuleScriptToStencilImpl(fc, optionsInput, srcBuf,
                                          compileStorage);
}

#ifdef DEBUG
// We don't need to worry about GC if the CompilationInput has no GC pointers
static bool isGCSafe(js::frontend::CompilationInput& input) {
  bool isGlobalOrModule =
      input.target == CompilationInput::CompilationTarget::Global ||
      input.target == CompilationInput::CompilationTarget::Module;
  bool scopeHasNoGC =
      input.enclosingScope.isStencil() || input.enclosingScope.isNull();
  bool scriptHasNoGC =
      input.lazyOuterScript().isStencil() || input.lazyOuterScript().isNull();
  bool cacheHasNoGC = input.atomCache.empty();

  return isGlobalOrModule && scopeHasNoGC && scriptHasNoGC && cacheHasNoGC;
}
#endif  // DEBUG

bool JS::PrepareForInstantiate(JS::FrontendContext* fc,
                               JS::CompilationStorage& compileStorage,
                               JS::Stencil& stencil,
                               JS::InstantiationStorage& storage) {
  MOZ_ASSERT(compileStorage.hasInput());
  MOZ_ASSERT(isGCSafe(compileStorage.getInput()));
  if (!storage.gcOutput_) {
    storage.gcOutput_ =
        fc->getAllocator()->new_<js::frontend::CompilationGCOutput>();
    if (!storage.gcOutput_) {
      return false;
    }
  }
  return CompilationStencil::prepareForInstantiate(
      fc, compileStorage.getInput().atomCache, stencil, *storage.gcOutput_);
}
