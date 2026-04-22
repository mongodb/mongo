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
#include "mongo/otel/metrics/metrics_metric.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>

#ifdef MONGO_CONFIG_OTEL
#include <opentelemetry/metrics/meter.h>
#endif

namespace mongo::otel::metrics {

/** A counter with typed attributes. */
template <typename T, AttributeType... AttributeTs>
class MONGO_MOD_PUBLIC Counter {
public:
    using Attributes = std::tuple<AttributeTs...>;

    virtual ~Counter() = default;

    /** T must be nonnegative. */
    virtual void add(T value, const Attributes& attributes) = 0;

    /**
     * Sets the reporting policy for a specific attribute combination, overriding the global
     * reporting policy. Throws BadValue if the combination is not declared.
     */
    virtual void setReportingPolicy(const Attributes& attributes,
                                    ReportingPolicy reportingPolicy) = 0;
};

/**
 * Specialization when there are no attributes so we don't need to add an empty tuple to add. See
 * the non-specialized version for documentation.
 */
template <typename T>
class MONGO_MOD_PUBLIC Counter<T> {
public:
    using Attributes = std::tuple<>;
    virtual ~Counter() = default;
    virtual void add(T value, const std::tuple<>& attributes) = 0;
    void add(T value) {
        add(value, {});
    }
    virtual void setReportingPolicy(const Attributes& attributes,
                                    ReportingPolicy reportingPolicy) = 0;
};

/**
 * Base interface for counter observation. Provides the read/observe side of a counter (value
 * reporting and resetting) without exposing any particular add() signature. MetricsService uses
 * this as its storage type so it doesn't need to template on the attribute types.
 */
template <typename T>
class ObservableCounter : public Metric {
public:
    ~ObservableCounter() override = default;

    /**
     * For each combination of attributes for which the counter has been incremented, returns the
     * set of attributes and the counter value associated with this. Note that the result is valid
     * only while this counter is valid.
     */
    virtual AttributesAndValues<T> values() const = 0;
};

/**
 * A lock free (non-decreasing) counter with attribute support.
 */
template <typename T, typename... AttributeTs>
class CounterImpl : public Counter<T, AttributeTs...>, public ObservableCounter<T> {
public:
    using Attributes = Counter<T, AttributeTs...>::Attributes;

    explicit CounterImpl(const AttributeDefinition<AttributeTs>&... defs);
    explicit CounterImpl(ReportingPolicy globalReportingPolicy,
                         const AttributeDefinition<AttributeTs>&... defs);

    ~CounterImpl() override = default;
    void add(T value, const Attributes& attributes) override;
    using Counter<T, AttributeTs...>::add;

    void setReportingPolicy(const Attributes& attributes, ReportingPolicy reportingPolicy) override;

#ifdef MONGO_CONFIG_OTEL
    void reset(opentelemetry::metrics::Meter* meter) override;
#endif  // MONGO_CONFIG_OTEL
    AttributesAndValues<T> values() const override;

    BSONObj serializeToBson(const std::string& key) const override;

private:
    struct CounterData {
        explicit CounterData(ReportingPolicy policy) : reportingPolicy{policy} {}
        Atomic<T> value{0};
        Atomic<ReportingPolicy> reportingPolicy;
        Atomic<bool> everNonZero{false};
    };

    std::array<std::string, sizeof...(AttributeTs)> _attributeNames;
    OwnedAttributeValueLists<AttributeTs...> _ownedValueLists;
    AttributesMap<Attributes, std::unique_ptr<CounterData>> _counters;
};

///////////////////////////////////////////////////////////////////////////////
// Implementation details
///////////////////////////////////////////////////////////////////////////////

template <typename T, typename... AttributeTs>
CounterImpl<T, AttributeTs...>::CounterImpl(const AttributeDefinition<AttributeTs>&... defs)
    : CounterImpl(sizeof...(AttributeTs) == 0 ? ReportingPolicy::kUnconditionally
                                              : ReportingPolicy::kIfCurrentlyNonZero,
                  defs...) {}

template <typename T, typename... AttributeTs>
CounterImpl<T, AttributeTs...>::CounterImpl(ReportingPolicy globalReportingPolicy,
                                            const AttributeDefinition<AttributeTs>&... defs)
    : _attributeNames{defs.name...}, _ownedValueLists(makeOwnedAttributeValueLists(defs...)) {
    // The Attributes tuples produced by safeMakeAttributeTuples contain view values (StringData,
    // span) that point into _ownedValueLists, so the keys inserted into _counters remain valid.
    for (Attributes t : safeMakeAttributeTuples(_ownedValueLists)) {
        _counters[t] = std::make_unique<CounterData>(globalReportingPolicy);
    }

    massert(ErrorCodes::BadValue,
            "Attribute names are duplicated",
            !containsDuplicates(_attributeNames));
}

template <typename T, typename... AttributeTs>
void CounterImpl<T, AttributeTs...>::setReportingPolicy(const Attributes& attributes,
                                                        ReportingPolicy reportingPolicy) {
    auto it = _counters.find(attributes);
    massert(ErrorCodes::BadValue,
            "setReportingPolicy called with undeclared attribute combination",
            it != _counters.end());
    it->second->reportingPolicy.storeRelaxed(reportingPolicy);
}

template <typename T, typename... AttributeTs>
void CounterImpl<T, AttributeTs...>::add(T value, const Attributes& attributes) {
    massert(ErrorCodes::BadValue, "Counter increment must be nonnegative", value >= 0);
    auto it = _counters.find(attributes);
    massert(ErrorCodes::BadValue,
            "Called add using undeclared set of attributes",
            it != _counters.end());
    it->second->value.fetchAndAddRelaxed(value);
    if (value > 0) {
        it->second->everNonZero.storeRelaxed(true);
    }
}

template <typename T, typename... AttributeTs>
BSONObj CounterImpl<T, AttributeTs...>::serializeToBson(const std::string& key) const {
    T total = 0;
    for (const auto& [attributes, data] : _counters) {
        total += data->value.loadRelaxed();
    }
    return BSON(key << total);
}

#ifdef MONGO_CONFIG_OTEL
template <typename T, typename... AttributeTs>
void CounterImpl<T, AttributeTs...>::reset(opentelemetry::metrics::Meter* meter) {
    invariant(!meter);
    for (const auto& [attrs, data] : _counters) {
        data->value.storeRelaxed(0);
        data->everNonZero.storeRelaxed(false);
    }
}
#endif  // MONGO_CONFIG_OTEL

template <typename T, typename... AttributeTs>
AttributesAndValues<T> CounterImpl<T, AttributeTs...>::values() const {
    AttributesAndValues<T> attributesAndValues;
    for (const auto& [attributes, data] : _counters) {
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
