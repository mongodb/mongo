#include "gtest/gtest.h"
#include "DateTimeFormat.h"

namespace mozilla {

TEST(DateTimeFormat, FormatPRExplodedTime)
{
  PRTime prTime = 0;
  PRExplodedTime prExplodedTime;
  PR_ExplodeTime(prTime, PR_GMTParameters, &prExplodedTime);

  mozilla::DateTimeFormat::mLocale = new nsCString("en-US");
  mozilla::DateTimeFormat::DeleteCache();

  nsAutoString formattedTime;
  nsresult rv = mozilla::DateTimeFormat::FormatPRExplodedTime(
      kDateFormatLong, kTimeFormatLong, &prExplodedTime, formattedTime);
  ASSERT_TRUE(NS_SUCCEEDED(rv));
  ASSERT_TRUE(formattedTime.Find("January") != kNotFound);
  ASSERT_TRUE(formattedTime.Find("1970") != kNotFound);
  ASSERT_TRUE(formattedTime.Find("12:00:00 AM") != kNotFound ||
              formattedTime.Find("00:00:00") != kNotFound);

  prExplodedTime = {0, 0, 19, 0, 1, 0, 1970, 4, 0, {(19 * 60), 0}};
  rv = mozilla::DateTimeFormat::FormatPRExplodedTime(
      kDateFormatLong, kTimeFormatLong, &prExplodedTime, formattedTime);
  ASSERT_TRUE(NS_SUCCEEDED(rv));
  ASSERT_TRUE(formattedTime.Find("January") != kNotFound);
  ASSERT_TRUE(formattedTime.Find("1970") != kNotFound);
  ASSERT_TRUE(formattedTime.Find("12:19:00 AM") != kNotFound ||
              formattedTime.Find("00:19:00") != kNotFound);

  prExplodedTime = {0, 0,    0, 7, 1,
                    0, 1970, 4, 0, {(6 * 60 * 60), (1 * 60 * 60)}};
  rv = mozilla::DateTimeFormat::FormatPRExplodedTime(
      kDateFormatLong, kTimeFormatLong, &prExplodedTime, formattedTime);
  ASSERT_TRUE(NS_SUCCEEDED(rv));
  ASSERT_TRUE(formattedTime.Find("January") != kNotFound);
  ASSERT_TRUE(formattedTime.Find("1970") != kNotFound);
  ASSERT_TRUE(formattedTime.Find("7:00:00 AM") != kNotFound ||
              formattedTime.Find("07:00:00") != kNotFound);

  prExplodedTime = {
      0, 0,    29, 11, 1,
      0, 1970, 4,  0,  {(10 * 60 * 60) + (29 * 60), (1 * 60 * 60)}};
  rv = mozilla::DateTimeFormat::FormatPRExplodedTime(
      kDateFormatLong, kTimeFormatLong, &prExplodedTime, formattedTime);
  ASSERT_TRUE(NS_SUCCEEDED(rv));
  ASSERT_TRUE(formattedTime.Find("January") != kNotFound);
  ASSERT_TRUE(formattedTime.Find("1970") != kNotFound);
  ASSERT_TRUE(formattedTime.Find("11:29:00 AM") != kNotFound ||
              formattedTime.Find("11:29:00") != kNotFound);

  prExplodedTime = {0, 0, 37, 23, 31, 11, 1969, 3, 364, {-(23 * 60), 0}};
  rv = mozilla::DateTimeFormat::FormatPRExplodedTime(
      kDateFormatLong, kTimeFormatLong, &prExplodedTime, formattedTime);
  ASSERT_TRUE(NS_SUCCEEDED(rv));
  ASSERT_TRUE(formattedTime.Find("December") != kNotFound);
  ASSERT_TRUE(formattedTime.Find("31") != kNotFound);
  ASSERT_TRUE(formattedTime.Find("1969") != kNotFound);
  ASSERT_TRUE(formattedTime.Find("11:37:00 PM") != kNotFound ||
              formattedTime.Find("23:37:00") != kNotFound);

  prExplodedTime = {0, 0, 0, 17, 31, 11, 1969, 3, 364, {-(7 * 60 * 60), 0}};
  rv = mozilla::DateTimeFormat::FormatPRExplodedTime(
      kDateFormatLong, kTimeFormatLong, &prExplodedTime, formattedTime);
  ASSERT_TRUE(NS_SUCCEEDED(rv));
  ASSERT_TRUE(formattedTime.Find("December") != kNotFound);
  ASSERT_TRUE(formattedTime.Find("31") != kNotFound);
  ASSERT_TRUE(formattedTime.Find("1969") != kNotFound);
  ASSERT_TRUE(formattedTime.Find("5:00:00 PM") != kNotFound ||
              formattedTime.Find("17:00:00") != kNotFound);

  prExplodedTime = {
      0,  0,    47, 14,  31,
      11, 1969, 3,  364, {-((10 * 60 * 60) + (13 * 60)), (1 * 60 * 60)}};
  rv = mozilla::DateTimeFormat::FormatPRExplodedTime(
      kDateFormatLong, kTimeFormatLong, &prExplodedTime, formattedTime);
  ASSERT_TRUE(NS_SUCCEEDED(rv));
  ASSERT_TRUE(formattedTime.Find("December") != kNotFound);
  ASSERT_TRUE(formattedTime.Find("31") != kNotFound);
  ASSERT_TRUE(formattedTime.Find("1969") != kNotFound);
  ASSERT_TRUE(formattedTime.Find("2:47:00 PM") != kNotFound ||
              formattedTime.Find("14:47:00") != kNotFound);
}

TEST(DateTimeFormat, DateFormatSelectors)
{
  PRTime prTime = 0;
  PRExplodedTime prExplodedTime;
  PR_ExplodeTime(prTime, PR_GMTParameters, &prExplodedTime);

  mozilla::DateTimeFormat::mLocale = new nsCString("en-US");
  mozilla::DateTimeFormat::DeleteCache();

  nsAutoString formattedTime;
  nsresult rv = mozilla::DateTimeFormat::FormatDateTime(
      &prExplodedTime, DateTimeFormat::Skeleton::yyyyMM, formattedTime);
  ASSERT_TRUE(NS_SUCCEEDED(rv));
  ASSERT_STREQ("01/1970", NS_ConvertUTF16toUTF8(formattedTime).get());

  rv = mozilla::DateTimeFormat::FormatDateTime(
      &prExplodedTime, DateTimeFormat::Skeleton::yyyyMMMM, formattedTime);
  ASSERT_TRUE(NS_SUCCEEDED(rv));
  ASSERT_STREQ("January 1970", NS_ConvertUTF16toUTF8(formattedTime).get());

  rv = mozilla::DateTimeFormat::GetCalendarSymbol(
      mozilla::DateTimeFormat::Field::Month,
      mozilla::DateTimeFormat::Style::Wide, &prExplodedTime, formattedTime);
  ASSERT_TRUE(NS_SUCCEEDED(rv));
  ASSERT_STREQ("January", NS_ConvertUTF16toUTF8(formattedTime).get());

  rv = mozilla::DateTimeFormat::GetCalendarSymbol(
      mozilla::DateTimeFormat::Field::Weekday,
      mozilla::DateTimeFormat::Style::Abbreviated, &prExplodedTime,
      formattedTime);
  ASSERT_TRUE(NS_SUCCEEDED(rv));
  ASSERT_STREQ("Thu", NS_ConvertUTF16toUTF8(formattedTime).get());
}

TEST(DateTimeFormat, FormatPRExplodedTimeForeign)
{
  PRTime prTime = 0;
  PRExplodedTime prExplodedTime;
  PR_ExplodeTime(prTime, PR_GMTParameters, &prExplodedTime);

  mozilla::DateTimeFormat::mLocale = new nsCString("de-DE");
  mozilla::DateTimeFormat::DeleteCache();

  nsAutoString formattedTime;
  nsresult rv = mozilla::DateTimeFormat::FormatPRExplodedTime(
      kDateFormatLong, kTimeFormatLong, &prExplodedTime, formattedTime);
  ASSERT_TRUE(NS_SUCCEEDED(rv));
  ASSERT_TRUE(formattedTime.Find("1.") != kNotFound);
  ASSERT_TRUE(formattedTime.Find("Januar") != kNotFound);
  ASSERT_TRUE(formattedTime.Find("1970") != kNotFound);
  ASSERT_TRUE(formattedTime.Find("12:00:00 AM") != kNotFound ||
              formattedTime.Find("00:00:00") != kNotFound);

  prExplodedTime = {0, 0, 19, 0, 1, 0, 1970, 4, 0, {(19 * 60), 0}};
  rv = mozilla::DateTimeFormat::FormatPRExplodedTime(
      kDateFormatLong, kTimeFormatLong, &prExplodedTime, formattedTime);
  ASSERT_TRUE(NS_SUCCEEDED(rv));
  ASSERT_TRUE(formattedTime.Find("1.") != kNotFound);
  ASSERT_TRUE(formattedTime.Find("Januar") != kNotFound);
  ASSERT_TRUE(formattedTime.Find("1970") != kNotFound);
  ASSERT_TRUE(formattedTime.Find("12:19:00 AM") != kNotFound ||
              formattedTime.Find("00:19:00") != kNotFound);

  prExplodedTime = {0, 0,    0, 7, 1,
                    0, 1970, 4, 0, {(6 * 60 * 60), (1 * 60 * 60)}};
  rv = mozilla::DateTimeFormat::FormatPRExplodedTime(
      kDateFormatLong, kTimeFormatLong, &prExplodedTime, formattedTime);
  ASSERT_TRUE(NS_SUCCEEDED(rv));
  ASSERT_TRUE(formattedTime.Find("1.") != kNotFound);
  ASSERT_TRUE(formattedTime.Find("Januar") != kNotFound);
  ASSERT_TRUE(formattedTime.Find("1970") != kNotFound);
  ASSERT_TRUE(formattedTime.Find("7:00:00 AM") != kNotFound ||
              formattedTime.Find("07:00:00") != kNotFound);

  prExplodedTime = {
      0, 0,    29, 11, 1,
      0, 1970, 4,  0,  {(10 * 60 * 60) + (29 * 60), (1 * 60 * 60)}};
  rv = mozilla::DateTimeFormat::FormatPRExplodedTime(
      kDateFormatLong, kTimeFormatLong, &prExplodedTime, formattedTime);
  ASSERT_TRUE(NS_SUCCEEDED(rv));
  ASSERT_TRUE(formattedTime.Find("1.") != kNotFound);
  ASSERT_TRUE(formattedTime.Find("Januar") != kNotFound);
  ASSERT_TRUE(formattedTime.Find("1970") != kNotFound);
  ASSERT_TRUE(formattedTime.Find("11:29:00 AM") != kNotFound ||
              formattedTime.Find("11:29:00") != kNotFound);

  prExplodedTime = {0, 0, 37, 23, 31, 11, 1969, 3, 364, {-(23 * 60), 0}};
  rv = mozilla::DateTimeFormat::FormatPRExplodedTime(
      kDateFormatLong, kTimeFormatLong, &prExplodedTime, formattedTime);
  ASSERT_TRUE(NS_SUCCEEDED(rv));
  ASSERT_TRUE(formattedTime.Find("31.") != kNotFound);
  ASSERT_TRUE(formattedTime.Find("Dezember") != kNotFound);
  ASSERT_TRUE(formattedTime.Find("1969") != kNotFound);
  ASSERT_TRUE(formattedTime.Find("11:37:00 PM") != kNotFound ||
              formattedTime.Find("23:37:00") != kNotFound);

  prExplodedTime = {0, 0, 0, 17, 31, 11, 1969, 3, 364, {-(7 * 60 * 60), 0}};
  rv = mozilla::DateTimeFormat::FormatPRExplodedTime(
      kDateFormatLong, kTimeFormatLong, &prExplodedTime, formattedTime);
  ASSERT_TRUE(NS_SUCCEEDED(rv));
  ASSERT_TRUE(formattedTime.Find("31.") != kNotFound);
  ASSERT_TRUE(formattedTime.Find("Dezember") != kNotFound);
  ASSERT_TRUE(formattedTime.Find("1969") != kNotFound);
  ASSERT_TRUE(formattedTime.Find("5:00:00 PM") != kNotFound ||
              formattedTime.Find("17:00:00") != kNotFound);

  prExplodedTime = {
      0,  0,    47, 14,  31,
      11, 1969, 3,  364, {-((10 * 60 * 60) + (13 * 60)), (1 * 60 * 60)}};
  rv = mozilla::DateTimeFormat::FormatPRExplodedTime(
      kDateFormatLong, kTimeFormatLong, &prExplodedTime, formattedTime);
  ASSERT_TRUE(NS_SUCCEEDED(rv));
  ASSERT_TRUE(formattedTime.Find("31.") != kNotFound);
  ASSERT_TRUE(formattedTime.Find("Dezember") != kNotFound);
  ASSERT_TRUE(formattedTime.Find("1969") != kNotFound);
  ASSERT_TRUE(formattedTime.Find("2:47:00 PM") != kNotFound ||
              formattedTime.Find("14:47:00") != kNotFound);
}

TEST(DateTimeFormat, DateFormatSelectorsForeign)
{
  PRTime prTime = 0;
  PRExplodedTime prExplodedTime;
  PR_ExplodeTime(prTime, PR_GMTParameters, &prExplodedTime);

  mozilla::DateTimeFormat::mLocale = new nsCString("de-DE");
  mozilla::DateTimeFormat::DeleteCache();

  nsAutoString formattedTime;
  nsresult rv = mozilla::DateTimeFormat::FormatDateTime(
      &prExplodedTime, DateTimeFormat::Skeleton::yyyyMM, formattedTime);
  ASSERT_TRUE(NS_SUCCEEDED(rv));
  ASSERT_STREQ("01.1970", NS_ConvertUTF16toUTF8(formattedTime).get());

  rv = mozilla::DateTimeFormat::FormatDateTime(
      &prExplodedTime, DateTimeFormat::Skeleton::yyyyMMMM, formattedTime);
  ASSERT_TRUE(NS_SUCCEEDED(rv));
  ASSERT_STREQ("Januar 1970", NS_ConvertUTF16toUTF8(formattedTime).get());

  rv = mozilla::DateTimeFormat::FormatDateTime(
      &prExplodedTime, DateTimeFormat::Skeleton::E, formattedTime);
  ASSERT_TRUE(NS_SUCCEEDED(rv));
  ASSERT_STREQ("Do", NS_ConvertUTF16toUTF8(formattedTime).get());

  rv = mozilla::DateTimeFormat::FormatDateTime(
      &prExplodedTime, DateTimeFormat::Skeleton::EEEE, formattedTime);
  ASSERT_TRUE(NS_SUCCEEDED(rv));
  ASSERT_STREQ("Donnerstag", NS_ConvertUTF16toUTF8(formattedTime).get());

  rv = mozilla::DateTimeFormat::GetCalendarSymbol(
      mozilla::DateTimeFormat::Field::Month,
      mozilla::DateTimeFormat::Style::Wide, &prExplodedTime, formattedTime);
  ASSERT_TRUE(NS_SUCCEEDED(rv));
  ASSERT_STREQ("Januar", NS_ConvertUTF16toUTF8(formattedTime).get());

  rv = mozilla::DateTimeFormat::GetCalendarSymbol(
      mozilla::DateTimeFormat::Field::Weekday,
      mozilla::DateTimeFormat::Style::Abbreviated, &prExplodedTime,
      formattedTime);
  ASSERT_TRUE(NS_SUCCEEDED(rv));
  ASSERT_STREQ("Do", NS_ConvertUTF16toUTF8(formattedTime).get());
}

}  // namespace mozilla
