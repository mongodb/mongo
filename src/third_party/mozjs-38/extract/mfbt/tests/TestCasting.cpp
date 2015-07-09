/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/Casting.h"

#include <stdint.h>

using mozilla::BitwiseCast;
using mozilla::detail::IsInBounds;

template<typename Uint, typename Ulong, bool = (sizeof(Uint) == sizeof(Ulong))>
struct UintUlongBitwiseCast;

template<typename Uint, typename Ulong>
struct UintUlongBitwiseCast<Uint, Ulong, true>
{
  static void test()
  {
    MOZ_RELEASE_ASSERT(BitwiseCast<Ulong>(Uint(8675309)) == Ulong(8675309));
  }
};

template<typename Uint, typename Ulong>
struct UintUlongBitwiseCast<Uint, Ulong, false>
{
  static void test() { }
};

static void
TestBitwiseCast()
{
  MOZ_RELEASE_ASSERT(BitwiseCast<int>(int(8675309)) == int(8675309));
  UintUlongBitwiseCast<unsigned int, unsigned long>::test();
}

static void
TestSameSize()
{
  MOZ_RELEASE_ASSERT((IsInBounds<int16_t, int16_t>(int16_t(0))));
  MOZ_RELEASE_ASSERT((IsInBounds<int16_t, int16_t>(int16_t(INT16_MIN))));
  MOZ_RELEASE_ASSERT((IsInBounds<int16_t, int16_t>(int16_t(INT16_MAX))));
  MOZ_RELEASE_ASSERT((IsInBounds<uint16_t, uint16_t>(uint16_t(UINT16_MAX))));
  MOZ_RELEASE_ASSERT((IsInBounds<uint16_t, int16_t>(uint16_t(0))));
  MOZ_RELEASE_ASSERT((!IsInBounds<uint16_t, int16_t>(uint16_t(-1))));
  MOZ_RELEASE_ASSERT((!IsInBounds<int16_t, uint16_t>(int16_t(-1))));
  MOZ_RELEASE_ASSERT((IsInBounds<int16_t, uint16_t>(int16_t(INT16_MAX))));
  MOZ_RELEASE_ASSERT((!IsInBounds<int16_t, uint16_t>(int16_t(INT16_MIN))));
  MOZ_RELEASE_ASSERT((IsInBounds<int32_t, uint32_t>(int32_t(INT32_MAX))));
  MOZ_RELEASE_ASSERT((!IsInBounds<int32_t, uint32_t>(int32_t(INT32_MIN))));
}

static void
TestToBiggerSize()
{
  MOZ_RELEASE_ASSERT((IsInBounds<int16_t, int32_t>(int16_t(0))));
  MOZ_RELEASE_ASSERT((IsInBounds<int16_t, int32_t>(int16_t(INT16_MIN))));
  MOZ_RELEASE_ASSERT((IsInBounds<int16_t, int32_t>(int16_t(INT16_MAX))));
  MOZ_RELEASE_ASSERT((IsInBounds<uint16_t, uint32_t>(uint16_t(UINT16_MAX))));
  MOZ_RELEASE_ASSERT((IsInBounds<uint16_t, int32_t>(uint16_t(0))));
  MOZ_RELEASE_ASSERT((IsInBounds<uint16_t, int32_t>(uint16_t(-1))));
  MOZ_RELEASE_ASSERT((!IsInBounds<int16_t, uint32_t>(int16_t(-1))));
  MOZ_RELEASE_ASSERT((IsInBounds<int16_t, uint32_t>(int16_t(INT16_MAX))));
  MOZ_RELEASE_ASSERT((!IsInBounds<int16_t, uint32_t>(int16_t(INT16_MIN))));
  MOZ_RELEASE_ASSERT((IsInBounds<int32_t, uint64_t>(int32_t(INT32_MAX))));
  MOZ_RELEASE_ASSERT((!IsInBounds<int32_t, uint64_t>(int32_t(INT32_MIN))));
}

static void
TestToSmallerSize()
{
  MOZ_RELEASE_ASSERT((IsInBounds<int16_t, int8_t>(int16_t(0))));
  MOZ_RELEASE_ASSERT((!IsInBounds<int16_t, int8_t>(int16_t(INT16_MIN))));
  MOZ_RELEASE_ASSERT((!IsInBounds<int16_t, int8_t>(int16_t(INT16_MAX))));
  MOZ_RELEASE_ASSERT((!IsInBounds<uint16_t, uint8_t>(uint16_t(UINT16_MAX))));
  MOZ_RELEASE_ASSERT((IsInBounds<uint16_t, int8_t>(uint16_t(0))));
  MOZ_RELEASE_ASSERT((!IsInBounds<uint16_t, int8_t>(uint16_t(-1))));
  MOZ_RELEASE_ASSERT((!IsInBounds<int16_t, uint8_t>(int16_t(-1))));
  MOZ_RELEASE_ASSERT((!IsInBounds<int16_t, uint8_t>(int16_t(INT16_MAX))));
  MOZ_RELEASE_ASSERT((!IsInBounds<int16_t, uint8_t>(int16_t(INT16_MIN))));
  MOZ_RELEASE_ASSERT((!IsInBounds<int32_t, uint16_t>(int32_t(INT32_MAX))));
  MOZ_RELEASE_ASSERT((!IsInBounds<int32_t, uint16_t>(int32_t(INT32_MIN))));

  // Boundary cases
  MOZ_RELEASE_ASSERT((!IsInBounds<int64_t, int32_t>(int64_t(INT32_MIN) - 1)));
  MOZ_RELEASE_ASSERT((IsInBounds<int64_t, int32_t>(int64_t(INT32_MIN))));
  MOZ_RELEASE_ASSERT((IsInBounds<int64_t, int32_t>(int64_t(INT32_MIN) + 1)));
  MOZ_RELEASE_ASSERT((IsInBounds<int64_t, int32_t>(int64_t(INT32_MAX) - 1)));
  MOZ_RELEASE_ASSERT((IsInBounds<int64_t, int32_t>(int64_t(INT32_MAX))));
  MOZ_RELEASE_ASSERT((!IsInBounds<int64_t, int32_t>(int64_t(INT32_MAX) + 1)));

  MOZ_RELEASE_ASSERT((!IsInBounds<int64_t, uint32_t>(int64_t(-1))));
  MOZ_RELEASE_ASSERT((IsInBounds<int64_t, uint32_t>(int64_t(0))));
  MOZ_RELEASE_ASSERT((IsInBounds<int64_t, uint32_t>(int64_t(1))));
  MOZ_RELEASE_ASSERT((IsInBounds<int64_t, uint32_t>(int64_t(UINT32_MAX) - 1)));
  MOZ_RELEASE_ASSERT((IsInBounds<int64_t, uint32_t>(int64_t(UINT32_MAX))));
  MOZ_RELEASE_ASSERT((!IsInBounds<int64_t, uint32_t>(int64_t(UINT32_MAX) + 1)));
}

int
main()
{
  TestBitwiseCast();

  TestSameSize();
  TestToBiggerSize();
  TestToSmallerSize();

  return 0;
}
