#pragma once
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/crt/Exports.h>

#include <aws/crt/Types.h>

#include <aws/common/date_time.h>

#include <chrono>

namespace Aws
{
    namespace Crt
    {
        enum class DateFormat
        {
            RFC822 = AWS_DATE_FORMAT_RFC822,
            ISO_8601 = AWS_DATE_FORMAT_ISO_8601,
            AutoDetect = AWS_DATE_FORMAT_AUTO_DETECT,
        };

        enum class Month
        {
            January = AWS_DATE_MONTH_JANUARY,
            February = AWS_DATE_MONTH_FEBRUARY,
            March = AWS_DATE_MONTH_MARCH,
            April = AWS_DATE_MONTH_APRIL,
            May = AWS_DATE_MONTH_MAY,
            June = AWS_DATE_MONTH_JUNE,
            July = AWS_DATE_MONTH_JULY,
            August = AWS_DATE_MONTH_AUGUST,
            September = AWS_DATE_MONTH_SEPTEMBER,
            October = AWS_DATE_MONTH_OCTOBER,
            November = AWS_DATE_MONTH_NOVEMBER,
            December = AWS_DATE_MONTH_DECEMBER,
        };

        enum class DayOfWeek
        {
            Sunday = AWS_DATE_DAY_OF_WEEK_SUNDAY,
            Monday = AWS_DATE_DAY_OF_WEEK_MONDAY,
            Tuesday = AWS_DATE_DAY_OF_WEEK_TUESDAY,
            Wednesday = AWS_DATE_DAY_OF_WEEK_WEDNESDAY,
            Thursday = AWS_DATE_DAY_OF_WEEK_THURSDAY,
            Friday = AWS_DATE_DAY_OF_WEEK_FRIDAY,
            Saturday = AWS_DATE_DAY_OF_WEEK_SATURDAY,
        };

        class AWS_CRT_CPP_API DateTime final
        {
          public:
            /**
             *  Initializes time point to epoch
             */
            DateTime() noexcept;

            /**
             *  Initializes time point to any other arbitrary timepoint
             */
            DateTime(const std::chrono::system_clock::time_point &timepointToAssign) noexcept;

            /**
             * Initializes time point to millis Since epoch
             */
            DateTime(uint64_t millisSinceEpoch) noexcept;

            /**
             * Initializes time point to epoch time in seconds.millis
             */
            DateTime(double epoch_millis) noexcept;

            /**
             * Initializes time point to value represented by timestamp and format.
             */
            DateTime(const char *timestamp, DateFormat format) noexcept;

            bool operator==(const DateTime &other) const noexcept;
            bool operator<(const DateTime &other) const noexcept;
            bool operator>(const DateTime &other) const noexcept;
            bool operator!=(const DateTime &other) const noexcept;
            bool operator<=(const DateTime &other) const noexcept;
            bool operator>=(const DateTime &other) const noexcept;

            DateTime operator+(const std::chrono::milliseconds &a) const noexcept;
            DateTime operator-(const std::chrono::milliseconds &a) const noexcept;

            /**
             * Assign from seconds.millis since epoch.
             */
            DateTime &operator=(double secondsSinceEpoch) noexcept;

            /**
             * Assign from millis since epoch.
             */
            DateTime &operator=(uint64_t millisSinceEpoch) noexcept;

            /**
             * Assign from another time_point
             */
            DateTime &operator=(const std::chrono::system_clock::time_point &timepointToAssign) noexcept;

            /**
             * Assign from an ISO8601 or RFC822 formatted string
             */
            DateTime &operator=(const char *timestamp) noexcept;

            explicit operator bool() const noexcept;
            int GetLastError() const noexcept;

            /**
             * Convert dateTime to local time string using predefined format.
             */
            bool ToLocalTimeString(DateFormat format, ByteBuf &outputBuf) const noexcept;

            /**
             * Convert dateTime to GMT time string using predefined format.
             */
            bool ToGmtString(DateFormat format, ByteBuf &outputBuf) const noexcept;

            /**
             * Get the representation of this datetime as seconds.milliseconds since epoch
             */
            double SecondsWithMSPrecision() const noexcept;

            /**
             * Milliseconds since epoch of this datetime.
             */
            uint64_t Millis() const noexcept;

            /**
             *  In the likely case this class doesn't do everything you need to do, here's a copy of the time_point
             * structure. Have fun.
             */
            std::chrono::system_clock::time_point UnderlyingTimestamp() const noexcept;

            /**
             * Get the Year portion of this dateTime. localTime if true, return local time, otherwise return UTC
             */
            uint16_t GetYear(bool localTime = false) const noexcept;

            /**
             * Get the Month portion of this dateTime. localTime if true, return local time, otherwise return UTC
             */
            Month GetMonth(bool localTime = false) const noexcept;

            /**
             * Get the Day of the Month portion of this dateTime. localTime if true, return local time, otherwise return
             * UTC
             */
            uint8_t GetDay(bool localTime = false) const noexcept;

            /**
             * Get the Day of the Week portion of this dateTime. localTime if true, return local time, otherwise return
             * UTC
             */
            DayOfWeek GetDayOfWeek(bool localTime = false) const noexcept;

            /**
             * Get the Hour portion of this dateTime. localTime if true, return local time, otherwise return UTC
             */
            uint8_t GetHour(bool localTime = false) const noexcept;

            /**
             * Get the Minute portion of this dateTime. localTime if true, return local time, otherwise return UTC
             */
            uint8_t GetMinute(bool localTime = false) const noexcept;

            /**
             * Get the Second portion of this dateTime. localTime if true, return local time, otherwise return UTC
             */
            uint8_t GetSecond(bool localTime = false) const noexcept;

            /**
             * Get whether or not this dateTime is in Daylight savings time. localTime if true, return local time,
             * otherwise return UTC
             */
            bool IsDST(bool localTime = false) const noexcept;

            /**
             * Get an instance of DateTime representing this very instant.
             */
            static DateTime Now() noexcept;

            /**
             * Computes the difference between two DateTime instances and returns the difference
             * in milliseconds.
             */
            std::chrono::milliseconds operator-(const DateTime &other) const noexcept;

          private:
            aws_date_time m_date_time;
            bool m_good;
        };
    } // namespace Crt
} // namespace Aws
