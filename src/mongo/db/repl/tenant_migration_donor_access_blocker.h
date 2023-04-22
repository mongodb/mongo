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
#include "mongo/db/commands/tenant_migration_donor_cmds_gen.h"
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
 * The TenantMigrationDonorAccessBlocker is used to block and reject reads, writes and index builds
 * to a database while the Atlas Serverless tenant that owns the database is being migrated from
 * this replica set to another replica set.
 *
 * In order to preserve causal consistency across the migration, this replica set, the "donor",
 * blocks writes and reads as of a particular "blockTimestamp". The donor then advances the
 * recipient's clusterTime to "blockTimestamp" before committing the migration.
 *
 * Writes call checkIfCanWrite after being assigned an OpTime but before committing. If the
 * migration has committed, the method will throw TenantMigrationCommitted. If the migration has
 * aborted, the method will return an OK status and the writes can just proceed. Otherwise, if
 * writes are being blocked, the method will throw TenantMigrationConflict which will be caught by
 * tenant_migration_access_blocker::handleTenantMigrationConflict inside the ServiceEntryPoint. The
 * writes will then block until the migration either commits (in which case TenantMigrationCommitted
 * will be thrown) or aborts (in which case TenantMigrationAborted will be thrown). Writes are
 * blocked inside handleTenantMigrationConflict instead of checkIfCanWrite because writes must not
 * block between being assigned an OpTime and committing.
 *
 * Every command calls getCanReadFuture (via tenant_migration_access_blocker::checkIfCanReadOrBlock)
 * at some point after waiting for readConcern. If the migration has committed, the method will
 * return TenantMigrationCommitted if the command is in the commandDenyListAfterMigration. If the
 * migration has aborted, the method will return an OK status and the command can just proceed.
 * Otherwise, if the command has afterClusterTime or atClusterTime >= blockTimestamp, the promise
 * will remain unfulfilled until the migration either commits (in which case
 * TenantMigrationCommitted will be returned) or aborts (in which case an OK status will be returned
 * and the reads will be unblocked).
 *
 * Linearizable reads call checkIfLinearizableReadWasAllowed after doing the noop write at the end
 * the reads. The method returns TenantMigrationCommitted if the migration has committed, and an
 * OK status otherwise. The reads are not blocked in the blocking state because no writes to the
 * tenant's data can occur on the donor after the blockTimestamp, so a linearizable read only needs
 * to be rejected if it is possible that some writes have been accepted by the recipient (i.e. the
 * migration has committed).
 *
 * Change stream getMore commands call assertCanGetMoreChangeStream. After a commit normal getMores
 * are allowed to proceed and drain the cursors, but change stream cursors are infinite and can't be
 * fully drained. We added this special check for change streams specifically to signal that they
 * must be resumed on the recipient.
 *
 * Index build user threads call checkIfCanBuildIndex. Index builds are blocked and rejected
 * similarly to regular writes except that they are blocked from the start of the migration (i.e.
 * before "blockTimestamp" is chosen).
 * Because there may be a race between the start of a migration and the start of an index build,
 * the index builder will call checkIfCanBuildIndex after registering the build.
 *
 * The Atlas proxy is responsible for retrying writes and reads that fail with
 * TenantMigrationCommitted against the recipient, and writes that fail with TenantMigrationAborted
 * against the donor.
 *
 * Given this, the donor uses this class's API in the following way:
 *
 * 1. The donor primary creates a WriteUnitOfWork to do a write, call it the "start blocking" write.
 * The donor primary calls startBlockingWrites before the write is assigned an OpTime. This write's
 * Timestamp will be the "blockTimestamp".
 *
 * At this point:
 * - Writes that have already passed checkIfCanWrite must have been assigned an OpTime before
 *   the blockTimestamp, since the blockTimestamp hasn't been assigned yet, and OpTimes are handed
 *   out in monotonically increasing order.
 * - Writes that have not yet passed checkIfCanWrite will end up blocking. Some of these
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
 * The op observer for the "commit" write on primaries and secondaries calls setCommitOpTime to
 * store the commit opTime when the write commits.
 *
 * 4b. The donor primary can instead abort the migration by doing a write, call it the "abort"
 * write. The op observer for the "abort" write on primaries and secondaries calls setAbortOpTime
 * to store the abort opTime when the write commits.
 *
 * The class transitions out of the blocking state when onMajorityCommitPointUpdate gets called with
 * commit opTime or abort opTime, which indicates that the "commit" or "abort" write has been
 * majority committed.
 */
class TenantMigrationDonorAccessBlocker
    : public std::enable_shared_from_this<TenantMigrationDonorAccessBlocker>,
      public TenantMigrationAccessBlocker {
public:
    TenantMigrationDonorAccessBlocker(ServiceContext* serviceContext, const UUID& migrationId);

    //
    // Called by all writes and reads against the database.
    //

    Status checkIfCanWrite(Timestamp writeTs) final;
    Status waitUntilCommittedOrAborted(OperationContext* opCtx) final;

    Status checkIfLinearizableReadWasAllowed(OperationContext* opCtx) final;
    SharedSemiFuture<void> getCanReadFuture(OperationContext* opCtx, StringData command) final;

    //
    // Called by index build user threads before acquiring an index build slot, and again right
    // after registering the build.
    //
    Status checkIfCanBuildIndex() final;

    /**
     * Returns error status if "getMore" command of a change stream should fail.
     */
    Status checkIfCanGetMoreChangeStream() final;

    bool checkIfShouldBlockTTL() const final {
        // There is no TTL race at the donor side. See parent class for details.
        return false;
    }

    /**
     * If the given opTime is the commit or abort opTime and the completion promise has not been
     * fulfilled, calls _onMajorityCommitCommitOpTime or _onMajorityCommitAbortOpTime to transition
     * out of blocking and fulfill the promise.
     */
    void onMajorityCommitPointUpdate(repl::OpTime opTime) final;

    void appendInfoForServerStatus(BSONObjBuilder* builder) const final;

    void recordTenantMigrationError(Status status) final;

    //
    // Called while donating this database.
    //

    void startBlockingWrites();
    void startBlockingReadsAfter(const Timestamp& timestamp);
    void rollBackStartBlocking();

    /**
     * Called when this mtab is about to be removed from the TenantMigrationAccessBlockerRegistry.
     * Resolves all unfulfilled promises with an Interrupted error to unblock any blocked reads or
     * writes.
     */
    void interrupt();

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

    bool inStateAborted() const {
        return _state.isAborted();
    }

private:
    /**
     * The access states of an mtab.
     */
    class BlockerState {
    public:
        enum class State { kAllow, kBlockWrites, kBlockWritesAndReads, kReject, kAborted };

        void transitionTo(State newState);

        State getState() const {
            return _state;
        }

        bool isAllow() const {
            return _state == State::kAllow;
        }

        bool isBlockWrites() const {
            return _state == State::kBlockWrites;
        }

        bool isBlockWritesAndReads() const {
            return _state == State::kBlockWritesAndReads;
        }

        bool isReject() const {
            return _state == State::kReject;
        }

        bool isAborted() const {
            return _state == State::kAborted;
        }

        std::string toString() const {
            return toString(_state);
        }

        static std::string toString(State state);

    private:
        static bool _isLegalTransition(State oldState, State newState);

        State _state = State::kAllow;
    };

    /**
     * Encapsulates runtime statistics on blocked reads and writes, and tenant migration errors
     * thrown.
     */
    struct Stats {
        AtomicWord<long long> numBlockedReads;
        AtomicWord<long long> numBlockedWrites;
        AtomicWord<long long> numTenantMigrationCommittedErrors;
        AtomicWord<long long> numTenantMigrationAbortedErrors;

        /**
         * Reports the accumulated statistics for serverStatus.
         */
        void report(BSONObjBuilder* builder) const;

    } _stats;

    SharedSemiFuture<void> _onCompletion() {
        return _completionPromise.getFuture();
    }

    void _onMajorityCommitCommitOpTime(stdx::unique_lock<Latch>& lk);
    void _onMajorityCommitAbortOpTime(stdx::unique_lock<Latch>& lk);

    ServiceContext* _serviceContext;

    // Protects the state below.
    mutable Mutex _mutex = MONGO_MAKE_LATCH("TenantMigrationDonorAccessBlocker::_mutex");

    BlockerState _state;

    Timestamp _highestAllowedWriteTimestamp;
    boost::optional<Timestamp> _blockTimestamp;
    boost::optional<repl::OpTime> _commitOpTime;
    boost::optional<repl::OpTime> _abortOpTime;

    SharedPromise<void> _completionPromise;

    RepeatableSharedPromise<void> _transitionOutOfBlockingPromise;
};

}  // namespace mongo
