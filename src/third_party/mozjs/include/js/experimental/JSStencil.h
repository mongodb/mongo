/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_experimental_JSStencil_h
#define js_experimental_JSStencil_h

/* The `JS::Stencil` type holds the output of the JS Parser before it is
 * allocated on the GC heap as a `JSScript`. This form may be serialized as
 * part of building a bytecode cache. This `Stencil` is not associated with any
 * particular Realm and may be generated off-main-thread, making it useful for
 * building script loaders.
 */

#include "mozilla/RefPtr.h"  // RefPtr, already_AddRefed
#include "mozilla/Utf8.h"    // mozilla::Utf8Unit

#include <stddef.h>  // size_t

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/CompileOptions.h"              // JS::ReadOnlyCompileOptions
#include "js/OffThreadScriptCompilation.h"  // JS::OffThreadCompileCallback
#include "js/SourceText.h"                  // JS::SourceText
#include "js/Transcoding.h"

struct JS_PUBLIC_API JSContext;

// Underlying opaque type.
namespace js::frontend {
struct CompilationStencil;
};

namespace JS {

class OffThreadToken;

using Stencil = js::frontend::CompilationStencil;

// These non-member functions let us manipulate the ref counts of the opaque
// Stencil type. The RefPtrTraits below calls these for use when using the
// RefPtr type.
JS_PUBLIC_API void StencilAddRef(Stencil* stencil);
JS_PUBLIC_API void StencilRelease(Stencil* stencil);

// Compile the source text into a JS::Stencil using the provided options. The
// resulting stencil may be instantiated into any Realm on the current runtime
// and may be used multiple times.
//
// NOTE: On error, a null will be returned and an exception will be set on the
//       JSContext.
extern JS_PUBLIC_API already_AddRefed<Stencil> CompileGlobalScriptToStencil(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    SourceText<mozilla::Utf8Unit>& srcBuf);
extern JS_PUBLIC_API already_AddRefed<Stencil> CompileGlobalScriptToStencil(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    SourceText<char16_t>& srcBuf);

// Compile the source text into a JS::Stencil using "module" parse goal. The
// ECMAScript spec defines special semantics so we use a seperate entry point
// here for clarity. The result is still a JS::Stencil, but should use the
// appropriate instantiate API below.
extern JS_PUBLIC_API already_AddRefed<Stencil> CompileModuleScriptToStencil(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    SourceText<mozilla::Utf8Unit>& srcBuf);
extern JS_PUBLIC_API already_AddRefed<Stencil> CompileModuleScriptToStencil(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    SourceText<char16_t>& srcBuf);

// Off-thread compilation uses the normal off-thread APIs but uses a special
// finish method to avoid automatic instantiation. This is used for both global
// and modules compiles.
//
// NOTE: CompileOptions::useOffThreadParseGlobal must be false.
extern JS_PUBLIC_API already_AddRefed<Stencil> FinishOffThreadStencil(
    JSContext* cx, JS::OffThreadToken* token);

// Instantiate the Stencil into current Realm and return the JSScript.
extern JS_PUBLIC_API JSScript* InstantiateGlobalStencil(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    RefPtr<Stencil> stencil);

// Instantiate a module Stencil and return the associated object. Inside the
// engine this is a js::ModuleObject.
extern JS_PUBLIC_API JSObject* InstantiateModuleStencil(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    RefPtr<Stencil> stencil);

// Serialize the Stencil into the transcode buffer.
extern JS_PUBLIC_API TranscodeResult
EncodeStencil(JSContext* cx, const JS::ReadOnlyCompileOptions& options,
              RefPtr<Stencil> stencil, TranscodeBuffer& buffer);

// Deserialize data and create a new Stencil.
extern JS_PUBLIC_API TranscodeResult
DecodeStencil(JSContext* cx, const ReadOnlyCompileOptions& options,
              const TranscodeRange& range, RefPtr<Stencil>& stencilOut);

extern JS_PUBLIC_API OffThreadToken* CompileToStencilOffThread(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    SourceText<char16_t>& srcBuf, OffThreadCompileCallback callback,
    void* callbackData);

extern JS_PUBLIC_API OffThreadToken* CompileToStencilOffThread(
    JSContext* cx, const ReadOnlyCompileOptions& options,
    SourceText<mozilla::Utf8Unit>& srcBuf, OffThreadCompileCallback callback,
    void* callbackData);

extern JS_PUBLIC_API RefPtr<Stencil> FinishOffThreadCompileToStencil(
    JSContext* cx, OffThreadToken* token);

}  // namespace JS

namespace mozilla {
template <>
struct RefPtrTraits<JS::Stencil> {
  static void AddRef(JS::Stencil* stencil) { JS::StencilAddRef(stencil); }
  static void Release(JS::Stencil* stencil) { JS::StencilRelease(stencil); }
};
}  // namespace mozilla

#endif  // js_experimental_JSStencil_h
