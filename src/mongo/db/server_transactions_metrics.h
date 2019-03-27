
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/transactions_stats_gen.h"

namespace mongo {

/**
 * Container for server-wide multi-document transaction statistics.
 */
class ServerTransactionsMetrics {
    MONGO_DISALLOW_COPYING(ServerTransactionsMetrics);

public:
    ServerTransactionsMetrics() = default;

    static ServerTransactionsMetrics* get(ServiceContext* service);
    static ServerTransactionsMetrics* get(OperationContext* opCtx);

    unsigned long long getCurrentActive() const;
    void decrementCurrentActive();
    void incrementCurrentActive();

    unsigned long long getCurrentInactive() const;
    void decrementCurrentInactive();
    void incrementCurrentInactive();

    unsigned long long getCurrentOpen() const;
    void decrementCurrentOpen();
    void incrementCurrentOpen();

    unsigned long long getTotalStarted() const;
    void incrementTotalStarted();

    unsigned long long getTotalAborted() const;
    void incrementTotalAborted();

    unsigned long long getTotalCommitted() const;
    void incrementTotalCommitted();

    void updateLastTransaction(size_t operationCount,
                               size_t oplogOperationBytes,
                               BSONObj writeConcern);

    /**
     * Appends the accumulated stats to a transactions stats object.
     */
    void updateStats(TransactionsStats* stats);

private:
    // The number of multi-document transactions currently active.
    AtomicUInt64 _currentActive{0};

    // The number of multi-document transactions currently inactive.
    AtomicUInt64 _currentInactive{0};

    // The total number of open transactions.
    AtomicUInt64 _currentOpen{0};

    // The total number of multi-document transactions started since the last server startup.
    AtomicUInt64 _totalStarted{0};

    // The total number of multi-document transaction aborts.
    AtomicUInt64 _totalAborted{0};

    // The total number of multi-document transaction commits.
    AtomicUInt64 _totalCommitted{0};

    // Protects member variables below.
    mutable stdx::mutex _mutex;

    boost::optional<LastCommittedTransaction> _lastCommittedTransaction;
};

}  // namespace mongo
