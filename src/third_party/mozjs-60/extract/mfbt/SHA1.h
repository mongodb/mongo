/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Simple class for computing SHA1. */

#ifndef mozilla_SHA1_h
#define mozilla_SHA1_h

#include "mozilla/Types.h"

#include <stddef.h>
#include <stdint.h>

namespace mozilla {

/**
 * This class computes the SHA1 hash of a byte sequence, or of the concatenation
 * of multiple sequences.  For example, computing the SHA1 of two sequences of
 * bytes could be done as follows:
 *
 *   void SHA1(const uint8_t* buf1, uint32_t size1,
 *             const uint8_t* buf2, uint32_t size2,
 *             SHA1Sum::Hash& hash)
 *   {
 *     SHA1Sum s;
 *     s.update(buf1, size1);
 *     s.update(buf2, size2);
 *     s.finish(hash);
 *   }
 *
 * The finish method may only be called once and cannot be followed by calls
 * to update.
 */
class SHA1Sum
{
  union
  {
    uint32_t mW[16]; /* input buffer */
    uint8_t mB[64];
  } mU;
  uint64_t mSize; /* count of hashed bytes. */
  unsigned mH[22]; /* 5 state variables, 16 tmp values, 1 extra */
  bool mDone;

public:
  MFBT_API SHA1Sum();

  static const size_t kHashSize = 20;
  typedef uint8_t Hash[kHashSize];

  /* Add len bytes of dataIn to the data sequence being hashed. */
  MFBT_API void update(const void* aData, uint32_t aLength);

  /* Compute the final hash of all data into hashOut. */
  MFBT_API void finish(SHA1Sum::Hash& aHashOut);
};

} /* namespace mozilla */

#endif /* mozilla_SHA1_h */
