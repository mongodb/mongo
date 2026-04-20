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

template <typename T, AttributeType... AttributeTs>
class MONGO_MOD_PUBLIC Counter : public Metric {
public:
    using Attributes = std::tuple<AttributeTs...>;

    ~Counter() override = default;

    /** T must be nonnegative. */
    virtual void add(T value, const Attributes& attributes) = 0;

    /**
     * For each combination of attributes for which the counter has been incremented, returns the
     * set of attributes and the counter value associated with this. Note that the result is valid
     * only while this counter is valid.
     */
    virtual AttributesAndValues<T> values() const = 0;
};

/**
 * Specialization when there are no attributes so we don't need to add an empty tuple to add. See
 * the non-specialized version for documentation.
 */
template <typename T>
class MONGO_MOD_PUBLIC Counter<T> : public Metric {
public:
    using Attributes = std::tuple<>;
    ~Counter() override = default;
    virtual void add(T value, const std::tuple<>& attributes) = 0;
    void add(T value) {
        add(value, {});
    }
    virtual AttributesAndValues<T> values() const = 0;
};

/**
 * A lock free (non-decreasing) counter with attribute support.
 */
template <typename T, typename... AttributeTs>
class CounterImpl : public Counter<T, AttributeTs...> {
public:
    using Attributes = Counter<T, AttributeTs...>::Attributes;

    explicit CounterImpl(const AttributeDefinition<AttributeTs>&... defs);

    ~CounterImpl() override = default;
    void add(T value, const Attributes& attributes) override;
    using Counter<T, AttributeTs...>::add;

#ifdef MONGO_CONFIG_OTEL
    void reset(opentelemetry::metrics::Meter* meter) override;
#endif  // MONGO_CONFIG_OTEL
    AttributesAndValues<T> values() const override;

    BSONObj serializeToBson(const std::string& key) const override;

private:
    std::array<std::string, sizeof...(AttributeTs)> _attributeNames;
    OwnedAttributeValueLists<AttributeTs...> _ownedValueLists;
    AttributesMap<Attributes, std::unique_ptr<Atomic<T>>> _counters;
};

///////////////////////////////////////////////////////////////////////////////
// Implementation details
///////////////////////////////////////////////////////////////////////////////

template <typename T, typename... AttributeTs>
CounterImpl<T, AttributeTs...>::CounterImpl(const AttributeDefinition<AttributeTs>&... defs)
    : _attributeNames{defs.name...}, _ownedValueLists(makeOwnedAttributeValueLists(defs...)) {
    // The Attributes tuples produced by safeMakeAttributeTuples contain view values (StringData,
    // span) that point into _ownedValueLists, so the keys inserted into _counters remain valid.
    for (Attributes t : safeMakeAttributeTuples(_ownedValueLists))
        _counters[t] = std::make_unique<Atomic<T>>(0);

    massert(ErrorCodes::BadValue,
            "Attribute names are duplicated",
            !containsDuplicates(_attributeNames));
}

template <typename T, typename... AttributeTs>
void CounterImpl<T, AttributeTs...>::add(T value, const Attributes& attributes) {
    massert(ErrorCodes::BadValue, "Counter increment must be nonnegative", value >= 0);
    auto it = _counters.find(attributes);
    massert(ErrorCodes::BadValue,
            "Called add using undeclared set of attributes",
            it != _counters.end());
    it->second->fetchAndAddRelaxed(value);
}

template <typename T, typename... AttributeTs>
BSONObj CounterImpl<T, AttributeTs...>::serializeToBson(const std::string& key) const {
    T total = 0;
    for (const auto& [attributes, counter] : _counters) {
        total += counter->loadRelaxed();
    }
    return BSON(key << total);
}

#ifdef MONGO_CONFIG_OTEL
template <typename T, typename... AttributeTs>
void CounterImpl<T, AttributeTs...>::reset(opentelemetry::metrics::Meter* meter) {
    invariant(!meter);
    for (const auto& [attrs, counter] : _counters) {
        counter->storeRelaxed(0);
    }
}
#endif  // MONGO_CONFIG_OTEL

template <typename T, typename... AttributeTs>
AttributesAndValues<T> CounterImpl<T, AttributeTs...>::values() const {
    AttributesAndValues<T> attributesAndValues;
    for (const auto& [attributes, counter] : _counters) {
        T value = counter->loadRelaxed();
        // If there is a large number of possible attribute combinations but most aren't typically
        // incremented, including them would massively increase the size of the metrics output
        // without adding any significant value. Always include if there's no attributes.
        // TODO SERVER-124243: Add a way to include zero-valued metrics with attributes.
        if (value == 0 && sizeof...(AttributeTs) > 0) {
            continue;
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
