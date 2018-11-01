
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

Future<TransactionCoordinator::CommitDecision> TransactionCoordinatorService::coordinateCommit(
    OperationContext* opCtx,
    LogicalSessionId lsid,
    TxnNumber txnNumber,
    const std::set<ShardId>& participantList) {

    auto coordinator = _coordinatorCatalog->get(lsid, txnNumber);
    if (!coordinator) {
        // TODO (SERVER-37440): Return decision "kForgotten", which indicates that a decision was
        // already made and forgotten. The caller can recover the decision from the local
        // participant if a higher transaction has not been started on the session and the session
        // has not been reaped.
        // Currently is MONGO_UNREACHABLE because no tests should cause the router to re-send
        // coordinateCommitTransaction.
        MONGO_UNREACHABLE;
    }

    Action initialAction = coordinator->recvCoordinateCommit(participantList);
    if (initialAction != Action::kNone) {
        txn::launchCoordinateCommitTask(_threadPool, coordinator, lsid, txnNumber, initialAction);
    }

    return coordinator.get()->waitForDecision();
}

}  // namespace mongo
