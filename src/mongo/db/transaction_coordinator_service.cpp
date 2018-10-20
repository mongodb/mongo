
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
    if (auto latestTxnNumAndCoordinator = _coordinatorCatalog->getLatestOnSession(lsid)) {
        auto latestCoordinator = latestTxnNumAndCoordinator.get().second;
        if (txnNumber == latestTxnNumAndCoordinator.get().first) {
            // If we're trying to re-create a coordinator for an already-existing lsid and
            // txnNumber, we should be able to continue to use that coordinator, which MUST be in
            // an unused state. In the state machine, the initial state is encoded as
            // kWaitingForParticipantList, but this uassert won't necessarily catch all bugs
            // because it's possible that the participant list (via coordinateCommit) could be en
            // route when we reach this point or that votes have been received before reaching
            // coordinateCommit.
            uassert(50968,
                    "Cannot start a new transaction with the same session ID and transaction "
                    "number as a transaction that has already begun two-phase commit.",
                    latestCoordinator->state() ==
                        TransactionCoordinator::StateMachine::State::kWaitingForParticipantList);

            return;
        }
        // Call tryAbort on previous coordinator.
        auto actionToTake = latestCoordinator.get()->recvTryAbort();
        doCoordinatorAction(opCtx, latestCoordinator, actionToTake);
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

    auto actionToTake = coordinator.get()->recvVoteCommit(shardId, prepareTimestamp);
    doCoordinatorAction(opCtx, coordinator.get(), actionToTake);
}

void TransactionCoordinatorService::voteAbort(OperationContext* opCtx,
                                              LogicalSessionId lsid,
                                              TxnNumber txnNumber,
                                              const ShardId& shardId) {
    auto coordinator = _coordinatorCatalog->get(lsid, txnNumber);

    if (coordinator) {
        auto actionToTake = coordinator.get()->recvVoteAbort(shardId);
        doCoordinatorAction(opCtx, coordinator.get(), actionToTake);
    }
}

}  // namespace mongo
