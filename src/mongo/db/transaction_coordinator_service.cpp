
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
#include "mongo/db/transaction_coordinator_util.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_id.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {

const auto transactionCoordinatorServiceDecoration =
    ServiceContext::declareDecoration<TransactionCoordinatorService>();

using Action = TransactionCoordinator::StateMachine::Action;
using State = TransactionCoordinator::StateMachine::State;

/**
 * Constructs the default options for the thread pool used to run commit.
 */
ThreadPool::Options makeDefaultThreadPoolOptions() {
    ThreadPool::Options options;
    options.poolName = "TransactionCoordinatorService";
    options.minThreads = 0;
    options.maxThreads = 20;

    // Ensure all threads have a client
    options.onCreateThread = [](const std::string& threadName) {
        Client::initThread(threadName.c_str());
    };
    return options;
}

void driveCoordinatorUntilDone(OperationContext* opCtx,
                               std::shared_ptr<TransactionCoordinator> coordinator,
                               const LogicalSessionId& lsid,
                               const TxnNumber& txnNumber,
                               Action action) {
    while (true) {
        switch (action) {
            case Action::kWriteParticipantList:
                action = coordinator->madeParticipantListDurable();
                break;
            case Action::kSendPrepare:
                action = txn::sendPrepare(
                    opCtx, lsid, txnNumber, coordinator, coordinator->getParticipants());
                break;
            case Action::kWriteAbortDecision:
                action = coordinator->madeAbortDecisionDurable();
                break;
            case Action::kSendAbort:
                action = txn::sendAbort(opCtx,
                                        lsid,
                                        txnNumber,
                                        coordinator,
                                        coordinator->getNonVotedAbortParticipants());
                break;
            case Action::kWriteCommitDecision:
                action = coordinator->madeCommitDecisionDurable();
                break;
            case Action::kSendCommit:
                action = txn::sendCommit(opCtx,
                                         lsid,
                                         txnNumber,
                                         coordinator,
                                         coordinator->getNonAckedCommitParticipants(),
                                         coordinator->getCommitTimestamp().get());
                break;
            case Action::kDone:
                return;
            case Action::kNone:
                // This means an event was delivered to the coordinator outside the expected order
                // of events.
                MONGO_UNREACHABLE;
        }
    }
}

void launchCoordinateCommitTask(ThreadPool& threadPool,
                                std::shared_ptr<TransactionCoordinator> coordinator,
                                const LogicalSessionId& lsid,
                                const TxnNumber& txnNumber,
                                TransactionCoordinator::StateMachine::Action initialAction) {
    auto ch = threadPool.schedule([coordinator, lsid, txnNumber, initialAction]() {
        try {
            // The opCtx destructor handles unsetting itself from the Client
            auto opCtx = Client::getCurrent()->makeOperationContext();
            driveCoordinatorUntilDone(opCtx.get(), coordinator, lsid, txnNumber, initialAction);
        } catch (const DBException& e) {
            log() << "Exception was thrown while coordinating commit: " << causedBy(e.toStatus());
        }
    });
}

}  // namespace

TransactionCoordinatorService::TransactionCoordinatorService()
    : _coordinatorCatalog(std::make_shared<TransactionCoordinatorCatalog>()),
      _threadPool(makeDefaultThreadPoolOptions()) {
    _threadPool.startup();
}

void TransactionCoordinatorService::shutdown() {
    _threadPool.shutdown();
}

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
                        TransactionCoordinator::StateMachine::State::kUninitialized);

            return;
        }
        // Call tryAbort on previous coordinator.
        latestCoordinator.get()->recvTryAbort();
    }

    _coordinatorCatalog->create(lsid, txnNumber);

    // TODO (SERVER-37024): Schedule abort task on executor to execute at commitDeadline.
}

boost::optional<Future<TransactionCoordinator::CommitDecision>>
TransactionCoordinatorService::coordinateCommit(OperationContext* opCtx,
                                                LogicalSessionId lsid,
                                                TxnNumber txnNumber,
                                                const std::set<ShardId>& participantList) {

    auto coordinator = _coordinatorCatalog->get(lsid, txnNumber);
    if (!coordinator) {
        return boost::none;
    }

    Action initialAction = coordinator->recvCoordinateCommit(participantList);
    if (initialAction != Action::kNone) {
        launchCoordinateCommitTask(_threadPool, coordinator, lsid, txnNumber, initialAction);
    }

    return coordinator->waitForCompletion().then([](auto finalState) {
        switch (finalState) {
            case TransactionCoordinator::StateMachine::State::kAborted:
                return TransactionCoordinator::CommitDecision::kAbort;
            case TransactionCoordinator::StateMachine::State::kCommitted:
                return TransactionCoordinator::CommitDecision::kCommit;
            default:
                MONGO_UNREACHABLE;
        }
    });
    // TODO (SERVER-37364): Re-enable the coordinator returning the decision as soon as the decision
    // is made durable. Currently the coordinator waits to hear acks because participants in prepare
    // reject requests with a higher transaction number, causing tests to fail.
    // return coordinator.get()->waitForDecision();
}

boost::optional<Future<TransactionCoordinator::CommitDecision>>
TransactionCoordinatorService::recoverCommit(OperationContext* opCtx,
                                             LogicalSessionId lsid,
                                             TxnNumber txnNumber) {
    auto coordinator = _coordinatorCatalog->get(lsid, txnNumber);
    if (!coordinator) {
        return boost::none;
    }

    return coordinator->waitForCompletion().then([](auto finalState) {
        switch (finalState) {
            case TransactionCoordinator::StateMachine::State::kAborted:
                return TransactionCoordinator::CommitDecision::kAbort;
            case TransactionCoordinator::StateMachine::State::kCommitted:
                return TransactionCoordinator::CommitDecision::kCommit;
            default:
                MONGO_UNREACHABLE;
        }
    });
    // TODO (SERVER-37364): Re-enable the coordinator returning the decision as soon as the decision
    // is made durable. Currently the coordinator waits to hear acks because participants in prepare
    // reject requests with a higher transaction number, causing tests to fail.
    // return coordinator.get()->waitForDecision();
}

}  // namespace mongo
