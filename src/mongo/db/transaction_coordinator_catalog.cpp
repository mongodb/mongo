
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
#include "mongo/db/transaction_coordinator_catalog.h"

#include "mongo/db/transaction_coordinator.h"
#include "mongo/util/log.h"

namespace mongo {

TransactionCoordinatorCatalog::TransactionCoordinatorCatalog() = default;

TransactionCoordinatorCatalog::~TransactionCoordinatorCatalog() = default;

std::shared_ptr<TransactionCoordinator> TransactionCoordinatorCatalog::create(LogicalSessionId lsid,
                                                                              TxnNumber txnNumber) {

    stdx::lock_guard<stdx::mutex> lk(_mutex);

    // Create a new map for the session if it does not exist
    if (_coordinatorsBySession.find(lsid) == _coordinatorsBySession.end()) {
        _coordinatorsBySession[lsid] = {};
    }

    // We should never try to insert a coordinator if one already exists for this session and txn
    // number. Logic for avoiding this due to e.g. malformed commands should be handled external to
    // the catalog.
    invariant(_coordinatorsBySession[lsid].find(txnNumber) == _coordinatorsBySession[lsid].end(),
              "Cannot insert a TransactionCoordinator into the TransactionCoordinatorCatalog with "
              "the same session ID and transaction number as a previous coordinator");

    auto newCoordinator = std::make_shared<TransactionCoordinator>();
    // Schedule callback to remove coordinator from catalog when it either commits or aborts.
    newCoordinator->waitForCompletion().getAsync([
        catalogWeakPtr = std::weak_ptr<TransactionCoordinatorCatalog>(shared_from_this()),
        lsid,
        txnNumber
    ](auto finalState) {
        if (auto catalog = catalogWeakPtr.lock()) {
            catalog->remove(lsid, txnNumber);
        }
    });

    _coordinatorsBySession[lsid][txnNumber] = newCoordinator;

    return newCoordinator;
}

std::shared_ptr<TransactionCoordinator> TransactionCoordinatorCatalog::get(LogicalSessionId lsid,
                                                                           TxnNumber txnNumber) {

    stdx::lock_guard<stdx::mutex> lk(_mutex);

    const auto& coordinatorsForSessionIter = _coordinatorsBySession.find(lsid);

    if (coordinatorsForSessionIter == _coordinatorsBySession.end()) {
        return nullptr;
    }

    const auto& coordinatorsForSession = coordinatorsForSessionIter->second;
    const auto& coordinatorForTxnIter = coordinatorsForSession.find(txnNumber);

    if (coordinatorForTxnIter == coordinatorsForSession.end()) {
        return nullptr;
    }

    return coordinatorForTxnIter->second;
}

boost::optional<std::pair<TxnNumber, std::shared_ptr<TransactionCoordinator>>>
TransactionCoordinatorCatalog::getLatestOnSession(LogicalSessionId lsid) {

    stdx::lock_guard<stdx::mutex> lk(_mutex);

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
    using CoordinatorState = TransactionCoordinator::StateMachine::State;

    stdx::lock_guard<stdx::mutex> lk(_mutex);

    const auto& coordinatorsForSessionIter = _coordinatorsBySession.find(lsid);

    if (coordinatorsForSessionIter != _coordinatorsBySession.end()) {
        auto& coordinatorsForSession = coordinatorsForSessionIter->second;
        const auto& coordinatorForTxnIter = coordinatorsForSession.find(txnNumber);

        if (coordinatorForTxnIter != coordinatorsForSession.end()) {
            auto coordinator = coordinatorForTxnIter->second;
            // TODO (SERVER-36304/37021): Reenable the below invariant once transaction participants
            // are able to send votes and once we validate the state of the coordinator when a new
            // transaction comes in for an existing session. For now, we're not validating the state
            // of the coordinator which means it is possible that starting a new transaction before
            // waiting for the previous one's coordinator to reach state committed or aborted will
            // corrupt the previous transaction.

            // invariant(coordinator->state() == CoordinatorState::kCommitted ||
            //           coordinator->state() == CoordinatorState::kAborted);
            coordinatorsForSession.erase(coordinatorForTxnIter);
            if (coordinatorsForSession.size() == 0) {
                _coordinatorsBySession.erase(coordinatorsForSessionIter);
            }
        }
    }
}

}  // namespace mongo
