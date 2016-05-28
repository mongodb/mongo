/*    Copyright 2016 MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <cstdint>
#include <iosfwd>
#include <limits>
#include <ratio>

#include "mongo/platform/overflow_arithmetic.h"
#include "mongo/stdx/chrono.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

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

//
// Streaming output operators for common duration types. Writes the numerical value followed by
// an abbreviated unit, without a space.
//
// E.g., std::cout << Minutes{5} << std::endl; should produce the following:
// 5min
//

std::ostream& operator<<(std::ostream& os, Nanoseconds ns);
std::ostream& operator<<(std::ostream& os, Microseconds us);
std::ostream& operator<<(std::ostream& os, Milliseconds ms);
std::ostream& operator<<(std::ostream& os, Seconds s);
std::ostream& operator<<(std::ostream& os, Minutes m);
std::ostream& operator<<(std::ostream& os, Hours h);

template <typename Allocator>
StringBuilderImpl<Allocator>& operator<<(StringBuilderImpl<Allocator>& os, Nanoseconds ns);

template <typename Allocator>
StringBuilderImpl<Allocator>& operator<<(StringBuilderImpl<Allocator>& os, Microseconds us);

template <typename Allocator>
StringBuilderImpl<Allocator>& operator<<(StringBuilderImpl<Allocator>& os, Milliseconds ms);

template <typename Allocator>
StringBuilderImpl<Allocator>& operator<<(StringBuilderImpl<Allocator>& os, Seconds s);

template <typename Allocator>
StringBuilderImpl<Allocator>& operator<<(StringBuilderImpl<Allocator>& os, Minutes m);

template <typename Allocator>
StringBuilderImpl<Allocator>& operator<<(StringBuilderImpl<Allocator>& os, Hours h);


template <typename Duration1, typename Duration2>
using HigherPrecisionDuration =
    typename std::conditional<!Duration1::template IsLowerPrecisionThan<Duration2>::value,
                              Duration1,
                              Duration2>::type;

/**
 * Casts from one Duration precision to another.
 *
 * May throw a UserException if "from" is of lower-precision type and is outside the range of the
 * ToDuration. For example, Seconds::max() cannot be represented as a Milliseconds, and
 * so attempting to cast that value to Milliseconds will throw an exception.
 */
template <typename ToDuration, typename FromPeriod>
ToDuration duration_cast(const Duration<FromPeriod>& from) {
    using FromOverTo = std::ratio_divide<FromPeriod, typename ToDuration::period>;
    if (ToDuration::template isHigherPrecisionThan<Duration<FromPeriod>>()) {
        typename ToDuration::rep toCount;
        uassert(ErrorCodes::DurationOverflow,
                "Overflow casting from a lower-precision duration to a higher-precision duration",
                !mongoSignedMultiplyOverflow64(from.count(), FromOverTo::num, &toCount));
        return ToDuration{toCount};
    }
    return ToDuration{from.count() / FromOverTo::den};
}

template <typename ToDuration, typename FromRep, typename FromPeriod>
inline ToDuration duration_cast(const stdx::chrono::duration<FromRep, FromPeriod>& d) {
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
    static_assert(Period::num > 0, "Duration::period's numerator must be positive");
    static_assert(Period::den > 0, "Duration::period's denominator must be positive");

    using rep = int64_t;
    using period = Period;

    /**
     * Type with static bool "value" set to true if this Duration type is higher precision than
     * OtherDuration. That is, if OtherDuration::period > period.
     */
    template <typename OtherDuration>
    struct IsHigherPrecisionThan {
        using OtherOverThis = std::ratio_divide<typename OtherDuration::period, period>;
        static_assert(OtherOverThis::den == 1 || OtherOverThis::num == 1,
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
        static_assert(OtherOverThis::den == 1 || OtherOverThis::num == 1,
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
        static_assert(std::is_signed<Rep2>::value || sizeof(Rep2) < sizeof(rep),
                      "Durations must be constructed from values of integral type that are "
                      "representable as 64-bit signed integers");
    }

    /**
     * Constructs a higher-precision duration from a lower-precision one, as by duration_cast.
     *
     * Throws a UserException if "from" is out of the range of this duration type.
     *
     * It is a compilation error to attempt a conversion from higher-precision to lower-precision by
     * this constructor.
     */
    template <typename FromPeriod>
    /*implicit*/ Duration(const Duration<FromPeriod>& from)
        : Duration(duration_cast<Duration>(from)) {
        static_assert(!isLowerPrecisionThan<Duration<FromPeriod>>(),
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
        if (mongoSignedMultiplyOverflow64(other.count(), OtherOverThis::num, &otherCount)) {
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
                !mongoSignedAddOverflow64(count(), other.count(), &_count));
        return *this;
    }

    Duration& operator-=(const Duration& other) {
        uassert(ErrorCodes::DurationOverflow,
                str::stream() << "Overflow while subtracting " << other << " from " << *this,
                !mongoSignedSubtractOverflow64(count(), other.count(), &_count));
        return *this;
    }

    template <typename Rep2>
    Duration& operator*=(const Rep2& scale) {
        static_assert(std::is_integral<Rep2>::value && std::is_signed<Rep2>::value,
                      "Durations may only be multiplied by values of signed integral type");
        uassert(ErrorCodes::DurationOverflow,
                str::stream() << "Overflow while multiplying " << *this << " by " << scale,
                !mongoSignedMultiplyOverflow64(count(), scale, &_count));
        return *this;
    }

    template <typename Rep2>
    Duration& operator/=(const Rep2& scale) {
        static_assert(std::is_integral<Rep2>::value && std::is_signed<Rep2>::value,
                      "Durations may only be divided by values of signed integral type");
        uassert(ErrorCodes::DurationOverflow,
                str::stream() << "Overflow while dividing " << *this << " by -1",
                (count() != min().count() || scale != -1));
        _count /= scale;
        return *this;
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

}  // namespace mongo
