/**
 *    Copyright (C) 2018 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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

#include "mongo/util/log.h"

namespace mongo {

TransactionCoordinatorCatalog::TransactionCoordinatorCatalog() = default;

TransactionCoordinatorCatalog::~TransactionCoordinatorCatalog() = default;

std::shared_ptr<TransactionCoordinator> TransactionCoordinatorCatalog::create(LogicalSessionId lsid,
                                                                              TxnNumber txnNumber) {

    stdx::lock_guard<decltype(_mtx)> lk(_mtx);

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
    _coordinatorsBySession[lsid][txnNumber] = newCoordinator;

    return newCoordinator;
}

boost::optional<std::shared_ptr<TransactionCoordinator>> TransactionCoordinatorCatalog::get(
    LogicalSessionId lsid, TxnNumber txnNumber) {

    stdx::lock_guard<decltype(_mtx)> lk(_mtx);

    const auto& coordinatorsForSessionIter = _coordinatorsBySession.find(lsid);

    if (coordinatorsForSessionIter == _coordinatorsBySession.end()) {
        return boost::none;
    }

    const auto& coordinatorsForSession = coordinatorsForSessionIter->second;
    const auto& coordinatorForTxnIter = coordinatorsForSession.find(txnNumber);

    if (coordinatorForTxnIter == coordinatorsForSession.end()) {
        return boost::none;
    }

    return coordinatorForTxnIter->second;
}

}  // namespace mongo
