/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Equality operations. */

#ifndef js_Equality_h
#define js_Equality_h

#include "mozilla/FloatingPoint.h"

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/TypeDecls.h"

struct JS_PUBLIC_API JSContext;

namespace JS {

/**
 * Store |v1 === v2| to |*equal| -- strict equality, which performs no
 * conversions on |v1| or |v2| before comparing.
 *
 * This operation can fail only if an internal error occurs (e.g. OOM while
 * linearizing a string value).
 */
extern JS_PUBLIC_API bool StrictlyEqual(JSContext* cx, JS::Handle<JS::Value> v1,
                                        JS::Handle<JS::Value> v2, bool* equal);

/**
 * Store |v1 == v2| to |*equal| -- loose equality, which may perform
 * user-modifiable conversions on |v1| or |v2|.
 *
 * This operation can fail if a user-modifiable conversion fails *or* if an
 * internal error occurs. (e.g. OOM while linearizing a string value).
 */
extern JS_PUBLIC_API bool LooselyEqual(JSContext* cx, JS::Handle<JS::Value> v1,
                                       JS::Handle<JS::Value> v2, bool* equal);

/**
 * Stores |SameValue(v1, v2)| to |*equal| -- using the SameValue operation
 * defined in ECMAScript, initially exposed to script as |Object.is|.  SameValue
 * behaves identically to strict equality, except that it equates two NaN values
 * and does not equate differently-signed zeroes.  It performs no conversions on
 * |v1| or |v2| before comparing.
 *
 * This operation can fail only if an internal error occurs (e.g. OOM while
 * linearizing a string value).
 */
extern JS_PUBLIC_API bool SameValue(JSContext* cx, JS::Handle<JS::Value> v1,
                                    JS::Handle<JS::Value> v2, bool* same);

/**
 * Implements |SameValueZero(v1, v2)| for Number values |v1| and |v2|.
 * SameValueZero equates NaNs, equal nonzero values, and zeroes without respect
 * to their signs.
 */
static inline bool SameValueZero(double v1, double v2) {
  return mozilla::EqualOrBothNaN(v1, v2);
}

}  // namespace JS

#endif /* js_Equality_h */
