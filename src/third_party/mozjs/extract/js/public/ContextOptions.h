/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* JavaScript API. */

#ifndef js_ContextOptions_h
#define js_ContextOptions_h

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/WasmFeatures.h"

struct JS_PUBLIC_API JSContext;

namespace JS {

class JS_PUBLIC_API ContextOptions {
 public:
  // clang-format off
  ContextOptions()
      : asmJS_(true),
        wasm_(true),
        wasmForTrustedPrinciples_(true),
        wasmVerbose_(false),
        wasmBaseline_(true),
        wasmIon_(true),
#define WASM_DEFAULT_FEATURE(NAME, ...) wasm##NAME##_(true),
#define WASM_EXPERIMENTAL_FEATURE(NAME, ...) wasm##NAME##_(false),
        JS_FOR_WASM_FEATURES(WASM_DEFAULT_FEATURE, WASM_DEFAULT_FEATURE, WASM_EXPERIMENTAL_FEATURE)
#undef WASM_DEFAULT_FEATURE
#undef WASM_EXPERIMENTAL_FEATURE
        testWasmAwaitTier2_(false),
        throwOnAsmJSValidationFailure_(false),
        disableIon_(false),
        disableEvalSecurityChecks_(false),
        asyncStack_(true),
        asyncStackCaptureDebuggeeOnly_(false),
        sourcePragmas_(true),
        throwOnDebuggeeWouldRun_(true),
        dumpStackOnDebuggeeWouldRun_(false),
        strictMode_(false),
#ifdef JS_ENABLE_SMOOSH
        trackNotImplemented_(false),
        trySmoosh_(false),
#endif
        fuzzing_(false),
        importAssertions_(false) {
  }
  // clang-format on

  bool asmJS() const { return asmJS_; }
  ContextOptions& setAsmJS(bool flag) {
    asmJS_ = flag;
    return *this;
  }
  ContextOptions& toggleAsmJS() {
    asmJS_ = !asmJS_;
    return *this;
  }

  bool wasm() const { return wasm_; }
  ContextOptions& setWasm(bool flag) {
    wasm_ = flag;
    return *this;
  }
  ContextOptions& toggleWasm() {
    wasm_ = !wasm_;
    return *this;
  }

  bool wasmForTrustedPrinciples() const { return wasmForTrustedPrinciples_; }
  ContextOptions& setWasmForTrustedPrinciples(bool flag) {
    wasmForTrustedPrinciples_ = flag;
    return *this;
  }

  bool wasmVerbose() const { return wasmVerbose_; }
  ContextOptions& setWasmVerbose(bool flag) {
    wasmVerbose_ = flag;
    return *this;
  }

  bool wasmBaseline() const { return wasmBaseline_; }
  ContextOptions& setWasmBaseline(bool flag) {
    wasmBaseline_ = flag;
    return *this;
  }

  bool wasmIon() const { return wasmIon_; }
  ContextOptions& setWasmIon(bool flag) {
    wasmIon_ = flag;
    return *this;
  }

  bool testWasmAwaitTier2() const { return testWasmAwaitTier2_; }
  ContextOptions& setTestWasmAwaitTier2(bool flag) {
    testWasmAwaitTier2_ = flag;
    return *this;
  }

#define WASM_FEATURE(NAME, ...)                     \
  bool wasm##NAME() const { return wasm##NAME##_; } \
  ContextOptions& setWasm##NAME(bool flag) {        \
    wasm##NAME##_ = flag;                           \
    return *this;                                   \
  }
  JS_FOR_WASM_FEATURES(WASM_FEATURE, WASM_FEATURE, WASM_FEATURE)
#undef WASM_FEATURE

  bool throwOnAsmJSValidationFailure() const {
    return throwOnAsmJSValidationFailure_;
  }
  ContextOptions& setThrowOnAsmJSValidationFailure(bool flag) {
    throwOnAsmJSValidationFailure_ = flag;
    return *this;
  }
  ContextOptions& toggleThrowOnAsmJSValidationFailure() {
    throwOnAsmJSValidationFailure_ = !throwOnAsmJSValidationFailure_;
    return *this;
  }

  // Override to allow disabling Ion for this context irrespective of the
  // process-wide Ion-enabled setting. This must be set right after creating
  // the context.
  bool disableIon() const { return disableIon_; }
  ContextOptions& setDisableIon() {
    disableIon_ = true;
    return *this;
  }

  bool importAssertions() const { return importAssertions_; }
  ContextOptions& setImportAssertions(bool enabled) {
    importAssertions_ = enabled;
    return *this;
  }

  // Override to allow disabling the eval restriction security checks for
  // this context.
  bool disableEvalSecurityChecks() const { return disableEvalSecurityChecks_; }
  ContextOptions& setDisableEvalSecurityChecks() {
    disableEvalSecurityChecks_ = true;
    return *this;
  }

  bool asyncStack() const { return asyncStack_; }
  ContextOptions& setAsyncStack(bool flag) {
    asyncStack_ = flag;
    return *this;
  }

  bool asyncStackCaptureDebuggeeOnly() const {
    return asyncStackCaptureDebuggeeOnly_;
  }
  ContextOptions& setAsyncStackCaptureDebuggeeOnly(bool flag) {
    asyncStackCaptureDebuggeeOnly_ = flag;
    return *this;
  }

  // Enable/disable support for parsing '//(#@) source(Mapping)?URL=' pragmas.
  bool sourcePragmas() const { return sourcePragmas_; }
  ContextOptions& setSourcePragmas(bool flag) {
    sourcePragmas_ = flag;
    return *this;
  }

  bool throwOnDebuggeeWouldRun() const { return throwOnDebuggeeWouldRun_; }
  ContextOptions& setThrowOnDebuggeeWouldRun(bool flag) {
    throwOnDebuggeeWouldRun_ = flag;
    return *this;
  }

  bool dumpStackOnDebuggeeWouldRun() const {
    return dumpStackOnDebuggeeWouldRun_;
  }
  ContextOptions& setDumpStackOnDebuggeeWouldRun(bool flag) {
    dumpStackOnDebuggeeWouldRun_ = flag;
    return *this;
  }

  bool strictMode() const { return strictMode_; }
  ContextOptions& setStrictMode(bool flag) {
    strictMode_ = flag;
    return *this;
  }
  ContextOptions& toggleStrictMode() {
    strictMode_ = !strictMode_;
    return *this;
  }

#ifdef JS_ENABLE_SMOOSH
  // Track Number of Not Implemented Calls by writing to a file
  bool trackNotImplemented() const { return trackNotImplemented_; }
  ContextOptions& setTrackNotImplemented(bool flag) {
    trackNotImplemented_ = flag;
    return *this;
  }

  // Try compiling SmooshMonkey frontend first, and fallback to C++
  // implementation when it fails.
  bool trySmoosh() const { return trySmoosh_; }
  ContextOptions& setTrySmoosh(bool flag) {
    trySmoosh_ = flag;
    return *this;
  }

#endif  // JS_ENABLE_SMOOSH

  bool fuzzing() const { return fuzzing_; }
  // Defined out-of-line because it depends on a compile-time option
  ContextOptions& setFuzzing(bool flag);

  void disableOptionsForSafeMode() {
    setAsmJS(false);
    setWasmBaseline(false);
  }

 private:
  bool asmJS_ : 1;
  bool wasm_ : 1;
  bool wasmForTrustedPrinciples_ : 1;
  bool wasmVerbose_ : 1;
  bool wasmBaseline_ : 1;
  bool wasmIon_ : 1;
#define WASM_FEATURE(NAME, ...) bool wasm##NAME##_ : 1;
  JS_FOR_WASM_FEATURES(WASM_FEATURE, WASM_FEATURE, WASM_FEATURE)
#undef WASM_FEATURE
  bool testWasmAwaitTier2_ : 1;
  bool throwOnAsmJSValidationFailure_ : 1;
  bool disableIon_ : 1;
  bool disableEvalSecurityChecks_ : 1;
  bool asyncStack_ : 1;
  bool asyncStackCaptureDebuggeeOnly_ : 1;
  bool sourcePragmas_ : 1;
  bool throwOnDebuggeeWouldRun_ : 1;
  bool dumpStackOnDebuggeeWouldRun_ : 1;
  bool strictMode_ : 1;
#ifdef JS_ENABLE_SMOOSH
  bool trackNotImplemented_ : 1;
  bool trySmoosh_ : 1;
#endif
  bool fuzzing_ : 1;
  bool importAssertions_ : 1;
};

JS_PUBLIC_API ContextOptions& ContextOptionsRef(JSContext* cx);

}  // namespace JS

#endif  // js_ContextOptions_h
