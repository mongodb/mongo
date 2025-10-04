/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_ReciprocalMulConstants_h
#define jit_ReciprocalMulConstants_h

#include <stdint.h>

namespace js::jit {

struct ReciprocalMulConstants {
  int64_t multiplier;
  int32_t shiftAmount;

  static ReciprocalMulConstants computeSignedDivisionConstants(uint32_t d) {
    return computeDivisionConstants(d, 31);
  }

  static ReciprocalMulConstants computeUnsignedDivisionConstants(uint32_t d) {
    return computeDivisionConstants(d, 32);
  }

 private:
  static ReciprocalMulConstants computeDivisionConstants(uint32_t d,
                                                         int maxLog);
};

}  // namespace js::jit

#endif /* jit_ReciprocalMulConstants_h */
