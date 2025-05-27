/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/summation.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace mongo::query_stats {
namespace agg_metric_detail {

/**
 * Default to the _signed_ maximum (which fits in unsigned range) because we
 * cast to BSONNumeric when serializing.
 */
template <typename T>
constexpr inline T kInitialMin = static_cast<T>(std::numeric_limits<std::make_signed_t<T>>::max());
template <>
constexpr inline double kInitialMin<double> = std::numeric_limits<double>::max();

template <typename T>
constexpr inline T kInitialMax = 0;
template <>
constexpr inline double kInitialMax<double> = 0;

template <typename T>
constexpr inline T kInitialSummation = static_cast<T>(0);

/** Arithmetic wrapper around DoubleDoubleSummation */
class DoubleSum {
public:
    DoubleSum() = default;
    DoubleSum(double sum, double addend = 0.0) : _v(DoubleDoubleSummation::create(sum, addend)) {}
    operator double() const {
        return _v.getDouble();
    }
    DoubleSum& operator+=(double x) {
        _v.addDouble(x);
        return *this;
    }
    DoubleSum& operator+=(const DoubleSum& x) {
        _v.addDouble(x);
        return *this;
    }

private:
    DoubleDoubleSummation _v;
};

template <typename T>
long long bsonValue(const T& x) {
    return static_cast<long long>(x);
}
inline Decimal128 bsonValue(const Decimal128& x) {
    return x;
}
inline double bsonValue(double x) {
    return x;
}
inline double bsonValue(const DoubleSum& x) {
    return x;
}

template <typename T>
using Summation = std::conditional_t<std::is_same_v<T, double>, DoubleSum, T>;

template <typename T>
class AggregatedMetric {
public:
    AggregatedMetric() = default;

    explicit AggregatedMetric(const T& val) : max{val}, min{val} {
        sum += val;
        sumOfSquares = sumOfSquares.add(Decimal128(val).multiply(Decimal128(val)));
    }

    void combine(const AggregatedMetric& other) {
        sum += other.sum;
        max = std::max(other.max, max);
        min = std::min(other.min, min);
        sumOfSquares = sumOfSquares.add(other.sumOfSquares);
    }

    /**
     * Aggregate an observed value into the metric.
     */
    void aggregate(T val) {
        sum += val;
        max = std::max(val, max);
        min = std::min(val, min);
        sumOfSquares = sumOfSquares.add(Decimal128(val).multiply(Decimal128(val)));
    }

    void appendTo(BSONObjBuilder& builder, StringData fieldName) const {
        BSONObjBuilder{builder.subobjStart(fieldName)}
            .append("sum", bsonValue(sum))
            .append("max", bsonValue(max))
            .append("min", bsonValue(min))
            .append("sumOfSquares", bsonValue(sumOfSquares));
    }

    void appendToIfNonNegative(BSONObjBuilder& builder, StringData fieldName) const {
        if (sum >= 0) {
            appendTo(builder, fieldName);
        }
    }

private:
    Summation<T> sum{kInitialSummation<T>};
    T max{kInitialMax<T>};
    T min{kInitialMin<T>};

    /**
     * The sum of squares along with (an externally stored) count will allow us to compute the
     * variance/stddev.
     */
    Decimal128 sumOfSquares{};
};

extern template void AggregatedMetric<uint64_t>::appendTo(BSONObjBuilder& builder,
                                                          StringData fieldName) const;

}  // namespace agg_metric_detail

/**
 * An aggregated metric stores a compressed view of data. It balances the loss of information
 * with the reduction in required storage.
 */
template <typename T>
requires std::is_arithmetic_v<T>
class AggregatedMetric : public agg_metric_detail::AggregatedMetric<T> {
public:
    using agg_metric_detail::AggregatedMetric<T>::AggregatedMetric;
};

/**
 * An aggregated metric that counts frequency of different boolean values.
 */
struct AggregatedBool {

    /**
     * Aggregate an observed value into the metric.
     */
    void aggregate(bool val) {
        if (val) {
            ++trueCount;
        } else {
            ++falseCount;
        }
    }

    void appendTo(BSONObjBuilder& builder, StringData fieldName) const;

    uint32_t trueCount{0};
    uint32_t falseCount{0};
};


}  // namespace mongo::query_stats
