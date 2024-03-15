/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_FrontendContext_h
#define frontend_FrontendContext_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT
#include "mozilla/Attributes.h"  // MOZ_STACK_CLASS
#include "mozilla/Maybe.h"       // mozilla::Maybe

#include <stddef.h>  // size_t

#include "js/AllocPolicy.h"  // SystemAllocPolicy, AllocFunction
#include "js/ErrorReport.h"  // JSErrorCallback, JSErrorFormatString
#include "js/Modules.h"      // JS::ImportAssertionVector
#include "js/Stack.h"  // JS::NativeStackSize, JS::NativeStackLimit, JS::NativeStackLimitMax
#include "js/Vector.h"          // Vector
#include "vm/ErrorReporting.h"  // CompileError
#include "vm/MallocProvider.h"  // MallocProvider
#include "vm/SharedScriptDataTableHolder.h"  // js::SharedScriptDataTableHolder, js::globalSharedScriptDataTableHolder

struct JSContext;

namespace js {

class FrontendContext;

namespace frontend {
class NameCollectionPool;
}  // namespace frontend

struct FrontendErrors {
  FrontendErrors() = default;
  // Any errors or warnings produced during compilation. These are reported
  // when finishing the script.
  mozilla::Maybe<CompileError> error;
  Vector<CompileError, 0, SystemAllocPolicy> warnings;
  bool overRecursed = false;
  bool outOfMemory = false;
  bool allocationOverflow = false;

  bool hadErrors() const {
    return outOfMemory || overRecursed || allocationOverflow || error;
  }
};

class FrontendAllocator : public MallocProvider<FrontendAllocator> {
 private:
  FrontendContext* const fc_;

 public:
  explicit FrontendAllocator(FrontendContext* fc) : fc_(fc) {}

  void* onOutOfMemory(js::AllocFunction allocFunc, arena_id_t arena,
                      size_t nbytes, void* reallocPtr = nullptr);
  void reportAllocationOverflow();
};

class FrontendContext {
 private:
  FrontendAllocator alloc_;
  js::FrontendErrors errors_;

  // NameCollectionPool can be either:
  //   * owned by this FrontendContext, or
  //   * borrowed from JSContext
  frontend::NameCollectionPool* nameCollectionPool_;
  bool ownNameCollectionPool_;

  js::SharedScriptDataTableHolder* scriptDataTableHolder_;

  JS::ImportAssertionVector supportedImportAssertions_;

  JS::NativeStackLimit stackLimit_ = JS::NativeStackLimitMax;

 protected:
  // (optional) Current JSContext to support main-thread-specific
  // handling for error reporting, GC, and memory allocation.
  //
  // Set by setCurrentJSContext.
  JSContext* maybeCx_ = nullptr;

 public:
  FrontendContext()
      : alloc_(this),
        nameCollectionPool_(nullptr),
        ownNameCollectionPool_(false),
        scriptDataTableHolder_(&js::globalSharedScriptDataTableHolder),
        supportedImportAssertions_() {}
  ~FrontendContext();

  void setStackQuota(JS::NativeStackSize stackSize);
  JS::NativeStackLimit stackLimit() const { return stackLimit_; }

  bool allocateOwnedPool();

  frontend::NameCollectionPool& nameCollectionPool() {
    MOZ_ASSERT(
        nameCollectionPool_,
        "Either allocateOwnedPool or setCurrentJSContext must be called");
    return *nameCollectionPool_;
  }

  js::SharedScriptDataTableHolder* scriptDataTableHolder() {
    MOZ_ASSERT(scriptDataTableHolder_);
    return scriptDataTableHolder_;
  }

  FrontendAllocator* getAllocator() { return &alloc_; }

  // Use the given JSContext's for:
  //   * js::frontend::NameCollectionPool for reusing allocation
  //   * js::SharedScriptDataTableHolder for de-duplicating bytecode
  //     within given runtime
  //   * Copy the native stack limit from the JSContext
  //
  // And also this JSContext can be retrieved by maybeCurrentJSContext below.
  void setCurrentJSContext(JSContext* cx);

  // Returns JSContext if any.
  //
  // This can be used only for:
  //   * Main-thread-specific operation, such as operating on JSAtom
  //   * Optional operation, such as providing better error message
  JSContext* maybeCurrentJSContext() { return maybeCx_; }

  const JS::ImportAssertionVector& getSupportedImportAssertions() const {
    return supportedImportAssertions_;
  }
  bool setSupportedImportAssertions(
      const JS::ImportAssertionVector& supportedImportAssertions);

  enum class Warning { Suppress, Report };

  // Returns false if the error cannot be converted (such as due to OOM). An
  // error might still be reported to the given JSContext. Returns true
  // otherwise.
  bool convertToRuntimeError(JSContext* cx, Warning warning = Warning::Report);

  void linkWithJSContext(JSContext* cx);

  mozilla::Maybe<CompileError>& maybeError() { return errors_.error; }
  Vector<CompileError, 0, SystemAllocPolicy>& warnings() {
    return errors_.warnings;
  }

  // Report CompileErrors
  void reportError(js::CompileError&& err);
  bool reportWarning(js::CompileError&& err);

  // Report FrontendAllocator errors
  void* onOutOfMemory(js::AllocFunction allocFunc, arena_id_t arena,
                      size_t nbytes, void* reallocPtr = nullptr);
  void onAllocationOverflow();

  void onOutOfMemory();
  void onOverRecursed();

  void recoverFromOutOfMemory();

  const JSErrorFormatString* gcSafeCallback(JSErrorCallback callback,
                                            void* userRef,
                                            const unsigned errorNumber);

  // Status of errors reported to this FrontendContext
  bool hadOutOfMemory() const { return errors_.outOfMemory; }
  bool hadOverRecursed() const { return errors_.overRecursed; }
  bool hadAllocationOverflow() const { return errors_.allocationOverflow; }
  bool hadErrors() const;

#ifdef __wasi__
  void incWasiRecursionDepth();
  void decWasiRecursionDepth();
  bool checkWasiRecursionLimit();
#endif  // __wasi__

 private:
  void ReportOutOfMemory();
  void addPendingOutOfMemory();
};

// Automatically report any pending exception when leaving the scope.
class MOZ_STACK_CLASS AutoReportFrontendContext : public FrontendContext {
  // The target JSContext to report the errors to.
  JSContext* cx_;

  Warning warning_;

 public:
  explicit AutoReportFrontendContext(JSContext* cx,
                                     Warning warning = Warning::Report)
      : FrontendContext(), cx_(cx), warning_(warning) {
    setCurrentJSContext(cx_);
    MOZ_ASSERT(cx_ == maybeCx_);
  }

  ~AutoReportFrontendContext() {
    if (cx_) {
      convertToRuntimeErrorAndClear();
    }
  }

  void clearAutoReport() { cx_ = nullptr; }

  bool convertToRuntimeErrorAndClear() {
    bool result = convertToRuntimeError(cx_, warning_);
    cx_ = nullptr;
    return result;
  }
};

/*
 * Explicitly report any pending exception before leaving the scope.
 *
 * Before an instance of this class leaves the scope, you must call either
 * failure() (if there are exceptions to report) or ok() (if there are no
 * exceptions to report).
 */
class ManualReportFrontendContext : public FrontendContext {
  JSContext* cx_;
#ifdef DEBUG
  bool handled_ = false;
#endif

 public:
  explicit ManualReportFrontendContext(JSContext* cx)
      : FrontendContext(), cx_(cx) {
    setCurrentJSContext(cx_);
  }

  ~ManualReportFrontendContext() { MOZ_ASSERT(handled_); }

  void ok() {
#ifdef DEBUG
    handled_ = true;
#endif
  }

  void failure() {
#ifdef DEBUG
    handled_ = true;
#endif
    convertToRuntimeError(cx_);
  }
};

// Create function for FrontendContext, which is manually allocated and
// exclusively owned.
extern FrontendContext* NewFrontendContext();

// Destroy function for FrontendContext, which was allocated with
// NewFrontendContext.
extern void DestroyFrontendContext(FrontendContext* fc);

}  // namespace js

#endif /* frontend_FrontendContext_h */
