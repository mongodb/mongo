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

void doCoordinatorAction(OperationContext* opCtx,
                         std::shared_ptr<TransactionCoordinator> coordinator,
                         TransactionCoordinator::StateMachine::Action action) {
    switch (action) {
        case TransactionCoordinator::StateMachine::Action::kSendCommit: {
            txn::sendCommit(opCtx,
                            coordinator,
                            coordinator->getNonAckedCommitParticipants(),
                            coordinator->getCommitTimestamp());
            break;
        }
        case TransactionCoordinator::StateMachine::Action::kSendAbort: {
            txn::sendAbort(opCtx, coordinator->getNonVotedAbortParticipants());
            break;
        }
        case TransactionCoordinator::StateMachine::Action::kNone:
            break;
    }
}
}

TransactionCoordinatorService::TransactionCoordinatorService()
    : _coordinatorCatalog(std::make_shared<TransactionCoordinatorCatalog>()) {}

TransactionCoordinatorService::~TransactionCoordinatorService() = default;

TransactionCoordinatorService* TransactionCoordinatorService::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

TransactionCoordinatorService* TransactionCoordinatorService::get(ServiceContext* serviceContext) {
    return &transactionCoordinatorServiceDecoration(serviceContext);
}

void TransactionCoordinatorService::createCoordinator(OperationContext* opCtx,
                                                      LogicalSessionId lsid,
                                                      TxnNumber txnNumber,
                                                      Date_t commitDeadline) {
    // TODO (SERVER-37021): Validate lsid and txnNumber against latest txnNumber on session in the
    // catalog.

    auto latestTxnNumAndCoordinator = _coordinatorCatalog->getLatestOnSession(lsid);
    // TODO (SERVER-37039): The below removal logic for a coordinator will change/be removed once we
    // allow multiple coordinators for a session.
    if (latestTxnNumAndCoordinator) {
        auto latestCoordinator = latestTxnNumAndCoordinator.get().second;
        // Call tryAbort on previous coordinator.
        auto actionToTake = latestCoordinator.get()->recvTryAbort();
        doCoordinatorAction(opCtx, latestCoordinator, actionToTake);

        // Wait for coordinator to finish committing or aborting.
        latestCoordinator->waitForCompletion().get(opCtx);
    }

    _coordinatorCatalog->create(lsid, txnNumber);

    // TODO (SERVER-37024): Schedule abort task on executor to execute at commitDeadline.
    // TODO (SERVER-37025): Schedule poke task on executor.
}

Future<TransactionCoordinatorService::CommitDecision>
TransactionCoordinatorService::coordinateCommit(OperationContext* opCtx,
                                                LogicalSessionId lsid,
                                                TxnNumber txnNumber,
                                                const std::set<ShardId>& participantList) {

    auto coordinator = _coordinatorCatalog->get(lsid, txnNumber);
    if (!coordinator) {
        return TransactionCoordinatorService::CommitDecision::kAbort;
    }

    // TODO (SERVER-36687): Remove log line or demote to lower log level once cross-shard
    // transactions are stable.
    StringBuilder ss;
    ss << "[";
    for (const auto& shardId : participantList) {
        ss << shardId << " ";
    }
    ss << "]";
    LOG(0) << "Coordinator shard received participant list with shards " << ss.str();

    auto actionToTake = coordinator.get()->recvCoordinateCommit(participantList);
    doCoordinatorAction(opCtx, coordinator.get(), actionToTake);

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
    auto coordinator = _coordinatorCatalog->get(lsid, txnNumber);
    if (!coordinator) {
        txn::sendAbort(opCtx, {shardId});
        return;
    }

    // TODO (SERVER-36687): Remove log line or demote to lower log level once cross-shard
    // transactions are stable.
    LOG(0) << "Coordinator shard received voteCommit from " << shardId << " with prepare timestamp "
           << prepareTimestamp;

    auto actionToTake = coordinator.get()->recvVoteCommit(shardId, prepareTimestamp);
    doCoordinatorAction(opCtx, coordinator.get(), actionToTake);
}

void TransactionCoordinatorService::voteAbort(OperationContext* opCtx,
                                              LogicalSessionId lsid,
                                              TxnNumber txnNumber,
                                              const ShardId& shardId) {
    auto coordinator = _coordinatorCatalog->get(lsid, txnNumber);

    if (coordinator) {
        // TODO (SERVER-36687): Remove log line or demote to lower log level once cross-shard
        // transactions are stable.
        LOG(0) << "Coordinator shard received voteAbort from " << shardId;
        auto actionToTake = coordinator.get()->recvVoteAbort(shardId);
        doCoordinatorAction(opCtx, coordinator.get(), actionToTake);
    }
}

}  // namespace mongo
