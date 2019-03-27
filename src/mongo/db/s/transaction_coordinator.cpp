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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kTransaction

#include "mongo/platform/basic.h"

#include "mongo/db/s/transaction_coordinator.h"

#include "mongo/db/logical_clock.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

using CommitDecision = txn::CommitDecision;
using CoordinatorCommitDecision = txn::CoordinatorCommitDecision;
using PrepareVoteConsensus = txn::PrepareVoteConsensus;
using TransactionCoordinatorDocument = txn::TransactionCoordinatorDocument;

CoordinatorCommitDecision makeDecisionFromPrepareVoteConsensus(ServiceContext* service,
                                                               const PrepareVoteConsensus& result,
                                                               const LogicalSessionId& lsid,
                                                               TxnNumber txnNumber) {
    auto decision = result.decision();

    if (decision.getDecision() == CommitDecision::kCommit) {
        LOG(3) << "Advancing cluster time to the commit timestamp "
               << *decision.getCommitTimestamp() << " for " << lsid.getId() << ':' << txnNumber;

        uassertStatusOK(LogicalClock::get(service)->advanceClusterTime(
            LogicalTime(*decision.getCommitTimestamp())));

        decision.setCommitTimestamp(Timestamp(decision.getCommitTimestamp()->getSecs(),
                                              decision.getCommitTimestamp()->getInc() + 1));
    }

    return decision;
}

}  // namespace

TransactionCoordinator::TransactionCoordinator(ServiceContext* serviceContext,
                                               const LogicalSessionId& lsid,
                                               TxnNumber txnNumber,
                                               std::unique_ptr<txn::AsyncWorkScheduler> scheduler,
                                               boost::optional<Date_t> coordinateCommitDeadline)
    : _serviceContext(serviceContext),
      _lsid(lsid),
      _txnNumber(txnNumber),
      _scheduler(std::move(scheduler)) {
    if (coordinateCommitDeadline) {
        _deadlineScheduler = _scheduler->makeChildScheduler();
        _deadlineScheduler
            ->scheduleWorkAt(*coordinateCommitDeadline, [](OperationContext* opCtx) {})
            .getAsync([this](const Status& s) {
                if (s == ErrorCodes::TransactionCoordinatorDeadlineTaskCanceled)
                    return;
                cancelIfCommitNotYetStarted();
            });
    }
}

TransactionCoordinator::~TransactionCoordinator() {
    cancelIfCommitNotYetStarted();

    // Wait for all scheduled asynchronous activity to complete
    if (_deadlineScheduler)
        _deadlineScheduler->join();

    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(_state == TransactionCoordinator::CoordinatorState::kDone);

    // Make sure no callers of functions on the coordinator are waiting for a decision to be
    // signaled or the commit process to complete.
    invariant(_completionPromises.empty());
}

void TransactionCoordinator::runCommit(std::vector<ShardId> participantShards) {
    {
        stdx::lock_guard<stdx::mutex> lg(_mutex);
        if (_state != CoordinatorState::kInit)
            return;
        _state = CoordinatorState::kPreparing;
    }

    _cancelTimeoutWaitForCommitTask();

    txn::persistParticipantsList(*_scheduler, _lsid, _txnNumber, participantShards)
        .then([this, participantShards]() { return _runPhaseOne(participantShards); })
        .then([this, participantShards](CoordinatorCommitDecision decision) {
            return _runPhaseTwo(participantShards, decision);
        })
        .getAsync([this](Status s) { _handleCompletionError(s); });
}

void TransactionCoordinator::continueCommit(const TransactionCoordinatorDocument& doc) {
    {
        stdx::lock_guard<stdx::mutex> lg(_mutex);
        invariant(_state == CoordinatorState::kInit);
        invariant(!_deadlineScheduler);
        _state = CoordinatorState::kPreparing;
    }

    const auto& participantShards = doc.getParticipants();

    // Helper lambda to get the decision either from the document passed in or from the participants
    // (by performing 'phase one' of two-phase commit).
    auto getDecision = [&]() -> Future<CoordinatorCommitDecision> {
        const auto& decision = doc.getDecision();
        if (!decision) {
            return _runPhaseOne(participantShards);
        } else {
            return *decision;
        }
    };

    getDecision()
        .then([this, participantShards](CoordinatorCommitDecision decision) {
            return _runPhaseTwo(participantShards, decision);
        })
        .getAsync([this](Status s) { _handleCompletionError(s); });
}

Future<void> TransactionCoordinator::onCompletion() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_state == CoordinatorState::kDone) {
        return Future<void>::makeReady();
    }

    auto completionPromiseFuture = makePromiseFuture<void>();
    _completionPromises.emplace_back(std::move(completionPromiseFuture.promise));

    return std::move(completionPromiseFuture.future)
        .onError<ErrorCodes::TransactionCoordinatorSteppingDown>(
            [](const Status& s) { uasserted(ErrorCodes::InterruptedDueToStepDown, s.reason()); });
}

SharedSemiFuture<CommitDecision> TransactionCoordinator::getDecision() {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    return _decisionPromise.getFuture();
}

void TransactionCoordinator::cancelIfCommitNotYetStarted() {
    _cancelTimeoutWaitForCommitTask();

    stdx::unique_lock<stdx::mutex> lk(_mutex);
    if (_state == CoordinatorState::kInit) {
        invariant(!_decisionPromise.getFuture().isReady());
        _decisionPromise.emplaceValue(CommitDecision::kCanceled);
        _transitionToDone(std::move(lk));
    }
}

void TransactionCoordinator::_cancelTimeoutWaitForCommitTask() {
    if (_deadlineScheduler) {
        _deadlineScheduler->shutdown({ErrorCodes::TransactionCoordinatorDeadlineTaskCanceled,
                                      "Interrupting the commit received deadline task"});
    }
}

Future<CoordinatorCommitDecision> TransactionCoordinator::_runPhaseOne(
    const std::vector<ShardId>& participantShards) {
    return txn::sendPrepare(_serviceContext, *_scheduler, _lsid, _txnNumber, participantShards)
        .then([this, participantShards](PrepareVoteConsensus result) {
            invariant(_state == CoordinatorState::kPreparing);

            auto decision =
                makeDecisionFromPrepareVoteConsensus(_serviceContext, result, _lsid, _txnNumber);

            return txn::persistDecision(*_scheduler,
                                        _lsid,
                                        _txnNumber,
                                        participantShards,
                                        decision.getCommitTimestamp())
                .then([decision] { return decision; });
        });
}

Future<void> TransactionCoordinator::_runPhaseTwo(const std::vector<ShardId>& participantShards,
                                                  const CoordinatorCommitDecision& decision) {
    return _sendDecisionToParticipants(participantShards, decision)
        .then([this] {
            if (MONGO_FAIL_POINT(doNotForgetCoordinator))
                return Future<void>::makeReady();

            return txn::deleteCoordinatorDoc(*_scheduler, _lsid, _txnNumber);
        })
        .then([this] {
            LOG(3) << "Two-phase commit completed for " << _lsid.getId() << ':' << _txnNumber;

            stdx::unique_lock<stdx::mutex> ul(_mutex);
            _transitionToDone(std::move(ul));
        });
}

Future<void> TransactionCoordinator::_sendDecisionToParticipants(
    const std::vector<ShardId>& participantShards, CoordinatorCommitDecision decision) {
    invariant(_state == CoordinatorState::kPreparing);
    _decisionPromise.emplaceValue(decision.getDecision());

    switch (decision.getDecision()) {
        case CommitDecision::kCommit:
            _state = CoordinatorState::kCommitting;
            return txn::sendCommit(_serviceContext,
                                   *_scheduler,
                                   _lsid,
                                   _txnNumber,
                                   participantShards,
                                   *decision.getCommitTimestamp());
        case CommitDecision::kAbort:
            _state = CoordinatorState::kAborting;
            return txn::sendAbort(
                _serviceContext, *_scheduler, _lsid, _txnNumber, participantShards);
        case CommitDecision::kCanceled:
            MONGO_UNREACHABLE;
    };
    MONGO_UNREACHABLE;
};

void TransactionCoordinator::_handleCompletionError(Status s) {
    if (s.isOK()) {
        return;
    }

    stdx::unique_lock<stdx::mutex> lk(_mutex);

    LOG(3) << "Two-phase commit failed with error in state " << _state << " for " << _lsid.getId()
           << ':' << _txnNumber << causedBy(s);

    // If an error occurred prior to making a decision, set an error on the decision promise to
    // propagate it to callers of runCommit
    if (!_decisionPromise.getFuture().isReady()) {
        invariant(_state == CoordinatorState::kPreparing);

        // TransactionCoordinatorSteppingDown indicates the *sending* node (that is, *this* node) is
        // stepping down. Active coordinator tasks are interrupted with this code instead of
        // InterruptedDueToStepDown, because InterruptedDueToStepDown indicates the *receiving*
        // node was stepping down.
        if (s == ErrorCodes::TransactionCoordinatorSteppingDown) {
            s = Status(ErrorCodes::InterruptedDueToStepDown,
                       str::stream() << "Coordinator " << _lsid.getId() << ':' << _txnNumber
                                     << " stopping due to: "
                                     << s.reason());
        }

        _decisionPromise.setError(s);
    }

    _transitionToDone(std::move(lk));
}

void TransactionCoordinator::_transitionToDone(stdx::unique_lock<stdx::mutex> lk) noexcept {
    _state = CoordinatorState::kDone;

    auto promisesToTrigger = std::move(_completionPromises);
    lk.unlock();

    // No fields from 'this' are allowed to be accessed after the for loop below runs, because the
    // future handlers indicate to the potential lifetime controller that the object can be
    // destroyed
    for (auto&& promise : promisesToTrigger) {
        promise.emplaceValue();
    }
}

logger::LogstreamBuilder& operator<<(logger::LogstreamBuilder& stream,
                                     const TransactionCoordinator::CoordinatorState& state) {
    using State = TransactionCoordinator::CoordinatorState;
    // clang-format off
    switch (state) {
        case State::kInit:  stream.stream() << "kInit"; break;
        case State::kPreparing:   stream.stream() << "kPreparing"; break;
        case State::kAborting: stream.stream() << "kAborting"; break;
        case State::kCommitting: stream.stream() << "kCommitting"; break;
        case State::kDone: stream.stream() << "kDone"; break;
    };
    // clang-format on
    return stream;
}

}  // namespace mongo
