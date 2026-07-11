// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/otel/metrics/metrics_attributes.h"
#include "mongo/otel/metrics/metrics_metric.h"
#include "mongo/util/modules.h"

namespace mongo::otel::metrics {

/** Gauge interface with typed attributes. set() stores the current value (replaces, not adds). */
template <typename T, AttributeType... AttributeTs>
class [[MONGO_MOD_PUBLIC]] Gauge {
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
class [[MONGO_MOD_PUBLIC]] Gauge<T> {
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
class [[MONGO_MOD_PUBLIC]] MinGauge : public virtual Gauge<T, AttributeTs...> {
public:
    using Attributes = std::tuple<AttributeTs...>;
    virtual void setIfLess(T value, const Attributes& attributes) = 0;
    virtual AttributesAndValues<T> values() const = 0;
};

/** Specialization when there are no attributes, adding a convenience setIfLess(T) overload. */
template <typename T>
class [[MONGO_MOD_PUBLIC]] MinGauge<T> : public virtual Gauge<T> {
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
class [[MONGO_MOD_PUBLIC]] MaxGauge : public virtual Gauge<T, AttributeTs...> {
public:
    using Attributes = std::tuple<AttributeTs...>;
    virtual void setIfGreater(T value, const Attributes& attributes) = 0;
    virtual AttributesAndValues<T> values() const = 0;
};

/** Specialization when there are no attributes, adding a convenience setIfGreater(T) overload. */
template <typename T>
class [[MONGO_MOD_PUBLIC]] MaxGauge<T> : public virtual Gauge<T> {
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
