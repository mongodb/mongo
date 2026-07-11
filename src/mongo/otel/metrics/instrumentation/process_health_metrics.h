// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <memory>

namespace mongo {

struct ProcessHealthSnapshot {
    int64_t userMs{0};
    int64_t systemMs{0};
    int64_t waitMs{0};
    int64_t voluntaryContextSwitches{0};
    int64_t involuntaryContextSwitches{0};
    int64_t threadCount{0};
    int64_t majorPagingFaults{0};
    int64_t minorPagingFaults{0};
};

/**
 * Owns the OpenTelemetry instruments for process health metrics
 */
class ProcessHealthMetrics {
public:
    ProcessHealthMetrics();
    ~ProcessHealthMetrics();

    void update(const ProcessHealthSnapshot& snap);
    void recordCollectError();

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

/**
 * Registers OpenTelemetry process health instruments and starts a periodic job (1 Hz) that
 * collects their metrics.
 * No-op on unsupported platforms.
 */
[[MONGO_MOD_PUBLIC]] void installProcessHealthOtelMetrics(ServiceContext* svcCtx);

}  // namespace mongo
