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

#include <boost/optional.hpp>
#include <memory>

#include "mongo/base/status.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/logical_session_id_helpers.h"
#include "mongo/db/operation_id.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/transport/session.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/interruptible.h"
#include "mongo/util/lockable_adapter.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

namespace mongo {

class CurOp;
class ProgressMeter;
class ServiceContext;
class StringData;

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
class OperationContext : public Interruptible, public Decorable<OperationContext> {
    OperationContext(const OperationContext&) = delete;
    OperationContext& operator=(const OperationContext&) = delete;

public:
    static constexpr auto kDefaultOperationContextTimeoutError = ErrorCodes::ExceededTimeLimit;

    /**
     * Creates an op context with no unique operation ID tracking - prefer using the OperationIdSlot
     * CTOR if possible to avoid OperationId collisions.
     */
    OperationContext(Client* client, OperationId opId);
    OperationContext(Client* client, OperationIdSlot&& opIdSlot);
    virtual ~OperationContext();

    bool shouldParticipateInFlowControl() const {
        return _shouldParticipateInFlowControl;
    }

    void setShouldParticipateInFlowControl(bool target) {
        _shouldParticipateInFlowControl = target;
    }

    /**
     * Interface for durability.  Caller DOES NOT own pointer.
     */
    RecoveryUnit* recoveryUnit() const {
        return _recoveryUnit.get();
    }

    /**
     * Returns the RecoveryUnit (same return value as recoveryUnit()) but the caller takes
     * ownership of the returned RecoveryUnit, and the OperationContext instance relinquishes
     * ownership. Sets the RecoveryUnit to NULL.
     */
    std::unique_ptr<RecoveryUnit> releaseRecoveryUnit();

    /*
     * Similar to releaseRecoveryUnit(), but sets up a new, inactive RecoveryUnit after releasing
     * the existing one.
     */
    std::unique_ptr<RecoveryUnit> releaseAndReplaceRecoveryUnit();

    /**
     * Associates the OperatingContext with a different RecoveryUnit for getMore or
     * subtransactions, see RecoveryUnitSwap. The new state is passed and the old state is
     * returned separately even though the state logically belongs to the RecoveryUnit,
     * as it is managed by the OperationContext.
     */
    WriteUnitOfWork::RecoveryUnitState setRecoveryUnit(std::unique_ptr<RecoveryUnit> unit,
                                                       WriteUnitOfWork::RecoveryUnitState state);

    /**
     * Interface for locking.  Caller DOES NOT own pointer.
     */
    Locker* lockState() const {
        return _locker.get();
    }

    /**
     * Sets the locker for use by this OperationContext. Call during OperationContext
     * initialization, only.
     */
    void setLockState(std::unique_ptr<Locker> locker);

    /**
     * Swaps the locker, releasing the old locker to the caller.  The Client lock is required to
     * call this function.
     */
    std::unique_ptr<Locker> swapLockState(std::unique_ptr<Locker> locker, WithLock);

    /**
     * Returns Status::OK() unless this operation is in a killed state.
     */
    Status checkForInterruptNoAssert() noexcept override;

    /**
     * Returns the service context under which this operation context runs, or nullptr if there is
     * no such service context.
     */
    ServiceContext* getServiceContext() const {
        if (!_client) {
            return nullptr;
        }

        return _client->getServiceContext();
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
        return _opId.getId();
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
     * Returns the txnRetryCounter associated with this operation.
     */
    boost::optional<TxnRetryCounter> getTxnRetryCounter() const {
        return _txnRetryCounter;
    }

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
     * Associates a transaction number with this operation context. May only be called once for the
     * lifetime of the operation and the operation must have a logical session id assigned.
     */
    void setTxnNumber(TxnNumber txnNumber);

    /**
     * Associates a txnRetryCounter with this operation context. May only be called once for the
     * lifetime of the operation and the operation must have a logical session id and a transaction
     * number assigned.
     */
    void setTxnRetryCounter(TxnRetryCounter txnRetryCounter);

    /**
     * Returns the top-level WriteUnitOfWork associated with this operation context, if any.
     */
    WriteUnitOfWork* getWriteUnitOfWork() {
        return _writeUnitOfWork.get();
    }

    /**
     * Sets a top-level WriteUnitOfWork for this operation context, to be held for the duration
     * of the given network operation.
     */
    void setWriteUnitOfWork(std::unique_ptr<WriteUnitOfWork> writeUnitOfWork) {
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
     * recoverable.
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
     * Returns whether this operation is part of a multi-document transaction. Specifically, it
     * indicates whether the user asked for a multi-document transaction.
     */
    bool inMultiDocumentTransaction() const {
        return _inMultiDocumentTransaction;
    }

    bool isRetryableWrite() const {
        return _txnNumber &&
            (!_inMultiDocumentTransaction || isInternalSessionForRetryableWrite(*_lsid));
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
     * Sets that this operation should ignore interruption except for replication state change. Can
     * only be called by the thread executing this on behalf of this OperationContext.
     */
    void setIgnoreInterruptsExceptForReplStateChange(bool target) {
        _ignoreInterruptsExceptForReplStateChange = target;
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
        _lsid = boost::none;
        _txnNumber = boost::none;
        _txnRetryCounter = boost::none;
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

private:
    StatusWith<stdx::cv_status> waitForConditionOrInterruptNoAssertUntil(
        stdx::condition_variable& cv, BasicLockableAdapter m, Date_t deadline) noexcept override;

    IgnoreInterruptsState pushIgnoreInterrupts() override {
        IgnoreInterruptsState iis{_ignoreInterrupts,
                                  {_deadline, _timeoutError, _hasArtificialDeadline}};
        _hasArtificialDeadline = true;
        setDeadlineByDate(Date_t::max(), ErrorCodes::ExceededTimeLimit);
        _ignoreInterrupts = true;

        return iis;
    }

    void popIgnoreInterrupts(IgnoreInterruptsState iis) override {
        _ignoreInterrupts = iis.ignoreInterrupts;

        setDeadlineByDate(iis.deadline.deadline, iis.deadline.error);
        _hasArtificialDeadline = iis.deadline.hasArtificialDeadline;

        _markKilledIfDeadlineRequires();
    }

    DeadlineState pushArtificialDeadline(Date_t deadline, ErrorCodes::Error error) override {
        DeadlineState ds{_deadline, _timeoutError, _hasArtificialDeadline};

        _hasArtificialDeadline = true;
        setDeadlineByDate(std::min(_deadline, deadline), error);

        return ds;
    }

    void popArtificialDeadline(DeadlineState ds) override {
        setDeadlineByDate(ds.deadline, ds.error);
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
     * Returns true if ignoring interrupts other than repl state change and no repl state change
     * has occurred.
     */
    bool _noReplStateChangeWhileIgnoringOtherInterrupts() const {
        return _ignoreInterruptsExceptForReplStateChange &&
            getKillStatus() != ErrorCodes::InterruptedDueToReplStateChange &&
            !_killRequestedForReplStateChange.loadRelaxed();
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

    friend class WriteUnitOfWork;
    friend class repl::UnreplicatedWritesBlock;
    friend class LockFreeReadsBlock;

    Client* const _client;

    const OperationIdSlot _opId;
    boost::optional<OperationKey> _opKey;

    boost::optional<LogicalSessionId> _lsid;
    boost::optional<TxnNumber> _txnNumber;
    boost::optional<TxnRetryCounter> _txnRetryCounter;

    std::unique_ptr<Locker> _locker;

    std::unique_ptr<RecoveryUnit> _recoveryUnit;
    WriteUnitOfWork::RecoveryUnitState _ruState =
        WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork;

    // Operations run within a transaction will hold a WriteUnitOfWork for the duration in order
    // to maintain two-phase locking.
    std::unique_ptr<WriteUnitOfWork> _writeUnitOfWork;

    // Follows the values of ErrorCodes::Error. The default value is 0 (OK), which means the
    // operation is not killed. If killed, it will contain a specific code. This value changes only
    // once from OK to some kill code.
    AtomicWord<ErrorCodes::Error> _killCode{ErrorCodes::OK};

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

    // Max operation time requested by the user or by the cursor in the case of a getMore with no
    // user-specified maxTimeMS. This is tracked with microsecond granularity for the purpose of
    // assigning unused execution time back to a cursor at the end of an operation, only. The
    // _deadline and the service context's fast clock are the only values consulted for determining
    // if the operation's timelimit has been exceeded.
    Microseconds _maxTime = Microseconds::max();

    // The value of the maxTimeMS requested by user in the case it was overwritten.
    boost::optional<Microseconds> _storedMaxTime;

    // Timer counting the elapsed time since the construction of this OperationContext.
    Timer _elapsedTime;

    bool _writesAreReplicated = true;
    bool _shouldIncrementLatencyStats = true;
    bool _shouldParticipateInFlowControl = true;
    bool _inMultiDocumentTransaction = false;
    bool _isStartingMultiDocumentTransaction = false;
    bool _ignoreInterruptsExceptForReplStateChange = false;
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

    AtomicWord<bool> _killRequestedForReplStateChange{false};

    // If populated, this is an owned singleton BSONObj whose only field, 'comment', is a copy of
    // the 'comment' field from the input command object.
    boost::optional<BSONObj> _comment;

    // Whether this operation is an exhaust command.
    bool _exhaust = false;

    // Whether this operation was started by a compressed command.
    bool _opCompressed = false;
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
    LockFreeReadsBlock(OperationContext* opCtx) : _opCtx(opCtx) {
        _opCtx->incrementLockFreeReadOpCount();
    }

    ~LockFreeReadsBlock() {
        _opCtx->decrementLockFreeReadOpCount();
    }

private:
    OperationContext* _opCtx;
};
}  // namespace mongo
