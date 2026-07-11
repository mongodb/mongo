// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
