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
 * Owns the OpenTelemetry instruments for filesystem mount metrics. The set of mountpoints is
 * fixed at construction time and mounts that appear after startup are not tracked.
 */
class SystemMountMetrics {
public:
    /**
     * Registers per-mountpoint gauges for each of the provided mountpoints.
     */
    explicit SystemMountMetrics(std::vector<std::string> mountpoints);
    ~SystemMountMetrics();

    /**
     * Walks the BSON and pushes the values to the registered gauges. Mountpoints not declared at
     * construction time are ignored.
     */
    void update(const BSONObj& mountsBson);

private:
    class Impl;
    static otel::metrics::DynamicMetricNameMaker::Passkey dyn_metric_passkey() {
        return {};
    }
    std::unique_ptr<Impl> _impl;
};

/**
 * Registers OpenTelemetry mount filesystem gauges and starts a periodic job that samples
 * OS-level mount stats once per second. No-op on unsupported platforms.
 */
[[MONGO_MOD_PUBLIC]] void installSystemMountOtelMetrics(ServiceContext* svcCtx);

}  // namespace mongo
