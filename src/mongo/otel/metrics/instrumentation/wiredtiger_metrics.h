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
 * Point-in-time sample of a curated set of WiredTiger connection statistics
 */
struct WiredTigerStatsSnapshot {
    int64_t evictionCallsToGetPageFoundQueueEmpty{0};
    int64_t evictPageAttemptsByWorkerThreads{0};
    int64_t evictPageFailuresByWorkerThreads{0};
    int64_t pageEvictAttemptsByAppThreads{0};
    int64_t pageEvictFailuresByAppThreads{0};
    int64_t bytesReadIntoCache{0};
    int64_t bytesWrittenFromCache{0};
    int64_t pagesReadIntoCache{0};
    int64_t pagesRequestedFromCache{0};

    int64_t evictionEmptyScore{0};
    int64_t evictionWorkerThreadActive{0};
    int64_t evictionWorkerThreadStableNumber{0};
    int64_t bytesCurrentlyInCache{0};
    int64_t trackedDirtyBytesInCache{0};
    int64_t maximumBytesConfigured{0};
    int64_t connectionDataHandlesCurrentlyActive{0};
    int64_t transactionCheckpointMostRecentTimeMsecs{0};
};

/**
 * Point-in-time sample of a curated set of TicketingSystem connection statistics
 */
struct TicketingSystemStatsSnapshot {
    int64_t readAvailable{0};
    int64_t writeAvailable{0};
};

/**
 * Parse the raw BSON connection statistics from WiredTiger into a snapshot
 */
[[MONGO_MOD_PUBLIC]] WiredTigerStatsSnapshot parseWiredTigerStats(const BSONObj& stats);

/**
 * Parse the raw BSON connection statistics from the TicketingSystem into a snapshot
 */
[[MONGO_MOD_PUBLIC]] TicketingSystemStatsSnapshot parseTicketingSystemStats(const BSONObj& stats);

/**
 * Owns the OpenTelemetry instruments for WiredTiger metrics
 */
class WiredTigerMetrics {
public:
    WiredTigerMetrics();
    ~WiredTigerMetrics();

    /**
     * Update metrics tracking WiredTiger storage stats
     */
    void updateWiredTiger(const WiredTigerStatsSnapshot& snap);

    /**
     * Update metrics tracking ticketing system stats
     */
    void updateTicketingSystem(const TicketingSystemStatsSnapshot& snap);

    /**
     * Update metrics tracking WiredTiger errors
     */
    void recordWTCollectError();
    void recordWTEngineNotReadyError();

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
