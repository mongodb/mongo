// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"

#include <cstdint>

namespace mongo {

/**
 * Container for server-wide internal transaction statistics.
 */
class InternalTransactionMetrics {
    InternalTransactionMetrics(const InternalTransactionMetrics&) = delete;
    InternalTransactionMetrics& operator=(const InternalTransactionMetrics&) = delete;

public:
    InternalTransactionMetrics() = default;

    static InternalTransactionMetrics* get(ServiceContext* service);
    static InternalTransactionMetrics* get(OperationContext* opCtx);

    std::int64_t getStarted() const {
        return _started.loadRelaxed();
    }
    void incrementStarted() {
        _started.fetchAndAddRelaxed(1);
    }

    std::int64_t getRetriedTransactions() const {
        return _retriedTransactions.loadRelaxed();
    }
    void incrementRetriedTransactions() {
        _retriedTransactions.fetchAndAddRelaxed(1);
    }

    std::int64_t getRetriedCommits() const {
        return _retriedCommits.loadRelaxed();
    }
    void incrementRetriedCommits() {
        _retriedCommits.fetchAndAddRelaxed(1);
    }

    std::int64_t getSucceeded() const {
        return _succeeded.loadRelaxed();
    }
    void incrementSucceeded() {
        _succeeded.fetchAndAddRelaxed(1);
    }

private:
    // The total number of initiated internal transactions
    Atomic<std::int64_t> _started{0};

    // The total number of retried internal transactions
    Atomic<std::int64_t> _retriedTransactions{0};

    // The total number of retried internal transaction commits
    Atomic<std::int64_t> _retriedCommits{0};

    // The total number of successfully completed internal transactions
    Atomic<std::int64_t> _succeeded{0};
};

}  // namespace mongo
