
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

#include "mongo/db/transaction_coordinator_util.h"
#include "mongo/util/log.h"

namespace mongo {

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
        stdx::lock_guard<stdx::mutex> lk(_mutex);

        // If another thread has already begun the commit process, return early.
        if (_state != CoordinatorState::kInit) {
            return _finalDecisionPromise.getFuture();
        }

        _state = CoordinatorState::kPreparing;
    }

    auto coordinator = shared_from_this();
    txn::async(_callbackPool, []() { txn::persistParticipantList(); })
        .then([coordinator, participantShards]() {
            return txn::sendPrepare(coordinator,
                                    coordinator->_networkExecutor,
                                    coordinator->_callbackPool,
                                    participantShards,
                                    coordinator->_lsid,
                                    coordinator->_txnNumber);
        })
        .then([coordinator](txn::PrepareVoteConsensus response) {
            return coordinator->_persistDecision(response);
        })
        .then([coordinator, participantShards](txn::CoordinatorCommitDecision decision) {
            // Send the decision and then propagate it down the continuation chain.
            return coordinator->_sendDecisionToParticipants(participantShards, decision)
                .then([decision] { return decision.decision; });
        })
        .then([coordinator](CommitDecision finalDecision) {
            stdx::unique_lock<stdx::mutex> lk(coordinator->_mutex);
            LOG(3) << "Two-phase commit completed successfully with decision " << finalDecision
                   << " for session " << coordinator->_lsid.toBSON() << ", transaction number "
                   << coordinator->_txnNumber;
            coordinator->_transitionToDone(std::move(lk));
        })
        .then([coordinator] { return coordinator->_deleteDecision(); })
        .onError([coordinator](Status s) {
            stdx::unique_lock<stdx::mutex> lk(coordinator->_mutex);
            LOG(3) << "Two-phase commit failed with error in state " << coordinator->_state
                   << " for session " << coordinator->_lsid.toBSON() << ", transaction number "
                   << coordinator->_txnNumber << causedBy(s);
            // If an error occurred prior to making a decision, set an error on the decision
            // promise to propagate it to callers of runCommit.
            if (!coordinator->_finalDecisionPromise.getFuture().isReady()) {
                invariant(coordinator->_state == CoordinatorState::kPreparing);
                coordinator->_finalDecisionPromise.setError(s);
            }
            coordinator->_transitionToDone(std::move(lk));
        })
        .getAsync([](Status s) {});  // "Detach" the future chain, swallowing errors.

    return _finalDecisionPromise.getFuture();
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

Future<txn::CoordinatorCommitDecision> TransactionCoordinator::_persistDecision(
    const txn::PrepareVoteConsensus& prepareResponse) {
    invariant(_state == CoordinatorState::kPreparing);
    // TODO (SERVER-36853): Implement persistence of decision.
    // TODO (SERVER-36853): Handle errors appropriately.
    return txn::async(_callbackPool,
                      [prepareResponse]() { return persistDecision(prepareResponse); });
}

Future<void> TransactionCoordinator::_sendDecisionToParticipants(
    const std::vector<ShardId>& participantShards,
    txn::CoordinatorCommitDecision coordinatorDecision) {
    invariant(_state == CoordinatorState::kPreparing);

    _finalDecisionPromise.emplaceValue(coordinatorDecision.decision);

    // Send the decision to all participants.
    switch (coordinatorDecision.decision) {
        case CommitDecision::kCommit:
            _state = CoordinatorState::kCommitting;
            invariant(coordinatorDecision.commitTimestamp);
            return txn::sendCommit(_networkExecutor,
                                   _callbackPool,
                                   participantShards,
                                   _lsid,
                                   _txnNumber,
                                   coordinatorDecision.commitTimestamp.get());
        case CommitDecision::kAbort:
            _state = CoordinatorState::kAborting;
            return txn::sendAbort(
                _networkExecutor, _callbackPool, participantShards, _lsid, _txnNumber);
    };
    MONGO_UNREACHABLE;
};

Future<void> TransactionCoordinator::_deleteDecision() {
    invariant(_state == CoordinatorState::kDone);
    // TODO (SERVER-36853): Implement deletion of decision.
    return Future<void>::makeReady();
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
