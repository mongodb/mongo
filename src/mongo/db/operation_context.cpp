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


#include "mongo/db/operation_context.h"

#include "mongo/base/error_extra_info.h"
#include "mongo/base/string_data.h"
#include "mongo/db/client.h"
#include "mongo/db/local_catalog/lock_manager/locker.h"
#include "mongo/db/operation_context_options_gen.h"
#include "mongo/db/operation_key_manager.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/baton.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/system_tick_source.h"
#include "mongo/util/waitable.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {

MONGO_FAIL_POINT_DEFINE(maxTimeAlwaysTimeOut);

MONGO_FAIL_POINT_DEFINE(maxTimeNeverTimeOut);

namespace {

// Enabling the checkForInterruptFail fail point will start a game of random chance on the
// connection specified in the fail point data, generating an interrupt with a given fixed
// probability.  Example invocation:
//
// {configureFailPoint: "checkForInterruptFail",
//  mode: "alwaysOn",
//  data: {threadName: "threadName", chance: .01}}
//
// Both data fields must be specified. In the above example, all interrupt points on the thread with
// name 'threadName' will generate a kill on the current operation with probability p(.01),
// including interrupt points of nested operations. "chance" must be a double between 0 and 1,
// inclusive.
MONGO_FAIL_POINT_DEFINE(checkForInterruptFail);

const auto kNoWaiterThread = stdx::thread::id();

Milliseconds interruptCheckPeriod() {
    return Milliseconds{gOverdueInterruptCheckIntervalMillis.loadRelaxed()};
}
}  // namespace

OperationContext::OperationContext(Client* client, OperationId opId)
    : _client(client), _opId(opId) {}

OperationContext::~OperationContext() {
    releaseOperationKey();
}

void OperationContext::setDeadlineAndMaxTime(Date_t when,
                                             Microseconds maxTime,
                                             ErrorCodes::Error timeoutError) {
    invariant(!getClient()->isInDirectClient() || _hasArtificialDeadline);
    invariant(ErrorCodes::isExceededTimeLimitError(timeoutError));
    if (ErrorCodes::mustHaveExtraInfo(timeoutError)) {
        invariant(!ErrorExtraInfo::parserFor(timeoutError));
    }
    uassert(40120,
            "Illegal attempt to change operation deadline",
            _hasArtificialDeadline || !hasDeadline());
    _deadline = when;
    _maxTime = maxTime;
    _timeoutError = timeoutError;
}

Microseconds OperationContext::computeMaxTimeFromDeadline(Date_t when) {
    Microseconds maxTime;
    if (when == Date_t::max()) {
        maxTime = Microseconds::max();
    } else {
        maxTime = when - fastClockSource().now();
        if (maxTime < Microseconds::zero()) {
            maxTime = Microseconds::zero();
        }
    }
    return maxTime;
}

void OperationContext::setDeadlineByDate(Date_t when, ErrorCodes::Error timeoutError) {
    setDeadlineAndMaxTime(when, computeMaxTimeFromDeadline(when), timeoutError);
}

void OperationContext::setDeadlineAfterNowBy(Microseconds maxTime, ErrorCodes::Error timeoutError) {
    Date_t when;
    if (maxTime < Microseconds::zero()) {
        maxTime = Microseconds::zero();
    }
    if (maxTime == Microseconds::max()) {
        when = Date_t::max();
    } else {
        auto& clock = fastClockSource();
        when = clock.now();
        if (maxTime > Microseconds::zero()) {
            when += clock.getPrecision() + maxTime;
        }
    }
    setDeadlineAndMaxTime(when, maxTime, timeoutError);
}

bool OperationContext::hasDeadlineExpired() const {
    if (!hasDeadline()) {
        return false;
    }
    if (MONGO_unlikely(maxTimeNeverTimeOut.shouldFail())) {
        return false;
    }
    if (MONGO_unlikely(maxTimeAlwaysTimeOut.shouldFail())) {
        return true;
    }

    const auto now = fastClockSource().now();
    return now >= getDeadline();
}

ErrorCodes::Error OperationContext::getTimeoutError() const {
    return _timeoutError;
}

Milliseconds OperationContext::getRemainingMaxTimeMillis() const {
    if (!hasDeadline()) {
        return Milliseconds::max();
    }

    return std::max(Milliseconds{0}, getDeadline() - fastClockSource().now());
}

Microseconds OperationContext::getRemainingMaxTimeMicros() const {
    if (!hasDeadline()) {
        return Microseconds::max();
    }
    return _maxTime - getElapsedTime();
}

void OperationContext::restoreMaxTimeMS() {
    if (!_storedMaxTime) {
        return;
    }

    auto maxTime = *_storedMaxTime;
    _storedMaxTime = boost::none;

    if (maxTime <= Microseconds::zero()) {
        maxTime = Microseconds::max();
    }

    if (maxTime == Microseconds::max()) {
        _deadline = Date_t::max();
    } else {
        auto& clock = fastClockSource();
        _deadline = clock.now() + clock.getPrecision() + maxTime - _elapsedTime.elapsed();
    }
    _maxTime = maxTime;
}

namespace {

// Helper function for checkForInterrupt fail point.  Decides whether the operation currently
// being run by the given Client meet the (probabilistic) conditions for interruption as
// specified in the fail point info.
bool opShouldFail(Client* client, const BSONObj& failPointInfo) {
    // Only target the client with the specified connection number.
    if (client->desc() != failPointInfo.getStringField("threadName")) {
        return false;
    }

    // Return true with (approx) probability p = "chance".  Recall: 0 <= chance <= 1.
    double next = client->getPrng().nextCanonicalDouble();
    if (next > failPointInfo["chance"].numberDouble()) {
        return false;
    }
    return true;
}

}  // namespace

Status OperationContext::checkForInterruptNoAssert() noexcept {
    _numInterruptChecks.fetchAndAddRelaxed(1);
    if (_overdueInterruptCheckStats) {
        updateInterruptCheckCounters();
    }

    if (getClient()->getKilled() && !_isExecutingShutdown) {
        return Status(ErrorCodes::ClientMarkedKilled, "client has been killed");
    }

    if (getServiceContext()->getKillAllOperations() && !_isExecutingShutdown) {
        return Status(ErrorCodes::InterruptedAtShutdown, "interrupted at shutdown");
    }

    if (hasDeadlineExpired()) {
        if (!_hasArtificialDeadline) {
            markKilled(_timeoutError);
        }
        return Status(_timeoutError, "operation exceeded time limit");
    }

    if (_ignoreInterrupts) {
        return Status::OK();
    }

    checkForInterruptFail.executeIf(
        [&](auto&&) {
            LOGV2(20882, "Marking operation as killed for failpoint", "opId"_attr = getOpID());
            markKilled();
        },
        [&](auto&& data) { return opShouldFail(getClient(), data); });

    const auto killStatus = getKillStatus();
    if (killStatus != ErrorCodes::OK) {
        if (killStatus == ErrorCodes::TransactionExceededLifetimeLimitSeconds)
            return Status(
                killStatus,
                "operation was interrupted because the transaction exceeded the configured "
                "'transactionLifetimeLimitSeconds'");

        return Status(killStatus, "operation was interrupted");
    }

    if (_markKillOnClientDisconnect) {
        if (auto status = _checkClientConnected(); !status.isOK()) {
            return status;
        }
    }

    return Status::OK();
}

void OperationContext::trackOverdueInterruptChecks(TickSource::Tick startTime) {
    tassert(10988600,
            "OperationContext::trackOverdueInterruptChecks may be called at most once",
            !_overdueInterruptCheckStats);
    _overdueInterruptCheckStats = std::make_unique<OverdueInterruptCheckStats>(startTime);
}

void OperationContext::updateInterruptCheckCounters() {
    const TickSource::Tick now = tickSource().getTicks();
    const TickSource::Tick prevStart =
        std::exchange(_overdueInterruptCheckStats->interruptCheckWindowStartTime, now);

    if (_ignoreInterrupts || isWaitingForConditionOrInterrupt()) {
        // If we're ignoring interrupts or we're in an interruptible wait, we update the time of
        // the last interrupt check, but we do not bump the overdue counters.
        return;
    }

    if (auto overdue = tickSource().ticksTo<Milliseconds>(now - prevStart) - interruptCheckPeriod();
        overdue > Milliseconds{0}) {
        auto& stats = *_overdueInterruptCheckStats;
        stats.overdueInterruptChecks.fetchAndAddRelaxed(1);
        stats.overdueAccumulator.storeRelaxed(stats.overdueAccumulator.loadRelaxed() + overdue);
        stats.overdueMaxTime.storeRelaxed(std::max(stats.overdueMaxTime.loadRelaxed(), overdue));
    }
}

void OperationContext::_schedulePeriodicClientConnectedCheck() {
    if (!_baton) {
        return;
    }

    auto nextCheck = _lastClientCheck + Seconds(1);
    _baton->waitUntil(nextCheck, getCancellationToken()).getAsync([&](auto waitStatus) {
        if (!waitStatus.isOK()) {
            return;
        }
        if (!_checkClientConnected().isOK()) {
            return;
        }
        _schedulePeriodicClientConnectedCheck();
    });
}

Status OperationContext::_checkClientConnected() {
    const auto now = fastClockSource().now();

    if (now > _lastClientCheck + Milliseconds(500)) {
        _lastClientCheck = now;

        auto client = getClient();
        if (!client->session()->isConnected()) {
            markKilled(client->getDisconnectErrorCode());
            return Status(client->getDisconnectErrorCode(),
                          "operation was interrupted because a client disconnected");
        }
    }

    return Status::OK();
}

// waitForConditionOrInterruptNoAssertUntil returns when:
//
// Normal condvar wait criteria:
// - cv is notified
// - deadline is passed
//
// OperationContext kill criteria:
// - _deadline is passed (artificial deadline or maxTimeMS)
// - markKilled is called (killOp)
//
// Baton criteria:
// - _baton is notified (someone's queuing work for the baton)
// - _baton::run returns (timeout fired / networking is ready / socket disconnected)
//
// We release the lock held by m whenever we call markKilled, since it may trigger
// CancellationSource cancellation which can in turn emplace a SharedPromise which then may acquire
// a mutex.
StatusWith<stdx::cv_status> OperationContext::waitForConditionOrInterruptNoAssertUntil(
    stdx::condition_variable& cv, BasicLockableAdapter m, Date_t deadline) noexcept {
    invariant(getClient());

    // If the maxTimeNeverTimeOut failpoint is set, behave as though the operation's deadline does
    // not exist. Under normal circumstances, if the op has an existing deadline which is sooner
    // than the deadline passed into this method, we replace our deadline with the op's. This means
    // that we expect to time out at the same time as the existing deadline expires. If, when we
    // time out, we find that the op's deadline has not expired (as will always be the case if
    // maxTimeNeverTimeOut is set) then we assume that the incongruity is due to a clock mismatch
    // and return _timeoutError regardless. To prevent this behaviour, only consider the op's
    // deadline in the event that the maxTimeNeverTimeOut failpoint is not set.
    bool opHasDeadline = (hasDeadline() && !MONGO_unlikely(maxTimeNeverTimeOut.shouldFail()));

    if (opHasDeadline) {
        deadline = std::min(deadline, getDeadline());
    }

    try {
        const auto waitStatus = [&] {
            if (Date_t::max() == deadline) {
                Waitable::wait(_baton.get(), getServiceContext()->getPreciseClockSource(), cv, m);
                return stdx::cv_status::no_timeout;
            }
            return getServiceContext()->getPreciseClockSource()->waitForConditionUntil(
                cv, m, deadline, _baton.get());
        }();

        if (opHasDeadline && waitStatus == stdx::cv_status::timeout && deadline == getDeadline()) {
            // It's possible that the system clock used in stdx::condition_variable::wait_until
            // is slightly ahead of the FastClock used in checkForInterrupt. In this case,
            // we treat the operation as though it has exceeded its time limit, just as if the
            // FastClock and system clock had agreed.
            if (!_hasArtificialDeadline) {
                interruptible_detail::doWithoutLock(m, [&] { markKilled(_timeoutError); });
            }
            return Status(_timeoutError, "operation exceeded time limit");
        }

        return waitStatus;
    } catch (const ExceptionFor<ErrorCodes::DurationOverflow>& ex) {
        // Inside waitForConditionUntil() is a conversion from deadline's Date_t type to the system
        // clock's time_point type. If the time_point's compiler-dependent resolution is higher
        // than Date_t's milliseconds, it's possible for the conversion from Date_t to time_point
        // to overflow and trigger an exception. We catch that here to maintain the noexcept
        // contract.
        return ex.toStatus();
    }
}

void OperationContext::markKilled(ErrorCodes::Error killCode) {
    invariant(killCode != ErrorCodes::OK);
    if (ErrorCodes::mustHaveExtraInfo(killCode)) {
        invariant(!ErrorExtraInfo::parserFor(killCode));
    }

    if (killCode == getClient()->getDisconnectErrorCode()) {
        LOGV2(20883, "Interrupted operation as its client disconnected", "opId"_attr = getOpID());
    }

    // Set this before assigning the _killCode to ensure that any thread that observes the interrupt
    // is guaranteed to see the write. If multiple threads race here, the first one to execute will
    // win. While it isn't guaranteed to be the same thread that wins the _killCode race, it is
    // conceptually a better kill time anyway. In theory, we could do better with atomicMin,
    // however it is probably better to preserve the rule that this is written to exactly once, and
    // only prior to the store to _killCode.
    {
        auto setIfZero = TickSource::Tick(0);
        auto ticks = tickSource().getTicks();
        if (!ticks) {
            ticks = TickSource::Tick(-1);
        }
        _killTime.compareAndSwap(&setIfZero, ticks);
    }

    if (auto status = ErrorCodes::OK; _killCode.compareAndSwap(&status, killCode)) {
        _cancelSource.cancel();
        if (_baton) {
            _baton->notify();
        }
    }
}

void OperationContext::markKillOnClientDisconnect() {
    if (!getClient()) {
        return;
    }

    if (getClient()->isInDirectClient()) {
        return;
    }

    if (_markKillOnClientDisconnect) {
        return;
    }

    if (auto session = getClient()->session()) {
        _lastClientCheck = fastClockSource().now();

        _markKillOnClientDisconnect = true;

        if (_baton) {
            if (auto networkingBaton = _baton->networking(); networkingBaton &&
                networkingBaton->getTransportLayer() == session->getTransportLayer()) {
                networkingBaton->markKillOnClientDisconnect();
            } else {
                _schedulePeriodicClientConnectedCheck();
            }
        }
    }
}

void OperationContext::setIsExecutingShutdown() {
    invariant(!_isExecutingShutdown);

    _isExecutingShutdown = true;

    // The OperationContext executing shutdown is immune from interruption.
    _hasArtificialDeadline = true;
    setDeadlineByDate(Date_t::max(), ErrorCodes::ExceededTimeLimit);
    _ignoreInterrupts = true;
}

void OperationContext::setLogicalSessionId(LogicalSessionId lsid) {
    _lsid = std::move(lsid);
}

void OperationContext::setOperationKey(OperationKey opKey) {
    // Only set the opKey once
    invariant(!_opKey);

    _opKey.emplace(std::move(opKey));
    OperationKeyManager::get(_client).add(*_opKey, _opId);
}

void OperationContext::releaseOperationKey() {
    if (_opKey) {
        OperationKeyManager::get(_client).remove(*_opKey);
    }
    _opKey = boost::none;
}

void OperationContext::setTxnNumber(TxnNumber txnNumber) {
    invariant(_lsid);
    _txnNumber = txnNumber;
}

void OperationContext::setTxnRetryCounter(TxnRetryCounter txnRetryCounter) {
    invariant(_lsid);
    invariant(_txnNumber);
    invariant(!_txnRetryCounter.has_value());
    _txnRetryCounter = txnRetryCounter;
}

std::unique_ptr<RecoveryUnit> OperationContext::releaseRecoveryUnit_DO_NOT_USE(ClientLock&) {
    if (_recoveryUnit) {
        _recoveryUnit->setOperationContext(nullptr);
    }

    return std::move(_recoveryUnit);
}

std::unique_ptr<RecoveryUnit> OperationContext::releaseAndReplaceRecoveryUnit_DO_NOT_USE(
    ClientLock& clientLock) {
    auto ru = releaseRecoveryUnit_DO_NOT_USE(clientLock);
    setRecoveryUnit_DO_NOT_USE(getServiceContext()->getStorageEngine()->newRecoveryUnit(),
                               WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork,
                               clientLock);
    return ru;
}

void OperationContext::replaceRecoveryUnit_DO_NOT_USE(ClientLock& clientLock) {
    setRecoveryUnit_DO_NOT_USE(getServiceContext()->getStorageEngine()->newRecoveryUnit(),
                               WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork,
                               clientLock);
}

WriteUnitOfWork::RecoveryUnitState OperationContext::setRecoveryUnit_DO_NOT_USE(
    std::unique_ptr<RecoveryUnit> unit, WriteUnitOfWork::RecoveryUnitState state, ClientLock&) {
    _recoveryUnit = std::move(unit);
    if (_recoveryUnit) {
        _recoveryUnit->setOperationContext(this);
    }

    WriteUnitOfWork::RecoveryUnitState oldState = _ruState;
    _ruState = state;
    return oldState;
}

void OperationContext::setLockState_DO_NOT_USE(std::unique_ptr<Locker> locker) {
    invariant(!_locker);
    invariant(locker);
    _locker = std::move(locker);
}

std::unique_ptr<Locker> OperationContext::swapLockState_DO_NOT_USE(std::unique_ptr<Locker> locker,
                                                                   ClientLock& clientLock) {
    invariant(_locker);
    invariant(locker);
    _locker.swap(locker);
    return locker;
}

Date_t OperationContext::getExpirationDateForWaitForValue(Milliseconds waitFor) {
    return getServiceContext()->getPreciseClockSource()->now() + waitFor;
}

bool OperationContext::isIgnoringInterrupts() const {
    return _ignoreInterrupts;
}
}  // namespace mongo
