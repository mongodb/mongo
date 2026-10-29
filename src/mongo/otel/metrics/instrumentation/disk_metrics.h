// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>
#include <vector>

namespace mongo {

class ServiceContext;

/**
 * Owns the OpenTelemetry instruments for disk I/O metrics. The set of devices is fixed at
 * construction time: disks that appear after startup are not tracked.
 */
class DiskMetrics {
public:
    /**
     * Registers per-device counters for each of the provided disk devices.
     */
    explicit DiskMetrics(std::vector<std::string> disks);
    ~DiskMetrics();

    /**
     * Walks the BSON and adds deltas to the registered counters. Devices not declared at
     * construction time are ignored.
     */
    void update(BSONObj disksBson);

private:
    class Impl;
    static otel::metrics::DynamicMetricNameMaker::Passkey dyn_metric_passkey() {
        return {};
    }
    std::unique_ptr<Impl> _impl;
};

/**
 * Registers OpenTelemetry disk I/O counters and starts a periodic job that samples
 * once per second. No-op on unsupported platforms.
 */
[[MONGO_MOD_PUBLIC]] void installDiskOtelMetrics(ServiceContext* svcCtx);

}  // namespace mongo
