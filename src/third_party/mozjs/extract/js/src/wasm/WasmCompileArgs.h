/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2021 Mozilla Foundation
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

#ifndef wasm_compile_args_h
#define wasm_compile_args_h

#include "mozilla/RefPtr.h"
#include "mozilla/TypedEnumBits.h"

#include "js/Utility.h"
#include "js/WasmFeatures.h"
#include "wasm/WasmConstants.h"
#include "wasm/WasmShareable.h"

namespace js {
namespace wasm {

enum class Shareable { False, True };

// Code can be compiled either with the Baseline compiler or the Ion compiler,
// and tier-variant data are tagged with the Tier value.
//
// A tier value is used to request tier-variant aspects of code, metadata, or
// linkdata.  The tiers are normally explicit (Baseline and Ion); implicit tiers
// can be obtained through accessors on Code objects (eg, stableTier).

enum class Tier {
  Baseline,
  Debug = Baseline,
  Optimized,
  Serialized = Optimized
};

// Iterator over tiers present in a tiered data structure.

class Tiers {
  Tier t_[2];
  uint32_t n_;

 public:
  explicit Tiers() { n_ = 0; }
  explicit Tiers(Tier t) {
    t_[0] = t;
    n_ = 1;
  }
  explicit Tiers(Tier t, Tier u) {
    MOZ_ASSERT(t != u);
    t_[0] = t;
    t_[1] = u;
    n_ = 2;
  }

  Tier* begin() { return t_; }
  Tier* end() { return t_ + n_; }
};

// Describes per-compilation settings that are controlled by an options bag
// passed to compilation and validation functions. (Nonstandard extension
// available under prefs.)

struct FeatureOptions {
  FeatureOptions()
      : isBuiltinModule(false),
        jsStringBuiltins(false)
#ifdef ENABLE_WASM_GC
        ,
        requireGC(false)
#endif
#ifdef ENABLE_WASM_TAIL_CALLS
        ,
        requireTailCalls(false)
#endif
  {
  }

  // Enables builtin module opcodes, only set in WasmBuiltinModule.cpp.
  bool isBuiltinModule;
  // Enable JS String builtins for this module, only available if the feature
  // is also enabled.
  bool jsStringBuiltins;

#ifdef ENABLE_WASM_GC
  // Enable GC support.
  bool requireGC;
#endif
#ifdef ENABLE_WASM_TAIL_CALLS
  // Enable tail-calls support.
  bool requireTailCalls;
#endif

  // Parse the compile options bag.
  [[nodiscard]] bool init(JSContext* cx, HandleValue val);
};

// Describes the features that control wasm compilation.

struct FeatureArgs {
  FeatureArgs()
      :
#define WASM_FEATURE(NAME, LOWER_NAME, ...) LOWER_NAME(false),
        JS_FOR_WASM_FEATURES(WASM_FEATURE)
#undef WASM_FEATURE
            sharedMemory(Shareable::False),
        simd(false),
        isBuiltinModule(false) {
  }
  FeatureArgs(const FeatureArgs&) = default;
  FeatureArgs& operator=(const FeatureArgs&) = default;
  FeatureArgs(FeatureArgs&&) = default;

  static FeatureArgs build(JSContext* cx, const FeatureOptions& options);
  static FeatureArgs allEnabled() {
    FeatureArgs args;
#define WASM_FEATURE(NAME, LOWER_NAME, ...) args.LOWER_NAME = true;
    JS_FOR_WASM_FEATURES(WASM_FEATURE)
#undef WASM_FEATURE
    args.sharedMemory = Shareable::True;
    args.simd = true;
    return args;
  }

#define WASM_FEATURE(NAME, LOWER_NAME, ...) bool LOWER_NAME;
  JS_FOR_WASM_FEATURES(WASM_FEATURE)
#undef WASM_FEATURE

  Shareable sharedMemory;
  bool simd;
  // Whether this module is a wasm builtin module (see WasmBuiltinModule.h) and
  // can contain special opcodes in function bodies.
  bool isBuiltinModule;
  // The set of builtin modules that are imported by this module.
  BuiltinModuleIds builtinModules;
};

// Observed feature usage for a compiled module. Intended to be used for use
// counters.
enum class FeatureUsage : uint8_t {
  None = 0x0,
  LegacyExceptions = 0x1,
};

void SetUseCountersForFeatureUsage(JSContext* cx, JSObject* object,
                                   FeatureUsage usage);

MOZ_MAKE_ENUM_CLASS_BITWISE_OPERATORS(FeatureUsage);

// Describes the JS scripted caller of a request to compile a wasm module.

struct ScriptedCaller {
  UniqueChars filename;  // UTF-8 encoded
  bool filenameIsURL;
  uint32_t line;

  ScriptedCaller() : filenameIsURL(false), line(0) {}
};

// Describes the reasons we cannot compute compile args

enum class CompileArgsError {
  OutOfMemory,
  NoCompiler,
};

// Describes all the parameters that control wasm compilation.

struct CompileArgs;
using MutableCompileArgs = RefPtr<CompileArgs>;
using SharedCompileArgs = RefPtr<const CompileArgs>;

struct CompileArgs : ShareableBase<CompileArgs> {
  ScriptedCaller scriptedCaller;
  UniqueChars sourceMapURL;

  bool baselineEnabled;
  bool ionEnabled;
  bool debugEnabled;
  bool forceTiering;

  FeatureArgs features;

  // CompileArgs has several constructors:
  //
  // - two through factory functions `build`/`buildAndReport`, which checks
  //   that flags are consistent with each other, and optionally reports any
  //   errors.
  // - the 'buildForAsmJS' one, which uses the appropriate configuration for
  //   legacy asm.js code.
  // - one that gives complete access to underlying fields.
  //
  // You should use the factory functions in general, unless you have a very
  // good reason (i.e. no JSContext around and you know which flags have been
  // used).

  static SharedCompileArgs build(JSContext* cx, ScriptedCaller&& scriptedCaller,
                                 const FeatureOptions& options,
                                 CompileArgsError* error);
  static SharedCompileArgs buildForAsmJS(ScriptedCaller&& scriptedCaller);
  static SharedCompileArgs buildAndReport(JSContext* cx,
                                          ScriptedCaller&& scriptedCaller,
                                          const FeatureOptions& options,
                                          bool reportOOM = false);

  explicit CompileArgs(ScriptedCaller&& scriptedCaller)
      : scriptedCaller(std::move(scriptedCaller)),
        baselineEnabled(false),
        ionEnabled(false),
        debugEnabled(false),
        forceTiering(false) {}
};

// CompilerEnvironment holds any values that will be needed to compute
// compilation parameters once the module's feature opt-in sections have been
// parsed.
//
// Subsequent to construction a computeParameters() call will compute the final
// compilation parameters, and the object can then be queried for their values.

struct CompileArgs;
class Decoder;

struct CompilerEnvironment {
  // The object starts in one of two "initial" states; computeParameters moves
  // it into the "computed" state.
  enum State { InitialWithArgs, InitialWithModeTierDebug, Computed };

  State state_;
  union {
    // Value if the state_ == InitialWithArgs.
    const CompileArgs* args_;

    // Value in the other two states.
    struct {
      CompileMode mode_;
      Tier tier_;
      DebugEnabled debug_;
    };
  };

 public:
  // Retain a reference to the CompileArgs. A subsequent computeParameters()
  // will compute all parameters from the CompileArgs and additional values.
  explicit CompilerEnvironment(const CompileArgs& args);

  // Save the provided values for mode, tier, and debug, and the initial value
  // for gc/refTypes. A subsequent computeParameters() will compute the
  // final value of gc/refTypes.
  CompilerEnvironment(CompileMode mode, Tier tier, DebugEnabled debugEnabled);

  // Compute any remaining compilation parameters.
  void computeParameters(Decoder& d);

  // Compute any remaining compilation parameters.  Only use this method if
  // the CompilerEnvironment was created with values for mode, tier, and
  // debug.
  void computeParameters();

  bool isComputed() const { return state_ == Computed; }
  CompileMode mode() const {
    MOZ_ASSERT(isComputed());
    return mode_;
  }
  Tier tier() const {
    MOZ_ASSERT(isComputed());
    return tier_;
  }
  DebugEnabled debug() const {
    MOZ_ASSERT(isComputed());
    return debug_;
  }
  bool debugEnabled() const { return debug() == DebugEnabled::True; }
};

}  // namespace wasm
}  // namespace js

#endif  // wasm_compile_args_h
