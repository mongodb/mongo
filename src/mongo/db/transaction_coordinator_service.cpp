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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/transaction_coordinator_service.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/transaction_coordinator.h"
#include "mongo/db/transaction_coordinator_commands_impl.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/shard_id.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {
const auto transactionCoordinatorServiceDecoration =
    ServiceContext::declareDecoration<TransactionCoordinatorService>();
}

TransactionCoordinatorService::TransactionCoordinatorService() = default;

TransactionCoordinatorService::~TransactionCoordinatorService() = default;

TransactionCoordinatorService* TransactionCoordinatorService::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

TransactionCoordinatorService* TransactionCoordinatorService::get(ServiceContext* serviceContext) {
    return &transactionCoordinatorServiceDecoration(serviceContext);
}

void TransactionCoordinatorService::createCoordinator(LogicalSessionId lsid,
                                                      TxnNumber txnNumber,
                                                      Date_t commitDeadline) {
    // TODO (SERVER-37021): Validate lsid and txnNumber against latest txnNumber on session in the
    // catalog.

    auto latestTxnNumAndCoordinator = _coordinatorCatalog.getLatestOnSession(lsid);
    // TODO (SERVER-37039): The below removal logic for a coordinator will change/be removed once we
    // allow multiple coordinators for a session.
    if (latestTxnNumAndCoordinator) {
        _coordinatorCatalog.remove(lsid, latestTxnNumAndCoordinator->first);
    }
    _coordinatorCatalog.create(lsid, txnNumber);

    // TODO (SERVER-37024): Schedule abort task on executor to execute at commitDeadline.
    // TODO (SERVER-37025): Schedule poke task on executor.
}

Future<TransactionCoordinatorService::CommitDecision>
TransactionCoordinatorService::coordinateCommit(OperationContext* opCtx,
                                                LogicalSessionId lsid,
                                                TxnNumber txnNumber,
                                                const std::set<ShardId>& participantList) {

    auto coordinator = _coordinatorCatalog.get(lsid, txnNumber);
    if (!coordinator) {
        return TransactionCoordinatorService::CommitDecision::kAbort;
    }

    txn::recvCoordinateCommit(opCtx, coordinator.get(), participantList);

    return coordinator.get()->waitForCompletion().then([](auto finalState) {
        switch (finalState) {
            case TransactionCoordinator::StateMachine::State::kAborted:
                return TransactionCoordinatorService::CommitDecision::kAbort;
            case TransactionCoordinator::StateMachine::State::kCommitted:
                return TransactionCoordinatorService::CommitDecision::kCommit;
            default:
                MONGO_UNREACHABLE;
        }
    });
}

void TransactionCoordinatorService::voteCommit(OperationContext* opCtx,
                                               LogicalSessionId lsid,
                                               TxnNumber txnNumber,
                                               const ShardId& shardId,
                                               Timestamp prepareTimestamp) {
    auto coordinator = _coordinatorCatalog.get(lsid, txnNumber);
    if (!coordinator) {
        // TODO (SERVER-37018): Send abort to the participant who sent this vote (shardId)
        return;
    }

    txn::recvVoteCommit(opCtx, coordinator.get(), shardId, prepareTimestamp);
}

void TransactionCoordinatorService::voteAbort(OperationContext* opCtx,
                                              LogicalSessionId lsid,
                                              TxnNumber txnNumber,
                                              const ShardId& shardId) {
    auto coordinator = _coordinatorCatalog.get(lsid, txnNumber);

    if (coordinator) {
        txn::recvVoteAbort(opCtx, coordinator.get(), shardId);
    }
}

void TransactionCoordinatorService::tryAbort(OperationContext* opCtx,
                                             LogicalSessionId lsid,
                                             TxnNumber txnNumber) {
    auto coordinator = _coordinatorCatalog.get(lsid, txnNumber);

    if (coordinator) {
        // TODO (SERVER-37020): Do recvTryAbort, remove this once implemented.
        MONGO_UNREACHABLE;
    }
}

}  // namespace mongo
