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

#include "mongo/otel/metrics/metrics_attributes.h"
#include "mongo/otel/metrics/metrics_metric.h"
#include "mongo/util/modules.h"

namespace mongo::otel::metrics {

/** Gauge interface with typed attributes. set() stores the current value (replaces, not adds). */
template <typename T, AttributeType... AttributeTs>
class MONGO_MOD_PUBLIC Gauge {
public:
    using Attributes = std::tuple<AttributeTs...>;
    virtual ~Gauge() = default;

    virtual void set(T value, const Attributes& attributes) = 0;

    /**
     * Sets the reporting policy for a specific attribute combination, overriding the global
     * reporting policy. Throws BadValue if the combination is not declared.
     */
    virtual void setReportingPolicy(const Attributes& attributes,
                                    ReportingPolicy reportingPolicy) = 0;
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

    virtual void setReportingPolicy(const Attributes& attributes,
                                    ReportingPolicy reportingPolicy) = 0;
};

/**
 * A gauge that only updates when the new value is less than the current value. Useful for tracking
 * the minimum of an observed quantity over time (e.g. the minimum available memory across a set of
 * nodes). Initialized to numeric_limits<T>::max() so the first observation always wins.
 */
template <typename T, AttributeType... AttributeTs>
class MONGO_MOD_PUBLIC MinGauge : public virtual Gauge<T, AttributeTs...> {
public:
    using Attributes = std::tuple<AttributeTs...>;
    virtual void setIfLess(T value, const Attributes& attributes) = 0;
    virtual AttributesAndValues<T> values() const = 0;
};

/** Specialization when there are no attributes, adding a convenience setIfLess(T) overload. */
template <typename T>
class MONGO_MOD_PUBLIC MinGauge<T> : public virtual Gauge<T> {
public:
    using Attributes = std::tuple<>;
    void setIfLess(T value) {
        setIfLess(value, {});
    }
    virtual AttributesAndValues<T> values() const = 0;

protected:
    virtual void setIfLess(T value, const std::tuple<>&) = 0;
};

/**
 * A gauge that only updates when the new value is greater than the current value. Useful for
 * tracking the maximum of an observed quantity over time (e.g. peak memory usage). Initialized to
 * numeric_limits<T>::lowest() so the first observation always wins.
 */
template <typename T, AttributeType... AttributeTs>
class MONGO_MOD_PUBLIC MaxGauge : public virtual Gauge<T, AttributeTs...> {
public:
    using Attributes = std::tuple<AttributeTs...>;
    virtual void setIfGreater(T value, const Attributes& attributes) = 0;
    virtual AttributesAndValues<T> values() const = 0;
};

/** Specialization when there are no attributes, adding a convenience setIfGreater(T) overload. */
template <typename T>
class MONGO_MOD_PUBLIC MaxGauge<T> : public virtual Gauge<T> {
public:
    using Attributes = std::tuple<>;
    void setIfGreater(T value) {
        setIfGreater(value, {});
    }
    virtual AttributesAndValues<T> values() const = 0;

protected:
    virtual void setIfGreater(T value, const std::tuple<>&) = 0;
};

}  // namespace mongo::otel::metrics
