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

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <type_traits>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/summation.h"

namespace mongo::query_stats {

/**
 * An aggregated metric stores a compressed view of data. It balances the loss of information
 * with the reduction in required storage.
 */
template <typename T>
requires std::integral<T> || std::floating_point<T>
struct AggregatedMetric {

    using make_signed_t = typename std::make_signed<T>::type;
    AggregatedMetric() = default;

    AggregatedMetric(const T& val) : sum(val), min(val), max(val), sumOfSquares(val * val) {}

    void combine(const AggregatedMetric& other) {
        sum += other.sum;
        max = std::max(other.max, max);
        min = std::min(other.min, min);
        sumOfSquares += other.sumOfSquares;
    }

    /**
     * Aggregate an observed value into the metric.
     */
    void aggregate(T val) {
        sum += val;
        max = std::max(val, max);
        min = std::min(val, min);
        sumOfSquares += val * val;
    }

    void appendTo(BSONObjBuilder& builder, const StringData& fieldName) const {
        BSONObjBuilder metricsBuilder = builder.subobjStart(fieldName);
        metricsBuilder.append("sum", static_cast<long long>(sum));
        metricsBuilder.append("max", static_cast<long long>(max));
        metricsBuilder.append("min", static_cast<long long>(min));
        metricsBuilder.append("sumOfSquares", static_cast<long long>(sumOfSquares));
        metricsBuilder.done();
    }

    T sum = 0;
    // Default to the _signed_ maximum (which fits in unsigned range) because we cast to
    // BSONNumeric when serializing.
    T min = static_cast<T>(std::numeric_limits<make_signed_t>::max());
    T max = 0;

    /**
     * The sum of squares along with (an externally stored) count will allow us to compute the
     * variance/stddev.
     */
    T sumOfSquares = 0;
};

template <>
struct AggregatedMetric<double> {

    AggregatedMetric() = default;

    AggregatedMetric(const double& val) : min(val), max(val) {
        sum.addDouble(val);
        sumOfSquares.addDouble(val * val);
    }

    void combine(const AggregatedMetric& other) {
        sum.addDouble(other.sum.getDouble());
        max = std::max(other.max, max);
        min = std::min(other.min, min);
        sumOfSquares.addDouble(other.sumOfSquares.getDouble());
    }

    /**
     * Aggregate an observed value into the metric.
     */
    void aggregate(double val) {
        sum.addDouble(val);
        max = std::max(val, max);
        min = std::min(val, min);
        sumOfSquares.addDouble(val * val);
    }

    void appendTo(BSONObjBuilder& builder, const StringData& fieldName) const {
        BSONObjBuilder metricsBuilder = builder.subobjStart(fieldName);
        metricsBuilder.append("sum", static_cast<double>(sum.getDouble()));
        metricsBuilder.append("max", static_cast<double>(max));
        metricsBuilder.append("min", static_cast<double>(min));
        metricsBuilder.append("sumOfSquares", static_cast<double>(sumOfSquares.getDouble()));
        metricsBuilder.done();
    }

    DoubleDoubleSummation sum;
    // Default to the _signed_ maximum (which fits in unsigned range) because we cast to
    // BSONNumeric when serializing.
    double min = std::numeric_limits<double>::max();
    double max = 0;

    /**
     * The sum of squares along with (an externally stored) count will allow us to compute the
     * variance/stddev.
     */
    DoubleDoubleSummation sumOfSquares;
};

}  // namespace mongo::query_stats
