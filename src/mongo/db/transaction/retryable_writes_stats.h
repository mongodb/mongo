// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/transaction/transactions_stats_gen.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"

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

    void incrementRetriedCommandsCount();

    void incrementRetriedStatementsCount();

    void incrementTransactionsCollectionWriteCount();

    /**
     * Appends the accumulated stats to a transactions stats object to be returned through
     * serverStatus.
     */
    void updateStats(TransactionsStats* stats);

private:
    // The number of received commands that contained a statement that had already been executed.
    Atomic<unsigned long long> _retriedCommandsCount{0};

    // The number of received statements found to have been previously executed.
    Atomic<unsigned long long> _retriedStatementsCount{0};

    // The number of writes to the config.transactions collection. Includes writes initiated by a
    // migration.
    Atomic<unsigned long long> _transactionsCollectionWriteCount{0};
};

}  // namespace mongo
