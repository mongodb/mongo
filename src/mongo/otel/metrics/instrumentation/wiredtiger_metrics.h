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
