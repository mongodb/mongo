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


// IWYU pragma: no_include "cxxabi.h"
#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <chrono>
#include <type_traits>
#include <vector>

#include "mongo/base/status_with.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/s/transaction_coordinator_catalog.h"
#include "mongo/db/s/transaction_coordinator_structures.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/s/grid.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTransaction


namespace mongo {

TransactionCoordinatorCatalog::TransactionCoordinatorCatalog() = default;

TransactionCoordinatorCatalog::~TransactionCoordinatorCatalog() {
    join();
}

void TransactionCoordinatorCatalog::exitStepUp(Status status) {
    if (status.isOK()) {
        LOGV2(22438, "Incoming coordinateCommit requests are now enabled");
    } else {
        LOGV2_WARNING(22444,
                      "Coordinator recovery failed and coordinateCommit requests will not be "
                      "allowed: {error}",
                      "Coordinator recovery failed and coordinateCommit requests will not be "
                      "allowed",
                      "error"_attr = status);
    }

    stdx::lock_guard<Latch> lk(_mutex);
    invariant(!_stepUpCompletionStatus);
    _stepUpCompletionStatus = std::move(status);
    _stepUpCompleteCV.notify_all();
}

void TransactionCoordinatorCatalog::onStepDown() {
    stdx::unique_lock<Latch> ul(_mutex);

    std::vector<std::shared_ptr<TransactionCoordinator>> coordinatorsToCancel;
    for (auto&& [sessionId, coordinatorsForSession] : _coordinatorsBySession) {
        for (auto&& [txnNumber, coordinatorsForTxnNumber] : coordinatorsForSession) {
            for (auto&& [txnRetryCounter, coordinator] : coordinatorsForTxnNumber) {
                coordinatorsToCancel.emplace_back(coordinator);
            }
        }
    }

    ul.unlock();

    for (auto&& coordinator : coordinatorsToCancel) {
        coordinator->cancelIfCommitNotYetStarted();
    }
}

void TransactionCoordinatorCatalog::insert(OperationContext* opCtx,
                                           const LogicalSessionId& lsid,
                                           const TxnNumberAndRetryCounter& txnNumberAndRetryCounter,
                                           std::shared_ptr<TransactionCoordinator> coordinator,
                                           bool forStepUp) {
    LOGV2_DEBUG(
        22439,
        3,
        "{sessionId}:{txnNumberAndRetryCounter} Inserting coordinator into in-memory catalog",
        "Inserting coordinator into in-memory catalog",
        "sessionId"_attr = lsid.getId(),
        "txnNumberAndRetryCounter"_attr = txnNumberAndRetryCounter);

    auto txnNumber = txnNumberAndRetryCounter.getTxnNumber();
    auto txnRetryCounter = *txnNumberAndRetryCounter.getTxnRetryCounter();

    stdx::unique_lock<Latch> ul(_mutex);
    if (!forStepUp) {
        _waitForStepUpToComplete(ul, opCtx);
    }

    auto& coordinatorsBySession = _coordinatorsBySession[lsid];

    // We should never try to insert a coordinator if one already exists for this session and txn
    // number and transaction retry counter. Logic for avoiding this due to e.g. malformed commands
    // should be handled external to the catalog.
    if (coordinatorsBySession.find(txnNumber) != coordinatorsBySession.end()) {
        auto coordinatorByTxnNumber = coordinatorsBySession[txnNumber];
        invariant(
            coordinatorByTxnNumber.find(txnRetryCounter) == coordinatorByTxnNumber.end(),
            "Cannot insert a TransactionCoordinator into the TransactionCoordinatorCatalog with "
            "the same session ID, transaction number and retry counter as a previous coordinator");

        const auto latestCoordinatorOnSessionIter = coordinatorByTxnNumber.begin();

        const auto latestTxnRetryCounter = latestCoordinatorOnSessionIter->first;
        invariant(latestTxnRetryCounter < txnRetryCounter);

        const auto& latestCoordinatorOnSession = latestCoordinatorOnSessionIter->second;
        uassert(6032300,
                "Cannot create a new transaction coordinator with the same session ID and "
                "transaction number as a previous coordinator that has not reached a decision",
                latestCoordinatorOnSession->getDecision().isReady());
        auto swDecision = latestCoordinatorOnSession->getDecision().getNoThrow();
        uassert(6032301,
                "Cannot create a new transaction coordinator with the same session ID and "
                "transaction number as a previous coordinator that has already reached a commit "
                "decision",
                !swDecision.isOK());
    }

    coordinatorsBySession[txnNumber][txnRetryCounter] = coordinator;

    // Schedule callback to remove the coordinator from the catalog when all its activities have
    // completed. This needs to be done outside of the mutex, in case the coordinator completed
    // early because of stepdown. Otherwise the continuation could execute on the same thread and
    // recursively acquire the mutex.
    ul.unlock();

    coordinator->onCompletion()
        .thenRunOn(Grid::get(opCtx)->getExecutorPool()->getFixedExecutor())
        .ignoreValue()
        .getAsync([this, lsid, txnNumberAndRetryCounter](Status) {
            _remove(lsid, txnNumberAndRetryCounter);
        });
}

std::shared_ptr<TransactionCoordinator> TransactionCoordinatorCatalog::get(
    OperationContext* opCtx,
    const LogicalSessionId& lsid,
    const TxnNumberAndRetryCounter& txnNumberAndRetryCounter) {
    auto txnNumber = txnNumberAndRetryCounter.getTxnNumber();
    auto txnRetryCounter = *txnNumberAndRetryCounter.getTxnRetryCounter();

    stdx::unique_lock<Latch> ul(_mutex);
    _waitForStepUpToComplete(ul, opCtx);

    std::shared_ptr<TransactionCoordinator> coordinatorToReturn;

    auto coordinatorsForSessionIter = _coordinatorsBySession.find(lsid);
    if (coordinatorsForSessionIter != _coordinatorsBySession.end()) {
        const auto& coordinatorsForSession = coordinatorsForSessionIter->second;
        auto coordinatorForTxnNumberIter = coordinatorsForSession.find(txnNumber);
        if (coordinatorForTxnNumberIter != coordinatorsForSession.end()) {
            const auto& coordinatorsForTxnNumber = coordinatorForTxnNumberIter->second;
            auto coordinatorForTxnRetryCounterIter = coordinatorsForTxnNumber.find(txnRetryCounter);
            if (coordinatorForTxnRetryCounterIter != coordinatorsForTxnNumber.end()) {
                coordinatorToReturn = coordinatorForTxnRetryCounterIter->second;
            }
        }
    }

    return coordinatorToReturn;
}

boost::optional<std::pair<TxnNumberAndRetryCounter, std::shared_ptr<TransactionCoordinator>>>
TransactionCoordinatorCatalog::getLatestOnSession(OperationContext* opCtx,
                                                  const LogicalSessionId& lsid) {
    stdx::unique_lock<Latch> ul(_mutex);
    _waitForStepUpToComplete(ul, opCtx);

    const auto& coordinatorsForSessionIter = _coordinatorsBySession.find(lsid);

    if (coordinatorsForSessionIter == _coordinatorsBySession.end()) {
        return boost::none;
    }

    const auto& coordinatorsForSession = coordinatorsForSessionIter->second;

    // We should never have empty map for a session because entries for sessions with no
    // transactions are removed
    invariant(!coordinatorsForSession.empty());

    const auto& coordinatorsForLastTxnNumberIter = coordinatorsForSession.begin();

    auto lastTxnNumber = coordinatorsForLastTxnNumberIter->first;
    const auto& coordinatorsForLastTxnNumber = coordinatorsForLastTxnNumberIter->second;

    // We should never have empty map for a txnNumber because entries for txnNumbers with no
    // coordinators are removed.
    invariant(!coordinatorsForLastTxnNumber.empty());

    const auto& coordinatorsForLastTxnRetryCounterIter = coordinatorsForLastTxnNumber.begin();
    auto lastTxnRetryCounter = coordinatorsForLastTxnRetryCounterIter->first;
    return std::make_pair(TxnNumberAndRetryCounter{lastTxnNumber, lastTxnRetryCounter},
                          coordinatorsForLastTxnRetryCounterIter->second);
}

void TransactionCoordinatorCatalog::_remove(
    const LogicalSessionId& lsid, const TxnNumberAndRetryCounter& txnNumberAndRetryCounter) {
    LOGV2_DEBUG(
        22440,
        3,
        "{sessionId}:{txnNumberAndRetryCounter} Removing coordinator from in-memory catalog",
        "Removing coordinator from in-memory catalog",
        "sessionId"_attr = lsid.getId(),
        "txnNumberAndRetryCounter"_attr = txnNumberAndRetryCounter);

    auto txnNumber = txnNumberAndRetryCounter.getTxnNumber();
    auto txnRetryCounter = *txnNumberAndRetryCounter.getTxnRetryCounter();

    stdx::lock_guard<Latch> lk(_mutex);

    const auto& coordinatorsForSessionIter = _coordinatorsBySession.find(lsid);

    if (coordinatorsForSessionIter != _coordinatorsBySession.end()) {
        auto& coordinatorsForSession = coordinatorsForSessionIter->second;
        const auto& coordinatorForTxnNumberIter = coordinatorsForSession.find(txnNumber);

        if (coordinatorForTxnNumberIter != coordinatorsForSession.end()) {
            auto& coordinatorsForTxnNumber = coordinatorForTxnNumberIter->second;
            const auto& coordinatorForTxnRetryCounterIter =
                coordinatorsForTxnNumber.find(txnRetryCounter);

            if (coordinatorForTxnRetryCounterIter != coordinatorsForTxnNumber.end()) {
                coordinatorsForTxnNumber.erase(coordinatorForTxnRetryCounterIter);
                if (coordinatorsForTxnNumber.empty()) {
                    coordinatorsForSession.erase(coordinatorForTxnNumberIter);
                    if (coordinatorsForSession.empty()) {
                        _coordinatorsBySession.erase(coordinatorsForSessionIter);
                    }
                }
            }
        }
    }

    if (_coordinatorsBySession.empty()) {
        LOGV2_DEBUG(22441, 3, "Signaling last active coordinator removed");
        _noActiveCoordinatorsCV.notify_all();
    }
}

void TransactionCoordinatorCatalog::join() {
    stdx::unique_lock<Latch> ul(_mutex);

    while (!_noActiveCoordinatorsCV.wait_for(
        ul, stdx::chrono::seconds{5}, [this] { return _coordinatorsBySession.empty(); })) {
        LOGV2(22442,
              "After 5 seconds of wait there are still {numSessionsLeft} sessions left "
              "with active coordinators which have not yet completed",
              "After 5 seconds of wait there are still sessions left with active coordinators "
              "which have not yet completed",
              "numSessionsLeft"_attr = _coordinatorsBySession.size());
        LOGV2(22443,
              "Active coordinators remaining: {activeCoordinators}",
              "Active coordinators remaining",
              "activeCoordinators"_attr = _toString(ul));
    }
}

std::string TransactionCoordinatorCatalog::toString() const {
    stdx::lock_guard<Latch> lk(_mutex);
    return _toString(lk);
}

void TransactionCoordinatorCatalog::_waitForStepUpToComplete(stdx::unique_lock<Latch>& lk,
                                                             OperationContext* opCtx) {
    invariant(lk.owns_lock());
    opCtx->waitForConditionOrInterrupt(
        _stepUpCompleteCV, lk, [this]() { return bool(_stepUpCompletionStatus); });

    uassertStatusOK(*_stepUpCompletionStatus);
}

std::string TransactionCoordinatorCatalog::_toString(WithLock wl) const {
    StringBuilder ss;
    ss << "[";
    for (const auto& coordinatorsForSession : _coordinatorsBySession) {
        ss << "\n" << coordinatorsForSession.first.getId() << ": ";
        for (const auto& coordinator : coordinatorsForSession.second) {
            ss << coordinator.first << ",";
        }
    }
    ss << "]";
    return ss.str();
}

void TransactionCoordinatorCatalog::filter(FilterPredicate predicate, FilterVisitor visitor) {
    stdx::lock_guard<Latch> lk(_mutex);
    for (auto&& [sessionId, coordinatorsForSession] : _coordinatorsBySession) {
        for (auto&& [txnNumber, coordinatorsForTxnNumber] : coordinatorsForSession) {
            for (auto&& [txnRetryCounter, coordinator] : coordinatorsForTxnNumber) {
                auto txnNumberAndRetryCounter =
                    TxnNumberAndRetryCounter(txnNumber, txnRetryCounter);
                if (predicate(sessionId, txnNumberAndRetryCounter, coordinator)) {
                    visitor(sessionId, txnNumberAndRetryCounter, coordinator);
                }
            }
        }
    }
}
}  // namespace mongo
