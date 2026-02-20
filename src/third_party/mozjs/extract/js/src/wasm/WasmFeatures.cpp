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

#include "wasm/WasmFeatures.h"

#include "jit/AtomicOperations.h"
#include "jit/JitContext.h"
#include "jit/JitOptions.h"
#include "js/Prefs.h"
#include "util/StringBuilder.h"
#include "vm/JSContext.h"
#include "vm/Realm.h"
#include "vm/StringType.h"
#include "wasm/WasmBaselineCompile.h"
#include "wasm/WasmIonCompile.h"
#include "wasm/WasmSignalHandlers.h"

using namespace js;
using namespace js::wasm;
using namespace js::jit;

// About the fuzzer intercession points: If fuzzing has been selected and only a
// single compiler has been selected then we will disable features that are not
// supported by that single compiler.  This is strictly a concession to the
// fuzzer infrastructure.

static inline bool IsFuzzingIon(JSContext* cx) {
  return IsFuzzing() && !cx->options().wasmBaseline() &&
         cx->options().wasmIon();
}

// These functions read flags and apply fuzzing intercession policies.  Never go
// directly to the flags in code below, always go via these accessors.

static inline bool WasmThreadsFlag(JSContext* cx) {
  return cx->realm() &&
         cx->realm()->creationOptions().getSharedMemoryAndAtomicsEnabled();
}

#define WASM_FEATURE(NAME, ...) \
  static inline bool Wasm##NAME##Flag(JSContext* cx);
JS_FOR_WASM_FEATURES(WASM_FEATURE);
#undef WASM_FEATURE

#define WASM_FEATURE(NAME, LOWER_NAME, COMPILE_PRED, COMPILER_PRED, FLAG_PRED, \
                     FLAG_FORCE_ON, FLAG_FUZZ_ON, PREF)                        \
  static inline bool Wasm##NAME##Flag(JSContext* cx) {                         \
    if (!(COMPILE_PRED)) {                                                     \
      return false;                                                            \
    }                                                                          \
    return ((FLAG_PRED) && JS::Prefs::wasm_##PREF()) || (FLAG_FORCE_ON);       \
  }
JS_FOR_WASM_FEATURES(WASM_FEATURE);
#undef WASM_FEATURE

static inline bool WasmDebuggerActive(JSContext* cx) {
  if (IsFuzzingIon(cx)) {
    return false;
  }
  return cx->realm() && cx->realm()->debuggerObservesWasm();
}

/*
 * [SMDOC] Compiler and feature selection; compiler and feature availability.
 *
 * In order to make the computation of whether a wasm feature or wasm compiler
 * is available predictable, we have established some rules, and implemented
 * those rules.
 *
 * Code elsewhere should use the predicates below to test for features and
 * compilers, it should never try to compute feature and compiler availability
 * in other ways.
 *
 * At the outset, there is a set of selected compilers C containing at most one
 * baseline compiler [*] and at most one optimizing compiler [**], and a set of
 * selected features F.  These selections come from defaults and from overrides
 * by command line switches in the shell and javascript.option.wasm_X in the
 * browser.  Defaults for both features and compilers may be platform specific,
 * for example, some compilers may not be available on some platforms because
 * they do not support the architecture at all or they do not support features
 * that must be enabled by default on the platform.
 *
 * [*] Currently we have only one, "baseline" aka "Rabaldr", but other
 *     implementations have additional baseline translators, eg from wasm
 *     bytecode to an internal code processed by an interpreter.
 *
 * [**] Currently we have only one, "ion" aka "Baldr".
 *
 *
 * Compiler availability:
 *
 * The set of features F induces a set of available compilers A: these are the
 * compilers that all support all the features in F.  (Some of these compilers
 * may not be in the set C.)
 *
 * The sets C and A are intersected, yielding a set of enabled compilers E.
 * Notably, the set E may be empty, in which case wasm is effectively disabled
 * (though the WebAssembly object is still present in the global environment).
 *
 * An important consequence is that selecting a feature that is not supported by
 * a particular compiler disables that compiler completely -- there is no notion
 * of a compiler being available but suddenly failing when an unsupported
 * feature is used by a program.  If a compiler is available, it supports all
 * the features that have been selected.
 *
 * Equally important, a feature cannot be enabled by default on a platform if
 * the feature is not supported by all the compilers we wish to have enabled by
 * default on the platform.  We MUST by-default disable features on a platform
 * that are not supported by all the compilers on the platform.
 *
 * In a shell build, the testing functions wasmCompilersPresent,
 * wasmCompileMode, and wasmIonDisabledByFeatures can be used to probe compiler
 * availability and the reasons for a compiler being unavailable.
 *
 *
 * Feature availability:
 *
 * A feature is available if it is selected and there is at least one available
 * compiler that implements it.
 *
 * For example, --wasm-gc selects the GC feature, and if Baseline is available
 * then the feature is available.
 *
 * In a shell build, there are per-feature testing functions (of the form
 * wasmFeatureEnabled) to probe whether specific features are available.
 */

// Compiler availability predicates.  These must be kept in sync with the
// feature predicates in the next section below.
//
// These can't call the feature predicates since the feature predicates call
// back to these predicates.  So there will be a small amount of duplicated
// logic here, but as compilers reach feature parity that duplication will go
// away.

bool wasm::BaselineAvailable(JSContext* cx) {
  if (!cx->options().wasmBaseline() || !BaselinePlatformSupport()) {
    return false;
  }
  bool isDisabled = false;
  MOZ_ALWAYS_TRUE(BaselineDisabledByFeatures(cx, &isDisabled));
  return !isDisabled;
}

bool wasm::IonAvailable(JSContext* cx) {
  if (!cx->options().wasmIon() || !IonPlatformSupport()) {
    return false;
  }
  bool isDisabled = false;
  MOZ_ALWAYS_TRUE(IonDisabledByFeatures(cx, &isDisabled));
  return !isDisabled;
}

bool wasm::WasmCompilerForAsmJSAvailable(JSContext* cx) {
  return IonAvailable(cx);
}

template <size_t ArrayLength>
static inline bool Append(JSStringBuilder* reason, const char (&s)[ArrayLength],
                          char* sep) {
  if ((*sep && !reason->append(*sep)) || !reason->append(s)) {
    return false;
  }
  *sep = ',';
  return true;
}

bool wasm::BaselineDisabledByFeatures(JSContext* cx, bool* isDisabled,
                                      JSStringBuilder* reason) {
  // Baseline cannot be used if we are testing serialization.
  bool testSerialization = WasmTestSerializationFlag(cx);
  if (reason) {
    char sep = 0;
    if (testSerialization && !Append(reason, "testSerialization", &sep)) {
      return false;
    }
  }
  *isDisabled = testSerialization;
  return true;
}

bool wasm::IonDisabledByFeatures(JSContext* cx, bool* isDisabled,
                                 JSStringBuilder* reason) {
  // Ion has no debugging support.
  bool debug = WasmDebuggerActive(cx);
  if (reason) {
    char sep = 0;
    if (debug && !Append(reason, "debug", &sep)) {
      return false;
    }
  }
  *isDisabled = debug;
  return true;
}

bool wasm::AnyCompilerAvailable(JSContext* cx) {
  return wasm::BaselineAvailable(cx) || wasm::IonAvailable(cx);
}

// Feature predicates.  These must be kept in sync with the predicates in the
// section above.
//
// The meaning of these predicates is tricky: A predicate is true for a feature
// if the feature is enabled and/or compiled-in *and* we have *at least one*
// compiler that can support the feature.  Subsequent compiler selection must
// ensure that only compilers that actually support the feature are used.

#define WASM_FEATURE(NAME, LOWER_NAME, COMPILE_PRED, COMPILER_PRED, ...) \
  bool wasm::NAME##Available(JSContext* cx) {                            \
    return Wasm##NAME##Flag(cx) && (COMPILER_PRED);                      \
  }
JS_FOR_WASM_FEATURES(WASM_FEATURE)
#undef WASM_FEATURE

bool wasm::IsPrivilegedContext(JSContext* cx) {
  // This may be slightly more lenient than we want in an ideal world, but it
  // remains safe.
  return cx->realm() && cx->realm()->principals() &&
         cx->realm()->principals()->isSystemOrAddonPrincipal();
}

bool wasm::SimdAvailable(JSContext* cx) {
  return js::jit::JitSupportsWasmSimd();
}

bool wasm::ThreadsAvailable(JSContext* cx) {
  return WasmThreadsFlag(cx) && AnyCompilerAvailable(cx);
}

bool wasm::HasPlatformSupport() {
#if !MOZ_LITTLE_ENDIAN()
  return false;
#else

  if (!HasJitBackend()) {
    return false;
  }

  if (gc::SystemPageSize() > wasm::PageSize) {
    return false;
  }

  if (!JitOptions.supportsUnalignedAccesses) {
    return false;
  }

  if (!jit::JitSupportsAtomics()) {
    return false;
  }

  // Wasm threads require 8-byte lock-free atomics.
  if (!jit::AtomicOperations::isLockfree8()) {
    return false;
  }

  // Test only whether the compilers are supported on the hardware, not whether
  // they are enabled.
  return BaselinePlatformSupport() || IonPlatformSupport();
#endif
}

bool wasm::HasSupport(JSContext* cx) {
  // If the general wasm pref is on, it's on for everything.
  bool prefEnabled = cx->options().wasm();
  // If the general pref is off, check trusted principals.
  if (MOZ_UNLIKELY(!prefEnabled)) {
    prefEnabled = cx->options().wasmForTrustedPrinciples() && cx->realm() &&
                  cx->realm()->principals() &&
                  cx->realm()->principals()->isSystemOrAddonPrincipal();
  }
  // Do not check for compiler availability, as that may be run-time variant.
  // For HasSupport() we want a stable answer depending only on prefs.
  return prefEnabled && HasPlatformSupport() && EnsureFullSignalHandlers(cx);
}

bool wasm::StreamingCompilationAvailable(JSContext* cx) {
  // This should match EnsureStreamSupport().
  return HasSupport(cx) && AnyCompilerAvailable(cx) &&
         cx->runtime()->offThreadPromiseState.ref().initialized() &&
         CanUseExtraThreads() && cx->runtime()->consumeStreamCallback &&
         cx->runtime()->reportStreamErrorCallback;
}

bool wasm::CodeCachingAvailable(JSContext* cx) {
  // Fuzzilli breaks the out-of-process compilation mechanism,
  // so we disable it permanently in those builds.
#ifdef FUZZING_JS_FUZZILLI
  return false;
#else

  // TODO(bug 1913109): lazy tiering doesn't support serialization
  if (JS::Prefs::wasm_lazy_tiering() || JS::Prefs::wasm_lazy_tiering_for_gc()) {
    return false;
  }

  // At the moment, we require Ion support for code caching.  The main reason
  // for this is that wasm::CompileAndSerialize() does not have access to
  // information about which optimizing compiler it should use.  See comments in
  // CompileAndSerialize(), below.
  return StreamingCompilationAvailable(cx) && IonAvailable(cx);
#endif
}
