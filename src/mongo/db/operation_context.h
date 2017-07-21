/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include <boost/optional.hpp>
#include <memory>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/decorable.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

namespace mongo {

class Client;
class CurOp;
class ProgressMeter;
class ServiceContext;
class StringData;
class WriteUnitOfWork;

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
class OperationContext : public Decorable<OperationContext> {
    MONGO_DISALLOW_COPYING(OperationContext);

public:
    /**
     * The RecoveryUnitState is used by WriteUnitOfWork to ensure valid state transitions.
     */
    enum RecoveryUnitState {
        kNotInUnitOfWork,   // not in a unit of work, no writes allowed
        kActiveUnitOfWork,  // in a unit of work that still may either commit or abort
        kFailedUnitOfWork   // in a unit of work that has failed and must be aborted
    };

    OperationContext(Client* client, unsigned int opId);

    virtual ~OperationContext() = default;

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
    RecoveryUnit* releaseRecoveryUnit();

    /**
     * Associates the OperatingContext with a different RecoveryUnit for getMore or
     * subtransactions, see RecoveryUnitSwap. The new state is passed and the old state is
     * returned separately even though the state logically belongs to the RecoveryUnit,
     * as it is managed by the OperationContext.
     */
    RecoveryUnitState setRecoveryUnit(RecoveryUnit* unit, RecoveryUnitState state);

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
     * Releases the locker to the caller. Call during OperationContext cleanup or initialization,
     * only.
     */
    std::unique_ptr<Locker> releaseLockState();

    /**
     * Raises a UserException if this operation is in a killed state.
     */
    void checkForInterrupt();

    /**
     * Returns Status::OK() unless this operation is in a killed state.
     */
    Status checkForInterruptNoAssert();

    /**
     * Sleeps until "deadline"; throws an exception if the operation is interrupted before then.
     */
    void sleepUntil(Date_t deadline);

    /**
     * Sleeps for "duration" ms; throws an exception if the operation is interrupted before then.
     */
    void sleepFor(Milliseconds duration);

    /**
     * Waits for either the condition "cv" to be signaled, this operation to be interrupted, or the
     * deadline on this operation to expire.  In the event of interruption or operation deadline
     * expiration, raises a UserException with an error code indicating the interruption type.
     */
    void waitForConditionOrInterrupt(stdx::condition_variable& cv,
                                     stdx::unique_lock<stdx::mutex>& m);

    /**
     * Waits on condition "cv" for "pred" until "pred" returns true, or this operation
     * is interrupted or its deadline expires. Throws a DBException for interruption and
     * deadline expiration.
     */
    template <typename Pred>
    void waitForConditionOrInterrupt(stdx::condition_variable& cv,
                                     stdx::unique_lock<stdx::mutex>& m,
                                     Pred pred) {
        while (!pred()) {
            waitForConditionOrInterrupt(cv, m);
        }
    }

    /**
     * Same as waitForConditionOrInterrupt, except returns a Status instead of throwing
     * a DBException to report interruption.
     */
    Status waitForConditionOrInterruptNoAssert(stdx::condition_variable& cv,
                                               stdx::unique_lock<stdx::mutex>& m) noexcept;

    /**
     * Waits for condition "cv" to be signaled, or for the given "deadline" to expire, or
     * for the operation to be interrupted, or for the operation's own deadline to expire.
     *
     * If the operation deadline expires or the operation is interrupted, throws a DBException.  If
     * the given "deadline" expires, returns cv_status::timeout. Otherwise, returns
     * cv_status::no_timeout.
     */
    stdx::cv_status waitForConditionOrInterruptUntil(stdx::condition_variable& cv,
                                                     stdx::unique_lock<stdx::mutex>& m,
                                                     Date_t deadline);

    /**
     * Waits on condition "cv" for "pred" until "pred" returns true, or the given "deadline"
     * expires, or this operation is interrupted, or this operation's own deadline expires.
     *
     *
     * If the operation deadline expires or the operation is interrupted, throws a DBException.  If
     * the given "deadline" expires, returns cv_status::timeout. Otherwise, returns
     * cv_status::no_timeout indicating that "pred" finally returned true.
     */
    template <typename Pred>
    bool waitForConditionOrInterruptUntil(stdx::condition_variable& cv,
                                          stdx::unique_lock<stdx::mutex>& m,
                                          Date_t deadline,
                                          Pred pred) {
        while (!pred()) {
            if (stdx::cv_status::timeout == waitForConditionOrInterruptUntil(cv, m, deadline)) {
                return pred();
            }
        }
        return true;
    }

    /**
     * Same as the predicate form of waitForConditionOrInterruptUntil, but takes a relative
     * amount of time to wait instead of an absolute time point.
     */
    template <typename Pred>
    bool waitForConditionOrInterruptFor(stdx::condition_variable& cv,
                                        stdx::unique_lock<stdx::mutex>& m,
                                        Milliseconds duration,
                                        Pred pred) {
        return waitForConditionOrInterruptUntil(
            cv, m, getExpirationDateForWaitForValue(duration), pred);
    }

    /**
     * Same as waitForConditionOrInterruptUntil, except returns StatusWith<stdx::cv_status> and
     * non-ok status indicates the error instead of a DBException.
     */
    StatusWith<stdx::cv_status> waitForConditionOrInterruptNoAssertUntil(
        stdx::condition_variable& cv, stdx::unique_lock<stdx::mutex>& m, Date_t deadline) noexcept;

    /**
     * Returns the service context under which this operation context runs.
     */
    ServiceContext* getServiceContext() const {
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
    unsigned int getOpID() const {
        return _opId;
    }

    /**
     * Returns the session ID associated with this operation, if there is one.
     */
    boost::optional<LogicalSessionId> getLogicalSessionId() const {
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
    void setDeadlineByDate(Date_t when);

    /**
     * Sets the deadline for this operation to the maxTime plus the current time reported
     * by the ServiceContext's fast clock source.
     */
    void setDeadlineAfterNowBy(Microseconds maxTime);
    template <typename D>
    void setDeadlineAfterNowBy(D maxTime) {
        if (maxTime <= D::zero()) {
            maxTime = D::zero();
        }
        if (maxTime <= Microseconds::max()) {
            setDeadlineAfterNowBy(duration_cast<Microseconds>(maxTime));
        } else {
            setDeadlineByDate(Date_t::max());
        }
    }

    /**
     * Returns true if this operation has a deadline.
     */
    bool hasDeadline() const {
        return getDeadline() < Date_t::max();
    }

    /**
     * Returns the deadline for this operation, or Date_t::max() if there is no deadline.
     */
    Date_t getDeadline() const {
        return _deadline;
    }

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

private:
    /**
     * Returns true if this operation has a deadline and it has passed according to the fast clock
     * on ServiceContext.
     */
    bool hasDeadlineExpired() const;

    /**
     * Sets the deadline and maxTime as described. It is up to the caller to ensure that
     * these correctly correspond.
     */
    void setDeadlineAndMaxTime(Date_t when, Microseconds maxTime);

    /**
     * Returns the timepoint that is "waitFor" ms after now according to the
     * ServiceContext's precise clock.
     */
    Date_t getExpirationDateForWaitForValue(Milliseconds waitFor);

    /**
     * Set whether or not operations should generate oplog entries.
     */
    void setReplicatedWrites(bool writesAreReplicated = true) {
        _writesAreReplicated = writesAreReplicated;
    }

    friend class WriteUnitOfWork;
    friend class repl::UnreplicatedWritesBlock;
    Client* const _client;
    const unsigned int _opId;

    boost::optional<LogicalSessionId> _lsid;
    boost::optional<TxnNumber> _txnNumber;

    std::unique_ptr<Locker> _locker;

    std::unique_ptr<RecoveryUnit> _recoveryUnit;
    RecoveryUnitState _ruState = kNotInUnitOfWork;

    // Follows the values of ErrorCodes::Error. The default value is 0 (OK), which means the
    // operation is not killed. If killed, it will contain a specific code. This value changes only
    // once from OK to some kill code.
    AtomicWord<ErrorCodes::Error> _killCode{ErrorCodes::OK};


    // If non-null, _waitMutex and _waitCV are the (mutex, condition variable) pair that the
    // operation is currently waiting on inside a call to waitForConditionOrInterrupt...().
    // All access guarded by the Client's lock.
    stdx::mutex* _waitMutex = nullptr;
    stdx::condition_variable* _waitCV = nullptr;

    // If _waitMutex and _waitCV are non-null, this is the number of threads in a call to markKilled
    // actively attempting to kill the operation. If this value is non-zero, the operation is inside
    // waitForConditionOrInterrupt...() and must stay there until _numKillers reaches 0.
    //
    // All access guarded by the Client's lock.
    int _numKillers = 0;

    WriteConcernOptions _writeConcern;

    Date_t _deadline =
        Date_t::max();  // The timepoint at which this operation exceeds its time limit.

    // Max operation time requested by the user or by the cursor in the case of a getMore with no
    // user-specified maxTime. This is tracked with microsecond granularity for the purpose of
    // assigning unused execution time back to a cursor at the end of an operation, only. The
    // _deadline and the service context's fast clock are the only values consulted for determining
    // if the operation's timelimit has been exceeded.
    Microseconds _maxTime = Microseconds::max();

    // Timer counting the elapsed time since the construction of this OperationContext.
    Timer _elapsedTime;

    bool _writesAreReplicated = true;
};

class WriteUnitOfWork {
    MONGO_DISALLOW_COPYING(WriteUnitOfWork);

public:
    WriteUnitOfWork(OperationContext* opCtx)
        : _opCtx(opCtx),
          _committed(false),
          _toplevel(opCtx->_ruState == OperationContext::kNotInUnitOfWork) {
        uassert(ErrorCodes::IllegalOperation,
                "Cannot execute a write operation in read-only mode",
                !storageGlobalParams.readOnly);
        _opCtx->lockState()->beginWriteUnitOfWork();
        if (_toplevel) {
            _opCtx->recoveryUnit()->beginUnitOfWork(_opCtx);
            _opCtx->_ruState = OperationContext::kActiveUnitOfWork;
        }
    }

    ~WriteUnitOfWork() {
        dassert(!storageGlobalParams.readOnly);
        if (!_committed) {
            invariant(_opCtx->_ruState != OperationContext::kNotInUnitOfWork);
            if (_toplevel) {
                _opCtx->recoveryUnit()->abortUnitOfWork();
                _opCtx->_ruState = OperationContext::kNotInUnitOfWork;
            } else {
                _opCtx->_ruState = OperationContext::kFailedUnitOfWork;
            }
            _opCtx->lockState()->endWriteUnitOfWork();
        }
    }

    void commit() {
        invariant(!_committed);
        invariant(_opCtx->_ruState == OperationContext::kActiveUnitOfWork);
        if (_toplevel) {
            _opCtx->recoveryUnit()->commitUnitOfWork();
            _opCtx->_ruState = OperationContext::kNotInUnitOfWork;
        }
        _opCtx->lockState()->endWriteUnitOfWork();
        _committed = true;
    }

private:
    OperationContext* const _opCtx;

    bool _committed;
    bool _toplevel;
};

namespace repl {
/**
 * RAII-style class to turn off replicated writes. Writes do not create oplog entries while the
 * object is in scope.
 */
class UnreplicatedWritesBlock {
    MONGO_DISALLOW_COPYING(UnreplicatedWritesBlock);

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
