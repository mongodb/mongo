
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

#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/transaction_coordinator_document_gen.h"
#include "mongo/db/write_concern.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

const auto transactionCoordinatorServiceDecoration =
    ServiceContext::declareDecoration<TransactionCoordinatorService>();

}  // namespace

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
    if (auto latestTxnNumAndCoordinator = _coordinatorCatalog->getLatestOnSession(opCtx, lsid)) {
        auto latestCoordinator = latestTxnNumAndCoordinator->second;
        if (txnNumber == latestTxnNumAndCoordinator->first) {
            return;
        }
        latestCoordinator->cancelIfCommitNotYetStarted();
    }

    auto coordinator =
        std::make_shared<TransactionCoordinator>(opCtx->getServiceContext(), lsid, txnNumber);

    _coordinatorCatalog->insert(opCtx, lsid, txnNumber, coordinator);

    // Schedule a task in the future to cancel the commit coordination on the coordinator, so that
    // the coordinator does not remain in memory forever (in case the particpant list is never
    // received).
    const auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    auto cbHandle = uassertStatusOK(executor->scheduleWorkAt(
        commitDeadline,
        [coordinatorWeakPtr = std::weak_ptr<TransactionCoordinator>(coordinator)](
            const mongo::executor::TaskExecutor::CallbackArgs& cbArgs) mutable {
            auto coordinator = coordinatorWeakPtr.lock();
            if (coordinator) {
                coordinator->cancelIfCommitNotYetStarted();
            }
        }));

    // TODO (SERVER-38715): Store the callback handle in the coordinator, so that the coordinator
    // can cancel the cancel task on receiving the participant list.
}

boost::optional<Future<txn::CommitDecision>> TransactionCoordinatorService::coordinateCommit(
    OperationContext* opCtx,
    LogicalSessionId lsid,
    TxnNumber txnNumber,
    const std::set<ShardId>& participantList) {
    auto coordinator = _coordinatorCatalog->get(opCtx, lsid, txnNumber);
    if (!coordinator) {
        return boost::none;
    }

    std::vector<ShardId> participants(participantList.begin(), participantList.end());
    auto decisionFuture = coordinator->runCommit(participants);

    return coordinator->onCompletion().then(
        [coordinator] { return coordinator->getDecision().get(); });

    // TODO (SERVER-37364): Re-enable the coordinator returning the decision as soon as the decision
    // is made durable. Currently the coordinator waits to hear acks because participants in prepare
    // reject requests with a higher transaction number, causing tests to fail.
    // return coordinator.get()->runCommit(participants);
}

boost::optional<Future<txn::CommitDecision>> TransactionCoordinatorService::recoverCommit(
    OperationContext* opCtx, LogicalSessionId lsid, TxnNumber txnNumber) {
    auto coordinator = _coordinatorCatalog->get(opCtx, lsid, txnNumber);
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

void TransactionCoordinatorService::onStepUp(OperationContext* opCtx) {
    // Blocks until the stepup task from the last term completes, then marks a new stepup task as
    // having begun and blocks until all active coordinators complete (are removed from the
    // catalog).
    // Note: No other threads can read the catalog while the catalog is marked as having an active
    // stepup task.
    _coordinatorCatalog->enterStepUp(opCtx);

    const auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    auto status =
        executor
            ->scheduleWork([ this, service = opCtx->getServiceContext() ](
                const executor::TaskExecutor::CallbackArgs&) {
                try {
                    // The opCtx destructor handles unsetting itself from the Client
                    ThreadClient threadClient("TransactionCoordinator-StepUp", service);
                    auto opCtxPtr = Client::getCurrent()->makeOperationContext();
                    auto opCtx = opCtxPtr.get();

                    auto& replClientInfo = repl::ReplClientInfo::forClient(opCtx->getClient());
                    replClientInfo.setLastOpToSystemLastOpTime(opCtx);

                    const auto lastOpTime = replClientInfo.getLastOp();
                    LOG(3) << "Going to wait for client's last OpTime " << lastOpTime
                           << " to become majority committed";
                    WriteConcernResult unusedWCResult;
                    uassertStatusOK(waitForWriteConcern(
                        opCtx,
                        lastOpTime,
                        WriteConcernOptions{WriteConcernOptions::kInternalMajorityNoSnapshot,
                                            WriteConcernOptions::SyncMode::UNSET,
                                            WriteConcernOptions::kNoTimeout},
                        &unusedWCResult));

                    auto coordinatorDocs =
                        TransactionCoordinatorDriver::readAllCoordinatorDocs(opCtx);

                    LOG(0) << "Need to resume coordinating commit for " << coordinatorDocs.size()
                           << " transactions";

                    for (const auto& doc : coordinatorDocs) {
                        LOG(3) << "Going to resume coordinating commit for " << doc.toBSON();
                        const auto lsid = *doc.getId().getSessionId();
                        const auto txnNumber = *doc.getId().getTxnNumber();

                        auto coordinator = std::make_shared<TransactionCoordinator>(
                            opCtx->getServiceContext(), lsid, txnNumber);
                        _coordinatorCatalog->insert(
                            opCtx, lsid, txnNumber, coordinator, true /* forStepUp */);
                        coordinator->continueCommit(doc);
                    }

                    _coordinatorCatalog->exitStepUp();

                    LOG(3) << "Incoming coordinateCommit requests now accepted";
                } catch (const DBException& e) {
                    LOG(3) << "Failed while executing thread to resume coordinating commit for "
                              "pending "
                              "transactions "
                           << causedBy(e.toStatus());
                    _coordinatorCatalog->exitStepUp();
                }
            })
            .getStatus();

    // TODO (SERVER-38320): Reschedule the stepup task if the interruption was not due to stepdown.
    if (status == ErrorCodes::ShutdownInProgress || ErrorCodes::isInterruption(status.code())) {
        return;
    }
    invariant(status);
}

void TransactionCoordinatorService::onStepDown() {}

}  // namespace mongo
