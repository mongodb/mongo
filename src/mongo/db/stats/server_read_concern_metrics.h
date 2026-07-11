// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/read_concern_stats_gen.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Container for server-wide statistics on readConcern levels used by operations.
 */
class [[MONGO_MOD_PUBLIC]] ServerReadConcernMetrics {
    ServerReadConcernMetrics(const ServerReadConcernMetrics&) = delete;
    ServerReadConcernMetrics& operator=(const ServerReadConcernMetrics&) = delete;

public:
    ServerReadConcernMetrics() = default;

    static ServerReadConcernMetrics* get(ServiceContext* service);
    static ServerReadConcernMetrics* get(OperationContext* opCtx);

    /**
     * Updates counter for the level of 'readConcernArgs'.
     */
    void recordReadConcern(const repl::ReadConcernArgs& readConcernArgs, bool isTransaction);

    /**
     * Appends the accumulated stats to a readConcern stats object.
     */
    void updateStats(ReadConcernStats* stats, OperationContext* opCtx);

private:
    struct ReadConcernLevelCounters {
        Atomic<unsigned long long> levelAvailableCount{0};
        Atomic<unsigned long long> levelLinearizableCount{0};
        Atomic<unsigned long long> levelLocalCount{0};
        Atomic<unsigned long long> levelMajorityCount{0};
        Atomic<unsigned long long> levelSnapshotCount{0};
        Atomic<unsigned long long> atClusterTimeCount{0};
        /**
         * Updates RC level counters for the level of 'readConcernArgs'.
         */
        void recordReadConcern(const repl::ReadConcernArgs& readConcernArgs, bool isTransaction);

        /**
         * Appends the accumulated RC level counters to a readConcernOps stats object.
         */
        void updateStats(ReadConcernOps* stats, bool isTransaction);
        void updateStats(CWRCReadConcernOps* stats, bool isTransaction);
        void updateStats(ImplicitDefaultReadConcernOps* stats, bool isTransaction);
    };

    struct ReadConcernCounters {
        Atomic<unsigned long long> noLevelCount{0};
        ReadConcernLevelCounters explicitLevelCount;
        ReadConcernLevelCounters cWRCLevelCount;
        ReadConcernLevelCounters implicitDefaultLevelCount;

        /**
         * Updates RC counters for the level of 'readConcernArgs'.
         */
        void recordReadConcern(const repl::ReadConcernArgs& readConcernArgs, bool isTransaction);

        /**
         * Appends the accumulated RC counters to a readConcern stats object.
         */
        void updateStats(ReadConcernOps* stats, bool isTransaction);
    };
    ReadConcernCounters _nonTransactionOps;
    ReadConcernCounters _transactionOps;
};

}  // namespace mongo
