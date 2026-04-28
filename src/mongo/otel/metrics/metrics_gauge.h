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
#include "mongo/otel/metrics/metrics_attributes.h"
#include "mongo/otel/metrics/metrics_metric.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"

#include <limits>

#ifdef MONGO_CONFIG_OTEL
#include <opentelemetry/metrics/meter.h>
#endif

namespace mongo::otel::metrics {

/** Gauge interface with typed attributes. set() stores the current value (replaces, not adds). */
template <typename T, AttributeType... AttributeTs>
class MONGO_MOD_PUBLIC Gauge {
public:
    using Attributes = std::tuple<AttributeTs...>;
    virtual ~Gauge() = default;

    virtual void set(T value, const Attributes& attributes) = 0;
};

/** Specialization when there are no attributes, adding a convenience set(T) overload. */
template <typename T>
class MONGO_MOD_PUBLIC Gauge<T> {
public:
    using Attributes = std::tuple<>;
    virtual ~Gauge() = default;

    void set(T value) {
        set(value, {});
    }

protected:
    virtual void set(T value, const std::tuple<>& attributes) = 0;
};

/**
 * A gauge that only updates when the new value is less than the current value. Useful for tracking
 * the minimum of an observed quantity over time (e.g. the minimum available memory across a set of
 * nodes). Initialized to numeric_limits<T>::max() so the first observation always wins.
 */
template <typename T>
class MONGO_MOD_PUBLIC MinGauge : public virtual Gauge<T>, public virtual Metric {
public:
    virtual void setIfLess(T value) = 0;
    virtual AttributesAndValues<T> values() const = 0;
};

/**
 * A gauge that only updates when the new value is greater than the current value. Useful for
 * tracking the maximum of an observed quantity over time (e.g. peak memory usage). Initialized to
 * numeric_limits<T>::lowest() so the first observation always wins.
 */
template <typename T>
class MONGO_MOD_PUBLIC MaxGauge : public virtual Gauge<T>, public virtual Metric {
public:
    virtual void setIfGreater(T value) = 0;
    virtual AttributesAndValues<T> values() const = 0;
};

// A single lock-free implementation used for MinGauge and MaxGauge. The initialValue controls
// semantics: numeric_limits<T>::max() for min-tracking gauges, numeric_limits<T>::lowest() for
// max-tracking gauges.
template <typename T>
class GaugeImpl : public MinGauge<T>, public MaxGauge<T> {
public:
    explicit GaugeImpl(T initialValue = T{0}) : _initialValue(initialValue), _value(initialValue) {}

    using Gauge<T>::set;
    void set(T value, const std::tuple<>&) override;

    void setIfLess(T value) override;

    void setIfGreater(T value) override;

    AttributesAndValues<T> values() const override;

    BSONObj serializeToBson(const std::string& key) const override;

#ifdef MONGO_CONFIG_OTEL
    void reset(opentelemetry::metrics::Meter* meter) override;
#endif  // MONGO_CONFIG_OTEL

private:
    const T _initialValue;
    Atomic<T> _value;
};

///////////////////////////////////////////////////////////////////////////////
// Implementation details
///////////////////////////////////////////////////////////////////////////////

template <typename T>
void GaugeImpl<T>::set(T value, const std::tuple<>&) {
    _value.storeRelaxed(value);
}

template <typename T>
void GaugeImpl<T>::setIfLess(T value) {
    T old = _value.load();
    while (value < old && !_value.compareAndSwap(&old, value)) {
    }
}

template <typename T>
void GaugeImpl<T>::setIfGreater(T value) {
    T old = _value.load();
    while (value > old && !_value.compareAndSwap(&old, value)) {
    }
}

template <typename T>
AttributesAndValues<T> GaugeImpl<T>::values() const {
    // TODO SERVER-121408: Return per-attribute values once attribute support is added.
    return {{.attributes = AttributesKeyValueIterable(std::vector<AttributeNameAndValue>{}),
             .value = _value.loadRelaxed()}};
}

template <typename T>
BSONObj GaugeImpl<T>::serializeToBson(const std::string& key) const {
    return BSON(key << _value.loadRelaxed());
}

#ifdef MONGO_CONFIG_OTEL
template <typename T>
void GaugeImpl<T>::reset(opentelemetry::metrics::Meter* meter) {
    invariant(!meter);
    _value.store(_initialValue);
}
#endif  // MONGO_CONFIG_OTEL

}  // namespace mongo::otel::metrics
