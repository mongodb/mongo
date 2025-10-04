/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * [SMDOC] JS::Result
 *
 * `Result` is used as the return type of many SpiderMonkey functions that
 * can either succeed or fail. See "/mfbt/Result.h".
 *
 *
 * ## Which return type to use
 *
 * `Result` is for return values. Obviously, if you're writing a function that
 * can't fail, don't use Result. Otherwise:
 *
 *     JS::Result<>  - function can fail, doesn't return anything on success
 *         (defaults to `JS::Result<JS::Ok, JS::Error>`)
 *     JS::Result<JS::Ok, JS::OOM> - like JS::Result<>, but fails only on OOM
 *
 *     JS::Result<Data>  - function can fail, returns Data on success
 *     JS::Result<Data, JS::OOM>  - returns Data, fails only on OOM
 *
 *     mozilla::GenericErrorResult<JS::Error> - always fails
 *
 * That last type is like a Result with no success type. It's used for
 * functions like `js::ReportNotFunction` that always return an error
 * result. `GenericErrorResult<E>` implicitly converts to `Result<V, E>`,
 * regardless of V.
 *
 *
 * ## Checking Results when your return type is Result
 *
 * When you call a function that returns a `Result`, use the `MOZ_TRY` macro to
 * check for errors:
 *
 *     MOZ_TRY(DefenestrateObject(cx, obj));
 *
 * If `DefenestrateObject` returns a success result, `MOZ_TRY` is done, and
 * control flows to the next statement. If `DefenestrateObject` returns an
 * error result, `MOZ_TRY` will immediately return it, propagating the error to
 * your caller. It's kind of like exceptions, but more explicit -- you can see
 * in the code exactly where errors can happen.
 *
 * You can do a tail call instead of using `MOZ_TRY`:
 *
 *     return DefenestrateObject(cx, obj);
 *
 * Indicate success with `return Ok();`.
 *
 * If the function returns a value on success, use `MOZ_TRY_VAR` to get it:
 *
 *     RootedValue thrug(cx);
 *     MOZ_TRY_VAR(thrug, GetObjectThrug(cx, obj));
 *
 * This behaves the same as `MOZ_TRY` on error. On success, the success
 * value of `GetObjectThrug(cx, obj)` is assigned to the variable `thrug`.
 *
 *
 * ## GC safety
 *
 * When a function returns a `JS::Result<JSObject*>`, it is the program's
 * responsibility to check for errors and root the object before continuing:
 *
 *     RootedObject wrapper(cx);
 *     MOZ_TRY_VAR(wrapper, Enwrapify(cx, thing));
 *
 * This is ideal. On error, there is no object to root; on success, the
 * assignment to wrapper roots it. GC safety is ensured.
 *
 * `Result` has methods .isOk(), .isErr(), .unwrap(), and .unwrapErr(), but if
 * you're actually using them, it's possible to create a GC hazard. The static
 * analysis will catch it if so, but that's hardly convenient. So try to stick
 * to the idioms shown above.
 *
 *
 * ## Future directions
 *
 * At present, JS::Error and JS::OOM are empty structs. The plan is to make them
 * GC things that contain the actual error information (including the exception
 * value and a saved stack).
 *
 * The long-term plan is to remove JS_IsExceptionPending and
 * JS_GetPendingException in favor of JS::Error. Exception state will no longer
 * exist.
 */

#ifndef js_Result_h
#define js_Result_h

#include "mozilla/Result.h"

namespace JS {

using mozilla::Ok;

template <typename T>
struct UnusedZero;

/**
 * Type representing a JS error or exception. At the moment this only
 * "represents" an error in a rather abstract way.
 */
struct Error {
  // Since we claim UnusedZero<Error>::value and HasFreeLSB<Error>::value ==
  // true below, we must only use positive even enum values.
  enum class ErrorKind : uintptr_t { Unspecified = 2, OOM = 4 };

  const ErrorKind kind = ErrorKind::Unspecified;

  Error() = default;

 protected:
  friend struct UnusedZero<Error>;

  constexpr MOZ_IMPLICIT Error(ErrorKind aKind) : kind(aKind) {}
};

struct OOM : Error {
  constexpr OOM() : Error(ErrorKind::OOM) {}

 protected:
  friend struct UnusedZero<OOM>;

  using Error::Error;
};

template <typename T>
struct UnusedZero {
  using StorageType = std::underlying_type_t<Error::ErrorKind>;

  static constexpr bool value = true;
  static constexpr StorageType nullValue = 0;

  static constexpr void AssertValid(StorageType aValue) {}
  static constexpr T Inspect(const StorageType& aValue) {
    return static_cast<Error::ErrorKind>(aValue);
  }
  static constexpr T Unwrap(StorageType aValue) {
    return static_cast<Error::ErrorKind>(aValue);
  }
  static constexpr StorageType Store(T aValue) {
    return static_cast<StorageType>(aValue.kind);
  }
};

}  // namespace JS

namespace mozilla::detail {

template <>
struct UnusedZero<JS::Error> : JS::UnusedZero<JS::Error> {};

template <>
struct UnusedZero<JS::OOM> : JS::UnusedZero<JS::OOM> {};

template <>
struct HasFreeLSB<JS::Error> {
  static const bool value = true;
};

template <>
struct HasFreeLSB<JS::OOM> {
  static const bool value = true;
};
}  // namespace mozilla::detail

namespace JS {

/**
 * `Result` is intended to be the return type of JSAPI calls and internal
 * functions that can run JS code or allocate memory from the JS GC heap. Such
 * functions can:
 *
 * -   succeed, possibly returning a value;
 *
 * -   fail with a JS exception (out-of-memory falls in this category); or
 *
 * -   fail because JS execution was terminated, which occurs when e.g. a
 *     user kills a script from the "slow script" UI. This is also how we
 *     unwind the stack when the debugger forces the current function to
 *     return. JS `catch` blocks can't catch this kind of failure,
 *     and JS `finally` blocks don't execute.
 */
template <typename V = Ok, typename E = Error>
using Result = mozilla::Result<V, E>;

static_assert(sizeof(Result<>) == sizeof(uintptr_t),
              "Result<> should be pointer-sized");

static_assert(sizeof(Result<int*, Error>) == sizeof(uintptr_t),
              "Result<V*, Error> should be pointer-sized");

}  // namespace JS

#endif  // js_Result_h
