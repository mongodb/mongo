/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
