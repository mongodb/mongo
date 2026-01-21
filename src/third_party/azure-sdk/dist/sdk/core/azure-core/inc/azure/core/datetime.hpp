// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

/**
 * @file
 * @brief Support for date and time standardized string formats.
 */

#pragma once

#include "azure/core/dll_import_export.hpp"

#include <chrono>
#include <ostream>
#include <string>

namespace Azure {

namespace _detail {
  class Clock final {
  public:
    using rep = int64_t;
    using period = std::ratio<1, 10000000>;
    using duration = std::chrono::duration<rep, period>;
    using time_point = std::chrono::time_point<Clock>;

    // since now() calls system_clock::now(), we have the same to say about the clock steadiness.
    // system_clock is not a steady clock. It is calendar-based, which means it can be adjusted,
    // and it may go backwards in time after adjustments, or jump forward faster than the actual
    // time passes, if you catch the moment before and after syncing the clock.
    // Steady clock would be good for measuring elapsed time without reboots (or hibernation?).
    // Steady clock's epoch = boot time, and it would only go forward in steady fashion, after the
    // system has started.
    // Using this clock in combination with system_clock is common scenario.
    // It would not be possible to base this clock on steady_clock and provide an implementation
    // that universally works in any context in predictable manner. However, it does not mean that
    // implementation can't use steady_clock in conjunction with this clock: an author can get a
    // duration between two time_points of this clock (or between system_clock::time point at
    // this clock's time_point), and add that duration to steady clock's time_point to get a new
    // time_point in the steady clock's "coordinate system".
    static constexpr bool is_steady = std::chrono::system_clock::is_steady;
    static time_point now();
  };
} // namespace _detail

/**
 * @brief Manages date and time in standardized string formats.
 * @details Supports date range from year 0001 to end of year 9999 with 100ns (7 decimal places
 * for fractional second) precision.
 * @remark `std::chrono::system_clock::time_point` can't be used, because there is no guarantees
 * for the date range and precision.
 * @remark This class is supposed to be able to handle a DateTime that comes over the wire.
 */
class DateTime final : public _detail::Clock::time_point {

private:
  AZ_CORE_DLLEXPORT static DateTime const SystemClockEpoch;

  DateTime(
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
      bool roundFracSecUp = false);

  void ThrowIfUnsupportedYear() const;

  void GetDateTimeParts(
      int16_t* year,
      int8_t* month,
      int8_t* day,
      int8_t* hour,
      int8_t* minute,
      int8_t* second,
      int32_t* fracSec,
      int8_t* dayOfWeek) const;

  std::string ToStringRfc1123() const;

public:
  /**
   * @brief Constructs a default instance of `%DateTime` (`00:00:00.0000000 on January 1st, 0001`).
   *
   */
  constexpr DateTime() : time_point() {}

  /**
   * @brief Constructs an instance of `%DateTime`.
   *
   * @param year Year.
   * @param month Month.
   * @param day Day.
   * @param hour Hour.
   * @param minute Minute.
   * @param second Seconds.
   *
   * @throw std::invalid_argument If any parameter is invalid.
   */
  explicit DateTime(
      int16_t year,
      int8_t month = 1,
      int8_t day = 1,
      int8_t hour = 0,
      int8_t minute = 0,
      int8_t second = 0)
      : DateTime(year, month, day, hour, minute, second, 0, -1, 0, 0)
  {
  }

  /**
   * @brief Constructs an instance of `%DateTime` from a `time_point`.
   *
   */
  constexpr DateTime(time_point const& timePoint) : time_point(timePoint) {}

  /**
   * @brief Construct an instance of `%DateTime` from `std::chrono::system_clock::time_point`.
   * @param systemTime A value of `std::chrono::system_clock::time_point`.
   *
   */
  DateTime(std::chrono::system_clock::time_point const& systemTime)
      : DateTime(
          SystemClockEpoch + std::chrono::duration_cast<duration>(systemTime.time_since_epoch()))
  {
  }

  /**
   * @brief Convert an instance of #Azure::DateTime to
   * `std::chrono::system_clock::time_point`.
   * @throw std::invalid_argument if #Azure::DateTime is outside of the range that can be
   * represented.
   */
  explicit operator std::chrono::system_clock::time_point() const;

  /**
   * @brief Defines the format applied to the fraction part of any #Azure::DateTime.
   *
   */
  enum class TimeFractionFormat
  {
    /// Include only meaningful fractional time digits, up to and excluding trailing zeroes.
    DropTrailingZeros,

    /// Include all the fractional time digits up to maximum precision, even if the entire value
    /// is zero.
    AllDigits,

    /// Drop all the fractional time digits.
    Truncate
  };

  /**
   * @brief Defines the supported date and time string formats.
   *
   */
  enum class DateFormat
  {
    /// RFC 1123.
    Rfc1123,

    /// RFC 3339.
    Rfc3339,
  };

  /**
   * @brief Create #Azure::DateTime from a string representing time in UTC in the specified
   * format.
   *
   * @param dateTime A string with the date and time.
   * @param format A format to which \p dateTime string adheres to.
   *
   * @return #Azure::DateTime that was constructed from the \p dateTime string.
   *
   * @throw std::invalid_argument If \p format is not recognized, or if parsing error.
   */
  static DateTime Parse(std::string const& dateTime, DateFormat format);

  /**
   * @brief Get a string representation of the #Azure::DateTime.
   *
   * @param format The representation format to use, defaulted to use RFC 3339.
   *
   * @throw std::invalid_argument If year exceeds 9999, or if \p format is not recognized.
   */
  std::string ToString(DateFormat format = DateFormat::Rfc3339) const;

  /**
   * @brief Get a string representation of the #Azure::DateTime.
   *
   * @param format The representation format to use.
   * @param fractionFormat The format for the fraction part of the DateTime. Only
   * supported by RFC3339.
   *
   * @throw std::invalid_argument If year exceeds 9999, or if \p format is not recognized.
   */
  std::string ToString(DateFormat format, TimeFractionFormat fractionFormat) const;
};

/** @brief Return the current time. */
inline _detail::Clock::time_point _detail::Clock::now()
{
  return DateTime(std::chrono::system_clock::now());
}

/** @brief Compare a DateTime object with a std::chrono::system_clock::time_point object. */
inline bool operator==(DateTime const& dt, std::chrono::system_clock::time_point const& tp)
{
  return dt == DateTime(tp);
}

/** @brief Compare a DateTime object with a std::chrono::system_clock::time_point object. */
inline bool operator<(DateTime const& dt, std::chrono::system_clock::time_point const& tp)
{
  return dt < DateTime(tp);
}

/** @brief Compare a DateTime object with a std::chrono::system_clock::time_point object. */
inline bool operator<=(DateTime const& dt, std::chrono::system_clock::time_point const& tp)
{
  return dt <= DateTime(tp);
}

/** @brief Compare a DateTime object with a std::chrono::system_clock::time_point object. */
inline bool operator!=(DateTime const& dt, std::chrono::system_clock::time_point const& tp)
{
  return !(dt == tp);
}

/** @brief Compare a DateTime object with a std::chrono::system_clock::time_point object. */
inline bool operator>(DateTime const& dt, std::chrono::system_clock::time_point const& tp)
{
  return !(dt <= tp);
}

/** @brief Compare a DateTime object with a std::chrono::system_clock::time_point object. */
inline bool operator>=(DateTime const& dt, std::chrono::system_clock::time_point const& tp)
{
  return !(dt < tp);
}

/** @brief Compare a std::chrono::system_clock::time_point object with an Azure::DateTime object */
inline bool operator==(std::chrono::system_clock::time_point const& tp, DateTime const& dt)
{
  return dt == tp;
}

/** @brief Compare a std::chrono::system_clock::time_point object with an Azure::DateTime object */
inline bool operator!=(std::chrono::system_clock::time_point const& tp, DateTime const& dt)
{
  return dt != tp;
}

/** @brief Compare a std::chrono::system_clock::time_point object with an Azure::DateTime object */
inline bool operator<(std::chrono::system_clock::time_point const& tp, DateTime const& dt)
{
  return (dt > tp);
}

/** @brief Compare a std::chrono::system_clock::time_point object with an Azure::DateTime object */
inline bool operator<=(std::chrono::system_clock::time_point const& tp, DateTime const& dt)
{
  return (dt >= tp);
}

/** @brief Compare a std::chrono::system_clock::time_point object with an Azure::DateTime object */
inline bool operator>(std::chrono::system_clock::time_point const& tp, DateTime const& dt)
{
  return (dt < tp);
}

/** @brief Compare a std::chrono::system_clock::time_point object with an Azure::DateTime object */
inline bool operator>=(std::chrono::system_clock::time_point const& tp, DateTime const& dt)
{
  return (dt <= tp);
}

namespace Core { namespace _internal {
  /**
   * @brief Provides convertion methods for POSIX time to an #Azure::DateTime.
   *
   */
  class PosixTimeConverter final {
  public:
    /**
     * @brief Converts POSIX time to an #Azure::DateTime.
     *
     * @param posixTime The number of seconds since 1970.
     * @return Calculated #Azure::DateTime.
     */
    static DateTime PosixTimeToDateTime(int64_t posixTime)
    {
      return {DateTime(1970) + std::chrono::seconds(posixTime)};
    }

    /**
     * @brief Converts a DateTime to POSIX time.
     *
     * @param dateTime The `%DateTime` to convert.
     * @return The number of seconds since 1970.
     */
    static int64_t DateTimeToPosixTime(DateTime const& dateTime)
    {
      //  This count starts at the POSIX epoch which is January 1st, 1970 UTC.
      return std::chrono::duration_cast<std::chrono::seconds>(dateTime - DateTime(1970)).count();
    }

  private:
    /**
     * @brief An instance of `%PosixTimeConverter` class cannot be created.
     *
     */
    PosixTimeConverter() = delete;

    /**
     * @brief An instance of `%PosixTimeConverter` class cannot be destructed, because no instance
     * can be created.
     *
     */
    ~PosixTimeConverter() = delete;
  };
}} // namespace Core::_internal

} // namespace Azure
