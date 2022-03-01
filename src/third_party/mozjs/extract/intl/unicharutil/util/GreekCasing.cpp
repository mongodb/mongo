/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "GreekCasing.h"
#include "nsUnicharUtils.h"
#include "nsUnicodeProperties.h"

// Custom uppercase mapping for Greek; see bug 307039 for details
#define GREEK_LOWER_ALPHA 0x03B1
#define GREEK_LOWER_ALPHA_TONOS 0x03AC
#define GREEK_LOWER_ALPHA_OXIA 0x1F71
#define GREEK_LOWER_EPSILON 0x03B5
#define GREEK_LOWER_EPSILON_TONOS 0x03AD
#define GREEK_LOWER_EPSILON_OXIA 0x1F73
#define GREEK_LOWER_ETA 0x03B7
#define GREEK_LOWER_ETA_TONOS 0x03AE
#define GREEK_LOWER_ETA_OXIA 0x1F75
#define GREEK_LOWER_IOTA 0x03B9
#define GREEK_LOWER_IOTA_TONOS 0x03AF
#define GREEK_LOWER_IOTA_OXIA 0x1F77
#define GREEK_LOWER_IOTA_DIALYTIKA 0x03CA
#define GREEK_LOWER_IOTA_DIALYTIKA_TONOS 0x0390
#define GREEK_LOWER_IOTA_DIALYTIKA_OXIA 0x1FD3
#define GREEK_LOWER_OMICRON 0x03BF
#define GREEK_LOWER_OMICRON_TONOS 0x03CC
#define GREEK_LOWER_OMICRON_OXIA 0x1F79
#define GREEK_LOWER_UPSILON 0x03C5
#define GREEK_LOWER_UPSILON_TONOS 0x03CD
#define GREEK_LOWER_UPSILON_OXIA 0x1F7B
#define GREEK_LOWER_UPSILON_DIALYTIKA 0x03CB
#define GREEK_LOWER_UPSILON_DIALYTIKA_TONOS 0x03B0
#define GREEK_LOWER_UPSILON_DIALYTIKA_OXIA 0x1FE3
#define GREEK_LOWER_OMEGA 0x03C9
#define GREEK_LOWER_OMEGA_TONOS 0x03CE
#define GREEK_LOWER_OMEGA_OXIA 0x1F7D
#define GREEK_UPPER_ALPHA 0x0391
#define GREEK_UPPER_EPSILON 0x0395
#define GREEK_UPPER_ETA 0x0397
#define GREEK_UPPER_IOTA 0x0399
#define GREEK_UPPER_IOTA_DIALYTIKA 0x03AA
#define GREEK_UPPER_OMICRON 0x039F
#define GREEK_UPPER_UPSILON 0x03A5
#define GREEK_UPPER_UPSILON_DIALYTIKA 0x03AB
#define GREEK_UPPER_OMEGA 0x03A9
#define GREEK_UPPER_ALPHA_TONOS 0x0386
#define GREEK_UPPER_ALPHA_OXIA 0x1FBB
#define GREEK_UPPER_EPSILON_TONOS 0x0388
#define GREEK_UPPER_EPSILON_OXIA 0x1FC9
#define GREEK_UPPER_ETA_TONOS 0x0389
#define GREEK_UPPER_ETA_OXIA 0x1FCB
#define GREEK_UPPER_IOTA_TONOS 0x038A
#define GREEK_UPPER_IOTA_OXIA 0x1FDB
#define GREEK_UPPER_OMICRON_TONOS 0x038C
#define GREEK_UPPER_OMICRON_OXIA 0x1FF9
#define GREEK_UPPER_UPSILON_TONOS 0x038E
#define GREEK_UPPER_UPSILON_OXIA 0x1FEB
#define GREEK_UPPER_OMEGA_TONOS 0x038F
#define GREEK_UPPER_OMEGA_OXIA 0x1FFB
#define COMBINING_ACUTE_ACCENT 0x0301
#define COMBINING_DIAERESIS 0x0308
#define COMBINING_ACUTE_TONE_MARK 0x0341
#define COMBINING_GREEK_DIALYTIKA_TONOS 0x0344

namespace mozilla {

uint32_t GreekCasing::UpperCase(uint32_t aCh, GreekCasing::State& aState,
                                bool& aMarkEtaPos, bool& aUpdateMarkedEta) {
  aMarkEtaPos = false;
  aUpdateMarkedEta = false;

  uint8_t category = unicode::GetGeneralCategory(aCh);

  if (aState == kEtaAccMarked) {
    switch (category) {
      case HB_UNICODE_GENERAL_CATEGORY_LOWERCASE_LETTER:
      case HB_UNICODE_GENERAL_CATEGORY_MODIFIER_LETTER:
      case HB_UNICODE_GENERAL_CATEGORY_OTHER_LETTER:
      case HB_UNICODE_GENERAL_CATEGORY_TITLECASE_LETTER:
      case HB_UNICODE_GENERAL_CATEGORY_UPPERCASE_LETTER:
      case HB_UNICODE_GENERAL_CATEGORY_SPACING_MARK:
      case HB_UNICODE_GENERAL_CATEGORY_ENCLOSING_MARK:
      case HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK:
        aUpdateMarkedEta = true;
        break;
      default:
        break;
    }
    aState = kEtaAcc;
  }

  switch (aCh) {
    case GREEK_UPPER_ALPHA:
    case GREEK_LOWER_ALPHA:
      aState = kAlpha;
      return GREEK_UPPER_ALPHA;

    case GREEK_UPPER_EPSILON:
    case GREEK_LOWER_EPSILON:
      aState = kEpsilon;
      return GREEK_UPPER_EPSILON;

    case GREEK_UPPER_ETA:
    case GREEK_LOWER_ETA:
      aState = kEta;
      return GREEK_UPPER_ETA;

    case GREEK_UPPER_IOTA:
      aState = kIota;
      return GREEK_UPPER_IOTA;

    case GREEK_UPPER_OMICRON:
    case GREEK_LOWER_OMICRON:
      aState = kOmicron;
      return GREEK_UPPER_OMICRON;

    case GREEK_UPPER_UPSILON:
      switch (aState) {
        case kOmicron:
          aState = kOmicronUpsilon;
          break;
        default:
          aState = kUpsilon;
          break;
      }
      return GREEK_UPPER_UPSILON;

    case GREEK_UPPER_OMEGA:
    case GREEK_LOWER_OMEGA:
      aState = kOmega;
      return GREEK_UPPER_OMEGA;

    // iota and upsilon may be the second vowel of a diphthong
    case GREEK_LOWER_IOTA:
      switch (aState) {
        case kAlphaAcc:
        case kEpsilonAcc:
        case kOmicronAcc:
        case kUpsilonAcc:
          aState = kInWord;
          return GREEK_UPPER_IOTA_DIALYTIKA;
        default:
          break;
      }
      aState = kIota;
      return GREEK_UPPER_IOTA;

    case GREEK_LOWER_UPSILON:
      switch (aState) {
        case kAlphaAcc:
        case kEpsilonAcc:
        case kEtaAcc:
        case kOmicronAcc:
          aState = kInWord;
          return GREEK_UPPER_UPSILON_DIALYTIKA;
        case kOmicron:
          aState = kOmicronUpsilon;
          break;
        default:
          aState = kUpsilon;
          break;
      }
      return GREEK_UPPER_UPSILON;

    case GREEK_UPPER_IOTA_DIALYTIKA:
    case GREEK_LOWER_IOTA_DIALYTIKA:
    case GREEK_UPPER_UPSILON_DIALYTIKA:
    case GREEK_LOWER_UPSILON_DIALYTIKA:
    case COMBINING_DIAERESIS:
      aState = kDiaeresis;
      return ToUpperCase(aCh);

    // remove accent if it follows a vowel or diaeresis,
    // and set appropriate state for diphthong detection
    case COMBINING_ACUTE_ACCENT:
    case COMBINING_ACUTE_TONE_MARK:
      switch (aState) {
        case kAlpha:
          aState = kAlphaAcc;
          return uint32_t(-1);  // omit this char from result string
        case kEpsilon:
          aState = kEpsilonAcc;
          return uint32_t(-1);
        case kEta:
          aState = kEtaAcc;
          return uint32_t(-1);
        case kIota:
          aState = kIotaAcc;
          return uint32_t(-1);
        case kOmicron:
          aState = kOmicronAcc;
          return uint32_t(-1);
        case kUpsilon:
          aState = kUpsilonAcc;
          return uint32_t(-1);
        case kOmicronUpsilon:
          aState = kInWord;  // this completed a diphthong
          return uint32_t(-1);
        case kOmega:
          aState = kOmegaAcc;
          return uint32_t(-1);
        case kDiaeresis:
          aState = kInWord;
          return uint32_t(-1);
        default:
          break;
      }
      break;

    // combinations with dieresis+accent just strip the accent,
    // and reset to start state (don't form diphthong with following vowel)
    case GREEK_LOWER_IOTA_DIALYTIKA_TONOS:
    case GREEK_LOWER_IOTA_DIALYTIKA_OXIA:
      aState = kInWord;
      return GREEK_UPPER_IOTA_DIALYTIKA;

    case GREEK_LOWER_UPSILON_DIALYTIKA_TONOS:
    case GREEK_LOWER_UPSILON_DIALYTIKA_OXIA:
      aState = kInWord;
      return GREEK_UPPER_UPSILON_DIALYTIKA;

    case COMBINING_GREEK_DIALYTIKA_TONOS:
      aState = kInWord;
      return COMBINING_DIAERESIS;

    // strip accents from vowels, and note the vowel seen so that we can detect
    // diphthongs where diaeresis needs to be added
    case GREEK_LOWER_ALPHA_TONOS:
    case GREEK_LOWER_ALPHA_OXIA:
    case GREEK_UPPER_ALPHA_TONOS:
    case GREEK_UPPER_ALPHA_OXIA:
      aState = kAlphaAcc;
      return GREEK_UPPER_ALPHA;

    case GREEK_LOWER_EPSILON_TONOS:
    case GREEK_LOWER_EPSILON_OXIA:
    case GREEK_UPPER_EPSILON_TONOS:
    case GREEK_UPPER_EPSILON_OXIA:
      aState = kEpsilonAcc;
      return GREEK_UPPER_EPSILON;

    case GREEK_LOWER_ETA_TONOS:
    case GREEK_UPPER_ETA_TONOS:
      if (aState == kStart) {
        aState = kEtaAccMarked;
        aMarkEtaPos = true;  // mark in case we need to remove the tonos later
        return GREEK_UPPER_ETA_TONOS;  // treat as disjunctive eta for now
      }
      // if not in initial state, fall through to strip the accent
      [[fallthrough]];

    case GREEK_LOWER_ETA_OXIA:
    case GREEK_UPPER_ETA_OXIA:
      aState = kEtaAcc;
      return GREEK_UPPER_ETA;

    case GREEK_LOWER_IOTA_TONOS:
    case GREEK_LOWER_IOTA_OXIA:
    case GREEK_UPPER_IOTA_TONOS:
    case GREEK_UPPER_IOTA_OXIA:
      aState = kIotaAcc;
      return GREEK_UPPER_IOTA;

    case GREEK_LOWER_OMICRON_TONOS:
    case GREEK_LOWER_OMICRON_OXIA:
    case GREEK_UPPER_OMICRON_TONOS:
    case GREEK_UPPER_OMICRON_OXIA:
      aState = kOmicronAcc;
      return GREEK_UPPER_OMICRON;

    case GREEK_LOWER_UPSILON_TONOS:
    case GREEK_LOWER_UPSILON_OXIA:
    case GREEK_UPPER_UPSILON_TONOS:
    case GREEK_UPPER_UPSILON_OXIA:
      switch (aState) {
        case kOmicron:
          aState = kInWord;  // this completed a diphthong
          break;
        default:
          aState = kUpsilonAcc;
          break;
      }
      return GREEK_UPPER_UPSILON;

    case GREEK_LOWER_OMEGA_TONOS:
    case GREEK_LOWER_OMEGA_OXIA:
    case GREEK_UPPER_OMEGA_TONOS:
    case GREEK_UPPER_OMEGA_OXIA:
      aState = kOmegaAcc;
      return GREEK_UPPER_OMEGA;
  }

  // all other characters just reset the state to either kStart or kInWord,
  // and use standard mappings
  switch (category) {
    case HB_UNICODE_GENERAL_CATEGORY_LOWERCASE_LETTER:
    case HB_UNICODE_GENERAL_CATEGORY_MODIFIER_LETTER:
    case HB_UNICODE_GENERAL_CATEGORY_OTHER_LETTER:
    case HB_UNICODE_GENERAL_CATEGORY_TITLECASE_LETTER:
    case HB_UNICODE_GENERAL_CATEGORY_UPPERCASE_LETTER:
    case HB_UNICODE_GENERAL_CATEGORY_SPACING_MARK:
    case HB_UNICODE_GENERAL_CATEGORY_ENCLOSING_MARK:
    case HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK:
      aState = kInWord;
      break;
    default:
      aState = kStart;
      break;
  }

  return ToUpperCase(aCh);
}

}  // namespace mozilla
