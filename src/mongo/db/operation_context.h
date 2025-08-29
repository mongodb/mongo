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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/baton.h"
#include "mongo/db/client.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/lock_manager/locker.h"
#include "mongo/db/operation_id.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/inline_memory.h"
#include "mongo/util/interruptible.h"
#include "mongo/util/lockable_adapter.h"
#include "mongo/util/modules_incompletely_marked_header.h"
#include "mongo/util/tick_source.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

#include <algorithm>
#include <memory>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class ServiceContext;

namespace repl {
class UnreplicatedWritesBlock;
}  // namespace repl

// Enabling the maxTimeAlwaysTimeOut fail point will cause any query or command run with a
// valid non-zero max time to fail immediately.  Any getmore operation on a cursor already
// created with a valid non-zero max time will also fail immediately.
//
// This fail point cannot be used with the maxTimeNeverTimeOut fail point.
extern FailPoint maxTimeAlwaysTimeOut;

// Enabling the maxTimeNeverTimeOut fail point will cause the server to never time out any
// query, command, or getmore operation, regardless of whether a max time is set.
//
// This fail point cannot be used with the maxTimeAlwaysTimeOut fail point.
extern FailPoint maxTimeNeverTimeOut;

/**
 * This class encapsulates the OperationContext state, that must be initialized before all the
 * decorations and destroyed after all the decorations.
 */
class OperationContextBase {
    // Initial size of the monotic buffer. This size may need to be adjusted when additional use
    // cases for monotonic buffer are added.
    static constexpr size_t kMonotonicBufferInlineSize = 32;

public:
    using MonotonicAllocator = inline_memory::ResourceAllocator<
        void,
        inline_memory::ExternalResource<inline_memory::MonotonicBufferResource<>>>;
    /**
     * Returns the memory buffer that can be used to monotonically allocate memory.
     * The memory will be freed at the time when OperationContext is destroyed.
     * Note:
     * This buffer *should not* be used for ephemeral allocations, as they will not be reclaimed
     * until OperationContext is destroyed. And similarly this buffer *must not* be used for
     * allocations that may be referenced after OperationContext is destroyed.
     */
    MonotonicAllocator monotonicAllocator() {
        return _monotonicBuffer.makeAllocator<void>();
    }

private:
    inline_memory::MemoryBuffer<kMonotonicBufferInlineSize, alignof(std::max_align_t)>
        _monotonicBuffer;
};

/**
 * This class encompasses the state required by an operation and lives from the time a network
 * operation is dispatched until its execution is finished. Note that each "getmore" on a cursor
 * is a separate operation. On construction, an OperationContext associates itself with the
 * current client, and only on destruction it deassociates itself. At any time a client can be
 * associated with at most one OperationContext. Each OperationContext has a RecoveryUnit
 * associated with it, though the lifetime is not necesarily the same, see releaseRecoveryUnit
 * and setRecoveryUnit. The operation context also keeps track of some transaction state
 * (RecoveryUnitState) to reduce complexity and duplication in the storage-engine specific
 * RecoveryUnit and to allow better invariant checking.
 */
class OperationContext final : public OperationContextBase,
                               public Interruptible,
                               public Decorable<OperationContext> {
    OperationContext(const OperationContext&) = delete;
    OperationContext& operator=(const OperationContext&) = delete;

public:
    /**
     * Used for tracking information about checkForInterrupt() calls that are overdue. This is only
     * used for a small sample of operations to avoid regressing performance.
     *
     * TODO: SERVER-105801 we should consider moving this information up to Interruptible.
     */
    struct OverdueInterruptCheckStats {
        OverdueInterruptCheckStats(TickSource::Tick startTime)
            : interruptCheckWindowStartTime(std::move(startTime)) {}

        Atomic<int64_t> overdueInterruptChecks{0};

        // Sum and max time an operation was overdue in checking for interrupts.
        Atomic<Milliseconds> overdueAccumulator{Milliseconds{0}};
        Atomic<Milliseconds> overdueMaxTime{Milliseconds{0}};

        // We only collect interrupt check stats for a small fraction of operations. For this sample
        // we can afford to read the TickSource each checkForInterrupt() call.
        TickSource::Tick interruptCheckWindowStartTime;
    };

    static constexpr auto kDefaultOperationContextTimeoutError = ErrorCodes::ExceededTimeLimit;

    /**
     * Creates an op context with no unique operation ID tracking - prefer using the OperationIdSlot
     * CTOR if possible to avoid OperationId collisions.
     */
    OperationContext(Client* client, OperationId opId);
    ~OperationContext() override;

    // TODO (SERVER-77213): The RecoveryUnit ownership is being moved to the TransactionResources.
    // Do not add any new usages to these methods as they will go away and will be folded as an
    // implementation detail of the Shard Role API.
    //
    // Interface for durability.  Caller DOES NOT own pointer.
    RecoveryUnit* recoveryUnit_DO_NOT_USE() const {
        return _recoveryUnit.get();
    }

    // TODO (SERVER-77213): The RecoveryUnit ownership is being moved to the TransactionResources.
    // Do not add any new usages to these methods as they will go away and will be folded as an
    // implementation detail of the Shard Role API.
    //
    // Returns the RecoveryUnit (same return value as recoveryUnit()) but the caller takes
    // ownership of the returned RecoveryUnit, and the OperationContext instance relinquishes
    // ownership. Sets the RecoveryUnit to NULL.
    std::unique_ptr<RecoveryUnit> releaseRecoveryUnit_DO_NOT_USE(ClientLock&);

    // TODO (SERVER-77213): The RecoveryUnit ownership is being moved to the TransactionResources.
    // Do not add any new usages to these methods as they will go away and will be folded as an
    // implementation detail of the Shard Role API.
    //
    // Sets up a new, inactive RecoveryUnit in the OperationContext. Destroys any previous recovery
    // unit and executes its rollback handlers.
    void replaceRecoveryUnit_DO_NOT_USE(ClientLock& clientLock);

    // TODO (SERVER-77213): The RecoveryUnit ownership is being moved to the TransactionResources.
    // Do not add any new usages to these methods as they will go away and will be folded as an
    // implementation detail of the Shard Role API.
    //
    // Similar to replaceRecoveryUnit(), but returns the previous recovery unit like
    // releaseRecoveryUnit().
    std::unique_ptr<RecoveryUnit> releaseAndReplaceRecoveryUnit_DO_NOT_USE(ClientLock& clientLock);


    // TODO (SERVER-77213): The RecoveryUnit ownership is being moved to the TransactionResources.
    // Do not add any new usages to these methods as they will go away and will be folded as an
    // implementation detail of the Shard Role API.
    //
    // Associates the OperatingContext with a different RecoveryUnit for getMore or
    // subtransactions, see RecoveryUnitSwap. The new state is passed and the old state is
    // returned separately even though the state logically belongs to the RecoveryUnit,
    // as it is managed by the OperationContext.
    WriteUnitOfWork::RecoveryUnitState setRecoveryUnit_DO_NOT_USE(
        std::unique_ptr<RecoveryUnit> unit, WriteUnitOfWork::RecoveryUnitState state, ClientLock&);

    // TODO (SERVER-77213): The locker ownership is being moved to the TransactionResources. Do not
    // add any new usages to these methods as they will go away and will be folded as an
    // implementation detail of the Shard Role API.
    //
    // The way to access the locker associated with a given OperationContext is through the
    // shard_role_details::getLocker methods.
    Locker* lockState_DO_NOT_USE() const {
        return _locker.get();
    }
    void setLockState_DO_NOT_USE(std::unique_ptr<Locker> locker);
    std::unique_ptr<Locker> swapLockState_DO_NOT_USE(std::unique_ptr<Locker> locker,
                                                     ClientLock& clientLock);

    /**
     * Returns Status::OK() unless this operation is in a killed state.
     */
    Status checkForInterruptNoAssert() noexcept override;

    /**
     * Updates the counters for tracking overdue interrupts.
     *
     * This function may only be called when the operation has opted into tracking interrupts
     * (check via overdueInterruptCheckStats()). For such operations, this should be called at the
     * operation's completion to ensure that the time after the final checkForInterrupt() is
     * accounted for.
     */
    void updateInterruptCheckCounters();

    /**
     * Returns the service context under which this operation context runs.
     */
    ServiceContext* getServiceContext() const {
        return _client->getServiceContext();
    }

    /** Returns the Service under which this operation operates. */
    Service* getService() const {
        return _client->getService();
    }

    /**
     * Returns the client under which this context runs.
     */
    Client* getClient() const {
        return _client;
    }

    /**
     * Returns the operation ID associated with this operation.
     */
    OperationId getOpID() const {
        return _opId;
    }

    /**
     * Returns the operation UUID associated with this operation or boost::none.
     */
    const boost::optional<OperationKey>& getOperationKey() const {
        return _opKey;
    }

    /**
     * Sets the operation UUID associated with this operation.
     *
     * This function may only be called once per OperationContext.
     */
    void setOperationKey(OperationKey opKey);

    /**
     * Removes the operation UUID associated with this operation.
     * DO NOT call this function outside `~OperationContext()` and `killAndDelistOperation()`.
     */
    void releaseOperationKey();

    // TODO (SERVER-80523): BEGIN Expose OperationSessionInfoFromClient as a decoration instead of
    // projecting all its fields as properties

    /**
     * Returns the session ID associated with this operation, if there is one.
     */
    const boost::optional<LogicalSessionId>& getLogicalSessionId() const {
        return _lsid;
    }

    /**
     * Associates a logical session id with this operation context. May only be called once for the
     * lifetime of the operation.
     */
    void setLogicalSessionId(LogicalSessionId lsid);

    /**
     * Returns the transaction number associated with thes operation. The combination of logical
     * session id + transaction number is what constitutes the operation transaction id.
     */
    boost::optional<TxnNumber> getTxnNumber() const {
        return _txnNumber;
    }

    /**
     * Associates a transaction number with this operation context. May only be called once for the
     * lifetime of the operation and the operation must have a logical session id assigned.
     */
    void setTxnNumber(TxnNumber txnNumber);

    /**
     * Returns the txnRetryCounter associated with this operation.
     */
    boost::optional<TxnRetryCounter> getTxnRetryCounter() const {
        return _txnRetryCounter;
    }

    /**
     * Associates a txnRetryCounter with this operation context. May only be called once for the
     * lifetime of the operation and the operation must have a logical session id and a transaction
     * number assigned.
     */
    void setTxnRetryCounter(TxnRetryCounter txnRetryCounter);

    /**
     * Returns whether this operation is part of a multi-document transaction. Specifically, it
     * indicates whether the user asked for a multi-document transaction.
     */
    bool inMultiDocumentTransaction() const {
        return _inMultiDocumentTransaction;
    }

    /**
     * Sets that this operation is part of a multi-document transaction. Once this is set, it cannot
     * be unset.
     */
    void setInMultiDocumentTransaction() {
        _inMultiDocumentTransaction = true;
        if (!_txnRetryCounter.has_value()) {
            _txnRetryCounter = 0;
        }
    }

    bool isRetryableWrite() const {
        return _txnNumber &&
            (!_inMultiDocumentTransaction ||
             isInternalSessionForRetryableWrite(*getLogicalSessionId()));
    }

    bool isCommandForwardedFromRouter() const {
        return _isCommandForwardedFromRouter;
    }

    void setCommandForwardedFromRouter() {
        _isCommandForwardedFromRouter = true;
    }

    // TODO (SERVER-80523): END Expose OperationSessionInfoFromClient as a decoration instead of
    // projecting all its fields as properties

    /**
     * Returns a CancellationToken that will be canceled when the OperationContext is killed via
     * markKilled (including for internal reasons, like the OperationContext deadline being
     * reached).
     */
    CancellationToken getCancellationToken() {
        return _cancelSource.token();
    }

    /**
     * Sets a transport Baton on the operation.  This will trigger the Baton on markKilled.
     */
    void setBaton(const BatonHandle& baton) {
        _baton = baton;
    }

    /**
     * Retrieves the baton associated with the operation.
     */
    const BatonHandle& getBaton() const {
        return _baton;
    }

    /**
     * Returns the top-level WriteUnitOfWork associated with this operation context, if any.
     */
    WriteUnitOfWork* getWriteUnitOfWork_DO_NOT_USE() {
        return _writeUnitOfWork.get();
    }

    /**
     * Sets a top-level WriteUnitOfWork for this operation context, to be held for the duration
     * of the given network operation.
     */
    void setWriteUnitOfWork_DO_NOT_USE(std::unique_ptr<WriteUnitOfWork> writeUnitOfWork) {
        invariant(writeUnitOfWork || _writeUnitOfWork);
        invariant(!(writeUnitOfWork && _writeUnitOfWork));

        _writeUnitOfWork = std::move(writeUnitOfWork);
    }

    /**
     * Returns WriteConcernOptions of the current operation
     */
    const WriteConcernOptions& getWriteConcern() const {
        return _writeConcern;
    }

    void setWriteConcern(const WriteConcernOptions& writeConcern) {
        _writeConcern = writeConcern;
    }

    /**
     * Returns true if operations should generate oplog entries.
     */
    bool writesAreReplicated() const {
        return _writesAreReplicated;
    }

    /**
     * Returns true if the operation is running lock-free.
     */
    bool isLockFreeReadsOp() const {
        return _lockFreeReadOpCount;
    }

    /**
     * Returns true if operations' durations should be added to serverStatus latency metrics.
     */
    bool shouldIncrementLatencyStats() const {
        return _shouldIncrementLatencyStats;
    }

    /**
     * Sets the shouldIncrementLatencyStats flag.
     */
    void setShouldIncrementLatencyStats(bool shouldIncrementLatencyStats) {
        _shouldIncrementLatencyStats = shouldIncrementLatencyStats;
    }

    void markKillOnClientDisconnect();

    /**
     * Identifies the opCtx as an operation which is executing global shutdown.  This has the effect
     * of masking any existing time limits, removing markKill-ability and is slightly stronger than
     * code run under runWithoutInterruptionExceptAtGlobalShutdown, because it is also immune to
     * global shutdown.
     *
     * This should only be called from the registered task of global shutdown and is not
     * recoverable. May only be called by the thread executing on behalf of this OperationContext,
     * and only while it has the Client that owns this OperationContext locked.
     */
    void setIsExecutingShutdown();

    /**
     * Marks this operation as killed so that subsequent calls to checkForInterrupt and
     * checkForInterruptNoAssert by the thread executing the operation will start returning the
     * specified error code.
     *
     * If multiple threads kill the same operation with different codes, only the first code
     * will be preserved.
     *
     * May be called by any thread that has locked the Client owning this operation context, or
     * by the thread executing this on behalf of this OperationContext.
     */
    void markKilled(ErrorCodes::Error killCode = ErrorCodes::Interrupted);

    /**
     * Returns the code passed to markKilled if this operation context has been killed previously
     * or ErrorCodes::OK otherwise.
     *
     * May be called by any thread that has locked the Client owning this operation context, or
     * without lock by the thread executing on behalf of this operation context.
     */
    ErrorCodes::Error getKillStatus() const {
        if (_ignoreInterrupts) {
            return ErrorCodes::OK;
        }
        return _killCode.loadRelaxed();
    }

    /**
     * Shortcut method, which checks whether getKillStatus returns a non-OK value. Has the same
     * concurrency rules as getKillStatus.
     */
    bool isKillPending() const {
        return getKillStatus() != ErrorCodes::OK;
    }

    /**
     * Returns the amount of time since the operation was constructed. Uses the system's most
     * precise tick source, and may not be cheap to call in a tight loop.
     */
    Microseconds getElapsedTime() const {
        return _elapsedTime.elapsed();
    }

    /**
     * Returns when the operation was marked as killed, or 0 if the operation has not been
     * marked as killed.
     *
     * This function can return 0 even after checkForInterrupt() has returned an error as during
     * global shutdowns checkForInterrupt looks at external flags. In this case the caller is
     * effectively noticing the interrupt before the opCtx is marked dead.
     */
    TickSource::Tick getKillTime() const {
        return _killTime.load();
    }

    /**
     * Sets the deadline for this operation to the given point in time.
     *
     * To remove a deadline, pass in Date_t::max().
     */
    void setDeadlineByDate(Date_t when, ErrorCodes::Error timeoutError);

    /**
     * Sets the deadline for this operation to the maxTime plus the current time reported
     * by the ServiceContext's fast clock source.
     */
    void setDeadlineAfterNowBy(Microseconds maxTime, ErrorCodes::Error timeoutError);
    template <typename D>
    void setDeadlineAfterNowBy(D maxTime, ErrorCodes::Error timeoutError) {
        if (maxTime <= D::zero()) {
            maxTime = D::zero();
        }
        if (maxTime <= Microseconds::max()) {
            setDeadlineAfterNowBy(duration_cast<Microseconds>(maxTime), timeoutError);
        } else {
            setDeadlineByDate(Date_t::max(), timeoutError);
        }
    }

    /**
     * Returns the deadline for this operation, or Date_t::max() if there is no deadline.
     */
    Date_t getDeadline() const override {
        return _deadline;
    }

    /**
     * Returns the error code used when this operation's time limit is reached.
     */
    ErrorCodes::Error getTimeoutError() const;

    /**
     * Returns the number of milliseconds remaining for this operation's time limit or
     * Milliseconds::max() if the operation has no time limit.
     */
    Milliseconds getRemainingMaxTimeMillis() const;

    /**
     * NOTE: This is a legacy "max time" method for controlling operation deadlines and it should
     * not be used in new code. Use getRemainingMaxTimeMillis instead.
     *
     * Returns the number of microseconds remaining for this operation's time limit, or the special
     * value Microseconds::max() if the operation has no time limit.
     */
    Microseconds getRemainingMaxTimeMicros() const;

    bool isIgnoringInterrupts() const;

    /**
     * Some operations coming into the system must be validated to ensure they meet constraints,
     * such as collection namespace length limits or unique index key constraints. However,
     * operations being performed from a source of truth such as during initial sync and oplog
     * application often must ignore constraint violations.
     *
     * Initial sync and oplog application opt in to relaxed constraint checking by setting this
     * value to false.
     */
    void setEnforceConstraints(bool enforceConstraints) {
        _enforceConstraints = enforceConstraints;
    }

    /**
     * This method can be used to tell if an operation requires validation of constraints. This
     * should be preferred to alternatives such as checking if a node is primary or if a client is
     * from a user connection as those have nuances (e.g: primary catch up and client disassociation
     * due to task executors).
     */
    bool isEnforcingConstraints() {
        return _enforceConstraints;
    }

    /**
     * Sets that this operation should always get killed during stepDown and stepUp, regardless of
     * whether or not it's taken a write lock.
     *
     * Note: This function is NOT synchronized with the ReplicationStateTransitionLock!  This means
     * that the node's view of it's replication state can change concurrently with this function
     * running - in which case your operation may _not_ be interrupted by that concurrent
     * replication state change. If you need to ensure that your node does not change
     * replication-state while calling this function, take the RSTL. See SERVER-66353 for more info.
     */
    void setAlwaysInterruptAtStepDownOrUp_UNSAFE() {
        _alwaysInterruptAtStepDownOrUp.store(true);
    }

    /**
     * Indicates that this operation should always get killed during stepDown and stepUp, regardless
     * of whether or not it's taken a write lock.
     */
    bool shouldAlwaysInterruptAtStepDownOrUp() {
        return _alwaysInterruptAtStepDownOrUp.load();
    }

    /**
     * Indicates that this operation should not receive interruptions while acquiring locks. Note
     * that new usages of this require the use of UninterruptibleLockGuard and are subject to the
     * same restrictions.
     *
     * TODO SERVER-68868: Remove this once UninterruptibleLockGuard is removed from the codebase.
     */
    bool uninterruptibleLocksRequested_DO_NOT_USE() const {
        return _interruptibleLocksRequested.load() < 0;
    }

    /**
     * Clears metadata associated with a multi-document transaction.
     */
    void resetMultiDocumentTransactionState() {
        invariant(_inMultiDocumentTransaction);
        invariant(!_writeUnitOfWork);
        invariant(_ruState == WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
        _inMultiDocumentTransaction = false;
        _isStartingMultiDocumentTransaction = false;
        _isActiveTransactionParticipant = false;
        _lsid = boost::none;
        _txnNumber = boost::none;
        _txnRetryCounter = boost::none;
        _killOpsExempt = false;
    }

    /**
     * Returns whether this operation is starting a multi-document transaction.
     */
    bool isStartingMultiDocumentTransaction() const {
        return _isStartingMultiDocumentTransaction;
    }

    /**
     * Returns whether this operation is continuing (not starting) a multi-document transaction.
     */
    bool isContinuingMultiDocumentTransaction() const {
        return inMultiDocumentTransaction() && !isStartingMultiDocumentTransaction();
    }

    /**
     * Sets whether this operation is starting a multi-document transaction.
     */
    void setIsStartingMultiDocumentTransaction(bool isStartingMultiDocumentTransaction) {
        _isStartingMultiDocumentTransaction = isStartingMultiDocumentTransaction;
    }

    /**
     * Set if this op is being executed by an active transaction participant. Used to differentiate
     * from an op being coordinated by a transaction router.
     */
    void setActiveTransactionParticipant() {
        invariant(_inMultiDocumentTransaction || isRetryableWrite());
        _isActiveTransactionParticipant = true;
    }

    /**
     * Returns whether this op is being executed by an active transaction participant.
     */
    bool isActiveTransactionParticipant() const {
        return _isActiveTransactionParticipant;
    }

    /**
     * Sets '_comment'. The client lock must be acquired before calling this method.
     */
    void setComment(const BSONObj& comment) {
        _comment = comment.getOwned();
    }

    /**
     * Gets '_comment'. The client lock must be acquired when calling from any thread that does
     * not own the client associated with the operation.
     */
    boost::optional<BSONElement> getComment() {
        // The '_comment' object, if present, will only ever have one field.
        return _comment ? boost::optional<BSONElement>(_comment->firstElement()) : boost::none;
    }

    boost::optional<BSONObj> getCommentOwnedCopy() const {
        return _comment.has_value() ? boost::optional<BSONObj>{_comment->copy()} : boost::none;
    }

    /**
     * Sets whether this operation is an exhaust command.
     */
    void setExhaust(bool exhaust) {
        _exhaust = exhaust;
    }

    /**
     * Returns whether this operation is an exhaust command.
     */
    bool isExhaust() const {
        return _exhaust;
    }

    void storeMaxTimeMS(Microseconds maxTime) {
        _storedMaxTime = maxTime;
    }

    /**
     * Sets whether the maxTime used by this operation is the default value.
     */
    void setUsesDefaultMaxTimeMS(bool usesDefaultMaxTimeMS) {
        _usesDefaultMaxTimeMS = usesDefaultMaxTimeMS;
    }

    /**
     * Gets whether the maxTime used by this operation is the default value.
     */
    bool usesDefaultMaxTimeMS() const {
        return _usesDefaultMaxTimeMS;
    }

    /**
     * Restore deadline to match the value stored in _storedMaxTime.
     */
    void restoreMaxTimeMS();

    /**
     * Returns whether this operation must run in read-only mode.
     *
     * If the read-only flag is set on the ServiceContext then:
     * - Internal operations are allowed to perform writes.
     * - User originating operations are not allowed to perform writes.
     */
    bool readOnly() const {
        if (!(getClient() && getClient()->isFromUserConnection()))
            return false;
        return !getServiceContext()->userWritesAllowed();
    }

    /**
     * Sets whether this operation was started by a compressed command.
     */
    void setOpCompressed(bool opCompressed) {
        _opCompressed = opCompressed;
    }

    /**
     * Returns whether this operation was started by a compressed command.
     */
    bool isOpCompressed() const {
        return _opCompressed;
    }

    /**
     * Returns whether or not a local killOps may kill this opCtx.
     */
    bool isKillOpsExempt() const {
        return _killOpsExempt;
    }

    /**
     * Set to prevent killOps from killing this opCtx even when an LSID is set.
     * You may only call this method prior to setting an LSID on this opCtx.
     * Calls to resetMultiDocumentTransactionState will reset _killOpsExempt to false.
     */
    void setKillOpsExempt() {
        invariant(!getLogicalSessionId());
        _killOpsExempt = true;
    }

    /**
     * Returns number of checkForInterrupts() done on this OperationContext.
     */
    int64_t numInterruptChecks() const {
        return _numInterruptChecks.loadRelaxed();
    }

    /**
     * Begins tracking time between interrupt checks using the given time as the operation start
     * time. It is an error to call this when already tracking interrupt checks
     * (overdueInterruptCheckStats() can be used to determine this).
     */
    void trackOverdueInterruptChecks(TickSource::Tick startTime);

    /**
     * Returns pointer to the struct containing interrupt check stats iff this OperationContext is
     * tracking interrupts. Otherwise returns nullptr.
     */
    const OverdueInterruptCheckStats* overdueInterruptCheckStats() const {
        return _overdueInterruptCheckStats.get();
    }

    // The query sampling options for operations on this opCtx. 'optIn' makes the operations
    // eligible for query sampling regardless of whether the client is considered as internal by
    // the sampler. 'optOut' does the opposite.
    enum QuerySamplingOptions { kOptIn, kOptOut };

    boost::optional<QuerySamplingOptions> getQuerySamplingOptions() {
        return _querySamplingOpts;
    }

    void setQuerySamplingOptions(QuerySamplingOptions option) {
        _querySamplingOpts = option;
    }

    void setRoutedByReplicaSetEndpoint(bool value) {
        _routedByReplicaSetEndpoint = value;
    }

    bool routedByReplicaSetEndpoint() const {
        return _routedByReplicaSetEndpoint;
    }

    /**
     * Invokes the passed callback while ignoring interrupts. Note that this causes the deadline to
     * be reset to Date_t::max(), but that it can also subsequently be reduced in size after the
     * fact. Additionally handles the dance of try/catching the invocation and checking
     * checkForInterrupt with the guard inactive (to allow a higher level timeout to override a
     * lower level one, or for top level interruption to propagate).
     *
     * This should only be called from the thread executing on behalf of this OperationContext.
     * The Client for this OperationContext should not be locked by the thread calling this
     * function, as this function will acquire the lock internally to modify the OperationContext's
     * interrupt state.
     */
    template <typename Callback>
    decltype(auto) runWithoutInterruptionExceptAtGlobalShutdown(Callback&& cb) {
        try {
            bool prevIgnoringInterrupts = _ignoreInterrupts;
            DeadlineState prevDeadlineState{
                _deadline, _maxTime, _timeoutError, _hasArtificialDeadline};
            ScopeGuard guard([&] {
                // Restore the original interruption and deadline state.
                stdx::lock_guard lg(*_client);
                _ignoreInterrupts = prevIgnoringInterrupts;
                setDeadlineAndMaxTime(
                    prevDeadlineState.deadline, prevDeadlineState.maxTime, prevDeadlineState.error);
                _hasArtificialDeadline = prevDeadlineState.hasArtificialDeadline;
                _markKilledIfDeadlineRequires();

                // For purposes of tracking overdue interrupts, act as if the moment we left
                // the ignore state was the last check for interrupt.
                if (auto* stats = _overdueInterruptCheckStats.get()) {
                    stats->interruptCheckWindowStartTime = tickSource().getTicks();
                }
            });
            // Ignore interrupts until the callback completes.
            {
                stdx::lock_guard lg(*_client);
                _hasArtificialDeadline = true;
                setDeadlineByDate(Date_t::max(), ErrorCodes::ExceededTimeLimit);
                _ignoreInterrupts = true;
            }
            return std::forward<Callback>(cb)();
        } catch (const ExceptionFor<ErrorCategory::ExceededTimeLimitError>&) {
            // May throw replacement exception
            checkForInterrupt();
            throw;
        }
    }

    /**
     * The following return the `FastClockSource` and `TickSource` instances that were available at
     * the time of creating this `opCtx`. Using the following ensures we use the same source for
     * timing during the lifetime of an `opCtx`, and we save on the cost of acquiring a reference to
     * the clock/tick source.
     */
    ClockSource& fastClockSource() const {
        return *_fastClockSource;
    }
    TickSource& tickSource() const {
        return *_tickSource;
    }

private:
    StatusWith<stdx::cv_status> waitForConditionOrInterruptNoAssertUntil(
        stdx::condition_variable& cv, BasicLockableAdapter m, Date_t deadline) noexcept override;

    DeadlineState pushArtificialDeadline(Date_t deadline, ErrorCodes::Error error) override {
        DeadlineState ds{_deadline, _maxTime, _timeoutError, _hasArtificialDeadline};

        _hasArtificialDeadline = true;
        setDeadlineByDate(std::min(_deadline, deadline), error);

        return ds;
    }

    void popArtificialDeadline(DeadlineState ds) override {
        setDeadlineAndMaxTime(ds.deadline, ds.maxTime, ds.error);
        _hasArtificialDeadline = ds.hasArtificialDeadline;

        _markKilledIfDeadlineRequires();
    }

    void _markKilledIfDeadlineRequires() {
        if (!_ignoreInterrupts && !_hasArtificialDeadline && hasDeadlineExpired() &&
            !isKillPending()) {
            markKilled(_timeoutError);
        }
    }

    /**
     * Returns true if this operation has a deadline and it has passed according to the fast clock
     * on ServiceContext.
     */
    bool hasDeadlineExpired() const;

    /**
     * Sets the deadline and maxTime as described. It is up to the caller to ensure that
     * these correctly correspond.
     */
    void setDeadlineAndMaxTime(Date_t when, Microseconds maxTime, ErrorCodes::Error timeoutError);

    /**
     * Compute maxTime based on the given deadline.
     */
    Microseconds computeMaxTimeFromDeadline(Date_t when);

    /**
     * Returns the timepoint that is "waitFor" ms after now according to the
     * ServiceContext's precise clock.
     */
    Date_t getExpirationDateForWaitForValue(Milliseconds waitFor) override;

    /**
     * Set whether or not operations should generate oplog entries.
     */
    void setReplicatedWrites(bool writesAreReplicated = true) {
        _writesAreReplicated = writesAreReplicated;
    }

    /**
     * Increment a count to indicate that the operation is running lock-free.
     */
    void incrementLockFreeReadOpCount() {
        ++_lockFreeReadOpCount;
    }
    void decrementLockFreeReadOpCount() {
        --_lockFreeReadOpCount;
    }

    /**
     * Schedule the client to be checked every second. If the client has disconnected, the operation
     * will be killed. Periodic checks are not needed if the client's session is compatible with the
     * networking baton associated with this opCtx.
     *
     * If there is no associated baton or it is not a networking baton, this method has no effect.
     */
    void _schedulePeriodicClientConnectedCheck();

    /**
     * If the client is networked, check that its underlying session is still connected. If the
     * session is not connected, kill the operation. The status used to kill the operation will be
     * returned.
     *
     * This will only actually check the underlying session every 500ms regardless of how often this
     * is called, since doing so may be expensive.
     */
    Status _checkClientConnected();

    friend class WriteUnitOfWork;
    friend class repl::UnreplicatedWritesBlock;
    friend class LockFreeReadsBlock;
    friend class InterruptibleLockGuard;
    friend class UninterruptibleLockGuard;

    Client* const _client;

    ClockSource* const _fastClockSource = getServiceContext()->getFastClockSource();
    TickSource* const _tickSource = getServiceContext()->getTickSource();

    const OperationId _opId;
    boost::optional<OperationKey> _opKey;

    boost::optional<LogicalSessionId> _lsid;
    boost::optional<TxnNumber> _txnNumber;
    boost::optional<TxnRetryCounter> _txnRetryCounter;

    std::unique_ptr<Locker> _locker;

    std::unique_ptr<RecoveryUnit> _recoveryUnit;

    // This is used directly by WriteUnitOfWork
    MONGO_MOD_NEEDS_REPLACEMENT WriteUnitOfWork::RecoveryUnitState _ruState =
        WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork;

    // Operations run within a transaction will hold a WriteUnitOfWork for the duration in order
    // to maintain two-phase locking.
    std::unique_ptr<WriteUnitOfWork> _writeUnitOfWork;

    // Follows the values of ErrorCodes::Error. The default value is 0 (OK), which means the
    // operation is not killed. If killed, it will contain a specific code. This value changes only
    // once from OK to some kill code.
    AtomicWord<ErrorCodes::Error> _killCode{ErrorCodes::OK};

    // When the operation was marked as killed.
    AtomicWord<TickSource::Tick> _killTime{0};

    // Tracks total number of interrupt checks.
    Atomic<int64_t> _numInterruptChecks{0};

    // State for tracking overdue interrupt checks. This is only allocated for a small sample of
    // operations that are randomly selected.
    std::unique_ptr<OverdueInterruptCheckStats> _overdueInterruptCheckStats;

    // Used to cancel all tokens obtained via getCancellationToken() when this OperationContext is
    // killed.
    CancellationSource _cancelSource;

    BatonHandle _baton;

    WriteConcernOptions _writeConcern;

    // The timepoint at which this operation exceeds its time limit.
    Date_t _deadline = Date_t::max();

    ErrorCodes::Error _timeoutError = kDefaultOperationContextTimeoutError;
    bool _ignoreInterrupts = false;
    bool _hasArtificialDeadline = false;
    bool _markKillOnClientDisconnect = false;
    Date_t _lastClientCheck;
    bool _isExecutingShutdown = false;

    /**
     * Contains the number of requesters for both InterruptibleLockGuard and
     * UninterruptibleLockGuard. It is > 0 on the first case and < 0 in the other. The absolute
     * number specifies how many requesters there are for each type.
     */
    Atomic<int> _interruptibleLocksRequested = 0;

    // Max operation time requested by the user or by the cursor in the case of a getMore with no
    // user-specified maxTimeMS. This is tracked with microsecond granularity for the purpose of
    // assigning unused execution time back to a cursor at the end of an operation, only. The
    // _deadline and the service context's fast clock are the only values consulted for determining
    // if the operation's timelimit has been exceeded.
    // TODO (SERVER-108835): Both deadline and maxTime represent the maximum latency, consider to
    // collapse them to mitigate the risk of bugs due to confusion.
    Microseconds _maxTime = Microseconds::max();

    // The value of the maxTimeMS requested by user in the case it was overwritten.
    boost::optional<Microseconds> _storedMaxTime;

    bool _usesDefaultMaxTimeMS = false;

    // Timer counting the elapsed time since the construction of this OperationContext.
    Timer _elapsedTime{_tickSource};

    bool _writesAreReplicated = true;
    bool _shouldIncrementLatencyStats = true;
    bool _inMultiDocumentTransaction = false;
    bool _isStartingMultiDocumentTransaction = false;
    bool _isActiveTransactionParticipant = false;
    bool _isCommandForwardedFromRouter = false;
    // Commands from user applications must run validations and enforce constraints. Operations from
    // a trusted source, such as initial sync or consuming an oplog entry generated by a primary
    // typically desire to ignore constraints.
    bool _enforceConstraints = true;

    // Counts how many lock-free read operations are running nested.
    // Necessary to use a counter rather than a boolean because there is existing code that
    // destructs lock helpers out of order.
    int _lockFreeReadOpCount = 0;

    // If true, this OpCtx will get interrupted during replica set stepUp and stepDown, regardless
    // of what locks it's taken.
    AtomicWord<bool> _alwaysInterruptAtStepDownOrUp{false};

    // If populated, this is an owned singleton BSONObj whose only field, 'comment', is a copy of
    // the 'comment' field from the input command object.
    boost::optional<BSONObj> _comment;

    // Whether this operation is an exhaust command.
    bool _exhaust = false;

    // Whether this operation was started by a compressed command.
    bool _opCompressed = false;

    // Prevent this opCtx from being killed by killSessionsLocalKillOps if an LSID is attached.
    // Normally, the presence of an LSID implies kill-eligibility as it uniquely identifies a
    // session and can thus be passed into a killSessions command to target that session and its
    // operations. However, there are some cases where we want the opCtx to have both an LSID and
    // kill-immunity. Current examples include checking out sessions on replica set step up in order
    // to refresh locks for prepared tranasctions or abort in-progress transactions.
    bool _killOpsExempt = false;

    // The query sampling options for operations on this opCtx.
    boost::optional<QuerySamplingOptions> _querySamplingOpts;

    // Set to true if this operation is going through the router code paths because of the replica
    // set endpoint.
    bool _routedByReplicaSetEndpoint = false;
};

/**
 * DO NOT USE THIS CLASS. USING THIS IS A PROGRAMMING ERROR AND WILL REQUIRE REVIEW FROM SERVICE
 * ARCH. This class is here in order to transition the logic from Locker away into the
 * OperationContext. It is here because multi-document transactions can migrate a locker across
 * multiple living opCtx executions. Please see more details in SERVER-88292.
 *
 * This class prevents the given OperationContext from being interrupted while acquiring locks as
 * long as it is in scope. The default behavior of acquisitions depends on the type of lock that is
 * being requested. Use this in the unlikely case that waiting for a lock can't be interrupted.
 *
 * It is possible that multiple callers are requesting uninterruptible behavior, so the guard
 * increments a counter on the OperationContext class to indicate how may guards are active.
 *
 * TODO SERVER-68868: Remove this class.
 */
class UninterruptibleLockGuard {
public:
    explicit UninterruptibleLockGuard(OperationContext* opCtx) : _opCtx(opCtx) {
        invariant(_opCtx);
        invariant(_opCtx->_interruptibleLocksRequested.fetchAndSubtract(1) <= 0);
    }

    UninterruptibleLockGuard(const UninterruptibleLockGuard& other) = delete;
    UninterruptibleLockGuard& operator=(const UninterruptibleLockGuard&) = delete;

    ~UninterruptibleLockGuard() {
        invariant(_opCtx->_interruptibleLocksRequested.fetchAndAdd(1) < 0);
    }

private:
    OperationContext* const _opCtx;
};

/**
 * This RAII type ensures that there are no UninterruptibleLockGuards while in scope. If an
 * UninterruptibleLockGuard is held at a higher level, or taken at a lower level, an invariant will
 * occur. This protects against UninterruptibleLockGuard uses on code paths that must be
 * interruptible. Safe to nest InterruptibleLockGuard instances.
 */
class InterruptibleLockGuard {
public:
    explicit InterruptibleLockGuard(OperationContext* opCtx) : _opCtx(opCtx) {
        invariant(_opCtx);
        invariant(_opCtx->_interruptibleLocksRequested.fetchAndAdd(1) >= 0);
    }

    InterruptibleLockGuard(const InterruptibleLockGuard& other) = delete;
    InterruptibleLockGuard& operator=(const InterruptibleLockGuard&) = delete;

    ~InterruptibleLockGuard() {
        invariant(_opCtx->_interruptibleLocksRequested.fetchAndSubtract(1) > 0);
    }

private:
    OperationContext* const _opCtx;
};

// Gets a TimeZoneDatabase pointer from the ServiceContext.
inline const TimeZoneDatabase* getTimeZoneDatabase(OperationContext* opCtx) {
    return opCtx && opCtx->getServiceContext() ? TimeZoneDatabase::get(opCtx->getServiceContext())
                                               : nullptr;
}

namespace repl {
/**
 * RAII-style class to turn off replicated writes. Writes do not create oplog entries while the
 * object is in scope.
 */
class UnreplicatedWritesBlock {
    UnreplicatedWritesBlock(const UnreplicatedWritesBlock&) = delete;
    UnreplicatedWritesBlock& operator=(const UnreplicatedWritesBlock&) = delete;

public:
    UnreplicatedWritesBlock(OperationContext* opCtx)
        : _opCtx(opCtx), _shouldReplicateWrites(opCtx->writesAreReplicated()) {
        opCtx->setReplicatedWrites(false);
    }

    ~UnreplicatedWritesBlock() {
        _opCtx->setReplicatedWrites(_shouldReplicateWrites);
    }

private:
    OperationContext* _opCtx;
    const bool _shouldReplicateWrites;
};
}  // namespace repl

/**
 * RAII-style class to indicate the operation is lock-free and code should behave accordingly.
 */
class LockFreeReadsBlock {
    LockFreeReadsBlock(const LockFreeReadsBlock&) = delete;
    LockFreeReadsBlock& operator=(const LockFreeReadsBlock&) = delete;

public:
    // Allow move operators.
    LockFreeReadsBlock(LockFreeReadsBlock&& rhs) : _opCtx(rhs._opCtx) {
        rhs._opCtx = nullptr;
    };
    LockFreeReadsBlock& operator=(LockFreeReadsBlock&& rhs) {
        _opCtx = rhs._opCtx;
        rhs._opCtx = nullptr;

        return *this;
    };

    LockFreeReadsBlock(OperationContext* opCtx) : _opCtx(opCtx) {
        _opCtx->incrementLockFreeReadOpCount();
    }

    ~LockFreeReadsBlock() {
        if (_opCtx) {
            _opCtx->decrementLockFreeReadOpCount();
        }
    }

private:
    OperationContext* _opCtx;
};
}  // namespace mongo
