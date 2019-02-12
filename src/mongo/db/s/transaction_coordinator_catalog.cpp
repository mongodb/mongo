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

#include "mongo/db/s/transaction_coordinator_catalog.h"

#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"

namespace mongo {

// TODO (SERVER-37886): Remove this failpoint once failover can be tested on coordinators that have
// a local participant.
MONGO_FAIL_POINT_DEFINE(doNotForgetCoordinator);

TransactionCoordinatorCatalog::TransactionCoordinatorCatalog() = default;

TransactionCoordinatorCatalog::~TransactionCoordinatorCatalog() {
    join();
}

void TransactionCoordinatorCatalog::exitStepUp(Status status) {
    if (status.isOK()) {
        LOG(0) << "Incoming coordinateCommit requests are now enabled";
    } else {
        warning() << "Coordinator recovery failed and coordinateCommit requests will not be allowed"
                  << causedBy(status);
    }

    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(!_stepUpCompletionStatus);
    _stepUpCompletionStatus = std::move(status);
    _stepUpCompleteCV.notify_all();
}

void TransactionCoordinatorCatalog::onStepDown() {
    stdx::unique_lock<stdx::mutex> ul(_mutex);

    std::vector<std::shared_ptr<TransactionCoordinator>> coordinatorsToCancel;
    for (auto && [ sessionId, coordinatorsForSession ] : _coordinatorsBySession) {
        for (auto && [ txnNumber, coordinator ] : coordinatorsForSession) {
            coordinatorsToCancel.emplace_back(coordinator);
        }
    }

    ul.unlock();

    for (auto&& coordinator : coordinatorsToCancel) {
        coordinator->cancelIfCommitNotYetStarted();
    }

    ul.lock();
    _cleanupCompletedCoordinators(ul);
}

void TransactionCoordinatorCatalog::insert(OperationContext* opCtx,
                                           const LogicalSessionId& lsid,
                                           TxnNumber txnNumber,
                                           std::shared_ptr<TransactionCoordinator> coordinator,
                                           bool forStepUp) {
    stdx::unique_lock<stdx::mutex> ul(_mutex);
    auto cleanupCoordinatorsGuard = makeGuard([&] { _cleanupCompletedCoordinators(ul); });
    if (!forStepUp) {
        _waitForStepUpToComplete(ul, opCtx);
    }

    auto& coordinatorsBySession = _coordinatorsBySession[lsid];

    // We should never try to insert a coordinator if one already exists for this session and txn
    // number. Logic for avoiding this due to e.g. malformed commands should be handled external to
    // the catalog.
    invariant(coordinatorsBySession.find(txnNumber) == coordinatorsBySession.end(),
              "Cannot insert a TransactionCoordinator into the TransactionCoordinatorCatalog with "
              "the same session ID and transaction number as a previous coordinator");

    // Schedule callback to remove coordinator from catalog when it either commits or aborts.
    coordinator->onCompletion().getAsync(
        [this, lsid, txnNumber](Status) { _remove(lsid, txnNumber); });

    LOG(3) << "Inserting coordinator for transaction " << txnNumber << " on session "
           << lsid.toBSON() << " into in-memory catalog";

    coordinatorsBySession[txnNumber] = std::move(coordinator);
}

std::shared_ptr<TransactionCoordinator> TransactionCoordinatorCatalog::get(
    OperationContext* opCtx, const LogicalSessionId& lsid, TxnNumber txnNumber) {
    stdx::unique_lock<stdx::mutex> ul(_mutex);
    auto cleanupCoordinatorsGuard = makeGuard([&] { _cleanupCompletedCoordinators(ul); });
    _waitForStepUpToComplete(ul, opCtx);

    std::shared_ptr<TransactionCoordinator> coordinatorToReturn;

    auto coordinatorsForSessionIter = _coordinatorsBySession.find(lsid);
    if (coordinatorsForSessionIter != _coordinatorsBySession.end()) {
        const auto& coordinatorsForSession = coordinatorsForSessionIter->second;
        auto coordinatorForTxnIter = coordinatorsForSession.find(txnNumber);
        if (coordinatorForTxnIter != coordinatorsForSession.end()) {
            coordinatorToReturn = coordinatorForTxnIter->second;
        }
    }

    if (MONGO_FAIL_POINT(doNotForgetCoordinator) && !coordinatorToReturn) {
        // If the failpoint is on and we couldn't find the coordinator in the main catalog, fall
        // back to the "defunct" catalog, which stores coordinators that have completed and would
        // normally be forgotten.
        auto coordinatorsForSessionIter = _coordinatorsBySessionDefunct.find(lsid);
        if (coordinatorsForSessionIter != _coordinatorsBySessionDefunct.end()) {
            const auto& coordinatorsForSession = coordinatorsForSessionIter->second;
            auto coordinatorForTxnIter = coordinatorsForSession.find(txnNumber);
            if (coordinatorForTxnIter != coordinatorsForSession.end()) {
                coordinatorToReturn = coordinatorForTxnIter->second;
            }
        }
    }

    return coordinatorToReturn;
}

boost::optional<std::pair<TxnNumber, std::shared_ptr<TransactionCoordinator>>>
TransactionCoordinatorCatalog::getLatestOnSession(OperationContext* opCtx,
                                                  const LogicalSessionId& lsid) {
    stdx::unique_lock<stdx::mutex> ul(_mutex);
    auto cleanupCoordinatorsGuard = makeGuard([&] { _cleanupCompletedCoordinators(ul); });
    _waitForStepUpToComplete(ul, opCtx);

    const auto& coordinatorsForSessionIter = _coordinatorsBySession.find(lsid);

    if (coordinatorsForSessionIter == _coordinatorsBySession.end()) {
        return boost::none;
    }

    const auto& coordinatorsForSession = coordinatorsForSessionIter->second;

    // We should never have empty map for a session because entries for sessions with no
    // transactions are removed
    invariant(!coordinatorsForSession.empty());

    const auto& lastCoordinatorOnSession = coordinatorsForSession.begin();
    return std::make_pair(lastCoordinatorOnSession->first, lastCoordinatorOnSession->second);
}

void TransactionCoordinatorCatalog::_remove(const LogicalSessionId& lsid, TxnNumber txnNumber) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    LOG(3) << "Removing coordinator for transaction " << txnNumber << " on session "
           << lsid.toBSON() << " from in-memory catalog";

    const auto& coordinatorsForSessionIter = _coordinatorsBySession.find(lsid);

    if (coordinatorsForSessionIter != _coordinatorsBySession.end()) {
        auto& coordinatorsForSession = coordinatorsForSessionIter->second;
        const auto& coordinatorForTxnIter = coordinatorsForSession.find(txnNumber);

        if (coordinatorForTxnIter != coordinatorsForSession.end()) {
            auto coordinator = coordinatorForTxnIter->second;

            if (MONGO_FAIL_POINT(doNotForgetCoordinator)) {
                auto decisionFuture = coordinator->getDecision();
                invariant(decisionFuture.isReady());
                // Only remember a coordinator that completed successfully. We expect that the
                // coordinator only completes with an error if the node stepped down or was shut
                // down while coordinating the commit. If either of these occurred, a
                // coordinateCommitTransaction retry will either find a new coordinator in the real
                // catalog (if the coordinator's state was made durable before the failover or
                // shutdown), or should find no coordinator and instead recover the decision from
                // the local participant (if the failover or shutdown occurred before any of the
                // coordinator's state was made durable).
                if (decisionFuture.getNoThrow().isOK()) {
                    _coordinatorsBySessionDefunct[lsid][txnNumber] = std::move(coordinator);
                }
            }

            // Since the '_remove' method executes on the AWS of the coordinator which is being
            // removed, we cannot destroy it inline. Because of this, put it on a cleanup list so
            // that subsequent catalog operations will perform the cleanup.
            _coordinatorsToCleanup.emplace_back(coordinatorForTxnIter->second);

            coordinatorsForSession.erase(coordinatorForTxnIter);
            if (coordinatorsForSession.empty()) {
                _coordinatorsBySession.erase(coordinatorsForSessionIter);
            }
        }
    }

    if (_coordinatorsBySession.empty()) {
        LOG(3) << "Signaling last active coordinator removed";
        _noActiveCoordinatorsCV.notify_all();
    }
}

void TransactionCoordinatorCatalog::join() {
    stdx::unique_lock<stdx::mutex> ul(_mutex);

    while (!_noActiveCoordinatorsCV.wait_for(
        ul, stdx::chrono::seconds{5}, [this] { return _coordinatorsBySession.empty(); })) {
        LOG(0) << "After 5 seconds of wait there are still " << _coordinatorsBySession.size()
               << " sessions left with active coordinators which have not yet completed";
        LOG(0) << _toString(ul);
    }
}

std::string TransactionCoordinatorCatalog::toString() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _toString(lk);
}

void TransactionCoordinatorCatalog::_waitForStepUpToComplete(stdx::unique_lock<stdx::mutex>& lk,
                                                             OperationContext* opCtx) {
    invariant(lk.owns_lock());
    opCtx->waitForConditionOrInterrupt(
        _stepUpCompleteCV, lk, [this]() { return bool(_stepUpCompletionStatus); });

    uassertStatusOK(*_stepUpCompletionStatus);
}

void TransactionCoordinatorCatalog::_cleanupCompletedCoordinators(
    stdx::unique_lock<stdx::mutex>& ul) {
    invariant(ul.owns_lock());
    auto coordinatorsToCleanup = std::move(_coordinatorsToCleanup);

    // Ensure the destructors run outside of the lock in order to minimize the time this methods
    // spends in a critical section
    ul.unlock();
}

std::string TransactionCoordinatorCatalog::_toString(WithLock wl) const {
    StringBuilder ss;
    ss << "[";
    for (const auto& coordinatorsForSession : _coordinatorsBySession) {
        ss << "\n" << coordinatorsForSession.first.toBSON() << ": ";
        for (const auto& coordinator : coordinatorsForSession.second) {
            ss << coordinator.first << ",";
        }
    }
    ss << "]";
    return ss.str();
}

}  // namespace mongo
