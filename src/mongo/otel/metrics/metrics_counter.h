// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/otel/metrics/metrics_attributes.h"
#include "mongo/otel/metrics/metrics_metric.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>

namespace mongo::otel::metrics {

/**
 * Non-decreasing counter interface with typed attributes. add() enforces nonnegative values via
 * a non-virtual wrapper that calls the virtual addNonNegative().
 */
template <typename T, AttributeType... AttributeTs>
class [[MONGO_MOD_PUBLIC]] Counter {
public:
    using Attributes = std::tuple<AttributeTs...>;
    virtual ~Counter() = default;

    /** T must be nonnegative. */
    void add(T value, const Attributes& attributes) {
        addNonNegative(value, attributes);
    }

    /**
     * Sets the reporting policy for a specific attribute combination, overriding the global
     * reporting policy. Throws BadValue if the combination is not declared.
     */
    virtual void setReportingPolicy(const Attributes& attributes,
                                    ReportingPolicy reportingPolicy) = 0;

    /** Observation-side accessor for valueForLegacyUse. Only valid for no-attribute counters. */
    virtual T valueForLegacyUse() const {
        MONGO_UNIMPLEMENTED_TASSERT(12393200);
    }

protected:
    virtual void addNonNegative(T value, const Attributes& attributes) = 0;
};

/** Specialization when there are no attributes, adding a convenience add(T) overload. */
template <typename T>
class [[MONGO_MOD_PUBLIC]] Counter<T> {
public:
    using Attributes = std::tuple<>;
    virtual ~Counter() = default;

    void add(T value) {
        addNonNegative(value, {});
    }

    virtual void setReportingPolicy(const Attributes& attributes,
                                    ReportingPolicy reportingPolicy) = 0;

    /**
     * Returns the current counter value directly. This exists only to support legacy code paths
     * (e.g., opcounters) that read counter values inline. OTel metrics are intended to be observed
     * externally — do not use this for new metrics. This API may also get slower over time as we
     * optimize for write throughput. In tests, prefer OtelMetricsCapturer and its
     * readInt64Counter()/readDoubleCounter() helpers from metrics_test_util.h instead.
     */
    virtual T valueForLegacyUse() const = 0;

protected:
    virtual void addNonNegative(T value, const std::tuple<>& attributes) = 0;
};

}  // namespace mongo::otel::metrics
