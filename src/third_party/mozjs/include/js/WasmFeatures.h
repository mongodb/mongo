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
// Each feature is either `DEFAULT` or `EXPERIMENTAL`. Default features are
// enabled by default in ContextOptions and in the JS-shell. Default features
// are given a `--no-wasm-FEATURE` shell flag, while experimental features are
// given a `--wasm-FEATURE` shell flag.
//
// The browser pref is `javascript.options.wasm-FEATURE` for both default and
// experimental features.
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
//      --no-wasm-FEATURE for default features, or --wasm-FEATURE for
//      experimental features.
//   g. preference name: The stem of the browser preference. Will be expanded
//      to `javascript.options.wasm-FEATURE`.
// 4. Add the preference to module/libpref/init/StaticPrefList.yaml
//   a. Use conditionally compiled flag
//   b. Set value to 'true' for default features, 'false' or @IS_NIGHTLY_BUILD@
//      for experimental features.
//

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
#ifdef ENABLE_WASM_EXCEPTIONS
#  define WASM_EXCEPTIONS_ENABLED 1
#else
#  define WASM_EXCEPTIONS_ENABLED 0
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

// clang-format off
#define JS_FOR_WASM_FEATURES(DEFAULT, EXPERIMENTAL)                           \
  DEFAULT(/* capitalized name   */ Simd,                                      \
          /* lower case name    */ v128,                                      \
          /* compile predicate  */ WASM_SIMD_ENABLED,                         \
          /* compiler predicate */ AnyCompilerAvailable(cx),                  \
          /* flag predicate     */ !IsFuzzingCranelift(cx) &&                 \
              js::jit::JitSupportsWasmSimd(),                                 \
          /* shell flag         */ "simd",                                    \
          /* preference name    */ "simd")                                    \
  EXPERIMENTAL(/* capitalized name   */ ExtendedConst,                        \
               /* lower case name    */ extendedConst,                        \
               /* compile predicate  */ WASM_EXTENDED_CONST_ENABLED,          \
               /* compiler predicate */ true,                                 \
               /* flag predicate     */ true,                                 \
               /* shell flag         */ "extended-const",                     \
               /* preference name    */ "extended_const")                     \
  EXPERIMENTAL(                                                               \
      /* capitalized name   */ Exceptions,                                    \
      /* lower case name    */ exceptions,                                    \
      /* compile predicate  */ WASM_EXCEPTIONS_ENABLED,                       \
      /* compiler predicate */ BaselineAvailable(cx),                         \
      /* flag predicate     */ !IsFuzzingIon(cx) && !IsFuzzingCranelift(cx),  \
      /* shell flag         */ "exceptions",                                  \
      /* preference name    */ "exceptions")                                  \
  EXPERIMENTAL(/* capitalized name   */ FunctionReferences,                   \
               /* lower case name    */ functionReferences,                   \
               /* compile predicate  */ WASM_FUNCTION_REFERENCES_ENABLED,     \
               /* compiler predicate */ BaselineAvailable(cx),                \
               /* flag predicate     */ !IsFuzzingIon(cx) &&                  \
                   !IsFuzzingCranelift(cx),                                   \
               /* shell flag         */ "function-references",                \
               /* preference name    */ "function_references")                \
  EXPERIMENTAL(/* capitalized name   */ Gc,                                   \
               /* lower case name    */ gc,                                   \
               /* compile predicate  */ WASM_GC_ENABLED,                      \
               /* compiler predicate */ BaselineAvailable(cx),                \
               /* flag predicate     */ WasmFunctionReferencesFlag(cx),       \
               /* shell flag         */ "gc",                                 \
               /* preference name    */ "gc")                                 \
  EXPERIMENTAL(/* capitalized name   */ RelaxedSimd,                          \
               /* lower case name    */ v128Relaxed,                          \
               /* compile predicate  */ WASM_RELAXED_SIMD_ENABLED,            \
               /* compiler predicate */ AnyCompilerAvailable(cx),             \
               /* flag predicate     */ !IsFuzzingCranelift(cx) &&            \
               js::jit::JitSupportsWasmSimd(),                                \
               /* shell flag         */ "relaxed-simd",                       \
               /* preference name    */ "relaxed_simd")

// clang-format on

#endif  // js_WasmFeatures_h
