/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
#include "mongo/platform/atomic_word.h"

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
    AtomicWord<std::int64_t> _started{0};

    // The total number of retried internal transactions
    AtomicWord<std::int64_t> _retriedTransactions{0};

    // The total number of retried internal transaction commits
    AtomicWord<std::int64_t> _retriedCommits{0};

    // The total number of successfully completed internal transactions
    AtomicWord<std::int64_t> _succeeded{0};
};

}  // namespace mongo
