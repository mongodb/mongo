/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/FrontendContext.h"

#ifdef _WIN32
#  include <windows.h>
#  include <process.h>  // GetCurrentThreadId
#else
#  include <pthread.h>  // pthread_self
#endif

#include "gc/GC.h"
#include "js/AllocPolicy.h"  // js::ReportOutOfMemory
#include "js/friend/StackLimits.h"  // js::ReportOverRecursed, js::MinimumStackLimitMargin
#include "js/Modules.h"
#include "util/DifferentialTesting.h"
#include "util/NativeStack.h"  // GetNativeStackBase
#include "vm/JSContext.h"

using namespace js;

void FrontendErrors::clearErrors() {
  error.reset();
  warnings.clear();
  overRecursed = false;
  outOfMemory = false;
  allocationOverflow = false;
}

void FrontendErrors::clearWarnings() { warnings.clear(); }

void FrontendAllocator::reportAllocationOverflow() {
  fc_->onAllocationOverflow();
}

void* FrontendAllocator::onOutOfMemory(AllocFunction allocFunc,
                                       arena_id_t arena, size_t nbytes,
                                       void* reallocPtr) {
  return fc_->onOutOfMemory(allocFunc, arena, nbytes, reallocPtr);
}

FrontendContext::~FrontendContext() {
  if (ownNameCollectionPool_) {
    MOZ_ASSERT(nameCollectionPool_);
    js_delete(nameCollectionPool_);
  }
}

void FrontendContext::setStackQuota(JS::NativeStackSize stackSize) {
#ifdef __wasi__
  stackLimit_ = JS::WASINativeStackLimit;
#else   // __wasi__
  if (stackSize == 0) {
    stackLimit_ = JS::NativeStackLimitMax;
  } else {
    stackLimit_ = JS::GetNativeStackLimit(GetNativeStackBase(), stackSize - 1);
  }
#endif  // !__wasi__

#ifdef DEBUG
  setNativeStackLimitThread();
#endif
}

bool FrontendContext::allocateOwnedPool() {
  MOZ_ASSERT(!nameCollectionPool_);

  nameCollectionPool_ = js_new<frontend::NameCollectionPool>();
  if (!nameCollectionPool_) {
    return false;
  }
  ownNameCollectionPool_ = true;
  return true;
}

bool FrontendContext::hadErrors() const {
  // All errors must be reported to FrontendContext.
  MOZ_ASSERT_IF(maybeCx_, !maybeCx_->isExceptionPending());

  return errors_.hadErrors();
}

void FrontendContext::clearErrors() {
  MOZ_ASSERT(!maybeCx_);
  return errors_.clearErrors();
}

void FrontendContext::clearWarnings() { return errors_.clearWarnings(); }

void* FrontendContext::onOutOfMemory(AllocFunction allocFunc, arena_id_t arena,
                                     size_t nbytes, void* reallocPtr) {
  addPendingOutOfMemory();
  return nullptr;
}

void FrontendContext::onAllocationOverflow() {
  errors_.allocationOverflow = true;
}

void FrontendContext::onOutOfMemory() { addPendingOutOfMemory(); }

void FrontendContext::onOverRecursed() { errors_.overRecursed = true; }

void FrontendContext::recoverFromOutOfMemory() {
  MOZ_ASSERT_IF(maybeCx_, !maybeCx_->isThrowingOutOfMemory());

  errors_.outOfMemory = false;
}

const JSErrorFormatString* FrontendContext::gcSafeCallback(
    JSErrorCallback callback, void* userRef, const unsigned errorNumber) {
  mozilla::Maybe<gc::AutoSuppressGC> suppressGC;
  if (maybeCx_) {
    suppressGC.emplace(maybeCx_);
  }

  return callback(userRef, errorNumber);
}

void FrontendContext::reportError(CompileError&& err) {
  if (errors_.error) {
    errors_.error.reset();
  }

  // When compiling off thread, save the error so that the thread finishing the
  // parse can report it later.
  errors_.error.emplace(std::move(err));
}

bool FrontendContext::reportWarning(CompileError&& err) {
  if (!errors_.warnings.append(std::move(err))) {
    ReportOutOfMemory();
    return false;
  }

  return true;
}

void FrontendContext::ReportOutOfMemory() {
  /*
   * OOMs are non-deterministic, especially across different execution modes
   * (e.g. interpreter vs JIT). When doing differential testing, print to
   * stderr so that the fuzzers can detect this.
   */
  if (SupportDifferentialTesting()) {
    fprintf(stderr, "ReportOutOfMemory called\n");
  }

  addPendingOutOfMemory();
}

void FrontendContext::addPendingOutOfMemory() { errors_.outOfMemory = true; }

void FrontendContext::setCurrentJSContext(JSContext* cx) {
  MOZ_ASSERT(!nameCollectionPool_);

  maybeCx_ = cx;
  nameCollectionPool_ = &cx->frontendCollectionPool();
  scriptDataTableHolder_ = &cx->runtime()->scriptDataTableHolder();
  stackLimit_ = cx->stackLimitForCurrentPrincipal();

#ifdef DEBUG
  setNativeStackLimitThread();
#endif
}

bool FrontendContext::convertToRuntimeError(
    JSContext* cx, Warning warning /* = Warning::Report */) {
  // Report out of memory errors eagerly, or errors could be malformed.
  if (hadOutOfMemory()) {
    js::ReportOutOfMemory(cx);
    return false;
  }

  if (maybeError()) {
    if (!maybeError()->throwError(cx)) {
      return false;
    }
  }
  if (warning == Warning::Report) {
    for (CompileError& error : warnings()) {
      if (!error.throwError(cx)) {
        return false;
      }
    }
  }
  if (hadOverRecursed()) {
    js::ReportOverRecursed(cx);
  }
  if (hadAllocationOverflow()) {
    js::ReportAllocationOverflow(cx);
  }

  MOZ_ASSERT(!extraBindingsAreNotUsed(),
             "extraBindingsAreNotUsed shouldn't escape from FrontendContext");
  return true;
}

#ifdef DEBUG
static size_t GetTid() {
#  if defined(_WIN32)
  return size_t(GetCurrentThreadId());
#  else
  return size_t(pthread_self());
#  endif
}

void FrontendContext::setNativeStackLimitThread() {
  stackLimitThreadId_.emplace(GetTid());
}

void FrontendContext::assertNativeStackLimitThread() {
  if (!stackLimitThreadId_.isSome()) {
    return;
  }

  MOZ_ASSERT(*stackLimitThreadId_ == GetTid());
}
#endif

#ifdef __wasi__
void FrontendContext::incWasiRecursionDepth() {
  if (maybeCx_) {
    IncWasiRecursionDepth(maybeCx_);
  }
}

void FrontendContext::decWasiRecursionDepth() {
  if (maybeCx_) {
    DecWasiRecursionDepth(maybeCx_);
  }
}

bool FrontendContext::checkWasiRecursionLimit() {
  if (maybeCx_) {
    return CheckWasiRecursionLimit(maybeCx_);
  }
  return true;
}

JS_PUBLIC_API void js::IncWasiRecursionDepth(FrontendContext* fc) {
  fc->incWasiRecursionDepth();
}

JS_PUBLIC_API void js::DecWasiRecursionDepth(FrontendContext* fc) {
  fc->decWasiRecursionDepth();
}

JS_PUBLIC_API bool js::CheckWasiRecursionLimit(FrontendContext* fc) {
  return fc->checkWasiRecursionLimit();
}
#endif  // __wasi__

FrontendContext* js::NewFrontendContext() {
  UniquePtr<FrontendContext> fc = MakeUnique<FrontendContext>();
  if (!fc) {
    return nullptr;
  }

  if (!fc->allocateOwnedPool()) {
    return nullptr;
  }

  return fc.release();
}

void js::DestroyFrontendContext(FrontendContext* fc) { js_delete_poison(fc); }

#ifdef DEBUG
void FrontendContext::checkAndUpdateFrontendContextRecursionLimit(void* sp) {
  // For the js::MinimumStackLimitMargin to be effective, it should be larger
  // than the largest stack space which might be consumed by successive calls
  // to AutoCheckRecursionLimit::check.
  //
  // This function asserts that this property holds by recalling the stack
  // pointer of the previous call and comparing the consumed stack size with
  // the minimum margin.
  //
  // If this property does not hold, either the stack limit should be increased
  // or more calls to check for recursion should be added.
  if (previousStackPointer_ != nullptr) {
#  if JS_STACK_GROWTH_DIRECTION > 0
    if (sp > previousStackPointer_) {
      size_t diff = uintptr_t(sp) - uintptr_t(previousStackPointer_);
      MOZ_ASSERT(diff < js::MinimumStackLimitMargin);
    }
#  else
    if (sp < previousStackPointer_) {
      size_t diff = uintptr_t(previousStackPointer_) - uintptr_t(sp);
      MOZ_ASSERT(diff < js::MinimumStackLimitMargin);
    }
#  endif
  }
  previousStackPointer_ = sp;
}

void js::CheckAndUpdateFrontendContextRecursionLimit(FrontendContext* fc,
                                                     void* sp) {
  fc->checkAndUpdateFrontendContextRecursionLimit(sp);
}
#endif
