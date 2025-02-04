/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/crt/DateTime.h>

#include <chrono>

namespace Aws
{
    namespace Crt
    {
        DateTime::DateTime() noexcept : m_good(true)
        {
            std::chrono::system_clock::time_point time;
            aws_date_time_init_epoch_millis(
                &m_date_time,
                static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count()));
        }

        DateTime::DateTime(const std::chrono::system_clock::time_point &timepointToAssign) noexcept : m_good(true)
        {
            aws_date_time_init_epoch_millis(
                &m_date_time,
                static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(timepointToAssign.time_since_epoch())
                        .count()));
        }

        DateTime::DateTime(uint64_t millisSinceEpoch) noexcept : m_good(true)
        {
            aws_date_time_init_epoch_millis(&m_date_time, millisSinceEpoch);
        }

        DateTime::DateTime(double epoch_millis) noexcept : m_good(true)
        {
            aws_date_time_init_epoch_secs(&m_date_time, epoch_millis);
        }

        DateTime::DateTime(const char *timestamp, DateFormat format) noexcept
        {
            ByteBuf timeStampBuf = ByteBufFromCString(timestamp);

            m_good =
                (aws_date_time_init_from_str(&m_date_time, &timeStampBuf, static_cast<aws_date_format>(format)) ==
                 AWS_ERROR_SUCCESS);
        }

        bool DateTime::operator==(const DateTime &other) const noexcept
        {
            return aws_date_time_diff(&m_date_time, &other.m_date_time) == 0;
        }

        bool DateTime::operator<(const DateTime &other) const noexcept
        {
            return aws_date_time_diff(&m_date_time, &other.m_date_time) < 0;
        }

        bool DateTime::operator>(const DateTime &other) const noexcept
        {
            return aws_date_time_diff(&m_date_time, &other.m_date_time) > 0;
        }

        bool DateTime::operator!=(const DateTime &other) const noexcept
        {
            return !(*this == other);
        }

        bool DateTime::operator<=(const DateTime &other) const noexcept
        {
            return aws_date_time_diff(&m_date_time, &other.m_date_time) <= 0;
        }

        bool DateTime::operator>=(const DateTime &other) const noexcept
        {
            return aws_date_time_diff(&m_date_time, &other.m_date_time) >= 0;
        }

        DateTime DateTime::operator+(const std::chrono::milliseconds &a) const noexcept
        {
            auto currentTime = aws_date_time_as_millis(&m_date_time);
            currentTime += a.count();
            return {currentTime};
        }

        DateTime DateTime::operator-(const std::chrono::milliseconds &a) const noexcept
        {
            auto currentTime = aws_date_time_as_millis(&m_date_time);
            currentTime -= a.count();
            return {currentTime};
        }

        DateTime &DateTime::operator=(double secondsSinceEpoch) noexcept
        {
            aws_date_time_init_epoch_secs(&m_date_time, secondsSinceEpoch);
            m_good = true;
            return *this;
        }

        DateTime &DateTime::operator=(uint64_t millisSinceEpoch) noexcept
        {
            aws_date_time_init_epoch_millis(&m_date_time, millisSinceEpoch);
            m_good = true;
            return *this;
        }

        DateTime &DateTime::operator=(const std::chrono::system_clock::time_point &timepointToAssign) noexcept
        {
            aws_date_time_init_epoch_millis(
                &m_date_time,
                static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(timepointToAssign.time_since_epoch())
                        .count()));
            m_good = true;
            return *this;
        }

        DateTime &DateTime::operator=(const char *timestamp) noexcept
        {
            ByteBuf timeStampBuf = aws_byte_buf_from_c_str(timestamp);

            m_good = aws_date_time_init_from_str(
                         &m_date_time, &timeStampBuf, static_cast<aws_date_format>(DateFormat::AutoDetect)) ==
                     AWS_ERROR_SUCCESS;
            return *this;
        }

        DateTime::operator bool() const noexcept
        {
            return m_good;
        }

        int DateTime::GetLastError() const noexcept
        {
            return aws_last_error();
        }

        bool DateTime::ToLocalTimeString(DateFormat format, ByteBuf &outputBuf) const noexcept
        {
            return (
                aws_date_time_to_local_time_str(&m_date_time, static_cast<aws_date_format>(format), &outputBuf) ==
                AWS_ERROR_SUCCESS);
        }

        bool DateTime::ToGmtString(DateFormat format, ByteBuf &outputBuf) const noexcept
        {
            return (
                aws_date_time_to_utc_time_str(&m_date_time, static_cast<aws_date_format>(format), &outputBuf) ==
                AWS_ERROR_SUCCESS);
        }

        double DateTime::SecondsWithMSPrecision() const noexcept
        {
            return aws_date_time_as_epoch_secs(&m_date_time);
        }

        uint64_t DateTime::Millis() const noexcept
        {
            return aws_date_time_as_millis(&m_date_time);
        }

        std::chrono::system_clock::time_point DateTime::UnderlyingTimestamp() const noexcept
        {
            return std::chrono::system_clock::from_time_t(m_date_time.timestamp);
        }

        uint16_t DateTime::GetYear(bool localTime) const noexcept
        {
            return aws_date_time_year(&m_date_time, localTime);
        }

        Month DateTime::GetMonth(bool localTime) const noexcept
        {
            return static_cast<Month>(aws_date_time_month(&m_date_time, localTime));
        }

        uint8_t DateTime::GetDay(bool localTime) const noexcept
        {
            return aws_date_time_month_day(&m_date_time, localTime);
        }

        DayOfWeek DateTime::GetDayOfWeek(bool localTime) const noexcept
        {
            return static_cast<DayOfWeek>(aws_date_time_day_of_week(&m_date_time, localTime));
        }

        uint8_t DateTime::GetHour(bool localTime) const noexcept
        {
            return aws_date_time_hour(&m_date_time, localTime);
        }

        uint8_t DateTime::GetMinute(bool localTime) const noexcept
        {
            return aws_date_time_minute(&m_date_time, localTime);
        }

        uint8_t DateTime::GetSecond(bool localTime) const noexcept
        {
            return aws_date_time_second(&m_date_time, localTime);
        }

        bool DateTime::IsDST(bool localTime) const noexcept
        {
            return aws_date_time_dst(&m_date_time, localTime);
        }

        DateTime DateTime::Now() noexcept
        {
            DateTime dateTime;
            aws_date_time_init_now(&dateTime.m_date_time);
            return dateTime;
        }

        std::chrono::milliseconds DateTime::operator-(const DateTime &other) const noexcept
        {
            auto diff = aws_date_time_diff(&m_date_time, &other.m_date_time);
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::seconds(diff));
        }
    } // namespace Crt
} // namespace Aws
