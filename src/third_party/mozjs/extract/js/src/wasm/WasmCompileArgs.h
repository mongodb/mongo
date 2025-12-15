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
#include "mozilla/SHA1.h"
#include "mozilla/TypedEnumBits.h"

#include "js/Utility.h"
#include "js/WasmFeatures.h"
#include "wasm/WasmBinaryTypes.h"
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

static constexpr const char* ToString(Tier tier) {
  switch (tier) {
    case wasm::Tier::Baseline:
      return "baseline";
    case wasm::Tier::Optimized:
      return "optimized";
    default:
      return "unknown";
  }
}

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

struct BuiltinModuleIds {
  BuiltinModuleIds() = default;

  bool selfTest = false;
  bool intGemm = false;
  bool jsString = false;
  bool jsStringConstants = false;
  SharedChars jsStringConstantsNamespace;

  bool hasNone() const {
    return !selfTest && !intGemm && !jsString && !jsStringConstants;
  }
};

// Describes per-compilation settings that are controlled by an options bag
// passed to compilation and validation functions. (Nonstandard extension
// available under prefs.)

struct FeatureOptions {
  FeatureOptions()
      : disableOptimizingCompiler(false),
        isBuiltinModule(false),
        jsStringBuiltins(false),
        jsStringConstants(false),
        requireExnref(false) {}

  // Whether we should try to disable our optimizing compiler. Only available
  // with `IsSimdPrivilegedContext`.
  bool disableOptimizingCompiler;

  // Enables builtin module opcodes, only set in WasmBuiltinModule.cpp.
  bool isBuiltinModule;

  // Enable JS String builtins for this module, only available if the feature
  // is also enabled.
  bool jsStringBuiltins;
  // Enable imported string constants for this module, only available if the
  // feature is also enabled.
  bool jsStringConstants;
  SharedChars jsStringConstantsNamespace;

  // Enable exnref support.
  bool requireExnref;

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
        isBuiltinModule(false),
        builtinModules() {
  }
  FeatureArgs(const FeatureArgs&) = default;
  FeatureArgs& operator=(const FeatureArgs&) = default;
  FeatureArgs(FeatureArgs&&) = default;
  FeatureArgs& operator=(FeatureArgs&&) = default;

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
  ReturnCall = 0x2,
};

using FeatureUsageVector = Vector<FeatureUsage, 0, SystemAllocPolicy>;

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
  // - the 'buildForValidation' one, which takes just the features to enable
  //   and sets the compilers to a null state.
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
  static SharedCompileArgs buildForValidation(const FeatureArgs& args);

  explicit CompileArgs()
      : scriptedCaller(),
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
  void computeParameters(const ModuleMetadata& moduleMeta);

  // Compute any remaining compilation parameters.  Only use this method if
  // the CompilerEnvironment was created with values for mode, tier, and
  // debug.
  void computeParameters();

  bool isComputed() const { return state_ == Computed; }
  CompileMode mode() const {
    MOZ_ASSERT(isComputed());
    return mode_;
  }
  CompileState initialState() const {
    switch (mode()) {
      case CompileMode::Once:
        return CompileState::Once;
      case CompileMode::EagerTiering:
        return CompileState::EagerTier1;
      case CompileMode::LazyTiering:
        return CompileState::LazyTier1;
      default:
        MOZ_CRASH();
    }
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

// A bytecode source is a wrapper around wasm bytecode that is to be compiled
// or validated. It has been pre-parsed into three regions:
//   1. 'env' - everything before the code section
//   2. 'code' - the code section
//   3. 'tail' - everything after the code section.
//
// The naming here matches the corresponding validation functions we have. This
// design comes from the requirements of streaming compilation which assembles
// separate buffers for each of these regions, and never constructs a single
// contiguous buffer. We use it for compiling contiguous buffers as well so that
// we have a single code path.
//
// If a module does not contain a code section (or is invalid and cannot be
// split into these regions), the bytecode source will only have an 'env'
// region.
//
// This class does not own any of the underlying buffers and only points to
// them. See BytecodeBuffer for that.
class BytecodeSource {
  BytecodeSpan env_;
  BytecodeSpan code_;
  BytecodeSpan tail_;

  size_t envOffset() const { return 0; }
  size_t codeOffset() const { return envOffset() + envLength(); }
  size_t tailOffset() const { return codeOffset() + codeLength(); }

  size_t envLength() const { return env_.size(); }
  size_t codeLength() const { return code_.size(); }
  size_t tailLength() const { return tail_.size(); }

 public:
  // Create a bytecode source with no bytecode.
  BytecodeSource() = default;

  // Create a bytecode source from regions that have already been split. Does
  // not do any validation.
  BytecodeSource(const BytecodeSpan& envSpan, const BytecodeSpan& codeSpan,
                 const BytecodeSpan& tailSpan)
      : env_(envSpan), code_(codeSpan), tail_(tailSpan) {}

  // Parse a contiguous buffer into a bytecode source. This cannot fail because
  // invalid modules will result in a bytecode source with only an 'env' region
  // that further validation will reject.
  BytecodeSource(const uint8_t* begin, size_t length);

  // Copying and moving is allowed.
  BytecodeSource(const BytecodeSource&) = default;
  BytecodeSource& operator=(const BytecodeSource&) = default;
  BytecodeSource(BytecodeSource&&) = default;
  BytecodeSource& operator=(BytecodeSource&&) = default;

  // The length in bytes of this module.
  size_t length() const { return env_.size() + code_.size() + tail_.size(); }

  // Whether we have a code section region or not. If there is no code section,
  // then the tail region will be in the env region and must be parsed from
  // there.
  bool hasCodeSection() const { return code_.size() != 0; }

  BytecodeRange envRange() const {
    return BytecodeRange(envOffset(), envLength());
  }
  BytecodeRange codeRange() const {
    // Do not ask for the code range if we don't have a code section.
    MOZ_ASSERT(hasCodeSection());
    return BytecodeRange(codeOffset(), codeLength());
  }
  BytecodeRange tailRange() const {
    // Do not ask for the tail range if we don't have a code section. Any
    // contents that would be in the tail section will be in the env section,
    // and the caller must use that section instead.
    MOZ_ASSERT(hasCodeSection());
    return BytecodeRange(tailOffset(), tailLength());
  }

  BytecodeSpan envSpan() const { return env_; }
  BytecodeSpan codeSpan() const {
    // Do not ask for the code span if we don't have a code section.
    MOZ_ASSERT(hasCodeSection());
    return code_;
  }
  BytecodeSpan tailSpan() const {
    // Do not ask for the tail span if we don't have a code section. Any
    // contents that would be in the tail section will be in the env section,
    // and the caller must use that section instead.
    MOZ_ASSERT(hasCodeSection());
    return tail_;
  }
  BytecodeSpan getSpan(const BytecodeRange& range) const {
    // Check if this range is within the env span
    if (range.end <= codeOffset()) {
      return range.toSpan(env_);
    }

    // Check if this range is within the code span
    if (range.end <= tailOffset()) {
      // The range cannot cross the span boundary
      MOZ_RELEASE_ASSERT(range.start >= codeOffset());
      return range.relativeTo(codeRange()).toSpan(code_);
    }

    // Otherwise we must be within the tail span
    // The range cannot cross the span boundary
    MOZ_RELEASE_ASSERT(range.start >= tailOffset());
    return range.relativeTo(tailRange()).toSpan(tail_);
  }

  // Copy the contents of this buffer to the destination. The destination must
  // be at least `this->length()` bytes.
  void copyTo(uint8_t* dest) const {
    memcpy(dest + envOffset(), env_.data(), env_.size());
    memcpy(dest + codeOffset(), code_.data(), code_.size());
    memcpy(dest + tailOffset(), tail_.data(), tail_.size());
  }

  // Compute a SHA1 hash of the module.
  void computeHash(mozilla::SHA1Sum::Hash* hash) const {
    mozilla::SHA1Sum sha1Sum;
    sha1Sum.update(env_.data(), env_.size());
    sha1Sum.update(code_.data(), code_.size());
    sha1Sum.update(tail_.data(), tail_.size());
    sha1Sum.finish(*hash);
  }
};

// A version of `BytecodeSource` that owns the underlying buffers for each
// region of bytecode. See the comment on `BytecodeSource` for interpretation
// of the different regions.
//
// The regions are allocated in separate vectors so that we can just hold onto
// the code section after we've finished compiling the module without having to
// split apart the bytecode buffer.
class BytecodeBuffer {
  SharedBytes env_;
  SharedBytes code_;
  SharedBytes tail_;
  BytecodeSource source_;

 public:
  // Create an empty buffer.
  BytecodeBuffer() = default;
  // Create a buffer from pre-parsed regions.
  BytecodeBuffer(const ShareableBytes* env, const ShareableBytes* code,
                 const ShareableBytes* tail);
  // Create a buffer from a source by allocating memory for each region.
  [[nodiscard]]
  static bool fromSource(const BytecodeSource& bytecodeSource,
                         BytecodeBuffer* bytecodeBuffer);

  // Copying and moving is allowed, we just hold references to the underyling
  // buffers.
  BytecodeBuffer(const BytecodeBuffer&) = default;
  BytecodeBuffer& operator=(const BytecodeBuffer&) = default;
  BytecodeBuffer(BytecodeBuffer&&) = default;
  BytecodeBuffer& operator=(BytecodeBuffer&&) = default;

  // Get a bytecode source that points into our owned memory.
  const BytecodeSource& source() const { return source_; }

  // Grab a reference to the code section region, if any.
  SharedBytes codeSection() const { return code_; }
};

// Utility for passing either a bytecode buffer (which owns the bytecode) or
// just the source (which does not own the bytecode).
class BytecodeBufferOrSource {
  union {
    const BytecodeBuffer* buffer_;
    BytecodeSource source_;
  };
  bool hasBuffer_;

 public:
  BytecodeBufferOrSource() : source_(BytecodeSource()), hasBuffer_(false) {}
  explicit BytecodeBufferOrSource(const BytecodeBuffer& buffer)
      : buffer_(&buffer), hasBuffer_(true) {}
  explicit BytecodeBufferOrSource(const BytecodeSource& source)
      : source_(source), hasBuffer_(false) {}

  BytecodeBufferOrSource(const BytecodeBufferOrSource&) = delete;
  const BytecodeBufferOrSource& operator=(const BytecodeBufferOrSource&) =
      delete;

  ~BytecodeBufferOrSource() {
    if (!hasBuffer_) {
      source_.~BytecodeSource();
    }
  }

  bool hasBuffer() const { return hasBuffer_; }
  const BytecodeBuffer& buffer() const {
    MOZ_ASSERT(hasBuffer());
    return *buffer_;
  }
  const BytecodeSource& source() const {
    if (hasBuffer_) {
      return buffer_->source();
    }
    return source_;
  }

  [[nodiscard]] bool getOrCreateBuffer(BytecodeBuffer* result) const {
    if (hasBuffer()) {
      *result = buffer();
    }
    return BytecodeBuffer::fromSource(source(), result);
  }

  [[nodiscard]] SharedBytes getOrCreateCodeSection() const {
    if (hasBuffer()) {
      return buffer().codeSection();
    }
    return ShareableBytes::fromSpan(source().codeSpan());
  }
};

}  // namespace wasm
}  // namespace js

#endif  // wasm_compile_args_h
