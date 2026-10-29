// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/transaction/transactions_stats_gen.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/duration.h"
#include "mongo/util/histogram.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <vector>

namespace mongo {

/**
 * Container for retryable writes statistics.
 */
class [[MONGO_MOD_PUBLIC]] RetryableWritesStats {
    RetryableWritesStats(const RetryableWritesStats&) = delete;
    RetryableWritesStats& operator=(const RetryableWritesStats&) = delete;

public:
    RetryableWritesStats() = default;

    static RetryableWritesStats* get(ServiceContext* service);
    static RetryableWritesStats* get(OperationContext* opCtx);

    void incrementRetryableCommandsCount();

    void incrementRetriedCommandsCount();

    void incrementRetriedStatementsCount();

    void incrementTransactionsCollectionWriteCount();

    /**
     * Records the elapsed time between the original write and the server's processing of the retry
     * of that write.
     */
    void recordRetriedWriteDelay(Milliseconds delay);

    /**
     * Appends the retry-delay histogram as a `retriedWritesDelayMillis` array of
     * `{ lowerBound, count }` pairs (lowerBound in ms). The first bucket covers 0+ and the last
     * bucket is open-ended.
     */
    void appendRetriedWriteStats(BSONObjBuilder& bob) const;

    /**
     * Appends the accumulated stats to a transactions stats object to be returned through
     * serverStatus.
     */
    void updateStats(TransactionsStats* stats);

private:
    static std::vector<uint64_t> retriedWriteDelayPartitionsMillis();
    // The total number of received retryable commands.
    // Contrast with '_retriedCommandsCount' to derive the proportion of retryable
    // commands that were ultimately retried.
    Atomic<unsigned long long> _retryableCommandsCount{0};

    // The number of received commands that contained a statement that had already been executed.
    Atomic<unsigned long long> _retriedCommandsCount{0};

    // The number of received statements found to have been previously executed.
    Atomic<unsigned long long> _retriedStatementsCount{0};

    // The number of writes to the config.transactions collection. Includes writes initiated by a
    // migration.
    Atomic<unsigned long long> _transactionsCollectionWriteCount{0};

    // Histogram of the retry gap between original write and retry in milliseconds.
    Histogram<uint64_t> _retriedWriteDelayMillis{retriedWriteDelayPartitionsMillis()};
};

}  // namespace mongo
