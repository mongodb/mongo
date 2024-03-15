/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/ReciprocalMulConstants.h"

#include "mozilla/Assertions.h"

using namespace js::jit;

ReciprocalMulConstants ReciprocalMulConstants::computeDivisionConstants(
    uint32_t d, int maxLog) {
  MOZ_ASSERT(maxLog >= 2 && maxLog <= 32);
  // In what follows, 0 < d < 2^maxLog and d is not a power of 2.
  MOZ_ASSERT(d < (uint64_t(1) << maxLog) && (d & (d - 1)) != 0);

  // Speeding up division by non power-of-2 constants is possible by
  // calculating, during compilation, a value M such that high-order
  // bits of M*n correspond to the result of the division of n by d.
  // No value of M can serve this purpose for arbitrarily big values
  // of n but, for optimizing integer division, we're just concerned
  // with values of n whose absolute value is bounded (by fitting in
  // an integer type, say). With this in mind, we'll find a constant
  // M as above that works for -2^maxLog <= n < 2^maxLog; maxLog can
  // then be 31 for signed division or 32 for unsigned division.
  //
  // The original presentation of this technique appears in Hacker's
  // Delight, a book by Henry S. Warren, Jr.. A proof of correctness
  // for our version follows; we'll denote maxLog by L in the proof,
  // for conciseness.
  //
  // Formally, for |d| < 2^L, we'll compute two magic values M and s
  // in the ranges 0 <= M < 2^(L+1) and 0 <= s <= L such that
  //     (M * n) >> (32 + s) = floor(n/d)    if    0 <= n < 2^L
  //     (M * n) >> (32 + s) = ceil(n/d) - 1 if -2^L <= n < 0.
  //
  // Define p = 32 + s, M = ceil(2^p/d), and assume that s satisfies
  //                     M - 2^p/d <= 2^(p-L)/d.                 (1)
  // (Observe that p = CeilLog32(d) + L satisfies this, as the right
  // side of (1) is at least one in this case). Then,
  //
  // a) If p <= CeilLog32(d) + L, then M < 2^(L+1) - 1.
  // Proof: Indeed, M is monotone in p and, for p equal to the above
  // value, the bounds 2^L > d >= 2^(p-L-1) + 1 readily imply that
  //    2^p / d <  2^p/(d - 1) * (d - 1)/d
  //            <= 2^(L+1) * (1 - 1/d) < 2^(L+1) - 2.
  // The claim follows by applying the ceiling function.
  //
  // b) For any 0 <= n < 2^L, floor(Mn/2^p) = floor(n/d).
  // Proof: Put x = floor(Mn/2^p); it's the unique integer for which
  //                    Mn/2^p - 1 < x <= Mn/2^p.                (2)
  // Using M >= 2^p/d on the LHS and (1) on the RHS, we get
  //           n/d - 1 < x <= n/d + n/(2^L d) < n/d + 1/d.
  // Since x is an integer, it's not in the interval (n/d, (n+1)/d),
  // and so n/d - 1 < x <= n/d, which implies x = floor(n/d).
  //
  // c) For any -2^L <= n < 0, floor(Mn/2^p) + 1 = ceil(n/d).
  // Proof: The proof is similar. Equation (2) holds as above. Using
  // M > 2^p/d (d isn't a power of 2) on the RHS and (1) on the LHS,
  //                 n/d + n/(2^L d) - 1 < x < n/d.
  // Using n >= -2^L and summing 1,
  //                  n/d - 1/d < x + 1 < n/d + 1.
  // Since x + 1 is an integer, this implies n/d <= x + 1 < n/d + 1.
  // In other words, x + 1 = ceil(n/d).
  //
  // Condition (1) isn't necessary for the existence of M and s with
  // the properties above. Hacker's Delight provides a slightly less
  // restrictive condition when d >= 196611, at the cost of a 3-page
  // proof of correctness, for the case L = 31.
  //
  // Note that, since d*M - 2^p = d - (2^p)%d, (1) can be written as
  //                   2^(p-L) >= d - (2^p)%d.
  // In order to avoid overflow in the (2^p) % d calculation, we can
  // compute it as (2^p-1) % d + 1, where 2^p-1 can then be computed
  // without overflow as UINT64_MAX >> (64-p).

  // We now compute the least p >= 32 with the property above...
  int32_t p = 32;
  while ((uint64_t(1) << (p - maxLog)) + (UINT64_MAX >> (64 - p)) % d + 1 < d) {
    p++;
  }

  // ...and the corresponding M. For either the signed (L=31) or the
  // unsigned (L=32) case, this value can be too large (cf. item a).
  // Codegen can still multiply by M by multiplying by (M - 2^L) and
  // adjusting the value afterwards, if this is the case.
  ReciprocalMulConstants rmc;
  rmc.multiplier = (UINT64_MAX >> (64 - p)) / d + 1;
  rmc.shiftAmount = p - 32;

  return rmc;
}
