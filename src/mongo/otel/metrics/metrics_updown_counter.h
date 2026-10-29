// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/otel/metrics/metrics_attributes.h"
#include "mongo/otel/metrics/metrics_metric.h"
#include "mongo/util/modules.h"

namespace mongo::otel::metrics {

/** UpDownCounter interface with typed attributes. add() accepts any delta, including negative. */
template <typename T, AttributeType... AttributeTs>
class [[MONGO_MOD_PUBLIC]] UpDownCounter {
public:
    using Attributes = std::tuple<AttributeTs...>;
    virtual ~UpDownCounter() = default;

    virtual void add(T value, const Attributes& attributes) = 0;

    /**
     * Sets the reporting policy for a specific attribute combination, overriding the global
     * reporting policy. Throws BadValue if the combination is not declared.
     */
    virtual void setReportingPolicy(const Attributes& attributes,
                                    ReportingPolicy reportingPolicy) = 0;
};

/** Specialization when there are no attributes, adding a convenience add(T) overload. */
template <typename T>
class [[MONGO_MOD_PUBLIC]] UpDownCounter<T> {
public:
    using Attributes = std::tuple<>;
    virtual ~UpDownCounter() = default;

    void add(T value) {
        add(value, {});
    }

protected:
    virtual void add(T value, const std::tuple<>& attributes) = 0;

    virtual void setReportingPolicy(const Attributes& attributes,
                                    ReportingPolicy reportingPolicy) = 0;
};

}  // namespace mongo::otel::metrics
