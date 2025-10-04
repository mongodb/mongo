/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/static_assert.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/builder.h"
#include "mongo/platform/overflow_arithmetic.h"
#include "mongo/stdx/chrono.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <chrono>
#include <cstdint>
#include <iosfwd>
#include <limits>
#include <ratio>
#include <string>
#include <type_traits>

#include <fmt/format.h>

namespace mongo {

class BSONObj;
template <typename Allocator>
class StringBuilderImpl;

template <typename Period>
class Duration;

using Nanoseconds = Duration<std::nano>;
using Microseconds = Duration<std::micro>;
using Milliseconds = Duration<std::milli>;
using Seconds = Duration<std::ratio<1>>;
using Minutes = Duration<std::ratio<60>>;
using Hours = Duration<std::ratio<3600>>;
using Days = Duration<std::ratio<86400>>;

namespace duration_detail {
template <typename>
inline constexpr bool isMongoDuration = false;
template <typename... Ts>
inline constexpr bool isMongoDuration<Duration<Ts...>> = true;
}  // namespace duration_detail

//
// Streaming output operators for common duration types. Writes the numerical value followed by
// an abbreviated unit, without a space.
//
// E.g., std::cout << Minutes{5} << std::endl; should produce the following:
// 5min
//

template <typename Duration1, typename Duration2>
using HigherPrecisionDuration =
    typename std::conditional<!Duration1::template IsLowerPrecisionThan<Duration2>::value,
                              Duration1,
                              Duration2>::type;

/**
 * Casts from one Duration precision to another.
 *
 * May throw a AssertionException if "from" is of lower-precision type and is outside the range of
 * the ToDuration. For example, Seconds::max() cannot be represented as a Milliseconds, and so
 * attempting to cast that value to Milliseconds will throw an exception.
 */
template <typename ToDuration,
          typename FromPeriod,
          std::enable_if_t<duration_detail::isMongoDuration<ToDuration>, int> = 0>
constexpr ToDuration duration_cast(const Duration<FromPeriod>& from) {
    using FromOverTo = std::ratio_divide<FromPeriod, typename ToDuration::period>;
    if (ToDuration::template isHigherPrecisionThan<Duration<FromPeriod>>()) {
        typename ToDuration::rep toCount = 0;
        uassert(ErrorCodes::DurationOverflow,
                "Overflow casting from a lower-precision duration to a higher-precision duration",
                !overflow::mul(from.count(), FromOverTo::num, &toCount));
        return ToDuration{toCount};
    }
    return ToDuration{from.count() / FromOverTo::den};
}

template <typename ToDuration,
          typename FromRep,
          typename FromPeriod,
          std::enable_if_t<duration_detail::isMongoDuration<ToDuration>, int> = 0>
constexpr ToDuration duration_cast(const stdx::chrono::duration<FromRep, FromPeriod>& d) {
    return duration_cast<ToDuration>(Duration<FromPeriod>{d.count()});
}

/**
 * Convenience method for reading the count of a duration with specified units.
 *
 * Use when logging or comparing to integers, to ensure that you're using
 * the units you intend.
 *
 * E.g., log() << durationCount<Seconds>(some duration) << " seconds";
 */
template <typename DOut, typename DIn>
inline long long durationCount(DIn d) {
    return duration_cast<DOut>(d).count();
}

template <typename DOut, typename RepIn, typename PeriodIn>
inline long long durationCount(const stdx::chrono::duration<RepIn, PeriodIn>& d) {
    return durationCount<DOut>(Duration<PeriodIn>{d.count()});
}

/**
 * Type representing a duration using a 64-bit counter.
 *
 * The Period template argument is a std::ratio describing the units of the duration type.
 *
 * This type's behavior is similar to std::chrono::duration, but instead of undefined behavior on
 * overflows and other conversions, throws exceptions.
 */
template <typename Period>
class Duration {
public:
    static constexpr StringData unit_short() {
        if constexpr (std::is_same_v<Duration, Nanoseconds>) {
            return "ns"_sd;
        } else if constexpr (std::is_same_v<Duration, Microseconds>) {
            return "\xce\xbcs"_sd;
        } else if constexpr (std::is_same_v<Duration, Milliseconds>) {
            return "ms"_sd;
        } else if constexpr (std::is_same_v<Duration, Seconds>) {
            return "s"_sd;
        } else if constexpr (std::is_same_v<Duration, Minutes>) {
            return "min"_sd;
        } else if constexpr (std::is_same_v<Duration, Hours>) {
            return "hr"_sd;
        } else if constexpr (std::is_same_v<Duration, Days>) {
            return "d"_sd;
        }
        return StringData{};
    }
    static constexpr StringData mongoUnitSuffix() {
        if constexpr (std::is_same_v<Duration, Nanoseconds>) {
            return "Nanos"_sd;
        } else if constexpr (std::is_same_v<Duration, Microseconds>) {
            return "Micros"_sd;
        } else if constexpr (std::is_same_v<Duration, Milliseconds>) {
            return "Millis"_sd;
        } else if constexpr (std::is_same_v<Duration, Seconds>) {
            return "Seconds"_sd;
        } else if constexpr (std::is_same_v<Duration, Minutes>) {
            return "Minutes"_sd;
        } else if constexpr (std::is_same_v<Duration, Hours>) {
            return "Hours"_sd;
        } else if constexpr (std::is_same_v<Duration, Days>) {
            return "Days"_sd;
        }
        return StringData{};
    }
    MONGO_STATIC_ASSERT_MSG(Period::num > 0, "Duration::period's numerator must be positive");
    MONGO_STATIC_ASSERT_MSG(Period::den > 0, "Duration::period's denominator must be positive");

    using rep = int64_t;
    using period = Period;

    /**
     * Type with static bool "value" set to true if this Duration type is higher precision than
     * OtherDuration. That is, if OtherDuration::period > period.
     */
    template <typename OtherDuration>
    struct IsHigherPrecisionThan {
        using OtherOverThis = std::ratio_divide<typename OtherDuration::period, period>;
        MONGO_STATIC_ASSERT_MSG(
            OtherOverThis::den == 1 || OtherOverThis::num == 1,
            "Mongo duration types are only compatible with each other when one's period "
            "is an even multiple of the other's.");
        static constexpr bool value = OtherOverThis::den == 1 && OtherOverThis::num != 1;
    };

    /**
     * Type with static bool "value" set to true if this Duration type is lower precision than
     * OtherDuration. That is, if OtherDuration::period > period.
     */
    template <typename OtherDuration>
    struct IsLowerPrecisionThan {
        using OtherOverThis = std::ratio_divide<typename OtherDuration::period, period>;
        MONGO_STATIC_ASSERT_MSG(
            OtherOverThis::den == 1 || OtherOverThis::num == 1,
            "Mongo duration types are only compatible with each other when one's period "
            "is an even multiple of the other's.");
        static constexpr bool value = OtherOverThis::num == 1 && OtherOverThis::den != 1;
    };

    /**
     * Function that returns true if period > OtherDuration::period.
     */
    template <typename OtherDuration>
    constexpr static bool isHigherPrecisionThan() {
        return IsHigherPrecisionThan<OtherDuration>::value;
    }

    /**
     * Function that returns true if period < OtherDuration::period.
     */
    template <typename OtherDuration>
    constexpr static bool isLowerPrecisionThan() {
        return IsLowerPrecisionThan<OtherDuration>::value;
    }

    static constexpr Duration zero() {
        return Duration{};
    }

    static constexpr Duration min() {
        return Duration{std::numeric_limits<rep>::min()};
    }

    static constexpr Duration max() {
        return Duration{std::numeric_limits<rep>::max()};
    }

    /**
     * Constructs the zero duration.
     */
    constexpr Duration() = default;

    /**
     * Constructs a duration representing "r" periods.
     */
    template <
        typename Rep2,
        stdx::enable_if_t<std::is_convertible<Rep2, rep>::value && std::is_integral<Rep2>::value,
                          int> = 0>
    constexpr explicit Duration(const Rep2& r) : _count(r) {
        MONGO_STATIC_ASSERT_MSG(
            std::is_signed<Rep2>::value || sizeof(Rep2) < sizeof(rep),
            "Durations must be constructed from values of integral type that are "
            "representable as 64-bit signed integers");
    }

    /**
     * Implicit converting constructor from a lower-precision duration to a higher-precision one, as
     * by duration_cast.
     *
     * It is a compilation error to convert from higher precision to lower, or if the conversion
     * would cause an integer overflow.
     */
    /** Implicitly convertible if `FromPeriod` is a multiple of `period`. */
    template <typename FromPeriod>
    requires(std::ratio_divide<FromPeriod, period>::den == 1)
    constexpr Duration(const Duration<FromPeriod>& from) : Duration(duration_cast<Duration>(from)) {
        MONGO_STATIC_ASSERT_MSG(
            !isLowerPrecisionThan<Duration<FromPeriod>>(),
            "Use duration_cast to convert from higher precision Duration types to lower "
            "precision ones");
    }

    stdx::chrono::system_clock::duration toSystemDuration() const {
        using SystemDuration = stdx::chrono::system_clock::duration;
        return SystemDuration{duration_cast<Duration<SystemDuration::period>>(*this).count()};
    }

    /**
     * Returns the number of periods represented by this duration.
     *
     * It is better to use durationCount<DesiredDurationType>(value), since it makes the unit of the
     * count clear at the call site.
     */
    constexpr rep count() const {
        return _count;
    }

    /**
     * Compares this duration to another duration of the same type.
     *
     * Returns 1, -1 or 0 depending on whether this duration is greater than, less than or equal to
     * the other duration, respectively.
     */
    constexpr int compare(const Duration& other) const {
        return (count() > other.count()) ? 1 : (count() < other.count()) ? -1 : 0;
    }

    /**
     * Compares this duration to a lower-precision duration, "other".
     */
    template <typename OtherPeriod>
    int compare(const Duration<OtherPeriod>& other) const {
        if (isLowerPrecisionThan<Duration<OtherPeriod>>()) {
            return -other.compare(*this);
        }
        using OtherOverThis = std::ratio_divide<OtherPeriod, period>;
        rep otherCount;
        if (overflow::mul(other.count(), OtherOverThis::num, &otherCount)) {
            return other.count() < 0 ? 1 : -1;
        }
        if (count() < otherCount) {
            return -1;
        }
        if (count() > otherCount) {
            return 1;
        }
        return 0;
    }

    constexpr Duration operator+() const {
        return *this;
    }

    Duration operator-() const {
        uassert(ErrorCodes::DurationOverflow, "Cannot negate the minimum duration", *this != min());
        return Duration(-count());
    }

    //
    // In-place arithmetic operators
    //

    Duration& operator++() {
        return (*this) += Duration{1};
    }

    Duration operator++(int) {
        auto result = *this;
        *this += Duration{1};
        return result;
    }

    Duration operator--() {
        return (*this) -= Duration{1};
    }

    Duration operator--(int) {
        auto result = *this;
        *this -= Duration{1};
        return result;
    }

    Duration& operator+=(const Duration& other) {
        uassert(ErrorCodes::DurationOverflow,
                str::stream() << "Overflow while adding " << other << " to " << *this,
                !overflow::add(count(), other.count(), &_count));
        return *this;
    }

    Duration& operator-=(const Duration& other) {
        uassert(ErrorCodes::DurationOverflow,
                str::stream() << "Overflow while subtracting " << other << " from " << *this,
                !overflow::sub(count(), other.count(), &_count));
        return *this;
    }

    template <typename Rep2>
    Duration& operator*=(const Rep2& scale) {
        MONGO_STATIC_ASSERT_MSG(
            std::is_integral<Rep2>::value && std::is_signed<Rep2>::value,
            "Durations may only be multiplied by values of signed integral type");
        uassert(ErrorCodes::DurationOverflow,
                str::stream() << "Overflow while multiplying " << *this << " by " << scale,
                !overflow::mul(count(), scale, &_count));
        return *this;
    }

    template <typename Rep2>
    Duration& operator/=(const Rep2& scale) {
        MONGO_STATIC_ASSERT_MSG(std::is_integral<Rep2>::value && std::is_signed<Rep2>::value,
                                "Durations may only be divided by values of signed integral type");
        uassert(ErrorCodes::DurationOverflow,
                str::stream() << "Overflow while dividing " << *this << " by -1",
                (count() != min().count() || scale != -1));
        _count /= scale;
        return *this;
    }

    BSONObj toBSON() const;

    std::string toString() const {
        return fmt::format("{} {}", count(), unit_short());
    }


private:
    rep _count = {};
};

template <typename LhsPeriod, typename RhsPeriod>
constexpr bool operator==(const Duration<LhsPeriod>& lhs, const Duration<RhsPeriod>& rhs) {
    return lhs.compare(rhs) == 0;
}

template <typename LhsPeriod, typename RhsPeriod>
constexpr bool operator!=(const Duration<LhsPeriod>& lhs, const Duration<RhsPeriod>& rhs) {
    return lhs.compare(rhs) != 0;
}

template <typename LhsPeriod, typename RhsPeriod>
constexpr bool operator<(const Duration<LhsPeriod>& lhs, const Duration<RhsPeriod>& rhs) {
    return lhs.compare(rhs) < 0;
}

template <typename LhsPeriod, typename RhsPeriod>
constexpr bool operator<=(const Duration<LhsPeriod>& lhs, const Duration<RhsPeriod>& rhs) {
    return lhs.compare(rhs) <= 0;
}

template <typename LhsPeriod, typename RhsPeriod>
constexpr bool operator>(const Duration<LhsPeriod>& lhs, const Duration<RhsPeriod>& rhs) {
    return lhs.compare(rhs) > 0;
}

template <typename LhsPeriod, typename RhsPeriod>
constexpr bool operator>=(const Duration<LhsPeriod>& lhs, const Duration<RhsPeriod>& rhs) {
    return lhs.compare(rhs) >= 0;
}

/**
 * Returns the sum of two durations, "lhs" and "rhs".
 */
template <
    typename LhsPeriod,
    typename RhsPeriod,
    typename ReturnDuration = HigherPrecisionDuration<Duration<LhsPeriod>, Duration<RhsPeriod>>>
ReturnDuration operator+(const Duration<LhsPeriod>& lhs, const Duration<RhsPeriod>& rhs) {
    ReturnDuration result = lhs;
    result += rhs;
    return result;
}

/**
 * Returns the result of subtracting "rhs" from "lhs".
 */
template <
    typename LhsPeriod,
    typename RhsPeriod,
    typename ReturnDuration = HigherPrecisionDuration<Duration<LhsPeriod>, Duration<RhsPeriod>>>
ReturnDuration operator-(const Duration<LhsPeriod>& lhs, const Duration<RhsPeriod>& rhs) {
    ReturnDuration result = lhs;
    result -= rhs;
    return result;
}

/**
 * Returns the product of a duration "d" and a unitless integer, "scale".
 */
template <typename Period, typename Rep2>
Duration<Period> operator*(Duration<Period> d, const Rep2& scale) {
    d *= scale;
    return d;
}

template <typename Period, typename Rep2>
Duration<Period> operator*(const Rep2& scale, Duration<Period> d) {
    d *= scale;
    return d;
}

/**
 * Returns duration "d" divided by unitless integer "scale".
 */
template <typename Period, typename Rep2>
Duration<Period> operator/(Duration<Period> d, const Rep2& scale) {
    d /= scale;
    return d;
}

template <typename Stream, typename Period>
Stream& streamPut(Stream& os, const Duration<Period>& dp) {
    MONGO_STATIC_ASSERT_MSG(!Duration<Period>::unit_short().empty(),
                            "Only standard Durations can logged");
    return os << dp.count() << dp.unit_short();
}

template <typename Period>
std::ostream& operator<<(std::ostream& os, Duration<Period> dp) {
    MONGO_STATIC_ASSERT_MSG(!Duration<Period>::unit_short().empty(),
                            "Only standard Durations can logged");
    return streamPut(os, dp);
}

template <typename Allocator, typename Period>
StringBuilderImpl<Allocator>& operator<<(StringBuilderImpl<Allocator>& os, Duration<Period> dp) {
    MONGO_STATIC_ASSERT_MSG(!Duration<Period>::unit_short().empty(),
                            "Only standard Durations can logged");
    return streamPut(os, dp);
}

template <typename Period>
auto format_as(const Duration<Period>& dur) {
    return dur.toString();
}

/**
 * Make a std::chrono::duration from an arithmetic expression and a period ratio.
 * This does not do any math or precision changes. It's just a type-deduced wrapper
 * that attaches a period to a number for typesafety. The output std::chrono::duration
 * will retain the Rep type and value of the input argument.
 *
 * E.g:
 *      int waited = 123;  // unitless, type-unsafe millisecond count.
 *      auto dur = deduceChronoDuration<std::milli>(waited);
 *      static_assert(std::is_same_v<decltype(dur),
 *                                   std::chrono::duration<int, std::milli>>);
 *      invariant(dur.count() == 123);
 *
 * Note that std::chrono::duration<int, std::milli> is not std::milliseconds,
 * which has a different (unspecified) Rep type.
 *
 * Then mongo::duration_cast can convert the deduced std::chrono::duration to
 * mongo::Duration, or `std::chrono::duration_cast` be used to adjust the rep
 * to create a more canonical std::chrono::duration:
 *
 *      auto durMongo = duration_cast<Milliseconds>(dur);
 *      auto durChrono = duration_cast<std::milliseconds>(dur);
 *
 * Order the cast operations carefully to avoid losing range or precision.
 */
template <typename Per = std::ratio<1>, typename Rep>
constexpr auto deduceChronoDuration(const Rep& count) {
    return stdx::chrono::duration<Rep, Per>{count};
}

}  // namespace mongo
