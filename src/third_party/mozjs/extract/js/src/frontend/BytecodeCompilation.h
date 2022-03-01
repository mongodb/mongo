/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_BytecodeCompilation_h
#define frontend_BytecodeCompilation_h

#include "mozilla/Maybe.h"  // mozilla::Maybe, mozilla::Nothing
#include "mozilla/Utf8.h"   // mozilla::Utf8Unit

#include <stddef.h>  // size_t
#include <stdint.h>  // uint32_t

#include "jstypes.h"  // JS_PUBLIC_API

#include "frontend/CompilationStencil.h"  // CompilationStencil, ExtensibleCompilationStencil, CompilationGCOutput
#include "frontend/ParseContext.h"  // js::frontend::UsedNameTracker
#include "frontend/SharedContext.h"  // js::frontend::Directives, js::frontend::{,Eval,Global}SharedContext
#include "js/CompileOptions.h"  // JS::ReadOnlyCompileOptions
#include "js/RootingAPI.h"      // JS::{,Mutable}Handle, JS::Rooted
#include "js/SourceText.h"      // JS::SourceText
#include "js/UniquePtr.h"       // js::UniquePtr
#include "vm/JSScript.h"  // js::{FunctionAsync,Generator}Kind, js::BaseScript, JSScript, js::ScriptSource, js::ScriptSourceObject
#include "vm/Scope.h"     // js::ScopeKind

class JS_PUBLIC_API JSFunction;
class JS_PUBLIC_API JSObject;

class JSObject;

namespace js {

class Scope;

namespace frontend {

struct BytecodeEmitter;
class EitherParser;

template <typename Unit>
class SourceAwareCompiler;
template <typename Unit>
class ScriptCompiler;
template <typename Unit>
class ModuleCompiler;
template <typename Unit>
class StandaloneFunctionCompiler;

extern UniquePtr<CompilationStencil> CompileGlobalScriptToStencil(
    JSContext* cx, CompilationInput& input, JS::SourceText<char16_t>& srcBuf,
    ScopeKind scopeKind);

extern UniquePtr<CompilationStencil> CompileGlobalScriptToStencil(
    JSContext* cx, CompilationInput& input,
    JS::SourceText<mozilla::Utf8Unit>& srcBuf, ScopeKind scopeKind);

extern UniquePtr<ExtensibleCompilationStencil>
CompileGlobalScriptToExtensibleStencil(JSContext* cx, CompilationInput& input,
                                       JS::SourceText<char16_t>& srcBuf,
                                       ScopeKind scopeKind);

extern UniquePtr<ExtensibleCompilationStencil>
CompileGlobalScriptToExtensibleStencil(
    JSContext* cx, CompilationInput& input,
    JS::SourceText<mozilla::Utf8Unit>& srcBuf, ScopeKind scopeKind);

// Perform some operation to reduce the time taken by instantiation.
//
// Part of InstantiateStencils can be done by calling PrepareForInstantiate.
// PrepareForInstantiate is GC-free operation that can be performed
// off-main-thread without parse global.
[[nodiscard]] extern bool PrepareForInstantiate(
    JSContext* cx, CompilationInput& input, const CompilationStencil& stencil,
    CompilationGCOutput& gcOutput);

[[nodiscard]] extern bool InstantiateStencils(JSContext* cx,
                                              CompilationInput& input,
                                              const CompilationStencil& stencil,
                                              CompilationGCOutput& gcOutput);

extern JSScript* CompileGlobalScript(JSContext* cx,
                                     const JS::ReadOnlyCompileOptions& options,
                                     JS::SourceText<char16_t>& srcBuf,
                                     ScopeKind scopeKind);

extern JSScript* CompileGlobalScript(JSContext* cx,
                                     const JS::ReadOnlyCompileOptions& options,
                                     JS::SourceText<mozilla::Utf8Unit>& srcBuf,
                                     ScopeKind scopeKind);

extern JSScript* CompileEvalScript(JSContext* cx,
                                   const JS::ReadOnlyCompileOptions& options,
                                   JS::SourceText<char16_t>& srcBuf,
                                   JS::Handle<js::Scope*> enclosingScope,
                                   JS::Handle<JSObject*> enclosingEnv);

extern bool DelazifyCanonicalScriptedFunction(JSContext* cx,
                                              Handle<JSFunction*> fun);

// Certain compile options will disable the syntax parser entirely.
inline bool CanLazilyParse(const JS::ReadOnlyCompileOptions& options) {
  return !options.discardSource && !options.sourceIsLazy &&
         !options.forceFullParse();
}

}  // namespace frontend

}  // namespace js

#endif  // frontend_BytecodeCompilation_h
