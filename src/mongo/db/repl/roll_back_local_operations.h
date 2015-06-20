/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/oplog_interface.h"
#include "mongo/stdx/functional.h"

namespace mongo {
namespace repl {

class RollBackLocalOperations {
    MONGO_DISALLOW_COPYING(RollBackLocalOperations);

public:
    /**
     * Type of function to roll back an operation or process it for future use.
     * It can return any status except ErrorCodes::NoSuchKey. See onRemoteOperation().
     */
    using RollbackOperationFn = stdx::function<Status(const BSONObj&)>;

    using RollbackCommonPoint = std::pair<Timestamp, RecordId>;

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
    StatusWith<RollbackCommonPoint> onRemoteOperation(const BSONObj& operation);

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
    const RollBackLocalOperations::RollbackOperationFn& rollbackOperation);

}  // namespace repl
}  // namespace mongo
