/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gtest/gtest.h"

#include "mozilla/intl/Calendar.h"
#include "mozilla/intl/DateTimeFormat.h"
#include "mozilla/intl/DateTimePatternGenerator.h"
#include "mozilla/Span.h"
#include "./TestBuffer.h"

namespace mozilla::intl {

// Firefox 1.0 release date.
const double DATE = 1032800850000.0;

static UniquePtr<DateTimeFormat> testStyle(const char* aLocale,
                                           DateTimeStyle aDateStyle,
                                           DateTimeStyle aTimeStyle) {
  // Always specify a time zone in the tests, otherwise it will use the system
  // time zone which can vary between test runs.
  return DateTimeFormat::TryCreateFromStyle(MakeStringSpan(aLocale), aDateStyle,
                                            aTimeStyle,
                                            Some(MakeStringSpan(u"GMT+3")))
      .unwrap();
}

TEST(IntlDateTimeFormat, Style_enUS_utf8)
{
  auto dtFormat =
      testStyle("en-US", DateTimeStyle::Medium, DateTimeStyle::Medium);
  TestBuffer<uint8_t> buffer;
  dtFormat->TryFormat(DATE, buffer).unwrap();

  ASSERT_EQ(buffer.get_string_view<char>(), "Sep 23, 2002, 8:07:30 PM");
}

TEST(IntlDateTimeFormat, Style_enUS_utf16)
{
  auto dtFormat =
      testStyle("en-US", DateTimeStyle::Medium, DateTimeStyle::Medium);
  TestBuffer<char16_t> buffer;
  dtFormat->TryFormat(DATE, buffer).unwrap();

  ASSERT_EQ(buffer.get_string_view<char16_t>(), u"Sep 23, 2002, 8:07:30 PM");
}

TEST(IntlDateTimeFormat, Style_ar_utf8)
{
  auto dtFormat = testStyle("ar", DateTimeStyle::Medium, DateTimeStyle::None);
  TestBuffer<uint8_t> buffer;
  dtFormat->TryFormat(DATE, buffer).unwrap();

  ASSERT_EQ(buffer.get_string_view<char>(), "٨:٠٧:٣٠ م");
}

TEST(IntlDateTimeFormat, Style_ar_utf16)
{
  auto dtFormat = testStyle("ar", DateTimeStyle::Medium, DateTimeStyle::None);
  TestBuffer<char16_t> buffer;
  dtFormat->TryFormat(DATE, buffer).unwrap();

  ASSERT_EQ(buffer.get_string_view<char16_t>(), u"٨:٠٧:٣٠ م");
}

TEST(IntlDateTimeFormat, Style_enUS_fallback_to_default_styles)
{
  auto dtFormat = testStyle("en-US", DateTimeStyle::None, DateTimeStyle::None);
  TestBuffer<uint8_t> buffer;
  dtFormat->TryFormat(DATE, buffer).unwrap();

  ASSERT_EQ(buffer.get_string_view<char>(), "Sep 23, 2002, 8:07:30 PM");
}

TEST(IntlDateTimeFormat, Skeleton_enUS_utf8_in)
{
  UniquePtr<DateTimeFormat> dtFormat =
      DateTimeFormat::TryCreateFromSkeleton(
          "en-US", MakeStringSpan("yMdhhmmss"), Some(MakeStringSpan("GMT+3")))
          .unwrap();
  TestBuffer<uint8_t> buffer;
  dtFormat->TryFormat(DATE, buffer).unwrap();

  ASSERT_EQ(buffer.get_string_view<char>(), "9/23/2002, 8:07:30 PM");
}

TEST(IntlDateTimeFormat, Skeleton_enUS_utf16_in)
{
  UniquePtr<DateTimeFormat> dtFormat =
      DateTimeFormat::TryCreateFromSkeleton(
          "en-US", MakeStringSpan(u"yMdhhmmss"), Some(MakeStringSpan(u"GMT+3")))
          .unwrap();
  TestBuffer<uint8_t> buffer;
  dtFormat->TryFormat(DATE, buffer).unwrap();

  ASSERT_EQ(buffer.get_string_view<char>(), "9/23/2002, 8:07:30 PM");
}

TEST(IntlDateTimeFormat, Time_zone_IANA_identifier)
{
  auto dtFormat =
      DateTimeFormat::TryCreateFromStyle(
          MakeStringSpan("en-US"), DateTimeStyle::Medium, DateTimeStyle::Medium,
          Some(MakeStringSpan(u"America/Chicago")))
          .unwrap();
  TestBuffer<uint8_t> buffer;
  dtFormat->TryFormat(DATE, buffer).unwrap();
  ASSERT_EQ(buffer.get_string_view<char>(), "Sep 23, 2002, 12:07:30 PM");
}

TEST(IntlDateTimePatternGenerator, GetBestPattern)
{
  auto gen = DateTimePatternGenerator::TryCreate("en").unwrap();
  TestBuffer<char16_t> buffer;

  gen->GetBestPattern(MakeStringSpan(u"yMd"), buffer).unwrap();
  ASSERT_EQ(buffer.get_string_view<char16_t>(), u"M/d/y");
}

TEST(IntlDateTimePatternGenerator, GetSkeleton)
{
  auto gen = DateTimePatternGenerator::TryCreate("en").unwrap();
  TestBuffer<char16_t> buffer;

  DateTimePatternGenerator::GetSkeleton(MakeStringSpan(u"M/d/y"), buffer)
      .unwrap();
  ASSERT_EQ(buffer.get_string_view<char16_t>(), u"yMd");
}

}  // namespace mozilla::intl
