
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

#include "mongo/db/transaction_coordinator_catalog.h"

#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"

namespace mongo {

// TODO (SERVER-37886): Remove this failpoint once failover can be tested on coordinators that have
// a local participant.
MONGO_FAIL_POINT_DEFINE(doNotForgetCoordinator);

TransactionCoordinatorCatalog::TransactionCoordinatorCatalog() = default;

TransactionCoordinatorCatalog::~TransactionCoordinatorCatalog() = default;

void TransactionCoordinatorCatalog::insert(OperationContext* opCtx,
                                           LogicalSessionId lsid,
                                           TxnNumber txnNumber,
                                           std::shared_ptr<TransactionCoordinator> coordinator,
                                           bool forStepUp) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    if (!forStepUp) {
        _waitForStepUpToComplete(lk, opCtx);
    }

    auto& coordinatorsBySession = _coordinatorsBySession[lsid];

    // We should never try to insert a coordinator if one already exists for this session and txn
    // number. Logic for avoiding this due to e.g. malformed commands should be handled external to
    // the catalog.
    invariant(coordinatorsBySession.find(txnNumber) == coordinatorsBySession.end(),
              "Cannot insert a TransactionCoordinator into the TransactionCoordinatorCatalog with "
              "the same session ID and transaction number as a previous coordinator");

    // Schedule callback to remove coordinator from catalog when it either commits or aborts.
    coordinator->onCompletion().getAsync([
        catalogWeakPtr = std::weak_ptr<TransactionCoordinatorCatalog>(shared_from_this()),
        lsid,
        txnNumber
    ](Status) {
        if (auto catalog = catalogWeakPtr.lock()) {
            catalog->remove(lsid, txnNumber);
        }
    });

    LOG(3) << "Inserting coordinator for transaction " << txnNumber << " on session "
           << lsid.toBSON() << " into in-memory catalog";
    coordinatorsBySession[txnNumber] = std::move(coordinator);
}

std::shared_ptr<TransactionCoordinator> TransactionCoordinatorCatalog::get(OperationContext* opCtx,
                                                                           LogicalSessionId lsid,
                                                                           TxnNumber txnNumber) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _waitForStepUpToComplete(lk, opCtx);

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
TransactionCoordinatorCatalog::getLatestOnSession(OperationContext* opCtx, LogicalSessionId lsid) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _waitForStepUpToComplete(lk, opCtx);

    const auto& coordinatorsForSessionIter = _coordinatorsBySession.find(lsid);

    if (coordinatorsForSessionIter == _coordinatorsBySession.end()) {
        return boost::none;
    }

    const auto& coordinatorsForSession = coordinatorsForSessionIter->second;
    const auto& lastCoordinatorOnSession = coordinatorsForSession.rbegin();

    // Should never have empty map for a session. Entries for sessions with no transactions should
    // be removed.
    invariant(lastCoordinatorOnSession != coordinatorsForSession.rend());

    return std::make_pair(lastCoordinatorOnSession->first, lastCoordinatorOnSession->second);
}

void TransactionCoordinatorCatalog::remove(LogicalSessionId lsid, TxnNumber txnNumber) {
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

            coordinatorsForSession.erase(coordinatorForTxnIter);
            if (coordinatorsForSession.size() == 0) {
                _coordinatorsBySession.erase(coordinatorsForSessionIter);
            }
        }
    }

    if (_coordinatorsBySession.empty()) {
        LOG(3) << "Signaling last active coordinator removed";
        _noActiveCoordinatorsCv.notify_all();
    }
}

void TransactionCoordinatorCatalog::enterStepUp(OperationContext* opCtx) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    // If this node stepped down and stepped back up, the asynchronous stepUp task from the previous
    // stepUp may still be running, so wait for the previous stepUp task to complete.
    LOG(3) << "Waiting for coordinator stepup task from previous term, if any, to complete";
    _waitForStepUpToComplete(lk, opCtx);

    _stepUpInProgress = true;

    LOG(3) << "Waiting for there to be no active coordinators; current coordinator catalog: "
           << this->_toString(lk);
    opCtx->waitForConditionOrInterrupt(
        _noActiveCoordinatorsCv, lk, [this]() { return _coordinatorsBySession.empty(); });
}

void TransactionCoordinatorCatalog::exitStepUp() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(_stepUpInProgress);

    LOG(3) << "Signaling stepup complete";
    _stepUpInProgress = false;
    _noStepUpInProgressCv.notify_all();
}

void TransactionCoordinatorCatalog::_waitForStepUpToComplete(stdx::unique_lock<stdx::mutex>& lk,
                                                             OperationContext* opCtx) {
    invariant(lk.owns_lock());
    opCtx->waitForConditionOrInterrupt(
        _noStepUpInProgressCv, lk, [this]() { return !_stepUpInProgress; });
}

std::string TransactionCoordinatorCatalog::toString() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _toString(lk);
}

std::string TransactionCoordinatorCatalog::_toString(WithLock wl) {
    StringBuilder ss;
    ss << "[";
    for (auto coordinatorsForSession = _coordinatorsBySession.begin();
         coordinatorsForSession != _coordinatorsBySession.end();
         ++coordinatorsForSession) {
        ss << "\n";
        ss << coordinatorsForSession->first.toBSON() << ": ";
        for (auto coordinatorForTxnNumber = coordinatorsForSession->second.begin();
             coordinatorForTxnNumber != coordinatorsForSession->second.end();
             ++coordinatorForTxnNumber) {
            ss << coordinatorForTxnNumber->first << " ";
        }
    }
    ss << "]";
    return ss.str();
}

}  // namespace mongo
