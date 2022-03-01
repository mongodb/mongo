/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gtest/gtest.h"

#include "mozilla/intl/Calendar.h"
#include "mozilla/Span.h"
#include "./TestBuffer.h"

namespace mozilla::intl {

// Firefox 1.0 release date.
const double CALENDAR_DATE = 1032800850000.0;

TEST(IntlCalendar, GetLegacyKeywordValuesForLocale)
{
  bool hasGregorian = false;
  bool hasIslamic = false;
  auto gregorian = MakeStringSpan("gregorian");
  auto islamic = MakeStringSpan("islamic");
  auto keywords = Calendar::GetLegacyKeywordValuesForLocale("en-US").unwrap();
  for (auto name : keywords) {
    // Check a few keywords, as this list may not be stable between ICU updates.
    if (name.unwrap() == gregorian) {
      hasGregorian = true;
    }
    if (name.unwrap() == islamic) {
      hasIslamic = true;
    }
  }
  ASSERT_TRUE(hasGregorian);
  ASSERT_TRUE(hasIslamic);
}

TEST(IntlCalendar, GetBcp47KeywordValuesForLocale)
{
  bool hasGregory = false;
  bool hasIslamic = false;
  auto gregory = MakeStringSpan("gregory");
  auto islamic = MakeStringSpan("islamic");
  auto keywords = Calendar::GetBcp47KeywordValuesForLocale("en-US").unwrap();
  for (auto name : keywords) {
    // Check a few keywords, as this list may not be stable between ICU updates.
    if (name.unwrap() == gregory) {
      hasGregory = true;
    }
    if (name.unwrap() == islamic) {
      hasIslamic = true;
    }
  }
  ASSERT_TRUE(hasGregory);
  ASSERT_TRUE(hasIslamic);
}

TEST(IntlCalendar, GetBcp47Type)
{
  auto calendar =
      Calendar::TryCreate("en-US", Some(MakeStringSpan(u"GMT+3"))).unwrap();
  ASSERT_STREQ(calendar->GetBcp47Type().unwrap(), "gregory");
}

// These tests are dependent on the machine that this test is being run on.
// Unwrap the results to ensure it doesn't fail, but don't check the values.
TEST(IntlCalendar, SystemDependentTests)
{
  auto calendar =
      Calendar::TryCreate("en-US", Some(MakeStringSpan(u"GMT+3"))).unwrap();
  TestBuffer<char16_t> buffer;
  // e.g. For America/Chicago: 1000 * 60 * 60 * -6
  calendar->GetDefaultTimeZoneOffsetMs().unwrap();

  // e.g. "America/Chicago"
  Calendar::GetDefaultTimeZone(buffer).unwrap();

  // This isn't system dependent, but currently there is no way to verify the
  // results.
  calendar->SetTimeInMs(CALENDAR_DATE).unwrap();
}

TEST(IntlCalendar, CloneFrom)
{
  auto dtFormat =
      DateTimeFormat::TryCreateFromStyle(
          MakeStringSpan("en-US"), DateTimeStyle::Medium, DateTimeStyle::Medium,
          Some(MakeStringSpan(u"America/Chicago")))
          .unwrap();

  dtFormat->CloneCalendar(CALENDAR_DATE).unwrap();
}

TEST(IntlCalendar, GetCanonicalTimeZoneID)
{
  TestBuffer<char16_t> buffer;

  // Providing a canonical time zone results in the same string at the end.
  Calendar::GetCanonicalTimeZoneID(MakeStringSpan(u"America/Chicago"), buffer)
      .unwrap();
  ASSERT_EQ(buffer.get_string_view<char16_t>(), u"America/Chicago");

  // Providing an alias will result in the canonical representation.
  Calendar::GetCanonicalTimeZoneID(MakeStringSpan(u"Europe/Belfast"), buffer)
      .unwrap();
  ASSERT_EQ(buffer.get_string_view<char16_t>(), u"Europe/London");

  // An unknown time zone results in an error.
  ASSERT_TRUE(Calendar::GetCanonicalTimeZoneID(
                  MakeStringSpan(u"Not a time zone"), buffer)
                  .isErr());
}

}  // namespace mozilla::intl
