/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Implementations of FloatingPoint functions */

#include "mozilla/FloatingPoint.h"

#include <cfloat>  // for FLT_MAX

namespace mozilla {

bool IsFloat32Representable(double aValue) {
  // NaNs and infinities are representable.
  if (!std::isfinite(aValue)) {
    return true;
  }

  // If it exceeds finite |float| range, casting to |double| is always undefined
  // behavior per C++11 [conv.double]p1 last sentence.
  if (Abs(aValue) > FLT_MAX) {
    return false;
  }

  // But if it's within finite range, then either it's 1) an exact value and so
  // representable, or 2) it's "between two adjacent destination values" and
  // safe to cast to "an implementation-defined choice of either of those
  // values".
  auto valueAsFloat = static_cast<float>(aValue);

  // Per [conv.fpprom] this never changes value.
  auto valueAsFloatAsDouble = static_cast<double>(valueAsFloat);

  // Finally, in 1) exact representable value equals exact representable value,
  // or 2) *changed* value does not equal original value, ergo unrepresentable.
  return valueAsFloatAsDouble == aValue;
}

} /* namespace mozilla */
