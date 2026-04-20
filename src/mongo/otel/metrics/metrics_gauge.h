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
#include "mongo/util/modules.h"

#ifdef MONGO_CONFIG_OTEL
#include <opentelemetry/metrics/meter.h>
#endif

namespace mongo::otel::metrics {

template <typename T>
class MONGO_MOD_PUBLIC Gauge : public Metric {
public:
    virtual void set(T value) = 0;

    /**
     * For each combination of attributes for which the gauge has been set, returns the set of
     * attributes and the gauge value associated with this. Note that the result is valid only while
     * this gauge is valid.
     * TODO SERVER-124075: Add attribute support.
     */
    virtual AttributesAndValues<T> values() const = 0;

    // TODO SERVER-124167 Remove this.
    virtual T value() const = 0;
};

// A lock free Gauge and metadata about it.
template <typename T>
class GaugeImpl : public Gauge<T> {
public:
    void set(T value) override;

    AttributesAndValues<T> values() const override;

    T value() const override {
        return _value.load();
    }

    BSONObj serializeToBson(const std::string& key) const override;

#ifdef MONGO_CONFIG_OTEL
    void reset(opentelemetry::metrics::Meter* meter) override;
#endif  // MONGO_CONFIG_OTEL

private:
    Atomic<T> _value;
};

///////////////////////////////////////////////////////////////////////////////
// Implementation details
///////////////////////////////////////////////////////////////////////////////

template <typename T>
void GaugeImpl<T>::set(T value) {
    _value.storeRelaxed(value);
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
    _value.store(0);
}
#endif  // MONGO_CONFIG_OTEL
}  // namespace mongo::otel::metrics
