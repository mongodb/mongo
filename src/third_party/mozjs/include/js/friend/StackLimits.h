/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_friend_StackLimits_h
#define js_friend_StackLimits_h

#include "mozilla/Attributes.h"  // MOZ_ALWAYS_INLINE, MOZ_COLD
#include "mozilla/Likely.h"      // MOZ_LIKELY

#include <stddef.h>  // size_t
#include <stdint.h>  // uintptr_t

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/HeapAPI.h"  // JS::StackKind, JS::StackForTrustedScript, JS::StackForUntrustedScript
#include "js/RootingAPI.h"  // JS::RootingContext
#include "js/Utility.h"     // JS_STACK_OOM_POSSIBLY_FAIL

struct JS_PUBLIC_API JSContext;

#ifndef JS_STACK_GROWTH_DIRECTION
#  ifdef __hppa
#    define JS_STACK_GROWTH_DIRECTION (1)
#  else
#    define JS_STACK_GROWTH_DIRECTION (-1)
#  endif
#endif

namespace js {

// AutoCheckRecursionLimit can be used to check whether we're close to using up
// the C++ stack.
//
// Typical usage is like this:
//
//   AutoCheckRecursionLimit recursion(cx);
//   if (!recursion.check(cx)) {
//     return false;
//   }
//
// The check* functions return |false| if we are close to the stack limit.
// They also report an overrecursion error, except for the DontReport variants.
//
// The checkSystem variant gives us a little extra space so we can ensure that
// crucial code is able to run.
//
// checkConservative allows less space than any other check, including a safety
// buffer (as in, it uses the untrusted limit and subtracts a little more from
// it).
class MOZ_RAII AutoCheckRecursionLimit {
  [[nodiscard]] MOZ_ALWAYS_INLINE bool checkLimitImpl(uintptr_t limit,
                                                      void* sp) const;

  MOZ_ALWAYS_INLINE uintptr_t getStackLimitSlow(JSContext* cx) const;
  MOZ_ALWAYS_INLINE uintptr_t getStackLimitHelper(JSContext* cx,
                                                  JS::StackKind kind,
                                                  int extraAllowance) const;

  JS_PUBLIC_API bool runningWithTrustedPrincipals(JSContext* cx) const;

#ifdef __wasi__
  // The JSContext outlives AutoCheckRecursionLimit so it is safe to use raw
  // pointer here.
  JSContext* cx_;
#endif  // __wasi__

 public:
  explicit MOZ_ALWAYS_INLINE AutoCheckRecursionLimit(JSContext* cx) {
#ifdef __wasi__
    cx_ = cx;
    ++JS::RootingContext::get(cx_)->wasiRecursionDepth;
#endif  // __wasi__
  }

  MOZ_ALWAYS_INLINE ~AutoCheckRecursionLimit() {
#ifdef __wasi__
    MOZ_ASSERT(JS::RootingContext::get(cx_)->wasiRecursionDepth > 0);
    --JS::RootingContext::get(cx_)->wasiRecursionDepth;
#endif  // __wasi__
  }

  AutoCheckRecursionLimit(const AutoCheckRecursionLimit&) = delete;
  void operator=(const AutoCheckRecursionLimit&) = delete;

  [[nodiscard]] MOZ_ALWAYS_INLINE bool check(JSContext* cx) const;
  [[nodiscard]] MOZ_ALWAYS_INLINE bool checkDontReport(JSContext* cx) const;
  [[nodiscard]] MOZ_ALWAYS_INLINE bool checkWithExtra(JSContext* cx,
                                                      size_t extra) const;
  [[nodiscard]] MOZ_ALWAYS_INLINE bool checkWithStackPointerDontReport(
      JSContext* cx, void* sp) const;

  [[nodiscard]] MOZ_ALWAYS_INLINE bool checkConservative(JSContext* cx) const;
  [[nodiscard]] MOZ_ALWAYS_INLINE bool checkConservativeDontReport(
      JSContext* cx) const;

  [[nodiscard]] MOZ_ALWAYS_INLINE bool checkSystem(JSContext* cx) const;
  [[nodiscard]] MOZ_ALWAYS_INLINE bool checkSystemDontReport(
      JSContext* cx) const;
};

extern MOZ_COLD JS_PUBLIC_API void ReportOverRecursed(JSContext* maybecx);

MOZ_ALWAYS_INLINE bool AutoCheckRecursionLimit::checkLimitImpl(uintptr_t limit,
                                                               void* sp) const {
  JS_STACK_OOM_POSSIBLY_FAIL();

#ifdef __wasi__
  // WASI has two limits:
  // 1) The stack pointer in linear memory that grows to zero. See --stack-first
  // in js/src/shell/moz.build. 2) The wasiRecursionDepth_ that counts recursion
  // depth. Here we should check both.
  if (JS::RootingContext::get(cx_)->wasiRecursionDepth >=
      JS::RootingContext::wasiRecursionDepthLimit) {
    return false;
  }
#endif  // __wasi__

#if JS_STACK_GROWTH_DIRECTION > 0
  return MOZ_LIKELY(uintptr_t(sp) < limit);
#else
  return MOZ_LIKELY(uintptr_t(sp) > limit);
#endif
}

MOZ_ALWAYS_INLINE uintptr_t
AutoCheckRecursionLimit::getStackLimitSlow(JSContext* cx) const {
  JS::StackKind kind = runningWithTrustedPrincipals(cx)
                           ? JS::StackForTrustedScript
                           : JS::StackForUntrustedScript;
  return getStackLimitHelper(cx, kind, 0);
}

MOZ_ALWAYS_INLINE uintptr_t AutoCheckRecursionLimit::getStackLimitHelper(
    JSContext* cx, JS::StackKind kind, int extraAllowance) const {
  uintptr_t limit = JS::RootingContext::get(cx)->nativeStackLimit[kind];
#if JS_STACK_GROWTH_DIRECTION > 0
  limit += extraAllowance;
#else
  limit -= extraAllowance;
#endif
  return limit;
}

MOZ_ALWAYS_INLINE bool AutoCheckRecursionLimit::check(JSContext* cx) const {
  if (MOZ_UNLIKELY(!checkDontReport(cx))) {
    ReportOverRecursed(cx);
    return false;
  }
  return true;
}

MOZ_ALWAYS_INLINE bool AutoCheckRecursionLimit::checkDontReport(
    JSContext* cx) const {
  int stackDummy;
  return checkWithStackPointerDontReport(cx, &stackDummy);
}

MOZ_ALWAYS_INLINE bool AutoCheckRecursionLimit::checkWithStackPointerDontReport(
    JSContext* cx, void* sp) const {
  // getStackLimitSlow(cx) is pretty slow because it has to do an uninlined
  // call to runningWithTrustedPrincipals to determine which stack limit to
  // use. To work around this, check the untrusted limit first to avoid the
  // overhead in most cases.
  uintptr_t untrustedLimit =
      getStackLimitHelper(cx, JS::StackForUntrustedScript, 0);
  if (MOZ_LIKELY(checkLimitImpl(untrustedLimit, sp))) {
    return true;
  }
  return checkLimitImpl(getStackLimitSlow(cx), sp);
}

MOZ_ALWAYS_INLINE bool AutoCheckRecursionLimit::checkWithExtra(
    JSContext* cx, size_t extra) const {
  char stackDummy;
  char* sp = &stackDummy;
#if JS_STACK_GROWTH_DIRECTION > 0
  sp += extra;
#else
  sp -= extra;
#endif
  if (MOZ_UNLIKELY(!checkWithStackPointerDontReport(cx, sp))) {
    ReportOverRecursed(cx);
    return false;
  }
  return true;
}

MOZ_ALWAYS_INLINE bool AutoCheckRecursionLimit::checkSystem(
    JSContext* cx) const {
  if (MOZ_UNLIKELY(!checkSystemDontReport(cx))) {
    ReportOverRecursed(cx);
    return false;
  }
  return true;
}

MOZ_ALWAYS_INLINE bool AutoCheckRecursionLimit::checkSystemDontReport(
    JSContext* cx) const {
  uintptr_t limit = getStackLimitHelper(cx, JS::StackForSystemCode, 0);
  int stackDummy;
  return checkLimitImpl(limit, &stackDummy);
}

MOZ_ALWAYS_INLINE bool AutoCheckRecursionLimit::checkConservative(
    JSContext* cx) const {
  if (MOZ_UNLIKELY(!checkConservativeDontReport(cx))) {
    ReportOverRecursed(cx);
    return false;
  }
  return true;
}

MOZ_ALWAYS_INLINE bool AutoCheckRecursionLimit::checkConservativeDontReport(
    JSContext* cx) const {
  uintptr_t limit = getStackLimitHelper(cx, JS::StackForUntrustedScript,
                                        -1024 * int(sizeof(size_t)));
  int stackDummy;
  return checkLimitImpl(limit, &stackDummy);
}

}  // namespace js

#endif  // js_friend_StackLimits_h
