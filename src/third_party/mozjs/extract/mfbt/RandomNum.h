/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/* Routines for generating random numbers */

#ifndef mozilla_RandomNum_h_
#define mozilla_RandomNum_h_

#include "mozilla/Maybe.h"
#include "mozilla/Types.h"

namespace mozilla {

/**
 *  Generate cryptographically secure random bytes using the best facilities
 *  available on the current OS.
 *
 *  Return value: true if random bytes were copied into `aBuffer` or false on
 *  error.
 *
 *  Useful whenever a secure random number is needed and NSS isn't available.
 *  (Perhaps because it hasn't been initialized yet)
 *
 *  Current mechanisms:
 *    Windows: RtlGenRandom()
 *    Android, Darwin, DragonFly, FreeBSD, OpenBSD, NetBSD: arc4random()
 *    Linux: getrandom() if available, "/dev/urandom" otherwise
 *    Other Unix: "/dev/urandom"
 *
 */
[[nodiscard]] MFBT_API bool GenerateRandomBytesFromOS(void* aBuffer,
                                                      size_t aLength);

/**
 *  Generate a cryptographically secure random 64-bit unsigned number using the
 *  best facilities available on the current OS.
 */
MFBT_API Maybe<uint64_t> RandomUint64();

/**
 *  Like RandomUint64, but always returns a uint64_t or crashes with an assert
 *  if the underlying RandomUint64 call failed.
 */
MFBT_API uint64_t RandomUint64OrDie();

}  // namespace mozilla

#endif  // mozilla_RandomNum_h_
