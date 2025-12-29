/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/config.h"
#include "mongo/otel/metrics/metrics_metric.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/moving_average.h"

#ifdef MONGO_CONFIG_OTEL
#include <opentelemetry/context/context.h>
#include <opentelemetry/metrics/meter.h>
#endif

namespace mongo::otel::metrics {

/**
 * Concept that restricts histogram types to int64_t and double only.
 */
template <typename T>
concept HistogramValueType = std::same_as<T, int64_t> || std::same_as<T, double>;

template <HistogramValueType T>
class MONGO_MOD_PUBLIC Histogram : public Metric {
public:
    ~Histogram() override = default;

    /**
     * Records a value.
     *
     * The value must be nonnegative.
     */
    virtual void record(T value) = 0;
};

#ifdef MONGO_CONFIG_OTEL
/**
 * Thin wrapper around OpenTelemetry Histogram for recording distributions of values.
 *
 * Supported types: int64_t and double.
 *
 * WARNING: The underlying OpenTelemetry library acquires locks during Record() operations.
 * Avoid using histograms in performance-sensitive code paths where lock contention could
 * impact latency/throughput.
 */
template <HistogramValueType T>
class HistogramImpl final : public Histogram<T> {
public:
    /**
     * Creates a new Histogram instance.
     *
     * Meter must be non-null and remain valid for the lifetime of the Histogram instance.
     */
    HistogramImpl(opentelemetry::metrics::Meter* meter,
                  const std::string& name,
                  const std::string& description,
                  const std::string& unit);

    /**
     * Records a value.
     *
     * The value must be nonnegative.
     */
    void record(T value) override;

    /**
     * Serializes the internal metrics `_avg` and `_count` to BSON.
     */
    BSONObj serializeToBson(const std::string& key) const override;

private:
    using UnderlyingType = std::conditional_t<std::is_same_v<T, int64_t>, uint64_t, double>;

    // The underlying OpenTelemety histogram implementation.
    std::unique_ptr<opentelemetry::metrics::Histogram<UnderlyingType>> _histogram;

    // Internal metrics used for server status reporting.
    MovingAverage _avg;
    Atomic<int64_t> _count;
};

// The smoothing factor for the exponential moving average. See moving_average.h.
constexpr double kAlpha = 0.2;

template <HistogramValueType T>
HistogramImpl<T>::HistogramImpl(opentelemetry::metrics::Meter* meter,
                                const std::string& name,
                                const std::string& description,
                                const std::string& unit)
    : _avg(kAlpha) {
    invariant(meter);
    if constexpr (std::is_same_v<T, int64_t>) {
        // The OpenTelemetry library provides histogram implementations for uint64_t and double.
        // If a negative integer is passed to `HistogramImpl<uint64_t>::record`, it wraps to an
        // extremely large positive value due to unsigned conversion. To prevent this unintended
        // behavior, we use int64_t as our API type but store an uint64_t histogram, allowing
        // us to explicitly reject negative inputs to the record member function.
        _histogram = meter->CreateUInt64Histogram(name, description, unit);
    } else {
        _histogram = meter->CreateDoubleHistogram(name, description, unit);
    }
}

template <HistogramValueType T>
void HistogramImpl<T>::record(T value) {
    massert(ErrorCodes::BadValue, "Histogram values must be nonnegative", value >= 0);
    _histogram->Record(value, opentelemetry::context::Context{});
    _avg.addSample(value);
    _count.fetchAndAddRelaxed(1);
}

template <HistogramValueType T>
BSONObj HistogramImpl<T>::serializeToBson(const std::string& key) const {
    BSONObjBuilder builder;
    BSONObjBuilder metrics{builder.subobjStart(key)};
    metrics.append("average", _avg.get().value_or(0.0));
    metrics.append("count", _count.load());
    metrics.done();
    return builder.obj();
}

#else
template <HistogramValueType T>
class NoopHistogramImpl final : public Histogram<T> {
public:
    void record(T value) override {}

    BSONObj serializeToBson(const std::string& key) const override {
        return BSONObj();
    }
};
#endif
}  // namespace mongo::otel::metrics
