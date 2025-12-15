/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_Try_h
#define mozilla_Try_h

#include "mozilla/Result.h"

/**
 * MOZ_TRY(expr) is the C++ equivalent of Rust's `target = try!(expr);`, using
 * gcc's statement expressions [0]. First, it evaluates expr, which must produce
 * a Result value. On success, the result's success value is 'returned' as
 * rvalue. On error, immediately returns the error result. This pattern allows
 * to directly assign the success value:
 *
 * ```
 * SuccessValue val = MOZ_TRY(Func());
 * ```
 *
 * Where `Func()` returns a `Result<SuccessValue, E>` and is called in a
 * function that returns `Result<T, E>`.
 *
 * [0]: https://gcc.gnu.org/onlinedocs/gcc/Statement-Exprs.html
 */
#ifndef _MSC_VER

#define MOZ_TRY(expr)                                     \
  __extension__({                                         \
    auto mozTryVarTempResult = ::mozilla::ToResult(expr); \
    if (MOZ_UNLIKELY(mozTryVarTempResult.isErr())) {      \
      return mozTryVarTempResult.propagateErr();          \
    }                                                     \
    mozTryVarTempResult.unwrap();                         \
  })

#else

/**
 * MONGODB MODIFICATION: statement expressions are a gcc extension
 * unsupported by MSVC, reimplement with standard C++.
 *
 * This implementation doesn't return the result of expr,
 * so MOZ_TRY_VAR should be used instead. If this is necessary
 * behavior, consider implementing using a lambda
 */
#define MOZ_TRY(expr)                                   \
  do {                                                  \
    auto mozTryTempResult_ = ::mozilla::ToResult(expr); \
    if (MOZ_UNLIKELY(mozTryTempResult_.isErr())) {      \
      return mozTryTempResult_.propagateErr();          \
    }                                                   \
  } while (0)

#endif  // _MSC_VER

/**
 * MOZ_TRY_VAR(target, expr) is the C++ equivalent of Rust's `target =
 * try!(expr);`. First, it evaluates expr, which must produce a Result value. On
 * success, the result's success value is assigned to target. On error,
 * immediately returns the error result. |target| must be an lvalue.
 *
 * This macro is obsolete and its usages should be replaced with `MOZ_TRY`.
 */
#ifndef _MSC_VER

#define MOZ_TRY_VAR(target, expr) (target) = MOZ_TRY(expr);

#else

/**
 * MONGODB MODIFICATION: statement expressions are a gcc extension
 * unsupported by MSVC, reimplement with standard C++.
 */
#define MOZ_TRY(expr)                                   \
  do {                                                  \
    auto mozTryTempResult_ = ::mozilla::ToResult(expr); \
    if (MOZ_UNLIKELY(mozTryTempResult_.isErr())) {      \
      return mozTryTempResult_.propagateErr();          \
    }                                                   \
  } while (0)

#define MOZ_TRY_VAR(target, expr)                     \
  do {                                                \
    auto mozTryVarTempResult_ = (expr);               \
    if (MOZ_UNLIKELY(mozTryVarTempResult_.isErr())) { \
      return mozTryVarTempResult_.propagateErr();     \
    }                                                 \
    (target) = mozTryVarTempResult_.unwrap();         \
  } while (0)

#endif  // _MSC_VER

#endif  // mozilla_Try_h
