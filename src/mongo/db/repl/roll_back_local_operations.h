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

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/oplog_interface.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/storage/remove_saver.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <functional>
#include <memory>

namespace mongo {
namespace repl {

// This is needed by rollback_impl.
extern FailPoint rollbackHangAfterTransitionToRollback;

class RollBackLocalOperations {
    RollBackLocalOperations(const RollBackLocalOperations&) = delete;
    RollBackLocalOperations& operator=(const RollBackLocalOperations&) = delete;

public:
    class RollbackCommonPoint {

    public:
        RollbackCommonPoint(BSONObj oplogBSON, RecordId recordId, BSONObj nextOplogBSON);

        RecordId getRecordId() const {
            return _recordId;
        }

        OpTime getOpTime() const {
            return _opTime;
        }

        Date_t getWallClockTime() const {
            return _wallClockTime;
        }

        Date_t getFirstOpWallClockTimeAfterCommonPoint() {
            return _firstWallClockTimeAfterCommonPoint;
        }

    private:
        RecordId _recordId;
        OpTime _opTime;
        Date_t _wallClockTime;
        // The wall clock time of the first operation after the common point if it exists.
        Date_t _firstWallClockTimeAfterCommonPoint;
    };

    /**
     * Type of function to roll back an operation or process it for future use.
     * It can return any status except ErrorCodes::NoSuchKey. See onRemoteOperation().
     */
    using RollbackOperationFn = std::function<Status(const BSONObj&)>;

    /**
     * Initializes rollback processor with a valid local oplog.
     * Whenever we encounter an operation in the local oplog that has to be rolled back,
     * we will pass it to 'rollbackOperation'.
     */
    RollBackLocalOperations(const OplogInterface& localOplog,
                            const RollbackOperationFn& rollbackOperation);

    virtual ~RollBackLocalOperations() = default;

    /**
     * Process single remote operation.
     * Returns ErrorCodes::NoSuchKey if common point has not been found and
     * additional operations have to be read from the remote oplog.
     */
    StatusWith<RollbackCommonPoint> onRemoteOperation(const BSONObj& operation,
                                                      RemoveSaver& removeSaver,
                                                      bool shouldCreateDataFiles);

private:
    std::unique_ptr<OplogInterface::Iterator> _localOplogIterator;
    RollbackOperationFn _rollbackOperation;
    OplogInterface::Iterator::Value _localOplogValue;
    unsigned long long _scanned;
};

/**
 * Rolls back every operation in the local oplog that is not in the remote oplog, in reverse
 * order.
 *
 * Whenever we encounter an operation in the local oplog that has to be rolled back,
 * we will pass it to 'rollbackOperation' starting with the most recent operation.
 * It is up to 'rollbackOperation' to roll back this operation immediately or
 * process it for future use.
 */
StatusWith<RollBackLocalOperations::RollbackCommonPoint> syncRollBackLocalOperations(
    const OplogInterface& localOplog,
    const OplogInterface& remoteOplog,
    const RollBackLocalOperations::RollbackOperationFn& rollbackOperation,
    bool shouldCreateDataFiles);

}  // namespace repl
}  // namespace mongo
