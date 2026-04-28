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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/config.h"
#include "mongo/otel/metrics/metrics_attributes.h"
#include "mongo/otel/metrics/metrics_counter.h"
#include "mongo/otel/metrics/metrics_gauge.h"
#include "mongo/otel/metrics/metrics_metric.h"
#include "mongo/otel/metrics/metrics_updown_counter.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <array>
#include <memory>
#include <string>

#ifdef MONGO_CONFIG_OTEL
#include <opentelemetry/metrics/meter.h>
#endif

namespace mongo::otel::metrics {

/**
 * Observable base for all scalar metrics (Counter, Gauge, UpDownCounter), parameterized only on
 * the value type T and not on attribute types. This allows MetricsService to store instances in the
 * OwnedMetric variant without knowing the attribute types. Provides values() for OTEL callbacks and
 * BSON serialization via Metric. MetricsService uses the three tag subclasses (ObservableCounter,
 * ObservableGauge, ObservableUpDownCounter) as the stored variant type so the visitor can create
 * the right OTEL instrument.
 */
template <typename T>
class ObservableScalarMetric : public Metric {
public:
    ~ObservableScalarMetric() override = default;

    /**
     * For each combination of attributes for which the metric has been written, returns the
     * set of attributes and the value associated with it. Result is valid only while this metric
     * is alive.
     */
    virtual AttributesAndValues<T> values() const = 0;
};

/** Tag base stored in MetricsService's OwnedMetric for counter instruments. */
template <typename T>
class ObservableCounter : public virtual ObservableScalarMetric<T> {
public:
    ~ObservableCounter() override = default;
};

/** Tag base stored in MetricsService's OwnedMetric for gauge instruments. */
template <typename T>
class ObservableGauge : public virtual ObservableScalarMetric<T> {
public:
    ~ObservableGauge() override = default;
};

/** Tag base stored in MetricsService's OwnedMetric for up-down counter instruments. */
template <typename T>
class ObservableUpDownCounter : public virtual ObservableScalarMetric<T> {
public:
    ~ObservableUpDownCounter() override = default;
};

/**
 * Single lock-free implementation for Counter, Gauge, and UpDownCounter with attribute support.
 * All three share the same per-attribute-combination storage. The difference in semantics is
 * enforced by the interfaces:
 *   - Counter: add() validates nonnegative (via the Counter non-virtual wrapper → doAdd).
 *   - UpDownCounter: add() accepts any delta (virtual dispatch directly to this impl).
 *   - Gauge: set() stores the value (storeRelaxed instead of fetchAndAdd).
 *
 * MetricsService stores this as ObservableCounter<T>, ObservableGauge<T>, or
 * ObservableUpDownCounter<T> depending on which instrument type was requested. The shared virtual
 * base ObservableScalarMetric<T> provides values() and is inherited once.
 *
 * Callers should always hold a typed interface reference (Counter<T>&, Gauge<T>&, or
 * UpDownCounter<T>&) rather than a ScalarMetricImpl reference directly, both for semantic clarity
 * and to avoid name-lookup ambiguity between the inherited add() overloads.
 */
template <typename T, typename... AttributeTs>
class ScalarMetricImpl : public Counter<T, AttributeTs...>,
                         public Gauge<T, AttributeTs...>,
                         public UpDownCounter<T, AttributeTs...>,
                         public ObservableCounter<T>,
                         public ObservableGauge<T>,
                         public ObservableUpDownCounter<T> {
public:
    using Attributes = typename Counter<T, AttributeTs...>::Attributes;

    explicit ScalarMetricImpl(const AttributeDefinition<AttributeTs>&... defs);
    explicit ScalarMetricImpl(ReportingPolicy globalReportingPolicy,
                              const AttributeDefinition<AttributeTs>&... defs);
    ~ScalarMetricImpl() override = default;

#ifdef MONGO_CONFIG_OTEL
    void reset(opentelemetry::metrics::Meter* meter) override;
#endif  // MONGO_CONFIG_OTEL

    AttributesAndValues<T> values() const override;
    BSONObj serializeToBson(const std::string& key) const override;

    void setReportingPolicy(const Attributes& attributes, ReportingPolicy reportingPolicy) override;

protected:
    // Satisfies Counter<T, AttributeTs...>::add(). Validates nonnegative.
    void addNonNegative(T value, const Attributes& attributes) override;

    // Satisfies Gauge<T, AttributeTs...>::set(). Stores value (storeRelaxed).
    void set(T value, const Attributes& attributes) override;

    // Satisfies UpDownCounter<T, AttributeTs...>::add(). No validation.
    void add(T value, const Attributes& attributes) override;

private:
    struct MetricData {
        explicit MetricData(ReportingPolicy policy) : reportingPolicy{policy} {}
        Atomic<T> value{0};
        Atomic<ReportingPolicy> reportingPolicy;
        Atomic<bool> everNonZero{false};
    };

    std::array<std::string, sizeof...(AttributeTs)> _attributeNames;
    OwnedAttributeValueLists<AttributeTs...> _ownedValueLists;
    AttributesMap<Attributes, std::unique_ptr<MetricData>> _metrics;
};

///////////////////////////////////////////////////////////////////////////////
// Implementation details
///////////////////////////////////////////////////////////////////////////////

template <typename T, typename... AttributeTs>
ScalarMetricImpl<T, AttributeTs...>::ScalarMetricImpl(
    const AttributeDefinition<AttributeTs>&... defs)
    : ScalarMetricImpl(sizeof...(AttributeTs) == 0 ? ReportingPolicy::kUnconditionally
                                                   : ReportingPolicy::kIfCurrentlyNonZero,
                       defs...) {}

template <typename T, typename... AttributeTs>
ScalarMetricImpl<T, AttributeTs...>::ScalarMetricImpl(
    ReportingPolicy globalReportingPolicy, const AttributeDefinition<AttributeTs>&... defs)
    : _attributeNames{defs.name...}, _ownedValueLists(makeOwnedAttributeValueLists(defs...)) {
    // The Attributes tuples produced by safeMakeAttributeTuples contain view values (StringData,
    // span) that point into _ownedValueLists, so the keys inserted into _metrics remain valid.
    for (Attributes t : safeMakeAttributeTuples(_ownedValueLists))
        _metrics[t] = std::make_unique<MetricData>(globalReportingPolicy);

    massert(ErrorCodes::BadValue,
            "Attribute names are duplicated",
            !containsDuplicates(_attributeNames));
}

template <typename T, typename... AttributeTs>
void ScalarMetricImpl<T, AttributeTs...>::setReportingPolicy(const Attributes& attributes,
                                                             ReportingPolicy reportingPolicy) {
    auto it = _metrics.find(attributes);
    massert(ErrorCodes::BadValue,
            "setReportingPolicy called with undeclared attribute combination",
            it != _metrics.end());
    it->second->reportingPolicy.storeRelaxed(reportingPolicy);
}

template <typename T, typename... AttributeTs>
void ScalarMetricImpl<T, AttributeTs...>::addNonNegative(T value, const Attributes& attributes) {
    massert(ErrorCodes::BadValue, "Counter increment must be nonnegative", value >= 0);
    add(value, attributes);
}

template <typename T, typename... AttributeTs>
void ScalarMetricImpl<T, AttributeTs...>::add(T value, const Attributes& attributes) {
    auto it = _metrics.find(attributes);
    massert(ErrorCodes::BadValue,
            "Called add using undeclared set of attributes",
            it != _metrics.end());
    it->second->value.fetchAndAddRelaxed(value);
    if (value != 0) {
        it->second->everNonZero.storeRelaxed(true);
    }
}

template <typename T, typename... AttributeTs>
void ScalarMetricImpl<T, AttributeTs...>::set(T value, const Attributes& attributes) {
    auto it = _metrics.find(attributes);
    massert(ErrorCodes::BadValue,
            "Called set using undeclared set of attributes",
            it != _metrics.end());
    it->second->value.storeRelaxed(value);
}

template <typename T, typename... AttributeTs>
BSONObj ScalarMetricImpl<T, AttributeTs...>::serializeToBson(const std::string& key) const {
    T total = 0;
    for (const auto& [attributes, data] : _metrics) {
        total += data->value.loadRelaxed();
    }
    return BSON(key << total);
}

#ifdef MONGO_CONFIG_OTEL
template <typename T, typename... AttributeTs>
void ScalarMetricImpl<T, AttributeTs...>::reset(opentelemetry::metrics::Meter* meter) {
    invariant(!meter);
    for (const auto& [attributes, data] : _metrics) {
        data->value.storeRelaxed(0);
        data->everNonZero.storeRelaxed(false);
    }
}
#endif  // MONGO_CONFIG_OTEL

template <typename T, typename... AttributeTs>
AttributesAndValues<T> ScalarMetricImpl<T, AttributeTs...>::values() const {
    AttributesAndValues<T> attributesAndValues;
    for (const auto& [attributes, data] : _metrics) {
        T value = data->value.loadRelaxed();
        if (value == 0) {
            switch (data->reportingPolicy.loadRelaxed()) {
                case ReportingPolicy::kIfCurrentlyNonZero:
                    continue;
                case ReportingPolicy::kUnconditionally:
                    break;
                case ReportingPolicy::kIfEverNonZero:
                    if (!data->everNonZero.loadRelaxed())
                        continue;
                    break;
            }
        }
        std::vector<AttributeNameAndValue> attrList;
        std::apply(
            [this, &attrList](auto&&... attribute) {
                size_t i = 0;
                (attrList.push_back({.name = _attributeNames[i++], .value = attribute}), ...);
            },
            attributes);
        attributesAndValues.push_back(
            {.attributes = AttributesKeyValueIterable(std::move(attrList)), .value = value});
    }
    return attributesAndValues;
}
}  // namespace mongo::otel::metrics
