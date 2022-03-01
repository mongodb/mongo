/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jslibmath_h
#define jslibmath_h

#include "mozilla/FloatingPoint.h"

#include <math.h>

#include "js/Value.h"
#include "vm/JSContext.h"

namespace js {

inline double NumberDiv(double a, double b) {
  AutoUnsafeCallWithABI unsafe(UnsafeABIStrictness::AllowPendingExceptions);
  if (b == 0) {
    if (a == 0 || mozilla::IsNaN(a)) {
      return JS::GenericNaN();
    }
    if (mozilla::IsNegative(a) != mozilla::IsNegative(b)) {
      return mozilla::NegativeInfinity<double>();
    }
    return mozilla::PositiveInfinity<double>();
  }

  return a / b;
}

inline double NumberMod(double a, double b) {
  AutoUnsafeCallWithABI unsafe(UnsafeABIStrictness::AllowPendingExceptions);
  if (b == 0) {
    return JS::GenericNaN();
  }
  double r = fmod(a, b);
#if defined(XP_WIN)
  // Some versions of Windows (Win 10 v1803, v1809) miscompute the sign of zero
  // results from fmod. The sign should match the sign of the LHS. This bug
  // only affects 64-bit builds. See bug 1527007.
  if (mozilla::IsPositiveZero(r) && mozilla::IsNegative(a)) {
    return -0.0;
  }
#endif
  return r;
}

}  // namespace js

#endif /* jslibmath_h */
