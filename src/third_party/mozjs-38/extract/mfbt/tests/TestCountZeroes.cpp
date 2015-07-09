/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/MathAlgorithms.h"

using mozilla::CountLeadingZeroes32;
using mozilla::CountLeadingZeroes64;
using mozilla::CountTrailingZeroes32;
using mozilla::CountTrailingZeroes64;

static void
TestLeadingZeroes32()
{
  MOZ_RELEASE_ASSERT(CountLeadingZeroes32(0xF0FF1000) == 0);
  MOZ_RELEASE_ASSERT(CountLeadingZeroes32(0x7F8F0001) == 1);
  MOZ_RELEASE_ASSERT(CountLeadingZeroes32(0x3FFF0100) == 2);
  MOZ_RELEASE_ASSERT(CountLeadingZeroes32(0x1FF50010) == 3);
  MOZ_RELEASE_ASSERT(CountLeadingZeroes32(0x00800000) == 8);
  MOZ_RELEASE_ASSERT(CountLeadingZeroes32(0x00400000) == 9);
  MOZ_RELEASE_ASSERT(CountLeadingZeroes32(0x00008000) == 16);
  MOZ_RELEASE_ASSERT(CountLeadingZeroes32(0x00004000) == 17);
  MOZ_RELEASE_ASSERT(CountLeadingZeroes32(0x00000080) == 24);
  MOZ_RELEASE_ASSERT(CountLeadingZeroes32(0x00000040) == 25);
  MOZ_RELEASE_ASSERT(CountLeadingZeroes32(0x00000001) == 31);
}

static void
TestLeadingZeroes64()
{
  MOZ_RELEASE_ASSERT(CountLeadingZeroes64(0xF000F0F010000000) == 0);
  MOZ_RELEASE_ASSERT(CountLeadingZeroes64(0x70F080F000000001) == 1);
  MOZ_RELEASE_ASSERT(CountLeadingZeroes64(0x30F0F0F000100000) == 2);
  MOZ_RELEASE_ASSERT(CountLeadingZeroes64(0x10F0F05000000100) == 3);
  MOZ_RELEASE_ASSERT(CountLeadingZeroes64(0x0080000000000001) == 8);
  MOZ_RELEASE_ASSERT(CountLeadingZeroes64(0x0040000010001000) == 9);
  MOZ_RELEASE_ASSERT(CountLeadingZeroes64(0x000080F010000000) == 16);
  MOZ_RELEASE_ASSERT(CountLeadingZeroes64(0x000040F010000000) == 17);
  MOZ_RELEASE_ASSERT(CountLeadingZeroes64(0x0000008000100100) == 24);
  MOZ_RELEASE_ASSERT(CountLeadingZeroes64(0x0000004100010010) == 25);
  MOZ_RELEASE_ASSERT(CountLeadingZeroes64(0x0000000080100100) == 32);
  MOZ_RELEASE_ASSERT(CountLeadingZeroes64(0x0000000041001010) == 33);
  MOZ_RELEASE_ASSERT(CountLeadingZeroes64(0x0000000000800100) == 40);
  MOZ_RELEASE_ASSERT(CountLeadingZeroes64(0x0000000000411010) == 41);
  MOZ_RELEASE_ASSERT(CountLeadingZeroes64(0x0000000000008001) == 48);
  MOZ_RELEASE_ASSERT(CountLeadingZeroes64(0x0000000000004010) == 49);
  MOZ_RELEASE_ASSERT(CountLeadingZeroes64(0x0000000000000081) == 56);
  MOZ_RELEASE_ASSERT(CountLeadingZeroes64(0x0000000000000040) == 57);
  MOZ_RELEASE_ASSERT(CountLeadingZeroes64(0x0000000000000001) == 63);
}

static void
TestTrailingZeroes32()
{
  MOZ_RELEASE_ASSERT(CountTrailingZeroes32(0x0100FFFF) == 0);
  MOZ_RELEASE_ASSERT(CountTrailingZeroes32(0x7000FFFE) == 1);
  MOZ_RELEASE_ASSERT(CountTrailingZeroes32(0x0080FFFC) == 2);
  MOZ_RELEASE_ASSERT(CountTrailingZeroes32(0x0080FFF8) == 3);
  MOZ_RELEASE_ASSERT(CountTrailingZeroes32(0x010FFF00) == 8);
  MOZ_RELEASE_ASSERT(CountTrailingZeroes32(0x7000FE00) == 9);
  MOZ_RELEASE_ASSERT(CountTrailingZeroes32(0x10CF0000) == 16);
  MOZ_RELEASE_ASSERT(CountTrailingZeroes32(0x0BDE0000) == 17);
  MOZ_RELEASE_ASSERT(CountTrailingZeroes32(0x0F000000) == 24);
  MOZ_RELEASE_ASSERT(CountTrailingZeroes32(0xDE000000) == 25);
  MOZ_RELEASE_ASSERT(CountTrailingZeroes32(0x80000000) == 31);
}

static void
TestTrailingZeroes64()
{
  MOZ_RELEASE_ASSERT(CountTrailingZeroes64(0x000100000F0F0F0F) == 0);
  MOZ_RELEASE_ASSERT(CountTrailingZeroes64(0x070000000F0F0F0E) == 1);
  MOZ_RELEASE_ASSERT(CountTrailingZeroes64(0x000008000F0F0F0C) == 2);
  MOZ_RELEASE_ASSERT(CountTrailingZeroes64(0x000008000F0F0F08) == 3);
  MOZ_RELEASE_ASSERT(CountTrailingZeroes64(0xC001000F0F0F0F00) == 8);
  MOZ_RELEASE_ASSERT(CountTrailingZeroes64(0x0200000F0F0F0E00) == 9);
  MOZ_RELEASE_ASSERT(CountTrailingZeroes64(0xB0C10F0FEFDF0000) == 16);
  MOZ_RELEASE_ASSERT(CountTrailingZeroes64(0x0AAA00F0FF0E0000) == 17);
  MOZ_RELEASE_ASSERT(CountTrailingZeroes64(0xD010F0FEDF000000) == 24);
  MOZ_RELEASE_ASSERT(CountTrailingZeroes64(0x7AAF0CF0BE000000) == 25);
  MOZ_RELEASE_ASSERT(CountTrailingZeroes64(0x20F0A5D100000000) == 32);
  MOZ_RELEASE_ASSERT(CountTrailingZeroes64(0x489BF0B200000000) == 33);
  MOZ_RELEASE_ASSERT(CountTrailingZeroes64(0xE0F0D10000000000) == 40);
  MOZ_RELEASE_ASSERT(CountTrailingZeroes64(0x97F0B20000000000) == 41);
  MOZ_RELEASE_ASSERT(CountTrailingZeroes64(0x2C07000000000000) == 48);
  MOZ_RELEASE_ASSERT(CountTrailingZeroes64(0x1FBA000000000000) == 49);
  MOZ_RELEASE_ASSERT(CountTrailingZeroes64(0x0100000000000000) == 56);
  MOZ_RELEASE_ASSERT(CountTrailingZeroes64(0x0200000000000000) == 57);
  MOZ_RELEASE_ASSERT(CountTrailingZeroes64(0x8000000000000000) == 63);
}

int
main()
{
  TestLeadingZeroes32();
  TestLeadingZeroes64();
  TestTrailingZeroes32();
  TestTrailingZeroes64();
  return 0;
}
