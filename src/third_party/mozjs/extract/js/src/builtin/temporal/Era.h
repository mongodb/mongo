/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_temporal_Era_h
#define builtin_temporal_Era_h

#include "mozilla/Assertions.h"
#include "mozilla/MathAlgorithms.h"

#include <initializer_list>
#include <stddef.h>
#include <stdint.h>
#include <string_view>

#include "jstypes.h"

#include "builtin/temporal/Calendar.h"
#include "builtin/temporal/Crash.h"

namespace js::temporal {

enum class EraCode {
  // The standard era of a calendar.
  Standard,

  // The era before the standard era of a calendar.
  Inverse,

  // Named Japanese eras.
  Meiji,
  Taisho,
  Showa,
  Heisei,
  Reiwa,
};

// static variables in constexpr functions requires C++23 support, so we can't
// declare the eras directly in CalendarEras.
namespace eras {
inline constexpr auto Standard = {EraCode::Standard};

inline constexpr auto StandardInverse = {EraCode::Standard, EraCode::Inverse};

inline constexpr auto Japanese = {
    EraCode::Standard, EraCode::Inverse,

    EraCode::Meiji,    EraCode::Taisho,  EraCode::Showa,
    EraCode::Heisei,   EraCode::Reiwa,
};

// https://tc39.es/proposal-intl-era-monthcode/#table-eras
//
// Calendars which don't use eras were omitted.
namespace names {
using namespace std::literals;

// Empty placeholder.
inline constexpr auto Empty = {
    ""sv,
};

inline constexpr auto Coptic = {
    "coptic"sv,
};

inline constexpr auto CopticInverse = {
    "coptic-inverse"sv,
};

inline constexpr auto Ethiopian = {
    "ethiopic"sv,
    "incar"sv,
};

inline constexpr auto EthiopianInverse = {
    "ethioaa"sv,
    "ethiopic-amete-alem"sv,
    "mundi"sv,
};

inline constexpr auto Gregorian = {
    "gregory"sv,
    "ce"sv,
    "ad"sv,
};

inline constexpr auto GregorianInverse = {
    "gregory-inverse"sv,
    "bc"sv,
    "bce"sv,
};

inline constexpr auto Japanese = {
    "japanese"sv,
    "gregory"sv,
    "ad"sv,
    "ce"sv,
};

inline constexpr auto JapaneseInverse = {
    "japanese-inverse"sv,
    "gregory-inverse"sv,
    "bc"sv,
    "bce"sv,
};

inline constexpr auto JapaneseMeiji = {
    "meiji"sv,
};

inline constexpr auto JapaneseTaisho = {
    "taisho"sv,
};

inline constexpr auto JapaneseShowa = {
    "showa"sv,
};

inline constexpr auto JapaneseHeisei = {
    "heisei"sv,
};

inline constexpr auto JapaneseReiwa = {
    "reiwa"sv,
};

inline constexpr auto ROC = {
    "roc"sv,
    "minguo"sv,
};

inline constexpr auto ROCInverse = {
    "roc-inverse"sv,
    "before-roc"sv,
};
}  // namespace names
}  // namespace eras

constexpr auto& CalendarEras(CalendarId id) {
  switch (id) {
    case CalendarId::ISO8601:
    case CalendarId::Buddhist:
    case CalendarId::Chinese:
    case CalendarId::Dangi:
    case CalendarId::EthiopianAmeteAlem:
    case CalendarId::Hebrew:
    case CalendarId::Indian:
    case CalendarId::Islamic:
    case CalendarId::IslamicCivil:
    case CalendarId::IslamicRGSA:
    case CalendarId::IslamicTabular:
    case CalendarId::IslamicUmmAlQura:
    case CalendarId::Persian:
      return eras::Standard;

    case CalendarId::Coptic:
    case CalendarId::Ethiopian:
    case CalendarId::Gregorian:
    case CalendarId::ROC:
      return eras::StandardInverse;

    case CalendarId::Japanese:
      return eras::Japanese;
  }
  JS_CONSTEXPR_CRASH("invalid calendar id");
}

constexpr bool CalendarEraRelevant(CalendarId calendar) {
  return CalendarEras(calendar).size() > 1;
}

constexpr auto& CalendarEraNames(CalendarId calendar, EraCode era) {
  switch (calendar) {
    case CalendarId::ISO8601:
    case CalendarId::Buddhist:
    case CalendarId::Chinese:
    case CalendarId::Dangi:
    case CalendarId::EthiopianAmeteAlem:
    case CalendarId::Hebrew:
    case CalendarId::Indian:
    case CalendarId::Islamic:
    case CalendarId::IslamicCivil:
    case CalendarId::IslamicRGSA:
    case CalendarId::IslamicTabular:
    case CalendarId::IslamicUmmAlQura:
    case CalendarId::Persian:
      return eras::names::Empty;

    case CalendarId::Coptic: {
      MOZ_ASSERT(era == EraCode::Standard || era == EraCode::Inverse);
      return era == EraCode::Standard ? eras::names::Coptic
                                      : eras::names::CopticInverse;
    }

    case CalendarId::Ethiopian: {
      MOZ_ASSERT(era == EraCode::Standard || era == EraCode::Inverse);
      return era == EraCode::Standard ? eras::names::Ethiopian
                                      : eras::names::EthiopianInverse;
    }

    case CalendarId::Gregorian: {
      MOZ_ASSERT(era == EraCode::Standard || era == EraCode::Inverse);
      return era == EraCode::Standard ? eras::names::Gregorian
                                      : eras::names::GregorianInverse;
    }

    case CalendarId::Japanese: {
      switch (era) {
        case EraCode::Standard:
          return eras::names::Japanese;
        case EraCode::Inverse:
          return eras::names::JapaneseInverse;
        case EraCode::Meiji:
          return eras::names::JapaneseMeiji;
        case EraCode::Taisho:
          return eras::names::JapaneseTaisho;
        case EraCode::Showa:
          return eras::names::JapaneseShowa;
        case EraCode::Heisei:
          return eras::names::JapaneseHeisei;
        case EraCode::Reiwa:
          return eras::names::JapaneseReiwa;
      }
      break;
    }

    case CalendarId::ROC: {
      MOZ_ASSERT(era == EraCode::Standard || era == EraCode::Inverse);
      return era == EraCode::Standard ? eras::names::ROC
                                      : eras::names::ROCInverse;
    }
  }
  JS_CONSTEXPR_CRASH("invalid era");
}

constexpr auto CalendarEraName(CalendarId calendar, EraCode era) {
  auto& names = CalendarEraNames(calendar, era);
  MOZ_ASSERT(names.size() > 0);
  return *names.begin();
}

constexpr bool CalendarEraStartsAtYearBoundary(CalendarId id) {
  switch (id) {
    // Calendar system which use a single era.
    case CalendarId::ISO8601:
    case CalendarId::Buddhist:
    case CalendarId::Chinese:
    case CalendarId::Dangi:
    case CalendarId::EthiopianAmeteAlem:
    case CalendarId::Hebrew:
    case CalendarId::Indian:
    case CalendarId::Islamic:
    case CalendarId::IslamicCivil:
    case CalendarId::IslamicRGSA:
    case CalendarId::IslamicTabular:
    case CalendarId::IslamicUmmAlQura:
    case CalendarId::Persian:
      return true;

    // Calendar system which use multiple eras, but each era starts at a year
    // boundary.
    case CalendarId::Coptic:
    case CalendarId::Ethiopian:
    case CalendarId::Gregorian:
    case CalendarId::ROC:
      return true;

    // Calendar system which use multiple eras and eras can start within a year.
    case CalendarId::Japanese:
      return false;
  }
  JS_CONSTEXPR_CRASH("invalid calendar id");
}

constexpr bool CalendarEraStartsAtYearBoundary(CalendarId id, EraCode era) {
  MOZ_ASSERT_IF(id != CalendarId::Japanese,
                CalendarEraStartsAtYearBoundary(id));
  return era == EraCode::Standard || era == EraCode::Inverse;
}

struct EraYear {
  EraCode era = EraCode::Standard;
  int32_t year = 0;
};

constexpr EraYear CalendarEraYear(CalendarId id, int32_t year) {
  if (year > 0 || !CalendarEraRelevant(id)) {
    return EraYear{EraCode::Standard, year};
  }
  return EraYear{EraCode::Inverse, int32_t(mozilla::Abs(year) + 1)};
}

}  // namespace js::temporal

#endif /* builtin_temporal_Era_h */
