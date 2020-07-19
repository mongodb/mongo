/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <boost/optional.hpp>

#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/executor/task_executor.h"

namespace mongo {

/**
 * The MigratingTenantAccessBlocker is used to block and eventually reject reads and writes to a
 * database while the Atlas Serverless tenant that owns the database is being migrated from this
 * replica set to another replica set.
 *
 * In order to preserve causal consistency across the migration, this replica set, the "donor",
 * blocks writes and reads as of a particular "blockTimestamp". The donor then advances the
 * recipient's clusterTime to "blockTimestamp" before committing the migration.
 *
 * Client writes are run inside a new loop, similar to writeConflictRetry:
 *
 * template <typename F>
 * auto migrationConflictRetry(OperationContext* opCtx, const Database* db, F&& f) {
 *     while (true) {
 *         try {
 *             return f();
 *         } catch (const MigrationConflictException&) {
 *             MigratingTenantAccessBlocker::get(db).checkIfCanWriteOrBlock(opCtx);
 *         }
 *     }
 * }
 *
 * Writes call checkIfCanWriteOrThrow after being assigned an OpTime but before committing. The
 * method throws MigratingTenantConflict if writes are being blocked, which is caught in the loop.
 * The write then blocks until the migration either commits (in which case checkIfCanWriteOrBlock
 * throws an error that causes the write to be rejected) or aborts (in which case
 * checkIfCanWriteOrBlock returns successfully and the write is retried in the loop). This loop is
 * used because writes must not block after being assigned an OpTime but before committing.
 *
 * Reads with afterClusterTime or atClusterTime call checkIfCanReadOrBlock at some point after
 * waiting for readConcern, that is, after waiting to reach their clusterTime, which includes
 * waiting for all earlier oplog holes to be filled.
 *
 * Given this, the donor uses this class's API in the following way:
 *
 * 1. The donor primary creates a WriteUnitOfWork to do a write, call it the "start blocking" write.
 * The donor primary calls startBlockingWrites before the write is assigned an OpTime. This write's
 * Timestamp will be the "blockTimestamp".
 *
 * At this point:
 * - Writes that have already passed checkIfCanWriteOrThrow must have been assigned an OpTime before
 *   the blockTimestamp, since the blockTimestamp hasn't been assigned yet, and OpTimes are handed
 *   out in monotonically increasing order.
 * - Writes that have not yet passed checkIfCanWriteOrThrow will end up blocking. Some of these
 *   writes may have already been assigned an OpTime, or may end up being assigned an OpTime that is
 *   before the blockTimestamp, and so will end up blocking unnecessarily, but not incorrectly.
 *
 * 2. In the op observer after the "start blocking" write's OpTime is set, primaries and secondaries
 * of the donor replica set call startBlockingReadsAfter with the write's Timestamp as
 * "blockTimestamp".
 *
 * At this point:
 * - Reads on the node that have already passed checkIfCanReadOrBlock must have a clusterTime before
 *   the blockTimestamp, since the write at blockTimestamp hasn't committed yet (i.e., there's still
 *   an oplog hole at blockTimestamp).
 * - Reads on the node that have not yet passed checkIfCanReadOrBlock will end up blocking.
 *
 * If the "start blocking" write aborts or the write rolls back via replication rollback, the node
 * calls rollBackStartBlocking.
 *
 * 4a. The donor primary commits the migration by doing another write, call it the "commit" write.
 * The op observer for the "commit" write on primaries and secondaries calls commit, which
 * asynchronously waits for the "commit" write's OpTime to become majority committed, then
 * transitions the class to reject writes and reads.
 *
 * 4b. The donor primary can instead abort the migration by doing a write, call it the "abort"
 * write. The op observer for the "abort" write on primaries and secondaries calls abort, which
 * asynchronously waits for the "abort" write's OpTime to become majority committed, then
 * transitions the class back to allowing reads and writes.
 *
 * If the "commit" or "abort" write aborts or rolls back via replication rollback, the node calls
 * rollBackCommitOrAbort, which cancels the asynchronous task.
 */
class MigratingTenantAccessBlocker {
public:
    MigratingTenantAccessBlocker(ServiceContext* serviceContext, executor::TaskExecutor* executor);

    //
    // Called by all writes and reads against the database.
    //

    void checkIfCanWriteOrThrow();
    void checkIfCanWriteOrBlock(OperationContext* opCtx);

    void checkIfLinearizableReadWasAllowedOrThrow(OperationContext* opCtx);
    void checkIfCanDoClusterTimeReadOrBlock(OperationContext* opCtx,
                                            const Timestamp& readTimestamp);

    //
    // Called while donating this database.
    //

    void startBlockingWrites();
    void startBlockingReadsAfter(const Timestamp& timestamp);
    void rollBackStartBlocking();

    void commit(repl::OpTime opTime);
    void abort(repl::OpTime opTime);
    void rollBackCommitOrAbort();

    void appendInfoForServerStatus(BSONObjBuilder* builder) const;

private:
    void _waitForOpTimeToMajorityCommit(repl::OpTime opTime, std::function<void()> callbackFn);

    enum class Access { kAllow, kBlockWrites, kBlockWritesAndReads, kReject };

    ServiceContext* _serviceContext;
    executor::TaskExecutor* _executor;

    // Protects the state below.
    mutable Mutex _mutex = MONGO_MAKE_LATCH("MigratingTenantAccessBlocker::_mutex");

    Access _access{Access::kAllow};
    boost::optional<Timestamp> _blockTimestamp;
    boost::optional<repl::OpTime> _commitOrAbortOpTime;
    OperationContext* _waitForCommitOrAbortToMajorityCommitOpCtx{nullptr};
    stdx::condition_variable _transitionOutOfBlockingCV;
};

}  // namespace mongo
