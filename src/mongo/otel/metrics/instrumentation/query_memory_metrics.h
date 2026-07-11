// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <cstdint>
#include <memory>

namespace mongo {

class ServiceContext;

/**
 * Owns the OpenTelemetry instrument that reports the current configured value of the
 * internalQueryMaxMemoryUsageBytesPerOperation server parameter. The gauge is registered at
 * construction; each call to `update` publishes the latest configured value. The same gauge is
 * exposed in serverStatus at `metrics.query.configuredMaxMemoryUsageBytesPerOperation`.
 */
class QueryMemoryMetrics {
public:
    QueryMemoryMetrics();
    ~QueryMemoryMetrics();

    /**
     * Sets the gauge to the given value, which is expected to be the current value of the
     * internalQueryMaxMemoryUsageBytesPerOperation server parameter.
     */
    void update(int64_t configuredMaxMemoryUsageBytesPerOperation);

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

/**
 * Registers the OpenTelemetry gauge for the configured per-operation memory limit and starts a
 * periodic job (1 Hz) that samples the internalQueryMaxMemoryUsageBytesPerOperation server
 * parameter and publishes the latest value. Sampling periodically keeps the gauge in sync with
 * runtime setParameter changes without coupling the low-level query knob library to the metrics
 * service. Intended to be called once at startup.
 */
[[MONGO_MOD_PUBLIC]] void installQueryMemoryOtelMetrics(ServiceContext* svcCtx);

}  // namespace mongo
