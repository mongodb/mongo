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


#include "mongo/platform/basic.h"

#include "mongo/db/s/transaction_coordinator_service.h"

#include "mongo/db/internal_transactions_feature_flag_gen.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/transaction_coordinator_document_gen.h"
#include "mongo/db/s/transaction_coordinator_params_gen.h"
#include "mongo/db/storage/flow_control.h"
#include "mongo/db/transaction_participant_gen.h"
#include "mongo/db/write_concern.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTransaction


namespace mongo {
namespace {

const auto transactionCoordinatorServiceDecoration =
    ServiceContext::declareDecoration<TransactionCoordinatorService>();

}  // namespace

TransactionCoordinatorService::TransactionCoordinatorService() = default;

TransactionCoordinatorService::~TransactionCoordinatorService() {
    joinPreviousRound();
}

TransactionCoordinatorService* TransactionCoordinatorService::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

TransactionCoordinatorService* TransactionCoordinatorService::get(ServiceContext* serviceContext) {
    return &transactionCoordinatorServiceDecoration(serviceContext);
}

void TransactionCoordinatorService::createCoordinator(
    OperationContext* opCtx,
    LogicalSessionId lsid,
    TxnNumberAndRetryCounter txnNumberAndRetryCounter,
    Date_t commitDeadline) {
    auto cas = _getCatalogAndScheduler(opCtx);
    auto& catalog = cas->catalog;
    auto& scheduler = cas->scheduler;

    if (auto latestTxnNumberRetryCounterAndCoordinator = catalog.getLatestOnSession(opCtx, lsid)) {
        auto latestTxnNumberAndRetryCounter = latestTxnNumberRetryCounterAndCoordinator->first;
        auto latestCoordinator = latestTxnNumberRetryCounterAndCoordinator->second;
        if (txnNumberAndRetryCounter == latestTxnNumberAndRetryCounter) {
            return;
        }
        latestCoordinator->cancelIfCommitNotYetStarted();
    }

    auto coordinator = std::make_shared<TransactionCoordinator>(
        opCtx, lsid, txnNumberAndRetryCounter, scheduler.makeChildScheduler(), commitDeadline);

    try {
        catalog.insert(opCtx, lsid, txnNumberAndRetryCounter, coordinator);
    } catch (const DBException&) {
        // Handle the case where the opCtx has been interrupted and we do not successfully insert
        // the coordinator into the catalog.
        coordinator->cancelIfCommitNotYetStarted();
        // Wait for it to finish processing the error before throwing, since leaving this scope will
        // cause the newly created coordinator to be destroyed. We ignore the return Status since we
        // want to just rethrow whatever exception was thrown when inserting into the catalog.
        [[maybe_unused]] auto status = coordinator->onCompletion().waitNoThrow();
        throw;
    }
}


void TransactionCoordinatorService::reportCoordinators(OperationContext* opCtx,
                                                       bool includeIdle,
                                                       std::vector<BSONObj>* ops) {
    std::shared_ptr<CatalogAndScheduler> cas;
    try {
        cas = _getCatalogAndScheduler(opCtx);
    } catch (ExceptionFor<ErrorCodes::NotWritablePrimary>&) {
        // If we are not primary, don't include any output for transaction coordinators in
        // the curOp command.
        return;
    }

    auto& catalog = cas->catalog;

    auto predicate =
        [includeIdle](const LogicalSessionId lsid,
                      const TxnNumberAndRetryCounter txnNumberAndRetryCounter,
                      const std::shared_ptr<TransactionCoordinator> transactionCoordinator) {
            TransactionCoordinator::Step step = transactionCoordinator->getStep();
            if (includeIdle || step > TransactionCoordinator::Step::kInactive) {
                return true;
            }
            return false;
        };

    auto reporter = [ops](const LogicalSessionId lsid,
                          const TxnNumberAndRetryCounter txnNumberAndRetryCounter,
                          const std::shared_ptr<TransactionCoordinator> transactionCoordinator) {
        BSONObjBuilder doc;
        transactionCoordinator->reportState(doc);
        ops->push_back(doc.obj());
    };

    catalog.filter(predicate, reporter);
}

boost::optional<SharedSemiFuture<txn::CommitDecision>>
TransactionCoordinatorService::coordinateCommit(OperationContext* opCtx,
                                                LogicalSessionId lsid,
                                                TxnNumberAndRetryCounter txnNumberAndRetryCounter,
                                                const std::set<ShardId>& participantList) {
    auto cas = _getCatalogAndScheduler(opCtx);
    auto& catalog = cas->catalog;

    auto coordinator = catalog.get(opCtx, lsid, txnNumberAndRetryCounter);
    if (!coordinator) {
        return boost::none;
    }

    coordinator->runCommit(opCtx,
                           std::vector<ShardId>{participantList.begin(), participantList.end()});

    return coordinateCommitReturnImmediatelyAfterPersistingDecision.load()
        ? coordinator->getDecision()
        : coordinator->onCompletion();
}

boost::optional<SharedSemiFuture<txn::CommitDecision>> TransactionCoordinatorService::recoverCommit(
    OperationContext* opCtx,
    LogicalSessionId lsid,
    TxnNumberAndRetryCounter txnNumberAndRetryCounter) {
    auto cas = _getCatalogAndScheduler(opCtx);
    auto& catalog = cas->catalog;

    auto coordinator = catalog.get(opCtx, lsid, txnNumberAndRetryCounter);
    if (!coordinator) {
        return boost::none;
    }

    // Make sure that recover can terminate right away if coordinateCommit never reached
    // the coordinator.
    coordinator->cancelIfCommitNotYetStarted();

    return coordinateCommitReturnImmediatelyAfterPersistingDecision.load()
        ? coordinator->getDecision()
        : coordinator->onCompletion();
}

void TransactionCoordinatorService::onStepUp(OperationContext* opCtx,
                                             Milliseconds recoveryDelayForTesting) {
    joinPreviousRound();

    stdx::lock_guard<Latch> lg(_mutex);
    if (_isShuttingDown) {
        return;
    }

    invariant(!_catalogAndScheduler);
    _catalogAndScheduler = std::make_shared<CatalogAndScheduler>(opCtx->getServiceContext());

    auto future =
        _catalogAndScheduler->scheduler
            .scheduleWorkIn(
                recoveryDelayForTesting,
                [catalogAndScheduler = _catalogAndScheduler](OperationContext* opCtx) {
                    auto& replClientInfo = repl::ReplClientInfo::forClient(opCtx->getClient());
                    replClientInfo.setLastOpToSystemLastOpTime(opCtx);

                    const auto lastOpTime = replClientInfo.getLastOp();
                    LOGV2_DEBUG(22451,
                                3,
                                "Waiting for OpTime {lastOpTime} to become majority committed",
                                "Waiting for OpTime to become majority committed",
                                "lastOpTime"_attr = lastOpTime);

                    WriteConcernResult unusedWCResult;
                    uassertStatusOK(waitForWriteConcern(
                        opCtx,
                        lastOpTime,
                        WriteConcernOptions{WriteConcernOptions::kMajority,
                                            WriteConcernOptions::SyncMode::UNSET,
                                            WriteConcernOptions::kNoTimeout},
                        &unusedWCResult));

                    FlowControl::Bypass flowControlBypass(opCtx);
                    auto coordinatorDocs = txn::readAllCoordinatorDocs(opCtx);

                    LOGV2(22452,
                          "Need to resume coordinating commit for {numPendingTransactions} "
                          "transactions",
                          "Need to resume coordinating commit for transactions with an in-progress "
                          "two-phase commit/abort",
                          "numPendingTransactions"_attr = coordinatorDocs.size());

                    const auto service = opCtx->getServiceContext();
                    const auto clockSource = service->getFastClockSource();

                    auto& catalog = catalogAndScheduler->catalog;
                    auto& scheduler = catalogAndScheduler->scheduler;

                    for (const auto& doc : coordinatorDocs) {
                        LOGV2_DEBUG(
                            22453,
                            3,
                            "Going to resume coordinating commit for {transactionCoordinatorInfo}",
                            "Going to resume coordinating commit",
                            "transactionCoordinatorInfo"_attr = doc.toBSON());

                        const auto lsid = *doc.getId().getSessionId();
                        const auto txnNumber = *doc.getId().getTxnNumber();
                        const auto txnRetryCounter = [&] {
                            if (auto optTxnRetryCounter = doc.getId().getTxnRetryCounter()) {
                                return *optTxnRetryCounter;
                            }
                            return 0;
                        }();

                        auto coordinator = std::make_shared<TransactionCoordinator>(
                            opCtx,
                            lsid,
                            TxnNumberAndRetryCounter{txnNumber, txnRetryCounter},
                            scheduler.makeChildScheduler(),
                            clockSource->now() + Seconds(gTransactionLifetimeLimitSeconds.load()));

                        catalog.insert(opCtx,
                                       lsid,
                                       {txnNumber, txnRetryCounter},
                                       coordinator,
                                       true /* forStepUp */);
                        coordinator->continueCommit(doc);
                    }
                })
            .tapAll([catalogAndScheduler = _catalogAndScheduler](Status status) {
                auto& catalog = catalogAndScheduler->catalog;
                catalog.exitStepUp(status);
            });

    _catalogAndScheduler->recoveryTaskCompleted.emplace(std::move(future));
}

void TransactionCoordinatorService::onStepDown() {
    {
        stdx::lock_guard<Latch> lg(_mutex);
        if (!_catalogAndScheduler)
            return;

        _catalogAndSchedulerToCleanup = std::move(_catalogAndScheduler);
    }

    _catalogAndSchedulerToCleanup->onStepDown();
}

void TransactionCoordinatorService::shutdown() {
    {
        stdx::lock_guard<Latch> lg(_mutex);
        _isShuttingDown = true;
    }

    onStepDown();
    joinPreviousRound();
}

void TransactionCoordinatorService::onShardingInitialization(OperationContext* opCtx,
                                                             bool isPrimary) {
    if (!isPrimary)
        return;

    stdx::lock_guard<Latch> lg(_mutex);
    if (_isShuttingDown) {
        return;
    }

    invariant(!_catalogAndScheduler);
    _catalogAndScheduler = std::make_shared<CatalogAndScheduler>(opCtx->getServiceContext());

    _catalogAndScheduler->catalog.exitStepUp(Status::OK());
    _catalogAndScheduler->recoveryTaskCompleted.emplace(Future<void>::makeReady());
}

std::shared_ptr<TransactionCoordinatorService::CatalogAndScheduler>
TransactionCoordinatorService::_getCatalogAndScheduler(OperationContext* opCtx) {
    stdx::unique_lock<Latch> ul(_mutex);
    uassert(ErrorCodes::NotWritablePrimary,
            "Transaction coordinator is not a primary",
            _catalogAndScheduler);

    return _catalogAndScheduler;
}

void TransactionCoordinatorService::joinPreviousRound() {
    stdx::unique_lock<Latch> ul(_mutex);

    // onStepDown must have been called
    invariant(!_catalogAndScheduler);

    if (!_catalogAndSchedulerToCleanup)
        return;

    auto schedulerToCleanup = _catalogAndSchedulerToCleanup;

    ul.unlock();

    LOGV2(22454, "Waiting for coordinator tasks from previous term to complete");

    // Block until all coordinators scheduled the previous time the service was primary to have
    // drained. Because the scheduler was interrupted, it should be extremely rare for there to be
    // any coordinators left, so if this actually causes blocking, it would most likely be a bug.
    schedulerToCleanup->join();

    ul.lock();
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

void TransactionCoordinatorService::cancelIfCommitNotYetStarted(
    OperationContext* opCtx,
    LogicalSessionId lsid,
    TxnNumberAndRetryCounter txnNumberAndRetryCounter) {
    auto cas = _getCatalogAndScheduler(opCtx);
    auto& catalog = cas->catalog;

    // No need to look at every coordinator since we cancel old coordinators when adding new ones.
    if (auto latestTxnNumberRetryCounterAndCoordinator = catalog.getLatestOnSession(opCtx, lsid)) {
        if (txnNumberAndRetryCounter == latestTxnNumberRetryCounterAndCoordinator->first) {
            latestTxnNumberRetryCounterAndCoordinator->second->cancelIfCommitNotYetStarted();
        }
    }
}

}  // namespace mongo
