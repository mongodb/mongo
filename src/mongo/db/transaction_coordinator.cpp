
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

#include "mongo/db/transaction_coordinator.h"

#include "mongo/db/logical_clock.h"
#include "mongo/db/transaction_coordinator_futures_util.h"
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

TransactionCoordinator::TransactionCoordinator(ServiceContext* service,
                                               executor::TaskExecutor* networkExecutor,
                                               ThreadPool* callbackPool,
                                               const LogicalSessionId& lsid,
                                               const TxnNumber& txnNumber)
    : _service(service),
      _driver(networkExecutor, callbackPool),
      _lsid(lsid),
      _txnNumber(txnNumber),
      _state(CoordinatorState::kInit) {}

TransactionCoordinator::~TransactionCoordinator() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(_state == TransactionCoordinator::CoordinatorState::kDone);

    // Make sure no callers of functions on the coordinator are waiting for a decision to be
    // signaled or the commit process to complete.
    invariant(_completionPromises.empty());
}

/**
 * Implements the high-level logic for two-phase commit.
 */
SharedSemiFuture<txn::CommitDecision> TransactionCoordinator::runCommit(
    const std::vector<ShardId>& participantShards) {
    {
        // If another thread has already begun the commit process, return early.
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        if (_state != CoordinatorState::kInit) {
            return _finalDecisionPromise.getFuture();
        }
        _state = CoordinatorState::kPreparing;
    }

    _driver.persistParticipantList(_lsid, _txnNumber, participantShards)
        .then([this, participantShards]() { return _runPhaseOne(participantShards); })
        .then([this, participantShards](CoordinatorCommitDecision decision) {
            return _runPhaseTwo(participantShards, decision);
        })
        .getAsync([this](Status s) { _handleCompletionStatus(s); });

    return _finalDecisionPromise.getFuture();
}

Future<CoordinatorCommitDecision> TransactionCoordinator::_runPhaseOne(
    const std::vector<ShardId>& participantShards) {
    return _driver.sendPrepare(participantShards, _lsid, _txnNumber)
        .then([this, participantShards](txn::PrepareVoteConsensus result) {
            invariant(_state == CoordinatorState::kPreparing);

            auto decision =
                makeDecisionFromPrepareVoteConsensus(_service, result, _lsid, _txnNumber);

            return _driver
                .persistDecision(_lsid, _txnNumber, participantShards, decision.commitTimestamp)
                .then([decision] { return decision; });
        });
}

Future<void> TransactionCoordinator::_runPhaseTwo(const std::vector<ShardId>& participantShards,
                                                  const CoordinatorCommitDecision& decision) {
    return _sendDecisionToParticipants(participantShards, decision)
        .then([this] { return _driver.deleteCoordinatorDoc(_lsid, _txnNumber); })
        .then([this] {
            LOG(3) << "Two-phase commit completed for session " << _lsid.toBSON()
                   << ", transaction number " << _txnNumber;

            stdx::unique_lock<stdx::mutex> ul(_mutex);
            _transitionToDone(std::move(ul));
        });
}

void TransactionCoordinator::continueCommit(const TransactionCoordinatorDocument& doc) {
    _state = CoordinatorState::kPreparing;
    const auto participantShards = doc.getParticipants();

    // Helper lambda to get the decision either from the document passed in or from the participants
    // (by performing 'phase one' of two-phase commit).
    auto getDecision = [&]() -> Future<CoordinatorCommitDecision> {
        if (!doc.getDecision()) {
            return _runPhaseOne(participantShards);
        } else {
            return (*doc.getDecision() == "commit")
                ? CoordinatorCommitDecision{txn::CommitDecision::kCommit, *doc.getCommitTimestamp()}
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
    return std::move(completionPromiseFuture.future);
}

void TransactionCoordinator::cancelIfCommitNotYetStarted() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    if (_state == CoordinatorState::kInit) {
        _transitionToDone(std::move(lk));
    }
}

Future<void> TransactionCoordinator::_sendDecisionToParticipants(
    const std::vector<ShardId>& participantShards, CoordinatorCommitDecision decision) {
    invariant(_state == CoordinatorState::kPreparing);
    _finalDecisionPromise.emplaceValue(decision.decision);

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

    // If an error occurred prior to making a decision, set an error on the decision
    // promise to propagate it to callers of runCommit.
    if (!_finalDecisionPromise.getFuture().isReady()) {
        invariant(_state == CoordinatorState::kPreparing);
        _finalDecisionPromise.setError(s);
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

}  // namespace mongo
