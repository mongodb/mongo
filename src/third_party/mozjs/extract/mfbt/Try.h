/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_Try_h
#define mozilla_Try_h

#include "mozilla/Result.h"

/**
 * MOZ_TRY(expr) is the C++ equivalent of Rust's `try!(expr);`. First, it
 * evaluates expr, which must produce a Result value. On success, it
 * discards the result altogether. On error, it immediately returns an error
 * Result from the enclosing function.
 */
#define MOZ_TRY(expr)                                   \
  do {                                                  \
    auto mozTryTempResult_ = ::mozilla::ToResult(expr); \
    if (MOZ_UNLIKELY(mozTryTempResult_.isErr())) {      \
      return mozTryTempResult_.propagateErr();          \
    }                                                   \
  } while (0)

/**
 * MOZ_TRY_VAR(target, expr) is the C++ equivalent of Rust's `target =
 * try!(expr);`. First, it evaluates expr, which must produce a Result value. On
 * success, the result's success value is assigned to target. On error,
 * immediately returns the error result. |target| must be an lvalue.
 */
#define MOZ_TRY_VAR(target, expr)                     \
  do {                                                \
    auto mozTryVarTempResult_ = (expr);               \
    if (MOZ_UNLIKELY(mozTryVarTempResult_.isErr())) { \
      return mozTryVarTempResult_.propagateErr();     \
    }                                                 \
    (target) = mozTryVarTempResult_.unwrap();         \
  } while (0)

#endif  // mozilla_Try_h
