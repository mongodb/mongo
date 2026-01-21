// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "azure/core/datetime.hpp"

#include "azure/core/internal/strings.hpp"
#include "azure/core/platform.hpp"

#include <algorithm>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <type_traits>

using namespace Azure;

namespace {
DateTime GetSystemClockEpoch()
{
  auto const systemClockEpochTimeT
      = std::chrono::system_clock::to_time_t(std::chrono::system_clock::time_point());

#ifdef _MSC_VER
#pragma warning(push)
// warning C4996: 'gmtime': This function or variable may be unsafe. Consider using gmtime_s
// instead.
#pragma warning(disable : 4996)
#endif
  auto const systemClockEpochUtcStructTm = std::gmtime(&systemClockEpochTimeT);
#ifdef _MSC_VER
#pragma warning(pop)
#endif

  return DateTime(
      static_cast<int16_t>(1900 + systemClockEpochUtcStructTm->tm_year),
      static_cast<int8_t>(1 + systemClockEpochUtcStructTm->tm_mon),
      static_cast<int8_t>(systemClockEpochUtcStructTm->tm_mday),
      static_cast<int8_t>(systemClockEpochUtcStructTm->tm_hour),
      static_cast<int8_t>(systemClockEpochUtcStructTm->tm_min),
      static_cast<int8_t>(systemClockEpochUtcStructTm->tm_sec));
}

DateTime GetMaxDateTime()
{
  auto const systemClockMax = std::chrono::duration_cast<DateTime::clock::duration>(
                                  (std::chrono::system_clock::time_point::max)().time_since_epoch())
                                  .count();

  auto const systemClockEpoch = GetSystemClockEpoch().time_since_epoch().count();

  constexpr auto repMax = (std::numeric_limits<DateTime::clock::duration::rep>::max)();

  return DateTime(DateTime::time_point(DateTime::duration(
      systemClockMax + (std::min)(systemClockEpoch, (repMax - systemClockMax)))));
}

template <typename T>
void ValidateDateElementRange(
    T value,
    decltype(value) minValue,
    decltype(value) maxValue,
    std::string const& valueName)
{
  auto outOfRange = 0;

  if (value < minValue)
  {
    outOfRange = -1;
  }
  else if (value > maxValue)
  {
    outOfRange = +1;
  }

  if (outOfRange != 0)
  {
    throw std::invalid_argument(
        "Azure::DateTime " + valueName + " (" + std::to_string(value) + ") cannot be "
        + (outOfRange < 0 ? std::string("less than ") + std::to_string(minValue)
                          : std::string("greater than ") + std::to_string(maxValue))
        + ".");
  }
}

constexpr bool IsLeapYear(int16_t year)
{
  return (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}

constexpr int16_t LeapYearsSinceEpoch(int16_t year)
{
  int16_t const fourHundredYearPeriods = year / 400;
  int16_t const hundredYearPeriods = (year % 400) / 100;
  int16_t const remainder = year - (400 * fourHundredYearPeriods) - 100 * (hundredYearPeriods);

  // Every 4 years have 1 leap year.
  // Every 100 years have 24, not 25 leap years (years divisible by 100 are not leap years).
  // Every 400 years have 97, not 96 (would be 24 * 4 = 96) years.
  // That's because year divisible by 100 is not a leap year, unless it is also divisible by 400.
  return (fourHundredYearPeriods * 97) + (hundredYearPeriods * 24) + (remainder / 4);
}

constexpr int8_t const MaxDaysPerMonth[12] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

constexpr int16_t DayOfYear(int16_t year, int8_t month, int8_t day)
{
  int16_t daysInPrevMonths = 0;
  for (int i = 1; i < month; ++i)
  {
    daysInPrevMonths += MaxDaysPerMonth[i - 1];
  }

  if (month > 2 && !IsLeapYear(year))
  {
    daysInPrevMonths -= 1;
  }

  return daysInPrevMonths + day;
}

constexpr int32_t DaySinceEpoch(int16_t year, int8_t month, int8_t day)
{
  int16_t const priorYear = year - 1;
  auto const leapYears = LeapYearsSinceEpoch(priorYear);
  auto const nonLeapYears = priorYear - leapYears;
  return (nonLeapYears * 365) + (leapYears * 366) + DayOfYear(year, month, day);
}

constexpr int8_t GetDayOfWeek(int16_t year, int8_t month, int8_t day)
{
  return DaySinceEpoch(year, month, day) % 7;
}

constexpr int8_t WeekDayAndMonthDayOfYear(
    int8_t* month,
    int8_t* day,
    int16_t year,
    int16_t dayOfYear)
{
  auto remainder = dayOfYear;
  for (int8_t i = 1; i <= 12; ++i)
  {
    // MonthDays = number of days in this month.
    // If this month is February, then check for leap year and adjust accordingly.
    int8_t const MonthDays = MaxDaysPerMonth[i - 1] - ((i == 2 && !IsLeapYear(year)) ? 1 : 0);
    if (remainder <= MonthDays)
    {
      *month = i;
      *day = static_cast<int8_t>(remainder);
      break;
    }

    remainder -= MonthDays;
  }

  return GetDayOfWeek(year, *month, *day);
}

constexpr auto OneSecondIn100ns = 10000000LL;
constexpr auto OneMinuteIn100ns = 60 * OneSecondIn100ns;
constexpr auto OneHourIn100ns = 60 * OneMinuteIn100ns;
constexpr auto OneDayIn100ns = 24 * OneHourIn100ns;

void ValidateDate(
    int16_t year,
    int8_t month,
    int8_t day,
    int8_t hour,
    int8_t minute,
    int8_t second,
    int32_t fracSec,
    int8_t dayOfWeek,
    int8_t localDiffHours,
    int8_t localDiffMinutes,
    bool roundFracSecUp)
{
  ValidateDateElementRange(year, 1, 9999, "year");
  ValidateDateElementRange(month, 1, 12, "month");
  ValidateDateElementRange(day, 1, 31, "day");
  ValidateDateElementRange(hour, 0, 23, "hour");
  ValidateDateElementRange(minute, 0, 59, "minute");
  ValidateDateElementRange(second, 0, 60, "second");
  ValidateDateElementRange(fracSec, 0, 9999999, "fractional (10^-7) second");
  ValidateDateElementRange(localDiffHours, -99, +99, "local differential hours");
  ValidateDateElementRange(localDiffMinutes, -59, +59, "local differential minutes");

  auto const maxDay = MaxDaysPerMonth[month - 1];
  if (day > maxDay)
  {
    throw std::invalid_argument(
        "Azure::DateTime: invalid month day number (month: " + std::to_string(month)
        + ", day: " + std::to_string(day) + ", maximum: " + std::to_string(maxDay) + ").");
  }

  if (!IsLeapYear(year) && month == 2 && day == 29)
  {
    throw std::invalid_argument(
        "Azure::DateTime: year " + std::to_string(year)
        + "is not a leap year, and therefore does not have February 29th.");
  }

  if (dayOfWeek != -1)
  {
    ValidateDateElementRange(dayOfWeek, 0, 6, "day of week");
    auto const expectedDayOfWeek = GetDayOfWeek(year, month, day);
    if (dayOfWeek != expectedDayOfWeek)
    {
      throw std::invalid_argument(
          "Azure::DateTime: incorrect day of week specified (actual: " + std::to_string(dayOfWeek)
          + ", expected: " + std::to_string(expectedDayOfWeek) + ").");
    }
  }

  {
    auto const fracSecTimeAdjustment
        = (localDiffHours * OneHourIn100ns) + (localDiffMinutes * OneMinuteIn100ns);

    if (fracSecTimeAdjustment > 0)
    {
      if (year == 1 && month == 1)
      {
        auto const fracSecSince0001_01_01 = ((static_cast<int64_t>(day) - 1) * OneDayIn100ns)
            + (hour * OneHourIn100ns) + (minute * OneMinuteIn100ns) + (second * OneSecondIn100ns)
            + fracSec + (roundFracSecUp ? 1 : 0);

        if ((fracSecSince0001_01_01 - fracSecTimeAdjustment) < 0)
        {
          throw std::invalid_argument(
              "Time zone time adjustments result in a UTC date prior to 0001-01-01.");
        }
      }
    }
    else if (year == 9999 && month == 12)
    {
      auto const fracSecSince9999_12_01 = ((static_cast<int64_t>(day) - 1) * OneDayIn100ns)
          + (hour * OneHourIn100ns) + (minute * OneMinuteIn100ns) + (second * OneSecondIn100ns)
          + fracSec + (roundFracSecUp ? 1 : 0);

      constexpr auto FracSecTo9999_12_31_23_59_59_9999999 = (30 * OneDayIn100ns)
          + (23 * OneHourIn100ns) + (59 * OneMinuteIn100ns) + (59 * OneSecondIn100ns) + 9999999;

      if (fracSecSince9999_12_01 - fracSecTimeAdjustment > FracSecTo9999_12_31_23_59_59_9999999)
      {
        throw std::invalid_argument(
            std::string(
                fracSecTimeAdjustment < 0
                    ? "Time zone"
                    : (second == 60 ? "Leap second" : "Second fraction round up"))
            + " time adjustments result in a UTC date after 9999-12-31T23:59:59.9999999.");
      }
    }
  }
}

static std::string const DayNames[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static std::string const MonthNames[12]
    = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

constexpr int32_t DivideAndUpdateRemainder(int64_t* remainder, int64_t value)
{
  auto retval = static_cast<int32_t>(*remainder / value);
  *remainder %= value;
  return retval;
}

bool SubstringEquals(
    std::string const& str,
    std::string::size_type startAt,
    std::string::size_type len,
    std::string const& compareTo)
{
  for (decltype(len) i = 0; i < len; ++i)
  {
    if (str[startAt + i] != compareTo[i])
    {
      return false;
    }
  }

  return true;
}

template <int8_t N>
int8_t SubstringEqualsAny(
    std::string const& str,
    std::string::size_type startAt,
    std::string::size_type len,
    std::string const (&compareTo)[N])
{
  for (int8_t i = 0; i < static_cast<int8_t>(N); ++i)
  {
    if (SubstringEquals(str, startAt, len, compareTo[i]))
    {
      return i;
    }
  }

  return -1;
}

[[noreturn]] void ThrowParseError(char const* description)
{
  throw std::invalid_argument(std::string("Error parsing DateTime: ") + description + ".");
}

template <typename T>
T ParseNumber(
    std::string::size_type* cursor,
    std::string const& str,
    std::string::size_type strLen,
    char const* description,
    int minLength,
    int maxLength)
{
  if (*cursor + minLength <= strLen)
  {
    auto const MaxChars = (std::min)(static_cast<int>(strLen - *cursor), maxLength);
    int64_t value = 0;
    auto i = 0;
    for (; i < MaxChars; ++i)
    {
      auto const ch = str[*cursor + i];
      if (Core::_internal::StringExtensions::IsDigit(ch))
      {
        value = (value * 10) + (static_cast<decltype(value)>(static_cast<unsigned char>(ch)) - '0');
        continue;
      }

      if (i < minLength)
      {
        value = -1;
      }

      break;
    }

    if (value >= 0 && value <= (std::numeric_limits<T>::max)())
    {
      *cursor += i;
      return static_cast<T>(value);
    }
  }

  ThrowParseError(description);
}

template <typename T>
void IncreaseAndCheckMinLength(
    std::string::size_type* minLength,
    std::string::size_type actualLength,
    T increment)
{
  *minLength += static_cast<std::string::size_type>(increment);
  if (actualLength < *minLength)
  {
    ThrowParseError("too short");
  }
}

void ParseSingleChar(
    std::string::size_type* cursor,
    std::string const& str,
    char const* parsingArea,
    char ch)
{
  if (str[*cursor] != ch)
  {
    ThrowParseError(parsingArea);
  }
  ++*cursor;
}

void ParseSingleOptionalChar(
    std::string::size_type* cursor,
    std::string const& str,
    char ch,
    std::string::size_type* minLength,
    std::string::size_type actualLength)
{
  if (str[*cursor] == ch)
  {
    ++*cursor;
    IncreaseAndCheckMinLength(minLength, actualLength, 1);
  }
}
} // namespace

DateTime const DateTime::SystemClockEpoch = GetSystemClockEpoch();

DateTime::DateTime(
    int16_t year,
    int8_t month,
    int8_t day,
    int8_t hour,
    int8_t minute,
    int8_t second,
    int32_t fracSec,
    int8_t dayOfWeek,
    int8_t localDiffHours,
    int8_t localDiffMinutes,
    bool roundFracSecUp)
    : time_point(duration(
        (OneDayIn100ns * (static_cast<int64_t>(DaySinceEpoch(year, month, day)) - 1))
        + (OneHourIn100ns * (static_cast<int64_t>(hour) - localDiffHours))
        + (OneMinuteIn100ns * (static_cast<int64_t>(minute) - localDiffMinutes))
        + (OneSecondIn100ns * second) + (static_cast<int64_t>(fracSec) + (roundFracSecUp ? 1 : 0))))
{
  ValidateDate(
      year,
      month,
      day,
      hour,
      minute,
      second,
      fracSec,
      dayOfWeek,
      localDiffHours,
      localDiffMinutes,
      roundFracSecUp);
}

DateTime::operator std::chrono::system_clock::time_point() const
{
  static DateTime SystemClockMin((std::chrono::system_clock::time_point::min)());
  static DateTime SystemClockMax(GetMaxDateTime());

  auto outOfRange = 0;
  if (*this < SystemClockMin)
  {
    outOfRange = -1;
  }
  else if (*this > SystemClockMax)
  {
    outOfRange = +1;
  }

  if (outOfRange != 0)
  {
    throw std::invalid_argument(
        std::string("Cannot represent Azure::DateTime as "
                    "std::chrono::system_clock::time_point: value is too ")
        + (outOfRange < 0 ? "small." : "big."));
  }

  return std::chrono::system_clock::time_point()
      + std::chrono::duration_cast<std::chrono::system_clock::duration>(*this - SystemClockEpoch);
}

DateTime DateTime::Parse(std::string const& dateTime, DateFormat format)
{
  // The values that are not supposed to be read before they are written are set to -123... to avoid
  // warnings on some compilers, yet provide a clearly bad value to make it obvious if things don't
  // work as expected.
  int16_t year = -12345;
  int8_t month = -123;
  int8_t day = -123;

  int8_t hour = 0;
  int8_t minute = 0;
  int8_t second = 0;
  int32_t fracSec = 0;
  int8_t dayOfWeek = -1;
  int8_t localDiffHours = 0;
  int8_t localDiffMinutes = 0;
  bool roundFracSecUp = false;
  {
    std::string::size_type const DateTimeLength = dateTime.length();
    std::string::size_type minDateTimeLength = 0;
    std::string::size_type cursor = 0;
    if (format == DateFormat::Rfc1123)
    {
      // Shortest possible string: "1 Jan 0001 00:00 UT"
      // Longest possible string: "Fri, 31 Dec 9999 23:59:60 +9959"
      IncreaseAndCheckMinLength(&minDateTimeLength, DateTimeLength, 19);

      if (dateTime[cursor + 3] == ',')
      {
        dayOfWeek = SubstringEqualsAny(dateTime, cursor, 3, DayNames);
        if (dayOfWeek == -1 || dateTime[cursor + 4] != ' ')
        {
          ThrowParseError("day of week");
        }

        cursor += 5;
        IncreaseAndCheckMinLength(&minDateTimeLength, DateTimeLength, 5);
      }

      {
        auto const parsingArea = "day";

        auto const oldCursor = cursor;
        day = ParseNumber<decltype(day)>(&cursor, dateTime, DateTimeLength, parsingArea, 1, 2);

        IncreaseAndCheckMinLength(&minDateTimeLength, DateTimeLength, (cursor - oldCursor) - 1);
        ParseSingleChar(&cursor, dateTime, parsingArea, ' ');
      }

      {
        month = 1 + SubstringEqualsAny(dateTime, cursor, 3, MonthNames);

        if (month == 0 || dateTime[cursor + 3] != ' ')
        {
          ThrowParseError("month");
        }

        cursor += 4;
      }

      {
        auto const parsingArea = "year";
        year = ParseNumber<decltype(year)>(&cursor, dateTime, DateTimeLength, parsingArea, 4, 4);
        ParseSingleChar(&cursor, dateTime, parsingArea, ' ');
      }

      {
        auto parsingArea = "hour and minute";

        hour = ParseNumber<decltype(hour)>(&cursor, dateTime, DateTimeLength, "hour", 2, 2);
        ParseSingleChar(&cursor, dateTime, parsingArea, ':');
        minute = ParseNumber<decltype(minute)>(&cursor, dateTime, DateTimeLength, "minute", 2, 2);

        if (dateTime[cursor] == ':')
        {
          ++cursor;
          parsingArea = "second";
          second
              = ParseNumber<decltype(second)>(&cursor, dateTime, DateTimeLength, parsingArea, 2, 2);

          IncreaseAndCheckMinLength(&minDateTimeLength, DateTimeLength, 3);
        }
        ParseSingleChar(&cursor, dateTime, parsingArea, ' ');
      }

      if (!SubstringEquals(dateTime, cursor, 2, "UT"))
      {
        IncreaseAndCheckMinLength(&minDateTimeLength, DateTimeLength, 1);
        if (!SubstringEquals(dateTime, cursor, 3, "GMT"))
        {
          static std::string const TimeZones[]
              = {"EST", "EDT", "CST", "CDT", "MST", "MDT", "PST", "PDT"};
          static int8_t const HourAdjustments[] = {-5, -4, -6, -5, -7, -6, -8, -7};

          auto tz = SubstringEqualsAny(dateTime, cursor, 3, TimeZones);
          if (tz >= 0)
          {
            localDiffHours = HourAdjustments[tz];
          }
          else
          {
            IncreaseAndCheckMinLength(&minDateTimeLength, DateTimeLength, 2);
            auto const parsingArea = "time zone";
            int8_t sign = 0;
            if (dateTime[cursor] == '+')
            {
              sign = +1;
            }
            else if (dateTime[cursor] == '-')
            {
              sign = -1;
            }
            else
            {
              ThrowParseError(parsingArea);
            }

            ++cursor;

            localDiffHours = sign
                * ParseNumber<decltype(localDiffHours)>(
                                 &cursor, dateTime, DateTimeLength, parsingArea, 2, 2);

            localDiffMinutes = sign
                * ParseNumber<decltype(localDiffMinutes)>(
                                   &cursor, dateTime, DateTimeLength, parsingArea, 2, 2);
          }
        }
      }
    }
    else if (format == DateFormat::Rfc3339)
    {
      // Shortest possible string: "00010101"
      // "Longest" possible string: "9999-12-31T23:59:60.1234567*+99:59"
      // * - any fractional second digits after the 7th are ignored.
      IncreaseAndCheckMinLength(&minDateTimeLength, DateTimeLength, 8);

      {
        auto const parsingArea = "year";
        year = ParseNumber<decltype(year)>(&cursor, dateTime, DateTimeLength, parsingArea, 4, 4);
        ParseSingleOptionalChar(&cursor, dateTime, '-', &minDateTimeLength, DateTimeLength);
      }

      {
        auto const parsingArea = "month";
        month = ParseNumber<decltype(month)>(&cursor, dateTime, DateTimeLength, parsingArea, 2, 2);
        ParseSingleOptionalChar(&cursor, dateTime, '-', &minDateTimeLength, DateTimeLength);
      }

      {
        auto const parsingArea = "day";
        day = ParseNumber<decltype(day)>(&cursor, dateTime, DateTimeLength, parsingArea, 2, 2);
      }

      if (cursor < DateTimeLength
          && (dateTime[cursor] == 'T' || dateTime[cursor] == 't' || dateTime[cursor] == ' '))
      {
        ++cursor;
        IncreaseAndCheckMinLength(&minDateTimeLength, DateTimeLength, 7);

        {
          auto const parsingArea = "hour";
          hour = ParseNumber<decltype(hour)>(&cursor, dateTime, DateTimeLength, parsingArea, 2, 2);
          ParseSingleOptionalChar(&cursor, dateTime, ':', &minDateTimeLength, DateTimeLength);
        }

        {
          auto const parsingArea = "minute";
          minute
              = ParseNumber<decltype(minute)>(&cursor, dateTime, DateTimeLength, parsingArea, 2, 2);
          ParseSingleOptionalChar(&cursor, dateTime, ':', &minDateTimeLength, DateTimeLength);
        }

        {
          auto const parsingArea = "second";
          second
              = ParseNumber<decltype(second)>(&cursor, dateTime, DateTimeLength, parsingArea, 2, 2);
        }

        if (cursor + 1 < DateTimeLength)
        {
          if (dateTime[cursor] == '.')
          {
            ++minDateTimeLength;
            ++cursor;
          }

          {
            auto oldCursor = cursor;
            fracSec = ParseNumber<decltype(fracSec)>(
                &cursor, dateTime, DateTimeLength, "second fraction", 0, 7);

            auto const charsRead = (cursor - oldCursor);
            {
              auto const zerosToAdd = static_cast<int>(7 - charsRead);
              for (auto i = 0; i < zerosToAdd; ++i)
              {
                fracSec *= 10;
              }
            }

            minDateTimeLength += charsRead;
            if (charsRead == 7 && (DateTimeLength - cursor) > 0)
            {
              auto const ch = dateTime[cursor];
              if (Core::_internal::StringExtensions::IsDigit(ch))
              {
                auto const num = static_cast<int>(static_cast<unsigned char>(ch) - '0');
                if (num > 4)
                {
                  if (fracSec < 9999999)
                  {
                    ++fracSec;
                  }
                  else
                  {
                    roundFracSecUp = true;
                  }
                }

                ++cursor;
              }
            }
          }

          for (auto i = DateTimeLength - cursor; i > 0; --i)
          {
            if (Core::_internal::StringExtensions::IsDigit(dateTime[cursor]))
            {
              ++minDateTimeLength;
              ++cursor;
            }
            else
            {
              break;
            }
          }

          if (DateTimeLength - cursor > 0)
          {
            auto const parsingArea = "time zone";
            int8_t sign = 0;
            if (dateTime[cursor] == '+')
            {
              sign = +1;
            }
            else if (dateTime[cursor] == '-')
            {
              sign = -1;
            }

            if (sign != 0)
            {
              ++cursor;
              IncreaseAndCheckMinLength(&minDateTimeLength, DateTimeLength, 6);

              localDiffHours = sign
                  * ParseNumber<decltype(localDiffHours)>(
                                   &cursor, dateTime, DateTimeLength, parsingArea, 2, 2);

              if (dateTime[cursor] == ':')
              {
                ++cursor;
              }
              else
              {
                ThrowParseError(parsingArea);
              }

              localDiffMinutes = sign
                  * ParseNumber<decltype(localDiffMinutes)>(
                                     &cursor, dateTime, DateTimeLength, parsingArea, 2, 2);
            }
          }
        }
      }
    }
    else
    {
      throw std::invalid_argument("Unrecognized date format.");
    }

    return DateTime(
        year,
        month,
        day,
        hour,
        minute,
        second,
        fracSec,
        dayOfWeek,
        localDiffHours,
        localDiffMinutes,
        roundFracSecUp);
  }
}

void DateTime::ThrowIfUnsupportedYear() const
{
  static DateTime const Year0001 = DateTime();
  static DateTime const Eoy9999 = DateTime(9999, 12, 31, 23, 59, 59, 9999999, -1, 0, 0);

  auto outOfRange = 0;
  if (*this < Year0001)
  {
    outOfRange = -1;
  }
  else if (*this > Eoy9999)
  {
    outOfRange = +1;
  }

  if (outOfRange != 0)
  {
    throw std::invalid_argument(
        std::string("Cannot represent Azure::DateTime as std::string: the date is ")
        + (outOfRange < 0 ? "before 0001-01-01." : "after 9999-12-31 23:59:59.9999999."));
  }
}

void DateTime::GetDateTimeParts(
    int16_t* year,
    int8_t* month,
    int8_t* day,
    int8_t* hour,
    int8_t* minute,
    int8_t* second,
    int32_t* fracSec,
    int8_t* dayOfWeek) const
{
  {
    auto remainder = time_since_epoch().count();

    constexpr auto OneYearIn100ns = 365 * OneDayIn100ns;
    constexpr auto FourYearsIn100ns = (4 * OneYearIn100ns) + OneDayIn100ns;
    constexpr auto OneHundredYearsIn100ns = (25 * FourYearsIn100ns) - OneDayIn100ns;
    constexpr auto FourHundredYearsIn100ns = (4 * OneHundredYearsIn100ns) + OneDayIn100ns;

    auto const years400 = DivideAndUpdateRemainder(&remainder, FourHundredYearsIn100ns);
    auto const years100 = DivideAndUpdateRemainder(&remainder, OneHundredYearsIn100ns);
    auto const years4 = DivideAndUpdateRemainder(&remainder, FourYearsIn100ns);

    int32_t years1;
    if (remainder > 3 * OneYearIn100ns)
    {
      // This is a leap year
      years1 = 3;
      remainder -= 3 * OneYearIn100ns;
    }
    else
    {
      years1 = DivideAndUpdateRemainder(&remainder, OneYearIn100ns);
    }

    *year += static_cast<int16_t>((years400 * 400) + (years100 * 100) + (years4 * 4) + years1);

    *dayOfWeek = WeekDayAndMonthDayOfYear(
        month,
        day,
        *year,
        static_cast<int16_t>(DivideAndUpdateRemainder(&remainder, OneDayIn100ns) + 1));

    *hour = static_cast<int8_t>(DivideAndUpdateRemainder(&remainder, OneHourIn100ns));
    *minute = static_cast<int8_t>(DivideAndUpdateRemainder(&remainder, OneMinuteIn100ns));
    *second = static_cast<int8_t>(DivideAndUpdateRemainder(&remainder, OneSecondIn100ns));
    *fracSec = static_cast<int32_t>(remainder);
  }
}

std::string DateTime::ToStringRfc1123() const
{
  ThrowIfUnsupportedYear();

  int16_t year = 1;

  // The values that are not supposed to be read before they are written are set to -123... to
  // avoid warnings on some compilers, yet provide a clearly bad value to make it obvious if
  // things don't work as expected.
  int8_t month = -123;
  int8_t day = -123;
  int8_t hour = -123;
  int8_t minute = -123;
  int8_t second = -123;
  int32_t fracSec = -1234567890;
  int8_t dayOfWeek = -123;

  GetDateTimeParts(&year, &month, &day, &hour, &minute, &second, &fracSec, &dayOfWeek);

  std::ostringstream dateString;

  dateString << DayNames[dayOfWeek] << ", " << std::setfill('0') << std::setw(2)
             << static_cast<int>(day) << ' ' << MonthNames[month - 1] << ' ' << std::setw(4)
             << static_cast<int>(year) << ' ' << std::setw(2) << static_cast<int>(hour) << ':'
             << std::setw(2) << static_cast<int>(minute) << ':' << std::setw(2)
             << static_cast<int>(second) << " GMT";

  return dateString.str();
}

std::string DateTime::ToString(DateFormat format) const
{
  if (format == DateFormat::Rfc1123)
  {
    return DateTime::ToStringRfc1123();
  }
  return ToString(format, TimeFractionFormat::DropTrailingZeros);
}

std::string DateTime::ToString(DateFormat format, TimeFractionFormat fractionFormat) const
{
  if (format != DateFormat::Rfc3339)
  {
    throw std::invalid_argument(
        "Unrecognized date format (" + std::to_string(static_cast<int64_t>(format)) + ").");
  }

  switch (fractionFormat)
  {
    case TimeFractionFormat::DropTrailingZeros:
    case TimeFractionFormat::AllDigits:
    case TimeFractionFormat::Truncate:
      break;
    default:
      throw std::invalid_argument(
          "Unrecognized time fraction format ("
          + std::to_string(
              static_cast<std::underlying_type<TimeFractionFormat>::type>(fractionFormat))
          + ").");
  }

  ThrowIfUnsupportedYear();

  int16_t year = 1;

  // The values that are not supposed to be read before they are written are set to -123... to avoid
  // warnings on some compilers, yet provide a clearly bad value to make it obvious if things don't
  // work as expected.
  int8_t month = -123;
  int8_t day = -123;
  int8_t hour = -123;
  int8_t minute = -123;
  int8_t second = -123;
  int32_t fracSec = -1234567890;
  int8_t dayOfWeek = -123;

  GetDateTimeParts(&year, &month, &day, &hour, &minute, &second, &fracSec, &dayOfWeek);

  std::ostringstream dateString;

  dateString << std::setfill('0') << std::setw(4) << static_cast<int>(year) << '-' << std::setw(2)
             << static_cast<int>(month) << '-' << std::setw(2) << static_cast<int>(day) << 'T'
             << std::setw(2) << static_cast<int>(hour) << ':' << std::setw(2)
             << static_cast<int>(minute) << ':' << std::setw(2) << static_cast<int>(second);

  if (fractionFormat == TimeFractionFormat::AllDigits)
  {
    dateString << '.' << std::setw(7) << static_cast<int>(fracSec);
  }
  else if (fracSec != 0 && fractionFormat != TimeFractionFormat::Truncate)
  {
    // Append fractional second, which is a 7-digit value with no trailing zeros
    // This way, '0001200' becomes '00012'
    auto setw = 1;
    auto frac = fracSec;
    for (auto div = 1000000; div >= 1; div /= 10)
    {
      if ((fracSec % div) == 0)
      {
        frac /= div;
        break;
      }
      ++setw;
    }

    dateString << '.' << std::setw(setw) << static_cast<int>(frac);
  }

  dateString << 'Z';

  return dateString.str();
}
