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


#include "mongo/db/s/transaction_coordinator_service.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/admission/execution_admission_context.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/transaction_coordinator.h"
#include "mongo/db/s/transaction_coordinator_document_gen.h"
#include "mongo/db/s/transaction_coordinator_params_gen.h"
#include "mongo/db/s/transaction_coordinator_util.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/transaction/transaction_participant_gen.h"
#include "mongo/db/write_concern.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/idl/mutable_observer_registry.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/decorable.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/observable_mutex_registry.h"

#include <cstdint>
#include <mutex>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTransaction

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(hangBeforeTxnCoordinatorOnStepUpWork);

const auto transactionCoordinatorServiceDecoration =
    ServiceContext::declareDecoration<TransactionCoordinatorService>();

}  // namespace

TransactionCoordinatorService::TransactionCoordinatorService() {
    ObservableMutexRegistry::get().add("TransactionCoordinatorService::_mutex", _mutex);
}

TransactionCoordinatorService::~TransactionCoordinatorService() {
    _joinAndCleanup();
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
    auto cas = getCatalogAndScheduler(opCtx);
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
    {
        stdx::lock_guard lock(_mutex);
        _activeTransactionCoordinators.insert(coordinator);
    }
    coordinator->start(opCtx);

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
        coordinator->shutdown();
        throw;
    }
}


void TransactionCoordinatorService::reportCoordinators(OperationContext* opCtx,
                                                       bool includeIdle,
                                                       std::vector<BSONObj>* ops) {
    // TODO: SERVER-82965 Remove early return
    if (!ShardingState::get(opCtx)->enabled()) {
        return;
    }

    std::shared_ptr<CatalogAndScheduler> cas;
    try {
        cas = getCatalogAndScheduler(opCtx);
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

    auto reporter = [opCtx,
                     ops](const LogicalSessionId lsid,
                          const TxnNumberAndRetryCounter txnNumberAndRetryCounter,
                          const std::shared_ptr<TransactionCoordinator> transactionCoordinator) {
        BSONObjBuilder doc;
        transactionCoordinator->reportState(opCtx, doc);
        ops->push_back(doc.obj());
    };

    catalog.filter(predicate, reporter);
}

boost::optional<SharedSemiFuture<txn::CommitDecision>>
TransactionCoordinatorService::coordinateCommit(OperationContext* opCtx,
                                                LogicalSessionId lsid,
                                                TxnNumberAndRetryCounter txnNumberAndRetryCounter,
                                                const std::set<ShardId>& participantList) {
    auto cas = getCatalogAndScheduler(opCtx);
    auto& catalog = cas->catalog;

    auto coordinator = catalog.get(opCtx, lsid, txnNumberAndRetryCounter);
    if (!coordinator) {
        return boost::none;
    }

    coordinator->runCommit(opCtx,
                           std::vector<ShardId>{participantList.begin(), participantList.end()});

    return coordinateCommitReturnImmediatelyAfterPersistingDecision.load()
        ? coordinator->getDecision()
        : coordinator->onDecisionAcknowledged();
}

boost::optional<SharedSemiFuture<txn::CommitDecision>> TransactionCoordinatorService::recoverCommit(
    OperationContext* opCtx,
    LogicalSessionId lsid,
    TxnNumberAndRetryCounter txnNumberAndRetryCounter) {
    auto cas = getCatalogAndScheduler(opCtx);
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
        : coordinator->onDecisionAcknowledged();
}

void TransactionCoordinatorService::cancelIfCommitNotYetStarted(
    OperationContext* opCtx,
    LogicalSessionId lsid,
    TxnNumberAndRetryCounter txnNumberAndRetryCounter) {
    // TODO: SERVER-82965 Remove early return
    if (!ShardingState::get(opCtx)->enabled()) {
        return;
    }

    auto cas = getCatalogAndScheduler(opCtx);
    auto& catalog = cas->catalog;

    // No need to look at every coordinator since we cancel old coordinators when adding new ones.
    if (auto latestTxnNumberRetryCounterAndCoordinator = catalog.getLatestOnSession(opCtx, lsid)) {
        if (txnNumberAndRetryCounter == latestTxnNumberRetryCounterAndCoordinator->first) {
            latestTxnNumberRetryCounterAndCoordinator->second->cancelIfCommitNotYetStarted();
        }
    }
}

void TransactionCoordinatorService::_scheduleRecoveryTask(OperationContext* opCtx,
                                                          Milliseconds recoveryDelay) {
    auto future =
        _catalogAndScheduler->scheduler
            .scheduleWorkIn(
                recoveryDelay,
                [catalogAndScheduler = _catalogAndScheduler](OperationContext* opCtx) {
                    if (MONGO_unlikely(hangBeforeTxnCoordinatorOnStepUpWork.shouldFail())) {
                        LOGV2(8288301, "Hit hangBeforeTxnCoordinatorOnStepUpWork failpoint");
                        hangBeforeTxnCoordinatorOnStepUpWork.pauseWhileSet(opCtx);
                    }

                    // Skip ticket acquisition in order to prevent possible deadlock when
                    // participants are in the prepared state. See SERVER-82883 and SERVER-60682.
                    ScopedAdmissionPriority<ExecutionAdmissionContext> skipTicketAcquisition(
                        opCtx, AdmissionContext::Priority::kExempt);

                    auto& replClientInfo = repl::ReplClientInfo::forClient(opCtx->getClient());
                    replClientInfo.setLastOpToSystemLastOpTime(opCtx);

                    const auto lastOpTime = replClientInfo.getLastOp();
                    LOGV2_DEBUG(22451,
                                3,
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

                    auto coordinatorDocs = txn::readAllCoordinatorDocs(opCtx);

                    LOGV2(22452,
                          "Need to resume coordinating commit for transactions with an in-progress "
                          "two-phase commit/abort",
                          "numPendingTransactions"_attr = coordinatorDocs.size());

                    const auto service = opCtx->getServiceContext();
                    const auto clockSource = service->getFastClockSource();

                    auto& catalog = catalogAndScheduler->catalog;
                    auto& scheduler = catalogAndScheduler->scheduler;

                    for (const auto& doc : coordinatorDocs) {
                        LOGV2_DEBUG(22453,
                                    3,
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
                        coordinator->start(opCtx);

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

void TransactionCoordinatorService::initializeIfNeeded(OperationContext* opCtx,
                                                       long long term,
                                                       Milliseconds recoveryDelay) {
    stdx::unique_lock ul(_mutex);
    LOGV2(9307800,
          "Starting TransactionCoordinatorService initialization for specified term if needed.",
          "Current Initialized Term"_attr = _initTerm,
          "Specified Term"_attr = term);

    if (_isShuttingDown || _isInitializing || _initTerm >= term) {
        return;
    } else if (_catalogAndScheduler) {
        _initTerm = term;
        return;
    }

    _isInitializing = true;
    auto resetInitializing = [&]() {
        _isInitializing = false;
    };
    try {
        ul.unlock();
        _joinAndCleanup();
        ul.lock();

        LOGV2(9307801, "Creating the transaction catalog and scheduler for primary node.");
        _catalogAndScheduler = std::make_shared<CatalogAndScheduler>(opCtx->getServiceContext());
        _initTerm = term;

        _scheduleRecoveryTask(opCtx, recoveryDelay);
        resetInitializing();
    } catch (...) {
        if (!ul.owns_lock()) {
            ul.lock();
        }

        resetInitializing();
        throw;
    }
}

void TransactionCoordinatorService::interrupt() {
    std::vector<std::shared_ptr<TransactionCoordinator>> coordinatorsToCancel;

    {
        stdx::lock_guard lg(_mutex);
        if (_catalogAndScheduler) {
            _catalogAndSchedulerToCleanup = std::move(_catalogAndScheduler);
        }
        for (auto& ptr : _activeTransactionCoordinators) {
            if (auto shared = ptr.lock(); shared) {
                coordinatorsToCancel.emplace_back(std::move(shared));
            }
        }
        _activeTransactionCoordinators.clear();
    }

    if (_catalogAndSchedulerToCleanup) {
        _catalogAndSchedulerToCleanup->interrupt();
    }

    for (auto& ptr : coordinatorsToCancel) {
        ptr->cancel();
    }
}

void TransactionCoordinatorService::shutdown() {
    {
        stdx::lock_guard lg(_mutex);
        _isShuttingDown = true;
    }
    interrupt();
    _joinAndCleanup();
}

std::shared_ptr<TransactionCoordinatorService::CatalogAndScheduler>
TransactionCoordinatorService::getCatalogAndScheduler(OperationContext* opCtx) {
    stdx::unique_lock ul(_mutex);
    uassert(ErrorCodes::NotWritablePrimary,
            "Transaction coordinator is not a primary",
            _catalogAndScheduler);

    return _catalogAndScheduler;
}

void TransactionCoordinatorService::_joinAndCleanup() {
    std::shared_ptr<CatalogAndScheduler> schedulerToCleanup;
    {
        stdx::lock_guard _lock(_mutex);
        // checking this invariant requires holding _mutex
        invariant(!_catalogAndScheduler);
        if (!_catalogAndSchedulerToCleanup)
            return;

        schedulerToCleanup = std::move(_catalogAndSchedulerToCleanup);
    }

    LOGV2(22454, "Waiting for coordinator tasks from previous term to complete");
    // Block until all coordinators scheduled the previous time the service was primary to have
    // drained. Because the scheduler was interrupted, it should be extremely rare for there to be
    // any coordinators left, so if this actually causes blocking, it would most likely be a bug.
    schedulerToCleanup->join();
}

void TransactionCoordinatorService::CatalogAndScheduler::interrupt() {
    scheduler.shutdown({ErrorCodes::TransactionCoordinatorSteppingDown,
                        "Transaction coordinator service stepping down"});
    catalog.onStepDown();
}

void TransactionCoordinatorService::CatalogAndScheduler::join() {
    recoveryTaskCompleted->wait();
    catalog.join();
}

void TransactionCoordinatorService::notifyCoordinatorFinished(
    const std::shared_ptr<TransactionCoordinator> coordinator) {
    std::lock_guard lock(_mutex);
    // we don't need to know or care if we actually erased this weak ptr. its valid for this
    // service to cancel and clear its set of coordinators and have already erased them when
    // this continuation executes. all we're trying to do is bound memory usage.
    _activeTransactionCoordinators.erase(coordinator);
}
}  // namespace mongo
