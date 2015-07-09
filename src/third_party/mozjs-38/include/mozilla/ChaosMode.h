/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_ChaosMode_h
#define mozilla_ChaosMode_h

#include "mozilla/EnumSet.h"

#include <stdint.h>
#include <stdlib.h>

namespace mozilla {

/**
 * When "chaos mode" is activated, code that makes implicitly nondeterministic
 * choices is encouraged to make random and extreme choices, to test more
 * code paths and uncover bugs.
 */
class ChaosMode
{
public:
  enum ChaosFeature {
    None = 0x0,
    // Altering thread scheduling.
    ThreadScheduling = 0x1,
    // Altering network request scheduling.
    NetworkScheduling = 0x2,
    // Altering timer scheduling.
    TimerScheduling = 0x4,
    // Read and write less-than-requested amounts.
    IOAmounts = 0x8,
    // Iterate over hash tables in random order.
    HashTableIteration = 0x10,
    Any = 0xffffffff,
  };

private:
  // Change this to any non-None value to activate ChaosMode.
  static const ChaosFeature sChaosFeatures = None;

public:
  static bool isActive(ChaosFeature aFeature)
  {
    return sChaosFeatures & aFeature;
  }

  /**
   * Returns a somewhat (but not uniformly) random uint32_t < aBound.
   * Not to be used for anything except ChaosMode, since it's not very random.
   */
  static uint32_t randomUint32LessThan(uint32_t aBound)
  {
    return uint32_t(rand()) % aBound;
  }
};

} /* namespace mozilla */

#endif /* mozilla_ChaosMode_h */
