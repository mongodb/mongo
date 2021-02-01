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
#include "tenant_migration_access_blocker.h"

namespace mongo {

// Safe wrapper for a SharedPromise that allows setting the promise more than once.
template <typename Payload>
class RepeatableSharedPromise {
    using payload_unless_void =
        std::conditional_t<std::is_void_v<Payload>, future_details::FakeVoid, Payload>;

public:
    RepeatableSharedPromise(payload_unless_void valueAtTermination)
        : _sharedPromise(std::make_unique<SharedPromise<Payload>>()),
          _valueAtTermination(valueAtTermination) {
        static_assert(!std::is_void_v<Payload>);
    }

    RepeatableSharedPromise()
        : _sharedPromise(std::make_unique<SharedPromise<Payload>>()), _valueAtTermination({}) {
        static_assert(std::is_void_v<Payload>);
    }

    inline ~RepeatableSharedPromise();

    SharedSemiFuture<Payload> getFuture() {
        stdx::unique_lock<Latch> ul(_mutex);
        return _sharedPromise->getFuture();
    }

    // Set promise with value for non-void Payload or with Status.
    // In case of void Payload always use Status with or without error code.
    void setFrom(StatusOrStatusWith<const Payload> sosw) noexcept {
        stdx::unique_lock<Latch> ul(_mutex);
        _sharedPromise->setFrom(std::move(sosw));
        // Promise can be set only once, replace it with a new one.
        _sharedPromise = std::make_unique<SharedPromise<Payload>>();
    }

private:
    mutable Mutex _mutex;
    std::unique_ptr<SharedPromise<Payload>> _sharedPromise;
    // In destructor, set the final payload value to this.
    const payload_unless_void _valueAtTermination;
};

template <typename Payload>
inline RepeatableSharedPromise<Payload>::~RepeatableSharedPromise() {
    _sharedPromise->setFrom(StatusOrStatusWith<Payload>(_valueAtTermination));
}

template <>
inline RepeatableSharedPromise<void>::~RepeatableSharedPromise() {
    _sharedPromise->setFrom(Status::OK());
}


/**
 * The TenantMigrationDonorAccessBlocker is used to block and eventually reject reads, writes, and
 * index builds to a database while the Atlas Serverless tenant that owns the database is being
 * migrated from this replica set to another replica set.
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
 *             TenantMigrationDonorAccessBlocker::get(db).checkIfCanWriteOrBlock(opCtx);
 *         }
 *     }
 * }
 *
 * Writes call checkIfCanWriteOrThrow after being assigned an OpTime but before committing. The
 * method throws TenantMigrationConflict if writes are being blocked, which is caught in the loop.
 * The write then blocks until the migration either commits (in which case checkIfCanWriteOrBlock
 * throws an error that causes the write to be rejected) or aborts (in which case
 * checkIfCanWriteOrBlock returns successfully and the write is retried in the loop). This loop is
 * used because writes must not block after being assigned an OpTime but before committing.
 *
 * Reads with afterClusterTime or atClusterTime call getCanReadFuture at some point after
 * waiting for readConcern, that is, after waiting to reach their clusterTime, which includes
 * waiting for all earlier oplog holes to be filled.
 *
 * Index build user threads call checkIfCanBuildIndex.  Index builds are blocked throughout
 * a migration, including the kAllow state.  If the state is kReject (indicating the migration has
 * committed), checkIfCanBuildIndex throws TenantMigrationCommitted which cancels the index
 * build.  If the state is kAborted, the index build is allowed.
 *
 * Because there may be a race between the start of a migration and the start of an index build,
 * the index builder will call checkIfCanBuildIndex after registering the build.
 *
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
 * - Reads on the node that have already passed getCanReadFuture must have a clusterTime before
 *   the blockTimestamp, since the write at blockTimestamp hasn't committed yet (i.e., there's still
 *   an oplog hole at blockTimestamp).
 * - Reads on the node that have not yet passed getCanReadFuture will end up blocking.
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
class TenantMigrationDonorAccessBlocker
    : public std::enable_shared_from_this<TenantMigrationDonorAccessBlocker>,
      public TenantMigrationAccessBlocker {
public:
    TenantMigrationDonorAccessBlocker(ServiceContext* serviceContext,
                                      std::string tenantId,
                                      std::string recipientConnString);

    //
    // Called by all writes and reads against the database.
    //

    void checkIfCanWriteOrThrow() final;
    Status waitUntilCommittedOrAborted(OperationContext* opCtx, OperationType operationType) final;

    void checkIfLinearizableReadWasAllowedOrThrow(OperationContext* opCtx) final;
    SharedSemiFuture<void> getCanReadFuture(OperationContext* opCtx) final;

    //
    // Called by index build user threads before acquiring an index build slot, and again right
    // after registering the build.
    //
    Status checkIfCanBuildIndex() final;

    /**
     * If the given opTime is the commit or abort opTime and the completion promise has not been
     * fulfilled, calls _onMajorityCommitCommitOpTime or _onMajorityCommitAbortOpTime to transition
     * out of blocking and fulfill the promise.
     */
    void onMajorityCommitPointUpdate(repl::OpTime opTime) final;

    std::shared_ptr<executor::TaskExecutor> getAsyncBlockingOperationsExecutor() final {
        return _asyncBlockingOperationsExecutor;
    }

    void appendInfoForServerStatus(BSONObjBuilder* builder) const final;

    // Returns structured info with current tenant ID and connection string.
    BSONObj getDebugInfo() const final;

    //
    // Called while donating this database.
    //

    void startBlockingWrites();
    void startBlockingReadsAfter(const Timestamp& timestamp);
    void rollBackStartBlocking();

    /**
     * Stores the commit opTime and calls _onMajorityCommitCommitOpTime if the opTime is already
     * majority-committed.
     */
    void setCommitOpTime(OperationContext* opCtx, repl::OpTime opTime);

    /**
     * Stores the abort opTime and calls _onMajorityCommitAbortOpTime if the opTime is already
     * majority-committed.
     */
    void setAbortOpTime(OperationContext* opCtx, repl::OpTime opTime);

private:
    /**
     * The access states of an mtab.
     */
    enum class State { kAllow, kBlockWrites, kBlockWritesAndReads, kReject, kAborted };
    std::string _stateToString(State state) const;

    SharedSemiFuture<void> _onCompletion() {
        return _completionPromise.getFuture();
    }

    void _onMajorityCommitCommitOpTime(stdx::unique_lock<Latch>& lk);
    void _onMajorityCommitAbortOpTime(stdx::unique_lock<Latch>& lk);

    // Helper for the method 'getCanReadFuture()'.
    SharedSemiFuture<void> _getCanDoClusterTimeReadFuture(OperationContext* opCtx,
                                                          Timestamp readTimestamp);

    ServiceContext* _serviceContext;
    const std::string _tenantId;
    const std::string _recipientConnString;

    // Protects the state below.
    mutable Mutex _mutex = MONGO_MAKE_LATCH("TenantMigrationDonorAccessBlocker::_mutex");

    State _state{State::kAllow};

    boost::optional<Timestamp> _blockTimestamp;
    boost::optional<repl::OpTime> _commitOpTime;
    boost::optional<repl::OpTime> _abortOpTime;

    SharedPromise<void> _completionPromise;

    RepeatableSharedPromise<void> _transitionOutOfBlockingPromise;

    std::shared_ptr<executor::TaskExecutor> _asyncBlockingOperationsExecutor;
};

}  // namespace mongo
