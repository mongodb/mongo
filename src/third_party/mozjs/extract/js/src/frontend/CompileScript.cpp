/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/experimental/CompileScript.h"

#include "frontend/BytecodeCompiler.h"  // frontend::{CompileGlobalScriptToStencil, ParseModuleToStencil}
#include "frontend/CompilationStencil.h"  // frontend::{CompilationStencil,CompilationInput}
#include "frontend/FrontendContext.h"    // frontend::FrontendContext
#include "frontend/ScopeBindingCache.h"  // frontend::NoScopeBindingCache
#include "js/friend/StackLimits.h"       // js::StackLimitMargin
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

JS_PUBLIC_API JS::NativeStackSize JS::ThreadStackQuotaForSize(
    size_t stackSize) {
  // Set the stack quota to 10% less that the actual size.
  static constexpr double RatioWithoutMargin = 0.9;

  MOZ_ASSERT(double(stackSize) * (1 - RatioWithoutMargin) >
             js::MinimumStackLimitMargin);

  return JS::NativeStackSize(double(stackSize) * RatioWithoutMargin);
}

JS_PUBLIC_API bool JS::HadFrontendErrors(JS::FrontendContext* fc) {
  return fc->hadErrors();
}

JS_PUBLIC_API bool JS::ConvertFrontendErrorsToRuntimeErrors(
    JSContext* cx, JS::FrontendContext* fc,
    const JS::ReadOnlyCompileOptions& options) {
  return fc->convertToRuntimeError(cx);
}

JS_PUBLIC_API const JSErrorReport* JS::GetFrontendErrorReport(
    JS::FrontendContext* fc, const JS::ReadOnlyCompileOptions& options) {
  if (!fc->maybeError().isSome()) {
    return nullptr;
  }
  return fc->maybeError().ptr();
}

JS_PUBLIC_API bool JS::HadFrontendOverRecursed(JS::FrontendContext* fc) {
  return fc->hadOverRecursed();
}

JS_PUBLIC_API bool JS::HadFrontendOutOfMemory(JS::FrontendContext* fc) {
  return fc->hadOutOfMemory();
}

JS_PUBLIC_API bool JS::HadFrontendAllocationOverflow(JS::FrontendContext* fc) {
  return fc->hadAllocationOverflow();
}

JS_PUBLIC_API void JS::ClearFrontendErrors(JS::FrontendContext* fc) {
  fc->clearErrors();
}

JS_PUBLIC_API size_t JS::GetFrontendWarningCount(JS::FrontendContext* fc) {
  return fc->warnings().length();
}

JS_PUBLIC_API const JSErrorReport* JS::GetFrontendWarningAt(
    JS::FrontendContext* fc, size_t index,
    const JS::ReadOnlyCompileOptions& options) {
  return &fc->warnings()[index];
}

template <typename CharT>
static already_AddRefed<JS::Stencil> CompileGlobalScriptToStencilImpl(
    JS::FrontendContext* fc, const JS::ReadOnlyCompileOptions& options,
    JS::SourceText<CharT>& srcBuf) {
  ScopeKind scopeKind =
      options.nonSyntacticScope ? ScopeKind::NonSyntactic : ScopeKind::Global;

  JS::SourceText<CharT> data(std::move(srcBuf));

  frontend::CompilationInput compilationInput(options);

  frontend::NoScopeBindingCache scopeCache;
  LifoAlloc tempLifoAlloc(JSContext::TEMP_LIFO_ALLOC_PRIMARY_CHUNK_SIZE);
  RefPtr<JS::Stencil> stencil_ = frontend::CompileGlobalScriptToStencil(
      nullptr, fc, tempLifoAlloc, compilationInput, &scopeCache, data,
      scopeKind);
  // CompilationInput initialized with CompileGlobalScriptToStencil only
  // references information from the JS::Stencil context and the
  // ref-counted ScriptSource, which are both GC-free.
  JS_HAZ_VALUE_IS_GC_SAFE(compilationInput);
  return stencil_.forget();
}

template <typename CharT>
static already_AddRefed<JS::Stencil> CompileModuleScriptToStencilImpl(
    JS::FrontendContext* fc, const JS::ReadOnlyCompileOptions& optionsInput,
    JS::SourceText<CharT>& srcBuf) {
  JS::CompileOptions options(nullptr, optionsInput);
  options.setModule();

  frontend::CompilationInput compilationInput(options);

  NoScopeBindingCache scopeCache;
  js::LifoAlloc tempLifoAlloc(JSContext::TEMP_LIFO_ALLOC_PRIMARY_CHUNK_SIZE);
  RefPtr<JS::Stencil> stencil = ParseModuleToStencil(
      nullptr, fc, tempLifoAlloc, compilationInput, &scopeCache, srcBuf);
  // CompilationInput initialized with ParseModuleToStencil only
  // references information from the JS::Stencil context and the
  // ref-counted ScriptSource, which are both GC-free.
  JS_HAZ_VALUE_IS_GC_SAFE(compilationInput);
  if (!stencil) {
    return nullptr;
  }

  // Convert the UniquePtr to a RefPtr and increment the count (to 1).
  return stencil.forget();
}

already_AddRefed<JS::Stencil> JS::CompileGlobalScriptToStencil(
    JS::FrontendContext* fc, const JS::ReadOnlyCompileOptions& options,
    JS::SourceText<mozilla::Utf8Unit>& srcBuf) {
#ifdef DEBUG
  fc->assertNativeStackLimitThread();
#endif
  return CompileGlobalScriptToStencilImpl(fc, options, srcBuf);
}

already_AddRefed<JS::Stencil> JS::CompileGlobalScriptToStencil(
    JS::FrontendContext* fc, const JS::ReadOnlyCompileOptions& options,
    JS::SourceText<char16_t>& srcBuf) {
#ifdef DEBUG
  fc->assertNativeStackLimitThread();
#endif
  return CompileGlobalScriptToStencilImpl(fc, options, srcBuf);
}

already_AddRefed<JS::Stencil> JS::CompileModuleScriptToStencil(
    JS::FrontendContext* fc, const JS::ReadOnlyCompileOptions& optionsInput,
    JS::SourceText<mozilla::Utf8Unit>& srcBuf) {
#ifdef DEBUG
  fc->assertNativeStackLimitThread();
#endif
  return CompileModuleScriptToStencilImpl(fc, optionsInput, srcBuf);
}

already_AddRefed<JS::Stencil> JS::CompileModuleScriptToStencil(
    JS::FrontendContext* fc, const JS::ReadOnlyCompileOptions& optionsInput,
    JS::SourceText<char16_t>& srcBuf) {
#ifdef DEBUG
  fc->assertNativeStackLimitThread();
#endif
  return CompileModuleScriptToStencilImpl(fc, optionsInput, srcBuf);
}

bool JS::PrepareForInstantiate(JS::FrontendContext* fc, JS::Stencil& stencil,
                               JS::InstantiationStorage& storage) {
  if (!storage.gcOutput_) {
    storage.gcOutput_ =
        fc->getAllocator()
            ->new_<js::frontend::PreallocatedCompilationGCOutput>();
    if (!storage.gcOutput_) {
      return false;
    }
  }
  return CompilationStencil::prepareForInstantiate(fc, stencil,
                                                   *storage.gcOutput_);
}
