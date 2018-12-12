
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
#include "mongo/db/transaction_coordinator_util.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {

txn::CoordinatorCommitDecision makeDecisionFromPrepareVoteConsensus(
    const txn::PrepareVoteConsensus& result,
    const LogicalSessionId& lsid,
    const TxnNumber& txnNumber) {
    invariant(result.decision);
    txn::CoordinatorCommitDecision decision{result.decision.get(), boost::none};

    if (result.decision == TransactionCoordinator::CommitDecision::kCommit) {
        invariant(result.maxPrepareTimestamp);

        decision.commitTimestamp = Timestamp(result.maxPrepareTimestamp->getSecs(),
                                             result.maxPrepareTimestamp->getInc() + 1);

        LOG(3) << "Advancing cluster time to commit Timestamp " << decision.commitTimestamp.get()
               << " of transaction " << txnNumber << " on session " << lsid.toBSON();

        uassertStatusOK(LogicalClock::get(getGlobalServiceContext())
                            ->advanceClusterTime(LogicalTime(result.maxPrepareTimestamp.get())));
    }

    return decision;
}

}  // namespace

TransactionCoordinator::~TransactionCoordinator() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(_state == TransactionCoordinator::CoordinatorState::kDone);
    // Make sure no callers of functions on the coordinator are waiting for a decision to be
    // signaled or the commit process to complete.
    invariant(_completionPromises.size() == 0);
}

/**
 * Implements the high-level logic for two-phase commit.
 */
SharedSemiFuture<TransactionCoordinator::CommitDecision> TransactionCoordinator::runCommit(
    const std::vector<ShardId>& participantShards) {
    {
        // If another thread has already begun the commit process, return early.
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        if (_state != CoordinatorState::kInit) {
            return _finalDecisionPromise.getFuture();
        }
        _state = CoordinatorState::kPreparing;
    }

    auto coordinator = shared_from_this();
    txn::async(_callbackPool,
               [coordinator, participantShards] {
                   auto opCtx = Client::getCurrent()->makeOperationContext();
                   txn::persistParticipantList(
                       opCtx.get(), coordinator->_lsid, coordinator->_txnNumber, participantShards);
               })
        .then([coordinator, participantShards]() {
            return coordinator->_runPhaseOne(participantShards);
        })
        .then([coordinator, participantShards](txn::CoordinatorCommitDecision decision) {
            return coordinator->_runPhaseTwo(participantShards, decision);
        })
        .getAsync([coordinator](Status s) { coordinator->_handleCompletionStatus(s); });

    return _finalDecisionPromise.getFuture();
}

Future<txn::CoordinatorCommitDecision> TransactionCoordinator::_runPhaseOne(
    const std::vector<ShardId>& participantShards) {
    auto coordinator = shared_from_this();
    return txn::sendPrepare(coordinator,
                            coordinator->_networkExecutor,
                            coordinator->_callbackPool,
                            participantShards,
                            coordinator->_lsid,
                            coordinator->_txnNumber)
        .then([coordinator, participantShards](txn::PrepareVoteConsensus result) {
            invariant(coordinator->_state == CoordinatorState::kPreparing);

            return txn::async(coordinator->_callbackPool, [coordinator, result, participantShards] {
                auto opCtx = Client::getCurrent()->makeOperationContext();

                const auto decision = makeDecisionFromPrepareVoteConsensus(
                    result, coordinator->_lsid, coordinator->_txnNumber);

                txn::persistDecision(opCtx.get(),
                                     coordinator->_lsid,
                                     coordinator->_txnNumber,
                                     participantShards,
                                     decision.commitTimestamp);
                return decision;
            });
        });
}

Future<void> TransactionCoordinator::_runPhaseTwo(const std::vector<ShardId>& participantShards,
                                                  const txn::CoordinatorCommitDecision& decision) {
    auto coordinator = shared_from_this();
    return _sendDecisionToParticipants(participantShards, decision)
        .then([coordinator]() {
            stdx::unique_lock<stdx::mutex> lk(coordinator->_mutex);
            coordinator->_transitionToDone(std::move(lk));
        })
        .then([coordinator] {
            return txn::async(coordinator->_callbackPool, [coordinator] {
                auto opCtx = Client::getCurrent()->makeOperationContext();
                return txn::deleteCoordinatorDoc(
                    opCtx.get(), coordinator->_lsid, coordinator->_txnNumber);
            });
        })
        .then([coordinator]() {
            stdx::unique_lock<stdx::mutex> lk(coordinator->_mutex);
            LOG(3) << "Two-phase commit completed for session " << coordinator->_lsid.toBSON()
                   << ", transaction number " << coordinator->_txnNumber;
        });
}

void TransactionCoordinator::continueCommit(const TransactionCoordinatorDocument& doc) {
    _state = CoordinatorState::kPreparing;
    auto coordinator = shared_from_this();
    const auto participantShards = doc.getParticipants();

    if (!doc.getDecision()) {
        _runPhaseOne(participantShards)
            .then([coordinator, participantShards](txn::CoordinatorCommitDecision decision) {
                return coordinator->_runPhaseTwo(participantShards, decision);
            })
            .getAsync([coordinator](Status s) { coordinator->_handleCompletionStatus(s); });
        return;
    }

    txn::CoordinatorCommitDecision decision;
    if (*doc.getDecision() == "commit") {
        decision = txn::CoordinatorCommitDecision{TransactionCoordinator::CommitDecision::kCommit,
                                                  *doc.getCommitTimestamp()};
    } else if (*doc.getDecision() == "abort") {
        decision = txn::CoordinatorCommitDecision{TransactionCoordinator::CommitDecision::kAbort,
                                                  boost::none};
    }
    _runPhaseTwo(participantShards, decision).getAsync([coordinator](Status s) {
        coordinator->_handleCompletionStatus(s);
    });
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
    const std::vector<ShardId>& participantShards, txn::CoordinatorCommitDecision decision) {
    invariant(_state == CoordinatorState::kPreparing);

    _finalDecisionPromise.emplaceValue(decision.decision);

    // Send the decision to all participants.
    switch (decision.decision) {
        case CommitDecision::kCommit:
            _state = CoordinatorState::kCommitting;
            invariant(decision.commitTimestamp);
            return txn::sendCommit(_networkExecutor,
                                   _callbackPool,
                                   participantShards,
                                   _lsid,
                                   _txnNumber,
                                   decision.commitTimestamp.get());
        case CommitDecision::kAbort:
            _state = CoordinatorState::kAborting;
            return txn::sendAbort(
                _networkExecutor, _callbackPool, participantShards, _lsid, _txnNumber);
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

    std::vector<Promise<void>> promisesToTrigger;
    std::swap(promisesToTrigger, _completionPromises);
    lk.unlock();

    for (auto&& promise : promisesToTrigger) {
        promise.emplaceValue();
    }
}
}  // namespace mongo
