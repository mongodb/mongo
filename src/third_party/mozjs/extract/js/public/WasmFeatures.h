/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_WasmFeatures_h
#define js_WasmFeatures_h

// [SMDOC] WebAssembly feature gating
//
// Declarative listing of WebAssembly optional features. This macro is used to
// generate most of the feature gating code in a centralized manner. See
// 'Adding a feature' below for the exact steps needed to add a new feature.
//
// Each feature is either `DEFAULT`, `TENTATIVE`, or `EXPERIMENTAL`:
//
// Default features are enabled by default in ContextOptions and in the
// JS-shell, and are given a `--no-wasm-FEATURE` shell flag to disable.  The
// `--wasm-FEATURE` flag is rejected.
//
// Tentative features are like Default features, but the `--wasm-FEATURE` flag
// is silently ignored.
//
// Experimental features are disabled by default in ContextOptions and in the
// JS-shell, and are given a `--wasm-FEATURE` shell flag to enable.  The
// `--no-wasm-FEATURE` flag is silently ignored.
//
// The browser pref is `javascript.options.wasm-FEATURE` for default, tentative,
// and experimental features alike.
//
// # Adding a feature
//
// 1. Add a configure switch for the feature in js/moz.configure
// 2. Add a WASM_FEATURE_ENABLED #define below
// 3. Add the feature to JS_FOR_WASM_FEATURES
//   a. capitalized name: Used for naming of feature functions, including
//      wasmFeatureEnabled shell function.
//   b. lower case name: Used for naming of feature flag variables, including
//      in wasm::FeatureArgs.
//   c. compile predicate: Set to WASM_FEATURE_ENABLED
//   d. compiler predicate: Expression of compilers that this feature depends
//      on.
//   e. flag predicate: Expression used to predicate enablement of feature
//      flag. Useful for disabling a feature when dependent feature is not
//      enabled or if we are fuzzing.
//   f. shell flag: The stem of the JS-shell flag. Will be expanded to
//      --no-wasm-FEATURE or --wasm-FEATURE as explained above.
//   g. preference name: The stem of the browser preference. Will be expanded
//      to `javascript.options.wasm-FEATURE`.
// 4. Add the preference to module/libpref/init/StaticPrefList.yaml
//   a. Use conditionally compiled flag
//   b. Set value to 'true' for default features, @IS_NIGHTLY_BUILD@ for
//      tentative features, and 'false' for experimental features.
// 5. [fuzzing] Add the feature to gluesmith/src/lib.rs, if wasm-smith has
//    support for it.

#ifdef ENABLE_WASM_SIMD
#  define WASM_SIMD_ENABLED 1
#else
#  define WASM_SIMD_ENABLED 0
#endif
#ifdef ENABLE_WASM_RELAXED_SIMD
#  define WASM_RELAXED_SIMD_ENABLED 1
#else
#  define WASM_RELAXED_SIMD_ENABLED 0
#endif
#ifdef ENABLE_WASM_EXTENDED_CONST
#  define WASM_EXTENDED_CONST_ENABLED 1
#else
#  define WASM_EXTENDED_CONST_ENABLED 0
#endif
#ifdef ENABLE_WASM_FUNCTION_REFERENCES
#  define WASM_FUNCTION_REFERENCES_ENABLED 1
#else
#  define WASM_FUNCTION_REFERENCES_ENABLED 0
#endif
#ifdef ENABLE_WASM_GC
#  define WASM_GC_ENABLED 1
#else
#  define WASM_GC_ENABLED 0
#endif
#ifdef ENABLE_WASM_MEMORY64
#  define WASM_MEMORY64_ENABLED 1
#else
#  define WASM_MEMORY64_ENABLED 0
#endif
#ifdef ENABLE_WASM_MEMORY_CONTROL
#  define WASM_MEMORY_CONTROL_ENABLED 1
#else
#  define WASM_MEMORY_CONTROL_ENABLED 0
#endif
#ifdef ENABLE_WASM_MOZ_INTGEMM
#  define WASM_MOZ_INTGEMM_ENABLED 1
#else
#  define WASM_MOZ_INTGEMM_ENABLED 0
#endif

// clang-format off
#define JS_FOR_WASM_FEATURES(DEFAULT, TENTATIVE, EXPERIMENTAL)                \
  TENTATIVE(/* capitalized name   */ ExtendedConst,                           \
            /* lower case name    */ extendedConst,                           \
            /* compile predicate  */ WASM_EXTENDED_CONST_ENABLED,             \
            /* compiler predicate */ true,                                    \
            /* flag predicate     */ true,                                    \
            /* shell flag         */ "extended-const",                        \
            /* preference name    */ "extended_const")                        \
  TENTATIVE(                                                                  \
      /* capitalized name   */ Exceptions,                                    \
      /* lower case name    */ exceptions,                                    \
      /* compile predicate  */ true,                                          \
      /* compiler predicate */ BaselineAvailable(cx) || IonAvailable(cx),     \
      /* flag predicate     */ true,                                          \
      /* shell flag         */ "exceptions",                                  \
      /* preference name    */ "exceptions")                                  \
  EXPERIMENTAL(/* capitalized name   */ FunctionReferences,                   \
               /* lower case name    */ functionReferences,                   \
               /* compile predicate  */ WASM_FUNCTION_REFERENCES_ENABLED,     \
               /* compiler predicate */ BaselineAvailable(cx) ||              \
                                        IonAvailable(cx),                     \
               /* flag predicate     */ true,                                 \
               /* shell flag         */ "function-references",                \
               /* preference name    */ "function_references")                \
  EXPERIMENTAL(/* capitalized name   */ Gc,                                   \
               /* lower case name    */ gc,                                   \
               /* compile predicate  */ WASM_GC_ENABLED,                      \
               /* compiler predicate */ AnyCompilerAvailable(cx),             \
               /* flag predicate     */ WasmFunctionReferencesFlag(cx),       \
               /* shell flag         */ "gc",                                 \
               /* preference name    */ "gc")                                 \
  TENTATIVE(/* capitalized name   */ RelaxedSimd,                             \
            /* lower case name    */ v128Relaxed,                             \
            /* compile predicate  */ WASM_RELAXED_SIMD_ENABLED,               \
            /* compiler predicate */ AnyCompilerAvailable(cx),                \
            /* flag predicate     */ js::jit::JitSupportsWasmSimd(),          \
            /* shell flag         */ "relaxed-simd",                          \
            /* preference name    */ "relaxed_simd")                          \
  TENTATIVE(                                                                  \
      /* capitalized name   */ Memory64,                                      \
      /* lower case name    */ memory64,                                      \
      /* compile predicate  */ WASM_MEMORY64_ENABLED,                         \
      /* compiler predicate */ BaselineAvailable(cx) || IonAvailable(cx),     \
      /* flag predicate     */ true,                                          \
      /* shell flag         */ "memory64",                                    \
      /* preference name    */ "memory64")                                    \
  EXPERIMENTAL(                                                               \
      /* capitalized name   */ MemoryControl,                                 \
      /* lower case name    */ memoryControl,                                 \
      /* compile predicate  */ WASM_MEMORY_CONTROL_ENABLED,                   \
      /* compiler predicate */ BaselineAvailable(cx) || IonAvailable(cx),     \
      /* flag predicate     */ true,                                          \
      /* shell flag         */ "memory-control",                              \
      /* preference name    */ "memory_control")                              \
  EXPERIMENTAL(/* capitalized name   */ MozIntGemm,                           \
               /* lower case name    */ mozIntGemm,                           \
               /* compile predicate  */ WASM_MOZ_INTGEMM_ENABLED,             \
               /* compiler predicate */ BaselineAvailable(cx) ||              \
                  IonAvailable(cx),                                           \
               /* flag predicate     */ IsSimdPrivilegedContext(cx),          \
               /* shell flag         */ "moz-intgemm",                        \
               /* preference name    */ "moz_intgemm")                        \
  EXPERIMENTAL(/* capitalized name   */ TestSerialization,                    \
               /* lower case name    */ testSerialization,                    \
               /* compile predicate  */ 1,                                    \
               /* compiler predicate */ IonAvailable(cx),                     \
               /* flag predicate     */ true,                                 \
               /* shell flag         */ "test-serialization",                 \
               /* preference name    */ "test-serialization")

// clang-format on

#endif  // js_WasmFeatures_h
