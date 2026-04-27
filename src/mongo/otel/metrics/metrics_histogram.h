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
#include "mongo/otel/metrics/metrics_attributes.h"
#include "mongo/otel/metrics/metrics_metric.h"
#include "mongo/platform/rwmutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/moving_average.h"

#include <absl/container/flat_hash_set.h>

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

/**
 * Type-erased base stored in MetricsService's variant. Carries the bucket boundary configuration
 * so MetricsService can access it without knowing the attribute types.
 */
template <HistogramValueType T>
class MONGO_MOD_PUBLIC HistogramBase : public Metric {
public:
    explicit HistogramBase(boost::optional<std::vector<double>> boundaries = boost::none)
        : explicitBucketBoundaries(std::move(boundaries)) {}

    ~HistogramBase() override = default;

    boost::optional<std::vector<double>> explicitBucketBoundaries;
};

/**
 * General case: histogram with one or more attributes.
 */
template <HistogramValueType T, AttributeType... AttributeTs>
class MONGO_MOD_PUBLIC Histogram : public HistogramBase<T> {
public:
    using Attributes = std::tuple<AttributeTs...>;

    explicit Histogram(boost::optional<std::vector<double>> boundaries = boost::none)
        : HistogramBase<T>(std::move(boundaries)) {}

    ~Histogram() override = default;

    /**
     * Records a value for a specific attribute combination.
     *
     * The value must be nonnegative.
     */
    virtual void record(T value, const Attributes& attributes) = 0;
};

/**
 * Specialization for no attributes. Adds a convenience record(T value) overload so existing
 * callers don't need to pass an empty tuple.
 */
template <HistogramValueType T>
class MONGO_MOD_PUBLIC Histogram<T> : public HistogramBase<T> {
public:
    using Attributes = std::tuple<>;

    explicit Histogram(boost::optional<std::vector<double>> boundaries = boost::none)
        : HistogramBase<T>(std::move(boundaries)) {}

    ~Histogram() override = default;

    virtual void record(T value, const std::tuple<>& attributes) = 0;

    void record(T value) {
        record(value, {});
    }
};

/**
 * Thin wrapper around OpenTelemetry Histogram for recording distributions of values.
 *
 * Supported types: int64_t and double.
 *
 * Attributes are passed directly to the underlying OTel histogram's Record() call. OTel
 * handles per-attribute aggregation internally, so no per-combination storage is pre-allocated
 * here.
 *
 * WARNING: The underlying OpenTelemetry library acquires locks during Record() operations.
 * Avoid using histograms in performance-sensitive code paths where lock contention could
 * impact latency/throughput. If you are using histogram in performance-sensitive code paths,
 * verify the performance is acceptable with a high level of concurrency.
 */
template <HistogramValueType T, typename... AttributeTs>
class HistogramImpl final : public Histogram<T, AttributeTs...> {
public:
    using Attributes = typename Histogram<T, AttributeTs...>::Attributes;

#ifdef MONGO_CONFIG_OTEL
    /**
     * Creates a new HistogramImpl instance.
     *
     * Meter must be non-null and remain valid for the lifetime of the HistogramImpl instance.
     */
    HistogramImpl(opentelemetry::metrics::Meter& meter,
                  std::string name,
                  std::string description,
                  std::string unit,
                  boost::optional<std::vector<double>> explicitBucketBoundaries,
                  const AttributeDefinition<AttributeTs>&... defs);
#else
    explicit HistogramImpl(const AttributeDefinition<AttributeTs>&... defs);
#endif  // MONGO_CONFIG_OTEL

    /**
     * Records a value for the given attribute combination.
     *
     * The value must be nonnegative.
     */
    void record(T value, const Attributes& attributes) override;
    using Histogram<T, AttributeTs...>::record;

    /**
     * Serializes the internal metrics `_avg` and `_count` to BSON, aggregated across all
     * attribute combinations.
     */
    BSONObj serializeToBson(const std::string& key) const override;

#ifdef MONGO_CONFIG_OTEL
    /**
     * Resets the HistogramImpl by creating a new OpenTelemetry histogram implementation and
     * resetting the internal metrics _avg and _count.
     */
    void reset(opentelemetry::metrics::Meter* meter) override;
#endif  // MONGO_CONFIG_OTEL

private:
    // Internal metrics used for server status reporting, aggregated across all attribute
    // combinations.
    MovingAverage _avg;
    Atomic<int64_t> _count;

    std::array<std::string, sizeof...(AttributeTs)> _attributeNames;

    OwnedAttributeValueLists<AttributeTs...> _ownedValueLists;
    absl::flat_hash_set<Attributes, AttributesHasher, AttributesEq> _validCombinations;

#ifdef MONGO_CONFIG_OTEL
    using UnderlyingType = std::conditional_t<std::is_same_v<T, int64_t>, uint64_t, double>;

    std::unique_ptr<opentelemetry::metrics::Histogram<UnderlyingType>> createOpenTelemetryHistogram(
        opentelemetry::metrics::Meter& meter,
        const std::string& name,
        const std::string& description,
        const std::string& unit);

    const std::string _name;
    const std::string _description;
    const std::string _unit;

    // Read-write mutex that protects the _histogram pointer.
    mutable WriteRarelyRWMutex _rwMutex;

    // The underlying OpenTelemetry histogram implementation.
    std::unique_ptr<opentelemetry::metrics::Histogram<UnderlyingType>> _histogram;
#endif  // MONGO_CONFIG_OTEL
};

// The smoothing factor for the exponential moving average. See moving_average.h.
constexpr double kAlpha = 0.2;

#ifdef MONGO_CONFIG_OTEL
template <HistogramValueType T, typename... AttributeTs>
std::unique_ptr<
    opentelemetry::metrics::Histogram<typename HistogramImpl<T, AttributeTs...>::UnderlyingType>>
HistogramImpl<T, AttributeTs...>::createOpenTelemetryHistogram(opentelemetry::metrics::Meter& meter,
                                                               const std::string& name,
                                                               const std::string& description,
                                                               const std::string& unit) {
    if constexpr (std::is_same_v<T, int64_t>) {
        // The OpenTelemetry library provides histogram implementations for uint64_t and double.
        // If a negative integer is passed to `HistogramImpl<uint64_t>::record`, it wraps to an
        // extremely large positive value due to unsigned conversion. To prevent this unintended
        // behavior, we use int64_t as our API type but store an uint64_t histogram, allowing
        // us to explicitly reject negative inputs to the record member function.
        return meter.CreateUInt64Histogram(name, description, unit);
    } else {
        return meter.CreateDoubleHistogram(name, description, unit);
    }
}

template <HistogramValueType T, typename... AttributeTs>
HistogramImpl<T, AttributeTs...>::HistogramImpl(
    opentelemetry::metrics::Meter& meter,
    std::string name,
    std::string description,
    std::string unit,
    boost::optional<std::vector<double>> explicitBucketBoundaries,
    const AttributeDefinition<AttributeTs>&... defs)
    : Histogram<T, AttributeTs...>(std::move(explicitBucketBoundaries)),
      _avg(kAlpha),
      _attributeNames{defs.name...},
      _ownedValueLists(makeOwnedAttributeValueLists(defs...)),
      _validCombinations([this] {
          auto tuples = safeMakeAttributeTuples(_ownedValueLists);
          return absl::flat_hash_set<Attributes, AttributesHasher, AttributesEq>(tuples.begin(),
                                                                                 tuples.end());
      }()),
      _name(std::move(name)),
      _description(std::move(description)),
      _unit(std::move(unit)) {
    massert(ErrorCodes::BadValue,
            "Attribute values list cannot be empty",
            ((!defs.values.empty()) && ...));
    massert(ErrorCodes::BadValue,
            "Attribute names are duplicated",
            !containsDuplicates(_attributeNames));
    _histogram = createOpenTelemetryHistogram(meter, _name, _description, _unit);
}
#else
template <HistogramValueType T, typename... AttributeTs>
HistogramImpl<T, AttributeTs...>::HistogramImpl(const AttributeDefinition<AttributeTs>&... defs)
    : _avg(kAlpha),
      _attributeNames{defs.name...},
      _ownedValueLists(makeOwnedAttributeValueLists(defs...)),
      _validCombinations([this] {
          auto tuples = safeMakeAttributeTuples(_ownedValueLists);
          return absl::flat_hash_set<Attributes, AttributesHasher, AttributesEq>(tuples.begin(),
                                                                                 tuples.end());
      }()) {
    massert(ErrorCodes::BadValue,
            "Attribute values list cannot be empty",
            ((!defs.values.empty()) && ...));
    massert(ErrorCodes::BadValue,
            "Attribute names are duplicated",
            !containsDuplicates(_attributeNames));
}
#endif  // MONGO_CONFIG_OTEL

template <HistogramValueType T, typename... AttributeTs>
void HistogramImpl<T, AttributeTs...>::record(T value, const Attributes& attributes) {
    massert(ErrorCodes::BadValue, "Histogram values must be nonnegative", value >= 0);
    if constexpr (sizeof...(AttributeTs) > 0) {
        massert(ErrorCodes::BadValue,
                "Called record using undeclared set of attributes",
                _validCombinations.contains(attributes));
    }
#ifdef MONGO_CONFIG_OTEL
    {
        auto readLock = _rwMutex.readLock();
        if constexpr (sizeof...(AttributeTs) == 0) {
            _histogram->Record(value, opentelemetry::context::Context{});
        } else {
            std::vector<AttributeNameAndValue> nameAndValues;
            nameAndValues.reserve(sizeof...(AttributeTs));
            size_t i = 0;
            std::apply(
                [&](const auto&... vals) {
                    (nameAndValues.push_back({.name = StringData(_attributeNames[i++]),
                                              .value = AnyAttributeType(vals)}),
                     ...);
                },
                attributes);
            _histogram->Record(value,
                               AttributesKeyValueIterable(std::move(nameAndValues)),
                               opentelemetry::context::Context{});
        }
    }
#endif  // MONGO_CONFIG_OTEL
    _avg.addSample(value);
    _count.fetchAndAddRelaxed(1);
}

template <HistogramValueType T, typename... AttributeTs>
BSONObj HistogramImpl<T, AttributeTs...>::serializeToBson(const std::string& key) const {
    BSONObjBuilder builder;
    BSONObjBuilder metrics{builder.subobjStart(key)};
    metrics.append("average", _avg.get().value_or(0.0));
    metrics.append("count", _count.load());
    metrics.doneFast();
    return builder.obj();
}

#ifdef MONGO_CONFIG_OTEL
template <HistogramValueType T, typename... AttributeTs>
void HistogramImpl<T, AttributeTs...>::reset(opentelemetry::metrics::Meter* meter) {
    invariant(meter);
    _avg.reset();
    _count.store(0);
    auto writeLock = _rwMutex.writeLock();
    _histogram = createOpenTelemetryHistogram(*meter, _name, _description, _unit);
};
#endif  // MONGO_CONFIG_OTEL

}  // namespace mongo::otel::metrics
