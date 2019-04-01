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

#include "mongo/db/s/transaction_coordinator_service.h"

#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/transaction_coordinator_document_gen.h"
#include "mongo/db/transaction_participant_gen.h"
#include "mongo/db/write_concern.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

const auto transactionCoordinatorServiceDecoration =
    ServiceContext::declareDecoration<TransactionCoordinatorService>();

}  // namespace

TransactionCoordinatorService::TransactionCoordinatorService() = default;

TransactionCoordinatorService::~TransactionCoordinatorService() {
    _joinPreviousRound();
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
    auto cas = _getCatalogAndScheduler(opCtx);
    auto& catalog = cas->catalog;
    auto& scheduler = cas->scheduler;

    if (auto latestTxnNumAndCoordinator = catalog.getLatestOnSession(opCtx, lsid)) {
        auto latestCoordinator = latestTxnNumAndCoordinator->second;
        if (txnNumber == latestTxnNumAndCoordinator->first) {
            return;
        }
        latestCoordinator->cancelIfCommitNotYetStarted();
    }

    catalog.insert(opCtx,
                   lsid,
                   txnNumber,
                   std::make_shared<TransactionCoordinator>(opCtx->getServiceContext(),
                                                            lsid,
                                                            txnNumber,
                                                            scheduler.makeChildScheduler(),
                                                            commitDeadline));
}

boost::optional<Future<txn::CommitDecision>> TransactionCoordinatorService::coordinateCommit(
    OperationContext* opCtx,
    LogicalSessionId lsid,
    TxnNumber txnNumber,
    const std::set<ShardId>& participantList) {
    auto cas = _getCatalogAndScheduler(opCtx);
    auto& catalog = cas->catalog;

    auto coordinator = catalog.get(opCtx, lsid, txnNumber);
    if (!coordinator) {
        return boost::none;
    }

    coordinator->runCommit(std::vector<ShardId>{participantList.begin(), participantList.end()});

    return coordinator->onCompletion().then(
        [coordinator] { return coordinator->getDecision().get(); });

    // TODO (SERVER-37364): Re-enable the coordinator returning the decision as soon as the decision
    // is made durable. Currently the coordinator waits to hear acks because participants in prepare
    // reject requests with a higher transaction number, causing tests to fail.
    // return coordinator->getDecision();
}

boost::optional<Future<txn::CommitDecision>> TransactionCoordinatorService::recoverCommit(
    OperationContext* opCtx, LogicalSessionId lsid, TxnNumber txnNumber) {
    auto cas = _getCatalogAndScheduler(opCtx);
    auto& catalog = cas->catalog;

    auto coordinator = catalog.get(opCtx, lsid, txnNumber);
    if (!coordinator) {
        return boost::none;
    }

    return coordinator->onCompletion().then(
        [coordinator] { return coordinator->getDecision().get(); });

    // TODO (SERVER-37364): Re-enable the coordinator returning the decision as soon as the decision
    // is made durable. Currently the coordinator waits to hear acks because participants in prepare
    // reject requests with a higher transaction number, causing tests to fail.
    // return coordinator->getDecision();
}

void TransactionCoordinatorService::onStepUp(OperationContext* opCtx,
                                             Milliseconds recoveryDelayForTesting) {
    _joinPreviousRound();

    stdx::lock_guard<stdx::mutex> lg(_mutex);
    invariant(!_catalogAndScheduler);
    _catalogAndScheduler = std::make_shared<CatalogAndScheduler>(opCtx->getServiceContext());

    auto future =
        _catalogAndScheduler->scheduler
            .scheduleWorkIn(
                recoveryDelayForTesting,
                [catalogAndScheduler = _catalogAndScheduler](OperationContext * opCtx) {
                    auto& replClientInfo = repl::ReplClientInfo::forClient(opCtx->getClient());
                    replClientInfo.setLastOpToSystemLastOpTime(opCtx);

                    const auto lastOpTime = replClientInfo.getLastOp();
                    LOG(3) << "Waiting for OpTime " << lastOpTime
                           << " to become majority committed";

                    WriteConcernResult unusedWCResult;
                    uassertStatusOK(waitForWriteConcern(
                        opCtx,
                        lastOpTime,
                        WriteConcernOptions{WriteConcernOptions::kMajority,
                                            WriteConcernOptions::SyncMode::UNSET,
                                            WriteConcernOptions::kNoTimeout},
                        &unusedWCResult));

                    auto coordinatorDocs = txn::readAllCoordinatorDocs(opCtx);

                    LOG(0) << "Need to resume coordinating commit for " << coordinatorDocs.size()
                           << " transactions";

                    auto clockSource = opCtx->getServiceContext()->getFastClockSource();
                    auto& catalog = catalogAndScheduler->catalog;
                    auto& scheduler = catalogAndScheduler->scheduler;

                    for (const auto& doc : coordinatorDocs) {
                        LOG(3) << "Going to resume coordinating commit for " << doc.toBSON();

                        const auto lsid = *doc.getId().getSessionId();
                        const auto txnNumber = *doc.getId().getTxnNumber();

                        auto coordinator = std::make_shared<TransactionCoordinator>(
                            opCtx->getServiceContext(),
                            lsid,
                            txnNumber,
                            scheduler.makeChildScheduler(),
                            clockSource->now() + Seconds(gTransactionLifetimeLimitSeconds.load()));

                        catalog.insert(opCtx, lsid, txnNumber, coordinator, true /* forStepUp */);
                        coordinator->continueCommit(doc);
                    }
                })
            .tapAll([catalogAndScheduler = _catalogAndScheduler](Status status) {
                // TODO (SERVER-38320): Reschedule the step-up task if the interruption was not due
                // to stepdown.

                auto& catalog = catalogAndScheduler->catalog;
                catalog.exitStepUp(status);
            });

    _catalogAndScheduler->recoveryTaskCompleted.emplace(std::move(future));
}

void TransactionCoordinatorService::onStepDown() {
    {
        stdx::lock_guard<stdx::mutex> lg(_mutex);
        if (!_catalogAndScheduler)
            return;

        _catalogAndSchedulerToCleanup = std::move(_catalogAndScheduler);
    }

    _catalogAndSchedulerToCleanup->onStepDown();
}

void TransactionCoordinatorService::onShardingInitialization(OperationContext* opCtx,
                                                             bool isPrimary) {
    if (!isPrimary)
        return;

    stdx::lock_guard<stdx::mutex> lg(_mutex);

    invariant(!_catalogAndScheduler);
    _catalogAndScheduler = std::make_shared<CatalogAndScheduler>(opCtx->getServiceContext());

    _catalogAndScheduler->catalog.exitStepUp(Status::OK());
    _catalogAndScheduler->recoveryTaskCompleted.emplace(Future<void>::makeReady());
}

std::shared_ptr<TransactionCoordinatorService::CatalogAndScheduler>
TransactionCoordinatorService::_getCatalogAndScheduler(OperationContext* opCtx) {
    stdx::unique_lock<stdx::mutex> ul(_mutex);
    uassert(
        ErrorCodes::NotMaster, "Transaction coordinator is not a primary", _catalogAndScheduler);

    return _catalogAndScheduler;
}

void TransactionCoordinatorService::_joinPreviousRound() {
    // onStepDown must have been called
    invariant(!_catalogAndScheduler);

    if (!_catalogAndSchedulerToCleanup)
        return;

    LOG(0) << "Waiting for coordinator tasks from previous term to complete";

    // Block until all coordinators scheduled the previous time the service was primary to have
    // drained. Because the scheduler was interrupted, it should be extremely rare for there to be
    // any coordinators left, so if this actually causes blocking, it would most likely be a bug.
    _catalogAndSchedulerToCleanup->join();
    _catalogAndSchedulerToCleanup.reset();
}

void TransactionCoordinatorService::CatalogAndScheduler::onStepDown() {
    scheduler.shutdown({ErrorCodes::TransactionCoordinatorSteppingDown,
                        "Transaction coordinator service stepping down"});
    catalog.onStepDown();
}

void TransactionCoordinatorService::CatalogAndScheduler::join() {
    recoveryTaskCompleted->wait();
    catalog.join();
}

}  // namespace mongo
