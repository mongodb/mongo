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
#include "mongo/db/s/transaction_coordinator_document_gen.h"
#include "mongo/db/s/transaction_coordinator_futures_util.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

using CoordinatorCommitDecision = TransactionCoordinator::CoordinatorCommitDecision;

CoordinatorCommitDecision makeDecisionFromPrepareVoteConsensus(
    ServiceContext* service,
    const txn::PrepareVoteConsensus& result,
    const LogicalSessionId& lsid,
    TxnNumber txnNumber) {
    invariant(result.decision);
    CoordinatorCommitDecision decision{*result.decision, boost::none};

    if (result.decision == txn::CommitDecision::kCommit) {
        invariant(result.maxPrepareTimestamp);

        decision.commitTimestamp = Timestamp(result.maxPrepareTimestamp->getSecs(),
                                             result.maxPrepareTimestamp->getInc() + 1);

        LOG(3) << "Advancing cluster time to commit Timestamp " << decision.commitTimestamp.get()
               << " of transaction " << txnNumber << " on session " << lsid.toBSON();

        uassertStatusOK(LogicalClock::get(service)->advanceClusterTime(
            LogicalTime(result.maxPrepareTimestamp.get())));
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
      _scheduler(std::move(scheduler)),
      _driver(serviceContext) {
    if (coordinateCommitDeadline) {
        _deadlineScheduler = _scheduler->makeChildScheduler();
        _deadlineScheduler
            ->scheduleWorkAt(*coordinateCommitDeadline,
                             [this](OperationContext* opCtx) { cancelIfCommitNotYetStarted(); })
            .getAsync([](const Status&) {});
    }
}

TransactionCoordinator::~TransactionCoordinator() {
    _cancelTimeoutWaitForCommitTask();

    if (_deadlineScheduler)
        _deadlineScheduler.reset();

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

    _driver.persistParticipantList(_lsid, _txnNumber, participantShards)
        .then([this, participantShards]() { return _runPhaseOne(participantShards); })
        .then([this, participantShards](CoordinatorCommitDecision decision) {
            return _runPhaseTwo(participantShards, decision);
        })
        .getAsync([this](Status s) { _handleCompletionStatus(s); });
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
        auto decision = doc.getDecision();
        if (!decision) {
            return _runPhaseOne(participantShards);
        } else {
            return (decision->decision == txn::CommitDecision::kCommit)
                ? CoordinatorCommitDecision{txn::CommitDecision::kCommit, decision->commitTimestamp}
                : CoordinatorCommitDecision{txn::CommitDecision::kAbort, boost::none};
        }
    };

    getDecision()
        .then([this, participantShards](CoordinatorCommitDecision decision) {
            return _runPhaseTwo(participantShards, decision);
        })
        .getAsync([this](Status s) { _handleCompletionStatus(s); });
}

Future<void> TransactionCoordinator::onCompletion() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_state == CoordinatorState::kDone) {
        return Future<void>::makeReady();
    }

    auto completionPromiseFuture = makePromiseFuture<void>();
    _completionPromises.push_back(std::move(completionPromiseFuture.promise));

    return std::move(completionPromiseFuture.future)
        .onError<ErrorCodes::TransactionCoordinatorSteppingDown>(
            [](const Status& s) { uasserted(ErrorCodes::InterruptedDueToStepDown, s.reason()); });
}

SharedSemiFuture<txn::CommitDecision> TransactionCoordinator::getDecision() {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    return _decisionPromise.getFuture();
}

void TransactionCoordinator::cancelIfCommitNotYetStarted() {
    _cancelTimeoutWaitForCommitTask();

    stdx::unique_lock<stdx::mutex> lk(_mutex);
    if (_state == CoordinatorState::kInit) {
        invariant(!_decisionPromise.getFuture().isReady());
        _decisionPromise.emplaceValue(txn::CommitDecision::kCanceled);
        _transitionToDone(std::move(lk));
    }
}

void TransactionCoordinator::_cancelTimeoutWaitForCommitTask() {
    if (_deadlineScheduler) {
        _deadlineScheduler->shutdown(
            {ErrorCodes::CallbackCanceled, "Interrupting the commit received deadline task"});
    }
}

Future<CoordinatorCommitDecision> TransactionCoordinator::_runPhaseOne(
    const std::vector<ShardId>& participantShards) {
    return _driver.sendPrepare(participantShards, _lsid, _txnNumber)
        .then([this, participantShards](txn::PrepareVoteConsensus result) {
            invariant(_state == CoordinatorState::kPreparing);

            auto decision =
                makeDecisionFromPrepareVoteConsensus(_serviceContext, result, _lsid, _txnNumber);

            return _driver
                .persistDecision(_lsid, _txnNumber, participantShards, decision.commitTimestamp)
                .then([decision] { return decision; });
        });
}

Future<void> TransactionCoordinator::_runPhaseTwo(const std::vector<ShardId>& participantShards,
                                                  const CoordinatorCommitDecision& decision) {
    return _sendDecisionToParticipants(participantShards, decision)
        .then([this] {
            if (getGlobalFailPointRegistry()
                    ->getFailPoint("doNotForgetCoordinator")
                    ->shouldFail()) {
                return Future<void>::makeReady();
            }

            return _driver.deleteCoordinatorDoc(_lsid, _txnNumber);
        })
        .then([this] {
            LOG(3) << "Two-phase commit completed for session " << _lsid.toBSON()
                   << ", transaction number " << _txnNumber;

            stdx::unique_lock<stdx::mutex> ul(_mutex);
            _transitionToDone(std::move(ul));
        });
}

Future<void> TransactionCoordinator::_sendDecisionToParticipants(
    const std::vector<ShardId>& participantShards, CoordinatorCommitDecision decision) {
    invariant(_state == CoordinatorState::kPreparing);
    _decisionPromise.emplaceValue(decision.decision);

    // Send the decision to all participants.
    switch (decision.decision) {
        case txn::CommitDecision::kCommit:
            _state = CoordinatorState::kCommitting;
            invariant(decision.commitTimestamp);
            return _driver.sendCommit(
                participantShards, _lsid, _txnNumber, decision.commitTimestamp.get());
        case txn::CommitDecision::kAbort:
            _state = CoordinatorState::kAborting;
            return _driver.sendAbort(participantShards, _lsid, _txnNumber);
        case txn::CommitDecision::kCanceled:
            MONGO_UNREACHABLE;
    };
    MONGO_UNREACHABLE;
};

void TransactionCoordinator::_handleCompletionStatus(Status s) {
    if (s.isOK()) {
        return;
    }

    stdx::unique_lock<stdx::mutex> lk(_mutex);

    LOG(3) << "Two-phase commit failed with error in state " << _state << " for transaction "
           << _txnNumber << " on session " << _lsid.toBSON() << causedBy(s);

    // If an error occurred prior to making a decision, set an error on the decision promise to
    // propagate it to callers of runCommit
    if (!_decisionPromise.getFuture().isReady()) {
        invariant(_state == CoordinatorState::kPreparing);
        _decisionPromise.setError(s == ErrorCodes::TransactionCoordinatorReachedAbortDecision
                                      ? Status{ErrorCodes::InterruptedDueToStepDown, s.reason()}
                                      : s);
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

StatusWith<CoordinatorCommitDecision> CoordinatorCommitDecision::fromBSON(const BSONObj& doc) {
    CoordinatorCommitDecision decision;

    for (const auto& e : doc) {
        const auto fieldName = e.fieldNameStringData();

        if (fieldName == "decision") {
            if (e.type() != String) {
                return Status(ErrorCodes::TypeMismatch, "decision must be a string");
            }

            if (e.str() == "commit") {
                decision.decision = txn::CommitDecision::kCommit;
            } else if (e.str() == "abort") {
                decision.decision = txn::CommitDecision::kAbort;
            } else {
                return Status(ErrorCodes::BadValue, "decision must be either 'abort' or 'commit'");
            }
        } else if (fieldName == "commitTimestamp") {
            if (e.type() != bsonTimestamp && e.type() != Date) {
                return Status(ErrorCodes::TypeMismatch, "commit timestamp must be a timestamp");
            }
            decision.commitTimestamp = {e.timestamp()};
        }
    }

    if (decision.decision == txn::CommitDecision::kAbort && decision.commitTimestamp) {
        return Status(ErrorCodes::BadValue, "abort decision cannot have a timestamp");
    }
    if (decision.decision == txn::CommitDecision::kCommit && !decision.commitTimestamp) {
        return Status(ErrorCodes::BadValue, "commit decision must have a timestamp");
    }

    return decision;
}

BSONObj CoordinatorCommitDecision::toBSON() const {
    BSONObjBuilder builder;

    if (decision == txn::CommitDecision::kCommit) {
        builder.append("decision", "commit");
    } else {
        builder.append("decision", "abort");
    }
    if (commitTimestamp) {
        builder.append("commitTimestamp", *commitTimestamp);
    }

    return builder.obj();
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

logger::LogstreamBuilder& operator<<(logger::LogstreamBuilder& stream,
                                     const txn::CommitDecision& decision) {
    // clang-format off
    switch (decision) {
        case txn::CommitDecision::kCommit:     stream.stream() << "kCommit"; break;
        case txn::CommitDecision::kAbort:      stream.stream() << "kAbort"; break;
        case txn::CommitDecision::kCanceled:   stream.stream() << "kCanceled"; break;
    };
    // clang-format on
    return stream;
}

}  // namespace mongo
