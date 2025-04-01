/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* JavaScript API. */

#ifndef js_ContextOptions_h
#define js_ContextOptions_h

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/CompileOptions.h"  // PrefableCompileOptions
#include "js/WasmFeatures.h"

struct JS_PUBLIC_API JSContext;

namespace JS {

class JS_PUBLIC_API ContextOptions {
 public:
  // clang-format off
  ContextOptions()
      : wasm_(true),
        wasmForTrustedPrinciples_(true),
        wasmVerbose_(false),
        wasmBaseline_(true),
        wasmIon_(true),
        testWasmAwaitTier2_(false),
        disableIon_(false),
        disableEvalSecurityChecks_(false),
        asyncStack_(true),
        asyncStackCaptureDebuggeeOnly_(false),
        throwOnDebuggeeWouldRun_(true),
        dumpStackOnDebuggeeWouldRun_(false),
#ifdef JS_ENABLE_SMOOSH
        trackNotImplemented_(false),
        trySmoosh_(false),
#endif
        fuzzing_(false) {
  }
  // clang-format on

  bool asmJS() const {
    return compileOptions_.asmJSOption() == AsmJSOption::Enabled;
  }
  AsmJSOption asmJSOption() const { return compileOptions_.asmJSOption(); }
  ContextOptions& setAsmJS(bool flag) {
    compileOptions_.setAsmJS(flag);
    return *this;
  }
  ContextOptions& setAsmJSOption(AsmJSOption option) {
    compileOptions_.setAsmJSOption(option);
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

  bool throwOnAsmJSValidationFailure() const {
    return compileOptions_.throwOnAsmJSValidationFailure();
  }
  ContextOptions& setThrowOnAsmJSValidationFailure(bool flag) {
    compileOptions_.setThrowOnAsmJSValidationFailure(flag);
    return *this;
  }
  ContextOptions& toggleThrowOnAsmJSValidationFailure() {
    compileOptions_.toggleThrowOnAsmJSValidationFailure();
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

  bool importAttributes() const { return compileOptions_.importAttributes(); }
  ContextOptions& setImportAttributes(bool enabled) {
    compileOptions_.setImportAttributes(enabled);
    return *this;
  }

  bool importAttributesAssertSyntax() const {
    return compileOptions_.importAttributesAssertSyntax();
  }
  ContextOptions& setImportAttributesAssertSyntax(bool enabled) {
    compileOptions_.setImportAttributesAssertSyntax(enabled);
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
  bool sourcePragmas() const { return compileOptions_.sourcePragmas(); }
  ContextOptions& setSourcePragmas(bool flag) {
    compileOptions_.setSourcePragmas(flag);
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
    setAsmJSOption(AsmJSOption::DisabledByAsmJSPref);
    setWasmBaseline(false);
  }

  PrefableCompileOptions& compileOptions() { return compileOptions_; }
  const PrefableCompileOptions& compileOptions() const {
    return compileOptions_;
  }

 private:
  // WASM options.
  bool wasm_ : 1;
  bool wasmForTrustedPrinciples_ : 1;
  bool wasmVerbose_ : 1;
  bool wasmBaseline_ : 1;
  bool wasmIon_ : 1;
  bool testWasmAwaitTier2_ : 1;

  // JIT options.
  bool disableIon_ : 1;
  bool disableEvalSecurityChecks_ : 1;

  // Runtime options.
  bool asyncStack_ : 1;
  bool asyncStackCaptureDebuggeeOnly_ : 1;
  bool throwOnDebuggeeWouldRun_ : 1;
  bool dumpStackOnDebuggeeWouldRun_ : 1;
#ifdef JS_ENABLE_SMOOSH
  bool trackNotImplemented_ : 1;
  bool trySmoosh_ : 1;
#endif
  bool fuzzing_ : 1;

  // Compile options.
  PrefableCompileOptions compileOptions_;
};

JS_PUBLIC_API ContextOptions& ContextOptionsRef(JSContext* cx);

}  // namespace JS

#endif  // js_ContextOptions_h
