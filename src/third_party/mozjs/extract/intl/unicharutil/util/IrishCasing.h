/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef IrishCasing_h_
#define IrishCasing_h_

#include <stdint.h>
#include "mozilla/Attributes.h"

namespace mozilla {

class IrishCasing {
 private:
  enum IrishStates {
    kState_Start,
    kState_InWord,
    kState_b,
    kState_bh,
    kState_d,
    kState_g,
    kState_h,
    kState_m,
    kState_n,
    kState_nt_,
    kState_t,
    kState_ts,
    kNumStates
  };

  enum IrishClasses {
    kClass_b,
    kClass_B,
    kClass_cC,
    kClass_d,
    kClass_DG,
    kClass_fF,
    kClass_g,
    kClass_h,
    kClass_lLNrR,
    kClass_m,
    kClass_n,
    kClass_pP,
    kClass_sS,
    kClass_t,
    kClass_T,
    kClass_vowel,
    kClass_Vowel,
    kClass_hyph,
    kClass_letter,
    kClass_other,
    kNumClasses
  };

 public:
  class State {
    friend class IrishCasing;

   public:
    State() : mState(kState_Start) {}

    MOZ_IMPLICIT State(const IrishStates& aState) : mState(aState) {}

    void Reset() { mState = kState_Start; }

    operator IrishStates() const { return mState; }

   private:
    explicit State(uint8_t aState) : mState(IrishStates(aState)) {}

    uint8_t GetClass(uint32_t aCh);

    IrishStates mState;
  };

  enum {
    kMarkPositionFlag = 0x80,
    kActionMask = 0x30,
    kActionShift = 4,
    kNextStateMask = 0x0f
  };

  static const uint8_t sUppercaseStateTable[kNumClasses][kNumStates];
  static const uint8_t sLcClasses[26];
  static const uint8_t sUcClasses[26];

  static uint32_t UpperCase(uint32_t aCh, State& aState, bool& aMarkPos,
                            uint8_t& aAction);

  static bool IsUpperVowel(uint32_t aCh) {
    return GetClass(aCh) == kClass_Vowel;
  }

 private:
  static uint8_t GetClass(uint32_t aCh);
};

}  // namespace mozilla

#endif
