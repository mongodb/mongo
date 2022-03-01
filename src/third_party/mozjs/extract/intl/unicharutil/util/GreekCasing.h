/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GreekCasing_h_
#define GreekCasing_h_

#include <stdint.h>
#include "mozilla/Attributes.h"

namespace mozilla {

class GreekCasing {
  // When doing an Uppercase transform in Greek, we need to keep track of the
  // current state while iterating through the string, to recognize and process
  // diphthongs correctly. For clarity, we define a state for each vowel and
  // each vowel with accent, although a few of these do not actually need any
  // special treatment and could be folded into kStart.
 private:
  enum GreekStates {
    kStart,
    kInWord,
    kAlpha,
    kEpsilon,
    kEta,
    kIota,
    kOmicron,
    kUpsilon,
    kOmega,
    kAlphaAcc,
    kEpsilonAcc,
    kEtaAcc,
    kEtaAccMarked,
    kIotaAcc,
    kOmicronAcc,
    kUpsilonAcc,
    kOmegaAcc,
    kOmicronUpsilon,
    kDiaeresis
  };

 public:
  class State {
   public:
    State() : mState(kStart) {}

    MOZ_IMPLICIT State(const GreekStates& aState) : mState(aState) {}

    void Reset() { mState = kStart; }

    operator GreekStates() const { return mState; }

   private:
    GreekStates mState;
  };

  static uint32_t UpperCase(uint32_t aCh, State& aState, bool& aMarkEtaPos,
                            bool& aUpdateMarkedEta);
};

}  // namespace mozilla

#endif
