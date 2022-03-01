/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_ScalarTypeUtils_h
#define jit_ScalarTypeUtils_h

#include "mozilla/CheckedInt.h"

#include <stdint.h>

#include "js/ScalarType.h"

namespace js {
namespace jit {

// Compute |index * Scalar::byteSize(type) + offsetAdjustment|. If this doesn't
// overflow and is non-negative, return true and store the result in *offset.
// If the computation overflows or the result is negative, false is returned and
// *offset is left unchanged.
[[nodiscard]] inline bool ArrayOffsetFitsInInt32(int32_t index,
                                                 Scalar::Type type,
                                                 int32_t offsetAdjustment,
                                                 int32_t* offset) {
  mozilla::CheckedInt<int32_t> val = index;
  val *= Scalar::byteSize(type);
  val += offsetAdjustment;
  if (!val.isValid() || val.value() < 0) {
    return false;
  }

  *offset = val.value();
  return true;
}

}  // namespace jit
}  // namespace js

#endif /* jit_ScalarTypeUtils_h */
