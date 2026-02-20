/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_wasm_WasmFeatures_h
#define js_wasm_WasmFeatures_h

#include "js/WasmFeatures.h"
#include "js/TypeDecls.h"

namespace js {

class JSStringBuilder;

namespace wasm {

// Return whether WebAssembly can in principle be compiled on this platform (ie
// combination of hardware and OS), assuming at least one of the compilers that
// supports the platform is not disabled by other settings.
//
// This predicate must be checked and must be true to call any of the top-level
// wasm eval/compile methods.

bool HasPlatformSupport();

// Return whether WebAssembly is supported on this platform. This determines
// whether the WebAssembly object is exposed to JS in this context / realm and
//
// It does *not* guarantee that a compiler is actually available; that has to be
// checked separately, as it is sometimes run-time variant, depending on whether
// a debugger has been created or not.

bool HasSupport(JSContext* cx);

// Predicates for compiler availability.
//
// These three predicates together select zero or one baseline compiler and zero
// or one optimizing compiler, based on: what's compiled into the executable,
// what's supported on the current platform, what's selected by options, and the
// current run-time environment.  As it is possible for the computed values to
// change (when a value changes in about:config or the debugger pane is shown or
// hidden), it is inadvisable to cache these values in such a way that they
// could become invalid.  Generally it is cheap always to recompute them.

bool BaselineAvailable(JSContext* cx);
bool IonAvailable(JSContext* cx);

// Test all three.

bool AnyCompilerAvailable(JSContext* cx);

// Asm.JS is translated to wasm and then compiled using the wasm optimizing
// compiler; test whether this compiler is available.

bool WasmCompilerForAsmJSAvailable(JSContext* cx);

// Predicates for white-box compiler disablement testing.
//
// These predicates determine whether the optimizing compilers were disabled by
// features that are enabled at compile-time or run-time.  They do not consider
// the hardware platform on whether other compilers are enabled.
//
// If `reason` is not null then it is populated with a string that describes
// the specific features that disable the compiler.
//
// Returns false on OOM (which happens only when a reason is requested),
// otherwise true, with the result in `*isDisabled` and optionally the reason in
// `*reason`.

bool BaselineDisabledByFeatures(JSContext* cx, bool* isDisabled,
                                JSStringBuilder* reason = nullptr);
bool IonDisabledByFeatures(JSContext* cx, bool* isDisabled,
                           JSStringBuilder* reason = nullptr);

// Predicates for feature availability.
//
// The following predicates check whether particular wasm features are enabled,
// and for each, whether at least one compiler is (currently) available that
// supports the feature.

// Streaming compilation.
bool StreamingCompilationAvailable(JSContext* cx);

// Caching of optimized code.  Implies both streaming compilation and an
// optimizing compiler tier.
bool CodeCachingAvailable(JSContext* cx);

// Shared memory and atomics.
bool ThreadsAvailable(JSContext* cx);

#define WASM_FEATURE(NAME, ...) bool NAME##Available(JSContext* cx);
JS_FOR_WASM_FEATURES(WASM_FEATURE)
#undef WASM_FEATURE

// SIMD operations.
bool SimdAvailable(JSContext* cx);

// Privileged content that can access experimental features.
bool IsPrivilegedContext(JSContext* cx);

#if defined(ENABLE_WASM_SIMD) && defined(DEBUG)
// Report the result of a Simd simplification to the testing infrastructure.
void ReportSimdAnalysis(const char* data);
#endif

// Returns true if WebAssembly as configured by compile-time flags and run-time
// options can support try/catch, throw, rethrow, and branch_on_exn (evolving).
bool ExceptionsAvailable(JSContext* cx);

}  // namespace wasm
}  // namespace js

#endif  // js_wasm_WasmFeatures_h
