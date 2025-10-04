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

#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/stdx/mutex.h"

#include <memory>

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
    mutable stdx::mutex _mutex;
    LogicalTime _maxOperationTime;
};

}  // namespace mongo
