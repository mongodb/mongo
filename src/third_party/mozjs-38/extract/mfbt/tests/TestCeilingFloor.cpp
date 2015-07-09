/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/MathAlgorithms.h"

using mozilla::CeilingLog2;
using mozilla::FloorLog2;
using mozilla::RoundUpPow2;

static void
TestCeiling()
{
  for (uint32_t i = 0; i <= 1; i++) {
    MOZ_RELEASE_ASSERT(CeilingLog2(i) == 0);
  }
  for (uint32_t i = 2; i <= 2; i++) {
    MOZ_RELEASE_ASSERT(CeilingLog2(i) == 1);
  }
  for (uint32_t i = 3; i <= 4; i++) {
    MOZ_RELEASE_ASSERT(CeilingLog2(i) == 2);
  }
  for (uint32_t i = 5; i <= 8; i++) {
    MOZ_RELEASE_ASSERT(CeilingLog2(i) == 3);
  }
  for (uint32_t i = 9; i <= 16; i++) {
    MOZ_RELEASE_ASSERT(CeilingLog2(i) == 4);
  }
}

static void
TestFloor()
{
  for (uint32_t i = 0; i <= 1; i++) {
    MOZ_RELEASE_ASSERT(FloorLog2(i) == 0);
  }
  for (uint32_t i = 2; i <= 3; i++) {
    MOZ_RELEASE_ASSERT(FloorLog2(i) == 1);
  }
  for (uint32_t i = 4; i <= 7; i++) {
    MOZ_RELEASE_ASSERT(FloorLog2(i) == 2);
  }
  for (uint32_t i = 8; i <= 15; i++) {
    MOZ_RELEASE_ASSERT(FloorLog2(i) == 3);
  }
  for (uint32_t i = 16; i <= 31; i++) {
    MOZ_RELEASE_ASSERT(FloorLog2(i) == 4);
  }
}

static void
TestRoundUpPow2()
{
  MOZ_RELEASE_ASSERT(RoundUpPow2(0) == 1);
  MOZ_RELEASE_ASSERT(RoundUpPow2(1) == 1);
  MOZ_RELEASE_ASSERT(RoundUpPow2(2) == 2);
  MOZ_RELEASE_ASSERT(RoundUpPow2(3) == 4);
  MOZ_RELEASE_ASSERT(RoundUpPow2(4) == 4);
  MOZ_RELEASE_ASSERT(RoundUpPow2(5) == 8);
  MOZ_RELEASE_ASSERT(RoundUpPow2(6) == 8);
  MOZ_RELEASE_ASSERT(RoundUpPow2(7) == 8);
  MOZ_RELEASE_ASSERT(RoundUpPow2(8) == 8);
  MOZ_RELEASE_ASSERT(RoundUpPow2(9) == 16);

  MOZ_RELEASE_ASSERT(RoundUpPow2(15) == 16);
  MOZ_RELEASE_ASSERT(RoundUpPow2(16) == 16);
  MOZ_RELEASE_ASSERT(RoundUpPow2(17) == 32);

  MOZ_RELEASE_ASSERT(RoundUpPow2(31) == 32);
  MOZ_RELEASE_ASSERT(RoundUpPow2(32) == 32);
  MOZ_RELEASE_ASSERT(RoundUpPow2(33) == 64);

  size_t MaxPow2 = size_t(1) << (sizeof(size_t) * CHAR_BIT - 1);
  MOZ_RELEASE_ASSERT(RoundUpPow2(MaxPow2 - 1) == MaxPow2);
  MOZ_RELEASE_ASSERT(RoundUpPow2(MaxPow2) == MaxPow2);
  // not valid to round up when past the max power of two
}

int
main()
{
  TestCeiling();
  TestFloor();

  TestRoundUpPow2();
  return 0;
}
