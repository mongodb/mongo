/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/FrontendContext.h"

#include "gc/GC.h"
#include "js/AllocPolicy.h"         // js::ReportOutOfMemory
#include "js/friend/StackLimits.h"  // js::ReportOverRecursed
#include "js/Modules.h"
#include "util/DifferentialTesting.h"
#include "util/NativeStack.h"  // GetNativeStackBase
#include "vm/JSContext.h"

using namespace js;

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

bool FrontendContext::setSupportedImportAssertions(
    const JS::ImportAssertionVector& supportedImportAssertions) {
  MOZ_ASSERT(supportedImportAssertions_.empty());
  if (!supportedImportAssertions_.appendAll(supportedImportAssertions)) {
    ReportOutOfMemory();
    return false;
  }
  return true;
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
  if (maybeCx_) {
    if (maybeCx_->isExceptionPending()) {
      return true;
    }
  }

  return errors_.hadErrors();
}

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
  // TODO: Remove this branch once error report directly against JSContext is
  //       removed from the frontend code.
  if (maybeCx_) {
    maybeCx_->recoverFromOutOfMemory();
  }

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
  return true;
}

void FrontendContext::linkWithJSContext(JSContext* cx) {
  if (cx) {
    cx->setFrontendErrors(&errors_);
  }
}

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
