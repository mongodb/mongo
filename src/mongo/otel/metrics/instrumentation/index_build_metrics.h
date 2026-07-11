// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <memory>

namespace mongo {

class ServiceContext;
struct IndexBulkBuilderMetricsSnapshot;

/**
 * Owns the OpenTelemetry instruments that track index build metrics.
 */
class IndexBuildOtelMetrics {
public:
    IndexBuildOtelMetrics();
    ~IndexBuildOtelMetrics();

    void update(const IndexBulkBuilderMetricsSnapshot& snapshot);

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

/**
 * Starts a periodic job (1 Hz) that samples the snapshots and updates OpenTelemetry metrics for
 * index build operations. Intended to be called once at startup from mongod_main.
 */
[[MONGO_MOD_PUBLIC]] void installIndexBuildOtelMetrics(ServiceContext* svcCtx);

}  // namespace mongo
