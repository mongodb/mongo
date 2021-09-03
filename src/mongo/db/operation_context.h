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
#include "mongo/db/operation_id.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/transport/session.h"
#include "mongo/util/decorable.h"
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
     * ownership.  Sets the RecoveryUnit to NULL.
     *
     * Used to transfer ownership of storage engine state from OperationContext
     * to ClientCursor for getMore-able queries.
     *
     * Note that we don't allow the top-level locks to be stored across getMore.
     * We rely on active cursors being killed when collections or databases are dropped,
     * or when collection metadata changes.
     */
    std::unique_ptr<RecoveryUnit> releaseRecoveryUnit();

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
     * Swaps the locker, releasing the old locker to the caller.
     */
    std::unique_ptr<Locker> swapLockState(std::unique_ptr<Locker> locker);

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
     * Sets that this operation should always get killed during stepDown and stepUp, regardless of
     * whether or not it's taken a write lock.
     */
    void setAlwaysInterruptAtStepDownOrUp() {
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
        _lsid = boost::none;
        _txnNumber = boost::none;
    }

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

    friend class WriteUnitOfWork;
    friend class repl::UnreplicatedWritesBlock;

    Client* const _client;

    const OperationIdSlot _opId;

    boost::optional<LogicalSessionId> _lsid;
    boost::optional<TxnNumber> _txnNumber;

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

    BatonHandle _baton;

    WriteConcernOptions _writeConcern;

    // The timepoint at which this operation exceeds its time limit.
    Date_t _deadline = Date_t::max();

    ErrorCodes::Error _timeoutError = ErrorCodes::ExceededTimeLimit;
    bool _ignoreInterrupts = false;
    bool _hasArtificialDeadline = false;
    bool _markKillOnClientDisconnect = false;
    Date_t _lastClientCheck;
    bool _isExecutingShutdown = false;

    // Max operation time requested by the user or by the cursor in the case of a getMore with no
    // user-specified maxTime. This is tracked with microsecond granularity for the purpose of
    // assigning unused execution time back to a cursor at the end of an operation, only. The
    // _deadline and the service context's fast clock are the only values consulted for determining
    // if the operation's timelimit has been exceeded.
    Microseconds _maxTime = Microseconds::max();

    // Timer counting the elapsed time since the construction of this OperationContext.
    Timer _elapsedTime;

    bool _writesAreReplicated = true;
    bool _shouldParticipateInFlowControl = true;
    bool _inMultiDocumentTransaction = false;
    bool _ignoreInterruptsExceptForReplStateChange = false;

    // If true, this OpCtx will get interrupted during replica set stepUp and stepDown, regardless
    // of what locks it's taken.
    AtomicWord<bool> _alwaysInterruptAtStepDownOrUp{false};

    AtomicWord<bool> _killRequestedForReplStateChange{false};
};

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
}  // namespace mongo
