/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_JitContext_h
#define jit_JitContext_h

#include "mozilla/Assertions.h"
#include "mozilla/Result.h"

#include <stdint.h>

#include "jstypes.h"

struct JS_PUBLIC_API JSContext;

namespace js {
namespace jit {

class CompileRealm;
class CompileRuntime;
class TempAllocator;

enum MethodStatus {
  Method_Error,
  Method_CantCompile,
  Method_Skipped,
  Method_Compiled
};

// Use only even, non-zero values for errors, to allow using the UnusedZero and
// HasFreeLSB optimizations for mozilla::Result (see specializations of
// UnusedZero/HasFreeLSB below).
enum class AbortReason : uint8_t {
  NoAbort,
  Alloc = 2,
  Disable = 4,
  Error = 6,
};
}  // namespace jit
}  // namespace js

namespace mozilla::detail {

template <>
struct UnusedZero<js::jit::AbortReason> : UnusedZeroEnum<js::jit::AbortReason> {
};

template <>
struct HasFreeLSB<js::jit::AbortReason> {
  static const bool value = true;
};

}  // namespace mozilla::detail

namespace js {
namespace jit {

template <typename V>
using AbortReasonOr = mozilla::Result<V, AbortReason>;
using mozilla::Err;
using mozilla::Ok;

static_assert(sizeof(AbortReasonOr<Ok>) <= sizeof(uintptr_t),
              "Unexpected size of AbortReasonOr<Ok>");
static_assert(mozilla::detail::SelectResultImpl<bool, AbortReason>::value ==
              mozilla::detail::PackingStrategy::NullIsOk);
static_assert(sizeof(AbortReasonOr<bool>) <= sizeof(uintptr_t),
              "Unexpected size of AbortReasonOr<bool>");
static_assert(sizeof(AbortReasonOr<uint16_t*>) == sizeof(uintptr_t),
              "Unexpected size of AbortReasonOr<uint16_t*>");

// A JIT context is needed for parts of the compiler backend such as the
// MacroAssembler. It points to the JSContext (nullptr for off-thread
// compilations or Wasm compilations) and also stores some extra information,
// most of it only used in debug builds.
//
// JIT contexts must not be nested.

class MOZ_RAII JitContext {
#ifdef DEBUG
  // Whether this thread is actively Baseline compiling offthread
  bool inBaselineBackend_ = false;

  // Whether this thread is actively Ion compiling (does not include Wasm or
  // WarpOracle).
  bool inIonBackend_ = false;

  bool isCompilingWasm_ = false;
  bool oom_ = false;
#endif

 public:
  // Running context when compiling on the main thread. Not available during
  // off-thread compilation.
  JSContext* cx = nullptr;

  // Wrapper with information about the current runtime. nullptr for Wasm
  // compilations.
  CompileRuntime* runtime = nullptr;

  // Constructor for compilations happening on the main thread.
  explicit JitContext(JSContext* cx);

  // Constructor for off-thread Ion compilations.
  explicit JitContext(CompileRuntime* rt);

  // Constructor for Wasm compilation.
  JitContext();

  ~JitContext();

#ifdef DEBUG
  bool isCompilingWasm() { return isCompilingWasm_; }
  bool setIsCompilingWasm(bool flag) {
    bool oldFlag = isCompilingWasm_;
    isCompilingWasm_ = flag;
    return oldFlag;
  }
  bool hasOOM() { return oom_; }
  void setOOM() { oom_ = true; }

  bool inBaselineBackend() const { return inBaselineBackend_; }
  bool inIonBackend() const { return inIonBackend_; }

  void enterBaselineBackend() {
    MOZ_ASSERT(!inBaselineBackend_);
    inBaselineBackend_ = true;
  }
  void leaveBaselineBackend() {
    MOZ_ASSERT(inBaselineBackend_);
    inBaselineBackend_ = false;
  }
  void enterIonBackend() {
    MOZ_ASSERT(!inIonBackend_);
    inIonBackend_ = true;
  }
  void leaveIonBackend() {
    MOZ_ASSERT(inIonBackend_);
    inIonBackend_ = false;
  }
#endif
};

// Process-wide initialization and shutdown of JIT data structures.
[[nodiscard]] bool InitializeJit();
void ShutdownJit();

// Get and set the current JIT context.
JitContext* GetJitContext();
JitContext* MaybeGetJitContext();

void SetJitContext(JitContext* ctx);

enum JitExecStatus {
  // The method call had to be aborted due to a stack limit check. This
  // error indicates that Ion never attempted to clean up frames.
  JitExec_Aborted,

  // The method call resulted in an error, and IonMonkey has cleaned up
  // frames.
  JitExec_Error,

  // The method call succeeded and returned a value.
  JitExec_Ok
};

static inline bool IsErrorStatus(JitExecStatus status) {
  return status == JitExec_Error || status == JitExec_Aborted;
}

bool JitSupportsWasmSimd();
bool JitSupportsAtomics();

}  // namespace jit
}  // namespace js

#endif /* jit_JitContext_h */
