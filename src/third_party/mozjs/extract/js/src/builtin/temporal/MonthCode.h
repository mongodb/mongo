/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_temporal_MonthCode_h
#define builtin_temporal_MonthCode_h

#include "mozilla/Assertions.h"
#include "mozilla/EnumSet.h"

#include <initializer_list>
#include <stddef.h>
#include <stdint.h>
#include <string_view>
#include <utility>

#include "jstypes.h"

#include "builtin/temporal/Calendar.h"
#include "builtin/temporal/Crash.h"

namespace js::temporal {

class MonthCode final {
 public:
  enum class Code {
    Invalid = 0,

    // Months 01 - M12.
    M01 = 1,
    M02,
    M03,
    M04,
    M05,
    M06,
    M07,
    M08,
    M09,
    M10,
    M11,
    M12,

    // Epagomenal month M13.
    M13,

    // Leap months M01 - M12.
    M01L,
    M02L,
    M03L,
    M04L,
    M05L,
    M06L,
    M07L,
    M08L,
    M09L,
    M10L,
    M11L,
    M12L,
  };

 private:
  static constexpr int32_t toLeapMonth =
      static_cast<int32_t>(Code::M01L) - static_cast<int32_t>(Code::M01);

  Code code_ = Code::Invalid;

 public:
  constexpr MonthCode() = default;

  constexpr explicit MonthCode(Code code) : code_(code) {}

  constexpr explicit MonthCode(int32_t month, bool isLeapMonth = false) {
    MOZ_ASSERT(1 <= month && month <= 13);
    MOZ_ASSERT_IF(isLeapMonth, 1 <= month && month <= 12);

    code_ = static_cast<Code>(month + (isLeapMonth ? toLeapMonth : 0));
  }

  constexpr auto code() const { return code_; }

  constexpr int32_t ordinal() const {
    int32_t ordinal = static_cast<int32_t>(code_);
    if (isLeapMonth()) {
      ordinal -= toLeapMonth;
    }
    return ordinal;
  }

  constexpr bool isLeapMonth() const { return code_ >= Code::M01L; }

  constexpr bool operator==(const MonthCode& other) const {
    return other.code_ == code_;
  }

  constexpr bool operator!=(const MonthCode& other) const {
    return !(*this == other);
  }

  constexpr bool operator<(const MonthCode& other) const {
    if (ordinal() != other.ordinal()) {
      return ordinal() < other.ordinal();
    }
    return code_ < other.code_;
  }

  constexpr bool operator>(const MonthCode& other) const {
    return other < *this;
  }

  constexpr bool operator<=(const MonthCode& other) const {
    return !(other < *this);
  }

  constexpr bool operator>=(const MonthCode& other) const {
    return !(*this < other);
  }

  constexpr explicit operator std::string_view() const {
    constexpr const char* name =
        "M01L"
        "M02L"
        "M03L"
        "M04L"
        "M05L"
        "M06L"
        "M07L"
        "M08L"
        "M09L"
        "M10L"
        "M11L"
        "M12L"
        "M13";
    size_t index = (ordinal() - 1) * 4;
    size_t length = 3 + isLeapMonth();
    return {name + index, length};
  }

  /**
   * Returns the maximum non-leap month. This is the epagomenal month "M13".
   */
  constexpr static auto maxNonLeapMonth() { return MonthCode{Code::M13}; }

  /**
   * Returns the maximum leap month.
   */
  constexpr static auto maxLeapMonth() { return MonthCode{Code::M12L}; }
};

class MonthCodes final {
  mozilla::EnumSet<MonthCode::Code> monthCodes_{
      MonthCode::Code::M01, MonthCode::Code::M02, MonthCode::Code::M03,
      MonthCode::Code::M04, MonthCode::Code::M05, MonthCode::Code::M06,
      MonthCode::Code::M07, MonthCode::Code::M08, MonthCode::Code::M09,
      MonthCode::Code::M10, MonthCode::Code::M11, MonthCode::Code::M12,
  };

 public:
  constexpr MOZ_IMPLICIT MonthCodes(std::initializer_list<MonthCode> list) {
    for (auto value : list) {
      monthCodes_ += value.code();
    }
  }

  bool contains(MonthCode monthCode) const {
    return monthCodes_.contains(monthCode.code());
  }

  bool contains(const MonthCodes& monthCodes) const {
    return monthCodes_.contains(monthCodes.monthCodes_);
  }
};

// static variables in constexpr functions requires C++23 support, so we can't
// declare the month codes directly in CalendarMonthCodes.
//
// https://tc39.es/proposal-intl-era-monthcode/#table-additional-month-codes
//
// https://docs.rs/icu/latest/icu/calendar/buddhist/struct.Buddhist.html#month-codes
// https://docs.rs/icu/latest/icu/calendar/indian/struct.Indian.html#month-codes
// https://docs.rs/icu/latest/icu/calendar/islamic/struct.IslamicCivil.html#month-codes
// https://docs.rs/icu/latest/icu/calendar/islamic/struct.IslamicObservational.html#month-codes
// https://docs.rs/icu/latest/icu/calendar/islamic/struct.IslamicTabular.html#month-codes
// https://docs.rs/icu/latest/icu/calendar/islamic/struct.IslamicUmmAlQura.html#month-codes
// https://docs.rs/icu/latest/icu/calendar/japanese/struct.Japanese.html#month-codes
// https://docs.rs/icu/latest/icu/calendar/persian/struct.Persian.html#month-codes
// https://docs.rs/icu/latest/icu/calendar/roc/struct.Roc.html#month-codes
//
// https://docs.rs/icu/latest/icu/calendar/chinese/struct.Chinese.html#month-codes
// https://docs.rs/icu/latest/icu/calendar/dangi/struct.Dangi.html#month-codes
//
// https://docs.rs/icu/latest/icu/calendar/coptic/struct.Coptic.html#month-codes
//
// https://docs.rs/icu/latest/icu/calendar/ethiopian/struct.Ethiopian.html#month-codes
// https://docs.rs/icu/latest/icu/calendar/hebrew/struct.Hebrew.html#month-codes
namespace monthcodes {
inline constexpr MonthCodes ISO8601 = {};

inline constexpr MonthCodes ChineseOrDangi = {
    // Leap months.
    MonthCode{1, /* isLeapMonth = */ true},
    MonthCode{2, /* isLeapMonth = */ true},
    MonthCode{3, /* isLeapMonth = */ true},
    MonthCode{4, /* isLeapMonth = */ true},
    MonthCode{5, /* isLeapMonth = */ true},
    MonthCode{6, /* isLeapMonth = */ true},
    MonthCode{7, /* isLeapMonth = */ true},
    MonthCode{8, /* isLeapMonth = */ true},
    MonthCode{9, /* isLeapMonth = */ true},
    MonthCode{10, /* isLeapMonth = */ true},
    MonthCode{11, /* isLeapMonth = */ true},
    MonthCode{12, /* isLeapMonth = */ true},
};

inline constexpr MonthCodes CopticOrEthiopian = {
    // Short epagomenal month.
    MonthCode{13},
};

inline constexpr MonthCodes Hebrew = {
    // Leap month Adar I.
    MonthCode{5, /* isLeapMonth = */ true},
};
}  // namespace monthcodes

constexpr auto& CalendarMonthCodes(CalendarId id) {
  switch (id) {
    case CalendarId::ISO8601:
    case CalendarId::Buddhist:
    case CalendarId::Gregorian:
    case CalendarId::Indian:
    case CalendarId::Islamic:
    case CalendarId::IslamicCivil:
    case CalendarId::IslamicRGSA:
    case CalendarId::IslamicTabular:
    case CalendarId::IslamicUmmAlQura:
    case CalendarId::Persian:
    case CalendarId::Japanese:
    case CalendarId::ROC:
      return monthcodes::ISO8601;

    case CalendarId::Chinese:
    case CalendarId::Dangi:
      return monthcodes::ChineseOrDangi;

    case CalendarId::Coptic:
    case CalendarId::Ethiopian:
    case CalendarId::EthiopianAmeteAlem:
      return monthcodes::CopticOrEthiopian;

    case CalendarId::Hebrew:
      return monthcodes::Hebrew;
  }
  JS_CONSTEXPR_CRASH("invalid calendar id");
}

constexpr bool CalendarHasLeapMonths(CalendarId id) {
  switch (id) {
    case CalendarId::ISO8601:
    case CalendarId::Buddhist:
    case CalendarId::Coptic:
    case CalendarId::Ethiopian:
    case CalendarId::EthiopianAmeteAlem:
    case CalendarId::Gregorian:
    case CalendarId::Indian:
    case CalendarId::Islamic:
    case CalendarId::IslamicCivil:
    case CalendarId::IslamicRGSA:
    case CalendarId::IslamicTabular:
    case CalendarId::IslamicUmmAlQura:
    case CalendarId::Japanese:
    case CalendarId::Persian:
    case CalendarId::ROC:
      return false;

    case CalendarId::Chinese:
    case CalendarId::Dangi:
    case CalendarId::Hebrew:
      return true;
  }
  JS_CONSTEXPR_CRASH("invalid calendar id");
}

constexpr bool CalendarHasEpagomenalMonths(CalendarId id) {
  switch (id) {
    case CalendarId::ISO8601:
    case CalendarId::Buddhist:
    case CalendarId::Chinese:
    case CalendarId::Dangi:
    case CalendarId::Gregorian:
    case CalendarId::Hebrew:
    case CalendarId::Indian:
    case CalendarId::Islamic:
    case CalendarId::IslamicCivil:
    case CalendarId::IslamicRGSA:
    case CalendarId::IslamicTabular:
    case CalendarId::IslamicUmmAlQura:
    case CalendarId::Japanese:
    case CalendarId::Persian:
    case CalendarId::ROC:
      return false;

    case CalendarId::Coptic:
    case CalendarId::Ethiopian:
    case CalendarId::EthiopianAmeteAlem:
      return true;
  }
  JS_CONSTEXPR_CRASH("invalid calendar id");
}

constexpr int32_t CalendarMonthsPerYear(CalendarId id) {
  if (CalendarHasLeapMonths(id) || CalendarHasEpagomenalMonths(id)) {
    return 13;
  }
  return 12;
}

constexpr std::pair<int32_t, int32_t> CalendarDaysInMonth(CalendarId id) {
  switch (id) {
    // ISO8601 calendar.
    // M02: 28-29 days
    // M04, M06, M09, M11: 30 days
    // M01, M03, M05, M07, M08, M10, M12: 31 days
    case CalendarId::ISO8601:
    case CalendarId::Buddhist:
    case CalendarId::Gregorian:
    case CalendarId::Japanese:
    case CalendarId::ROC:
      return {28, 31};

    // Chinese/Dangi calendars have 29-30 days per month.
    //
    // Hebrew:
    // M01, M05, M07, M09, M11: 30 days.
    // M02, M03: 29-30 days.
    // M04, M06, M08, M10, M12: 29 days.
    // M05L: 30 days
    //
    // Islamic calendars have 29-30 days.
    //
    // IslamicCivil, IslamicTabular:
    // M01, M03, M05, M07, M09, M11: 30 days
    // M02, M04, M06, M08, M10: 29 days
    // M12: 29-30 days.
    case CalendarId::Chinese:
    case CalendarId::Dangi:
    case CalendarId::Hebrew:
    case CalendarId::Islamic:
    case CalendarId::IslamicCivil:
    case CalendarId::IslamicRGSA:
    case CalendarId::IslamicTabular:
    case CalendarId::IslamicUmmAlQura:
      return {29, 30};

    // Coptic, Ethiopian, EthiopianAmeteAlem:
    // M01..M12: 30 days.
    // M13: 5-6 days.
    case CalendarId::Coptic:
    case CalendarId::Ethiopian:
    case CalendarId::EthiopianAmeteAlem:
      return {5, 30};

    // Indian:
    // M1: 30-31 days.
    // M02..M06: 31 days
    // M07..M12: 30 days
    case CalendarId::Indian:
      return {30, 31};

    // Persian:
    // M01..M06: 31 days
    // M07..M11: 30 days
    // M12: 29-30 days
    case CalendarId::Persian:
      return {29, 31};
  }
  JS_CONSTEXPR_CRASH("invalid calendar id");
}

// ISO8601 calendar.
// M02: 28-29 days
// M04, M06, M09, M11: 30 days
// M01, M03, M05, M07, M08, M10, M12: 31 days
constexpr std::pair<int32_t, int32_t> ISODaysInMonth(MonthCode monthCode) {
  int32_t ordinal = monthCode.ordinal();
  if (ordinal == 2) {
    return {28, 29};
  }
  if (ordinal == 4 || ordinal == 6 || ordinal == 9 || ordinal == 11) {
    return {30, 30};
  }
  return {31, 31};
}

constexpr std::pair<int32_t, int32_t> CalendarDaysInMonth(CalendarId id,
                                                          MonthCode monthCode) {
  switch (id) {
    case CalendarId::ISO8601:
    case CalendarId::Buddhist:
    case CalendarId::Gregorian:
    case CalendarId::Japanese:
    case CalendarId::ROC:
      return ISODaysInMonth(monthCode);

    // Chinese/Dangi calendars have 29-30 days per month.
    case CalendarId::Chinese:
    case CalendarId::Dangi:
      return {29, 30};

    // Coptic, Ethiopian, EthiopianAmeteAlem:
    // M01..M12: 30 days.
    // M13: 5-6 days.
    case CalendarId::Coptic:
    case CalendarId::Ethiopian:
    case CalendarId::EthiopianAmeteAlem: {
      if (monthCode.ordinal() <= 12) {
        return {30, 30};
      }
      return {5, 6};
    }

    // Hebrew:
    // M01, M05, M07, M09, M11: 30 days.
    // M02, M03: 29-30 days.
    // M04, M06, M08, M10, M12: 29 days.
    // M05L: 30 days
    case CalendarId::Hebrew: {
      int32_t ordinal = monthCode.ordinal();
      if (ordinal == 2 || ordinal == 3) {
        return {29, 30};
      }
      if ((ordinal & 1) == 1 || monthCode.isLeapMonth()) {
        return {30, 30};
      }
      return {29, 29};
    }

    // Indian:
    // M1: 30-31 days.
    // M02..M06: 31 days
    // M07..M12: 30 days
    case CalendarId::Indian: {
      int32_t ordinal = monthCode.ordinal();
      if (ordinal == 1) {
        return {30, 31};
      }
      if (ordinal <= 6) {
        return {31, 31};
      }
      return {30, 30};
    }

    // Islamic calendars have 29-30 days per month.
    case CalendarId::Islamic:
    case CalendarId::IslamicRGSA:
    case CalendarId::IslamicUmmAlQura:
      return {29, 30};

    // IslamicCivil, IslamicTabular:
    // M01, M03, M05, M07, M09, M11: 30 days
    // M02, M04, M06, M08, M10: 29 days
    // M12: 29-30 days.
    case CalendarId::IslamicCivil:
    case CalendarId::IslamicTabular: {
      int32_t ordinal = monthCode.ordinal();
      if ((ordinal & 1) == 1) {
        return {30, 30};
      }
      if (ordinal < 12) {
        return {29, 29};
      }
      return {29, 30};
    }

    // Persian:
    // M01..M06: 31 days
    // M07..M11: 30 days
    // M12: 29-30 days
    case CalendarId::Persian: {
      int32_t ordinal = monthCode.ordinal();
      if (ordinal <= 6) {
        return {31, 31};
      }
      if (ordinal <= 11) {
        return {30, 30};
      }
      return {29, 30};
    }
  }
  JS_CONSTEXPR_CRASH("invalid calendar id");
}

}  // namespace js::temporal

#endif /* builtin_temporal_MonthCode_h */
