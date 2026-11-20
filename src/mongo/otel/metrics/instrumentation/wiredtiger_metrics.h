// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <memory>

namespace mongo {

class BSONObj;

/**
 * Point-in-time sample of a curated set of TicketingSystem connection statistics
 */
struct TicketingSystemStatsSnapshot {
    int64_t readAvailable{0};
    int64_t writeAvailable{0};
};

/**
 * Parse the raw BSON connection statistics from the TicketingSystem into a snapshot
 */
[[MONGO_MOD_PUBLIC]] TicketingSystemStatsSnapshot parseTicketingSystemStats(const BSONObj& stats);

/**
 * Owns the OpenTelemetry instruments for the server-side WiredTiger metrics. Statistics read from
 * WiredTiger itself are published by WiredTigerMetricRegistry.
 */
class WiredTigerMetrics {
public:
    WiredTigerMetrics();
    ~WiredTigerMetrics();

    /**
     * Update metrics tracking ticketing system stats
     */
    void updateTicketingSystem(const TicketingSystemStatsSnapshot& snap);

    /**
     * Update metrics tracking TicketingSystem errors
     */
    void recordTSCollectError();

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};


/**
 * Registers OpenTelemetry WiredTiger instruments and starts a periodic job (1 Hz) that
 * collects their metrics.
 */
[[MONGO_MOD_PUBLIC]] void installWiredTigerOtelMetrics(ServiceContext* svcCtx);


}  // namespace mongo
