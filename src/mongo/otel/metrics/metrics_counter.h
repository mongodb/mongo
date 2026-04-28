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
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

namespace mongo::otel::metrics {

/**
 * Non-decreasing counter interface with typed attributes. add() enforces nonnegative values via
 * a non-virtual wrapper that calls the virtual addNonNegative().
 */
template <typename T, AttributeType... AttributeTs>
class MONGO_MOD_PUBLIC Counter {
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

protected:
    virtual void addNonNegative(T value, const Attributes& attributes) = 0;
};

/** Specialization when there are no attributes, adding a convenience add(T) overload. */
template <typename T>
class MONGO_MOD_PUBLIC Counter<T> {
public:
    using Attributes = std::tuple<>;
    virtual ~Counter() = default;

    void add(T value) {
        addNonNegative(value, {});
    }

    virtual void setReportingPolicy(const Attributes& attributes,
                                    ReportingPolicy reportingPolicy) = 0;

protected:
    virtual void addNonNegative(T value, const std::tuple<>& attributes) = 0;
};

}  // namespace mongo::otel::metrics
