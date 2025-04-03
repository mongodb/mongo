/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_friend_StackLimits_h
#define js_friend_StackLimits_h

#include "mozilla/Attributes.h"  // MOZ_ALWAYS_INLINE, MOZ_COLD
#include "mozilla/Likely.h"      // MOZ_LIKELY
#include "mozilla/Variant.h"     // mozilla::Variant, mozilla::AsVariant

#include <stddef.h>  // size_t

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/HeapAPI.h"  // JS::StackKind, JS::StackForTrustedScript, JS::StackForUntrustedScript
#include "js/RootingAPI.h"  // JS::RootingContext
#include "js/Stack.h"       // JS::NativeStackLimit
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

class FrontendContext;

#ifdef __wasi__
extern MOZ_COLD JS_PUBLIC_API void IncWasiRecursionDepth(JSContext* cx);
extern MOZ_COLD JS_PUBLIC_API void DecWasiRecursionDepth(JSContext* cx);
extern MOZ_COLD JS_PUBLIC_API bool CheckWasiRecursionLimit(JSContext* cx);

extern MOZ_COLD JS_PUBLIC_API void IncWasiRecursionDepth(FrontendContext* fc);
extern MOZ_COLD JS_PUBLIC_API void DecWasiRecursionDepth(FrontendContext* fc);
extern MOZ_COLD JS_PUBLIC_API bool CheckWasiRecursionLimit(FrontendContext* fc);
#endif  // __wasi__

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
  [[nodiscard]] MOZ_ALWAYS_INLINE bool checkLimitImpl(
      JS::NativeStackLimit limit, void* sp) const;

  MOZ_ALWAYS_INLINE JS::NativeStackLimit getStackLimitSlow(JSContext* cx) const;
  MOZ_ALWAYS_INLINE JS::NativeStackLimit getStackLimitHelper(
      JSContext* cx, JS::StackKind kind, int extraAllowance) const;

  JS::NativeStackLimit getStackLimit(FrontendContext* fc) const;

  JS_PUBLIC_API JS::StackKind stackKindForCurrentPrincipal(JSContext* cx) const;

  JS_PUBLIC_API void assertMainThread(JSContext* cx) const;

#ifdef __wasi__
  // The JSContext outlives AutoCheckRecursionLimit so it is safe to use raw
  // pointer here.
  mozilla::Variant<JSContext*, FrontendContext*> context_;
#endif  // __wasi__

 public:
  explicit MOZ_ALWAYS_INLINE AutoCheckRecursionLimit(JSContext* cx)
#ifdef __wasi__
      : context_(mozilla::AsVariant(cx))
#endif  // __wasi__
  {
#ifdef __wasi__
    incWasiRecursionDepth();
#endif  // __wasi__
  }

  explicit MOZ_ALWAYS_INLINE AutoCheckRecursionLimit(FrontendContext* fc)
#ifdef __wasi__
      : context_(mozilla::AsVariant(fc))
#endif  // __wasi__
  {
#ifdef __wasi__
    incWasiRecursionDepth();
#endif  // __wasi__
  }

  MOZ_ALWAYS_INLINE ~AutoCheckRecursionLimit() {
#ifdef __wasi__
    decWasiRecursionDepth();
#endif  // __wasi__
  }

#ifdef __wasi__
  MOZ_ALWAYS_INLINE void incWasiRecursionDepth() {
    if (context_.is<JSContext*>()) {
      JSContext* cx = context_.as<JSContext*>();
      IncWasiRecursionDepth(cx);
    } else {
      FrontendContext* fc = context_.as<FrontendContext*>();
      IncWasiRecursionDepth(fc);
    }
  }

  MOZ_ALWAYS_INLINE void decWasiRecursionDepth() {
    if (context_.is<JSContext*>()) {
      JSContext* cx = context_.as<JSContext*>();
      DecWasiRecursionDepth(cx);
    } else {
      FrontendContext* fc = context_.as<FrontendContext*>();
      DecWasiRecursionDepth(fc);
    }
  }

  MOZ_ALWAYS_INLINE bool checkWasiRecursionLimit() const {
    if (context_.is<JSContext*>()) {
      JSContext* cx = context_.as<JSContext*>();
      if (!CheckWasiRecursionLimit(cx)) {
        return false;
      }
    } else {
      FrontendContext* fc = context_.as<FrontendContext*>();
      if (!CheckWasiRecursionLimit(fc)) {
        return false;
      }
    }

    return true;
  }
#endif  // __wasi__

  AutoCheckRecursionLimit(const AutoCheckRecursionLimit&) = delete;
  void operator=(const AutoCheckRecursionLimit&) = delete;

  [[nodiscard]] MOZ_ALWAYS_INLINE bool check(JSContext* cx) const;
  [[nodiscard]] MOZ_ALWAYS_INLINE bool check(FrontendContext* fc) const;
  [[nodiscard]] MOZ_ALWAYS_INLINE bool checkDontReport(JSContext* cx) const;
  [[nodiscard]] MOZ_ALWAYS_INLINE bool checkDontReport(
      FrontendContext* fc) const;
  [[nodiscard]] MOZ_ALWAYS_INLINE bool checkWithExtra(JSContext* cx,
                                                      size_t extra) const;
  [[nodiscard]] MOZ_ALWAYS_INLINE bool checkWithStackPointerDontReport(
      JSContext* cx, void* sp) const;
  [[nodiscard]] MOZ_ALWAYS_INLINE bool checkWithStackPointerDontReport(
      FrontendContext* fc, void* sp) const;

  [[nodiscard]] MOZ_ALWAYS_INLINE bool checkConservative(JSContext* cx) const;
  [[nodiscard]] MOZ_ALWAYS_INLINE bool checkConservativeDontReport(
      JSContext* cx) const;
  [[nodiscard]] MOZ_ALWAYS_INLINE bool checkConservativeDontReport(
      JS::NativeStackLimit limit) const;

  [[nodiscard]] MOZ_ALWAYS_INLINE bool checkSystem(JSContext* cx) const;
  [[nodiscard]] MOZ_ALWAYS_INLINE bool checkSystemDontReport(
      JSContext* cx) const;
};

extern MOZ_COLD JS_PUBLIC_API void ReportOverRecursed(JSContext* maybecx);
extern MOZ_COLD JS_PUBLIC_API void ReportOverRecursed(FrontendContext* fc);

MOZ_ALWAYS_INLINE bool AutoCheckRecursionLimit::checkLimitImpl(
    JS::NativeStackLimit limit, void* sp) const {
  JS_STACK_OOM_POSSIBLY_FAIL();

#ifdef __wasi__
  if (!checkWasiRecursionLimit()) {
    return false;
  }
#endif  // __wasi__

#if JS_STACK_GROWTH_DIRECTION > 0
  return MOZ_LIKELY(JS::NativeStackLimit(sp) < limit);
#else
  return MOZ_LIKELY(JS::NativeStackLimit(sp) > limit);
#endif
}

MOZ_ALWAYS_INLINE JS::NativeStackLimit
AutoCheckRecursionLimit::getStackLimitSlow(JSContext* cx) const {
  JS::StackKind kind = stackKindForCurrentPrincipal(cx);
  return getStackLimitHelper(cx, kind, 0);
}

MOZ_ALWAYS_INLINE JS::NativeStackLimit
AutoCheckRecursionLimit::getStackLimitHelper(JSContext* cx, JS::StackKind kind,
                                             int extraAllowance) const {
  assertMainThread(cx);
  JS::NativeStackLimit limit =
      JS::RootingContext::get(cx)->nativeStackLimit[kind];
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

MOZ_ALWAYS_INLINE bool AutoCheckRecursionLimit::check(
    FrontendContext* fc) const {
  if (MOZ_UNLIKELY(!checkDontReport(fc))) {
    ReportOverRecursed(fc);
    return false;
  }
  return true;
}

MOZ_ALWAYS_INLINE bool AutoCheckRecursionLimit::checkDontReport(
    JSContext* cx) const {
  int stackDummy;
  return checkWithStackPointerDontReport(cx, &stackDummy);
}

MOZ_ALWAYS_INLINE bool AutoCheckRecursionLimit::checkDontReport(
    FrontendContext* fc) const {
  int stackDummy;
  return checkWithStackPointerDontReport(fc, &stackDummy);
}

MOZ_ALWAYS_INLINE bool AutoCheckRecursionLimit::checkWithStackPointerDontReport(
    JSContext* cx, void* sp) const {
  // getStackLimitSlow(cx) is pretty slow because it has to do an uninlined
  // call to stackKindForCurrentPrincipal to determine which stack limit to
  // use. To work around this, check the untrusted limit first to avoid the
  // overhead in most cases.
  JS::NativeStackLimit untrustedLimit =
      getStackLimitHelper(cx, JS::StackForUntrustedScript, 0);
  if (MOZ_LIKELY(checkLimitImpl(untrustedLimit, sp))) {
    return true;
  }
  return checkLimitImpl(getStackLimitSlow(cx), sp);
}

MOZ_ALWAYS_INLINE bool AutoCheckRecursionLimit::checkWithStackPointerDontReport(
    FrontendContext* fc, void* sp) const {
  return checkLimitImpl(getStackLimit(fc), sp);
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
  JS::NativeStackLimit limit =
      getStackLimitHelper(cx, JS::StackForSystemCode, 0);
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
  JS::NativeStackLimit limit = getStackLimitHelper(
      cx, JS::StackForUntrustedScript, -4096 * int(sizeof(size_t)));
  int stackDummy;
  return checkLimitImpl(limit, &stackDummy);
}

MOZ_ALWAYS_INLINE bool AutoCheckRecursionLimit::checkConservativeDontReport(
    JS::NativeStackLimit limit) const {
  int stackDummy;
  return checkLimitImpl(limit, &stackDummy);
}

}  // namespace js

#endif  // js_friend_StackLimits_h
