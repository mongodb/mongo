
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
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_id.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {

const auto transactionCoordinatorServiceDecoration =
    ServiceContext::declareDecoration<TransactionCoordinatorService>();

/**
 * Constructs the default options for the thread pool used to run commit.
 */
ThreadPool::Options makeDefaultThreadPoolOptions() {
    ThreadPool::Options options;
    options.poolName = "TransactionCoordinatorService";
    options.minThreads = 0;
    options.maxThreads = 16;

    // Ensure all threads have a client
    options.onCreateThread = [](const std::string& threadName) {
        Client::initThread(threadName.c_str());
    };
    return options;
}

}  // namespace

TransactionCoordinatorService::TransactionCoordinatorService()
    : _coordinatorCatalog(std::make_shared<TransactionCoordinatorCatalog>()),
      _threadPool(std::make_unique<ThreadPool>(makeDefaultThreadPoolOptions())) {
    _threadPool->startup();
}

void TransactionCoordinatorService::setThreadPool(std::unique_ptr<ThreadPool> pool) {
    _threadPool->shutdown();
    _threadPool = std::move(pool);
    _threadPool->startup();
}

void TransactionCoordinatorService::shutdown() {
    _threadPool->shutdown();
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
            return;
        }
        latestCoordinator.get()->cancelIfCommitNotYetStarted();
    }

    auto networkExecutor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    auto newCoordinator = std::make_shared<TransactionCoordinator>(
        networkExecutor, _threadPool.get(), lsid, txnNumber);

    _coordinatorCatalog->insert(lsid, txnNumber, newCoordinator);

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

    std::vector<ShardId> participants(participantList.begin(), participantList.end());
    auto decisionFuture = coordinator.get()->runCommit(participants);

    return coordinator.get()->onCompletion().then(
        [coordinator] { return coordinator.get()->getDecision().get(); });

    // TODO (SERVER-37364): Re-enable the coordinator returning the decision as soon as the decision
    // is made durable. Currently the coordinator waits to hear acks because participants in prepare
    // reject requests with a higher transaction number, causing tests to fail.
    // return coordinator.get()->runCommit(participants);
}

boost::optional<Future<TransactionCoordinator::CommitDecision>>
TransactionCoordinatorService::recoverCommit(OperationContext* opCtx,
                                             LogicalSessionId lsid,
                                             TxnNumber txnNumber) {
    auto coordinator = _coordinatorCatalog->get(lsid, txnNumber);
    if (!coordinator) {
        return boost::none;
    }

    return coordinator.get()->onCompletion().then(
        [coordinator] { return coordinator.get()->getDecision().get(); });
    // TODO (SERVER-37364): Re-enable the coordinator returning the decision as soon as the decision
    // is made durable. Currently the coordinator waits to hear acks because participants in prepare
    // reject requests with a higher transaction number, causing tests to fail.
    // return coordinator.get()->getDecision();
}

}  // namespace mongo
