// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

#include <memory>
#include <mutex>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * OperationTimeTracker holds the latest operationTime received from a mongod for the current
 * operation. Mongos commands are processed via ASIO, meaning a random thread will handle the
 * response, so this class is declared as a decoration on OperationContext.
 */
class OperationTimeTracker {
public:
    // Decorate OperationContext with OperationTimeTracker instance.
    static std::shared_ptr<OperationTimeTracker> get(OperationContext* ctx);

    /*
     * Return the latest operationTime.
     */
    LogicalTime getMaxOperationTime() const;

    /*
     * Update _maxOperationTime to max(newTime, _maxOperationTime)
     */
    void updateOperationTime(LogicalTime newTime);

private:
    // protects _maxOperationTime
    mutable std::mutex _mutex;
    LogicalTime _maxOperationTime;
};

}  // namespace mongo
