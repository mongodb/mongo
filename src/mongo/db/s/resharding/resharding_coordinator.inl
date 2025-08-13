/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/catalog_cache/routing_information_cache.h"
#include "mongo/db/global_catalog/ddl/commit_reshard_collection_gen.h"
#include "mongo/db/global_catalog/ddl/drop_collection_if_uuid_not_matching_gen.h"
#include "mongo/db/global_catalog/ddl/notify_sharding_event_utils.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/balancer/balance_stats.h"
#include "mongo/db/s/balancer/balancer_policy.h"
#include "mongo/db/s/resharding/resharding_coordinator.h"
#include "mongo/db/s/resharding/resharding_coordinator_commit_monitor.h"
#include "mongo/db/s/resharding/resharding_coordinator_dao.h"
#include "mongo/db/s/resharding/resharding_coordinator_observer.h"
#include "mongo/db/s/resharding/resharding_coordinator_service.h"
#include "mongo/db/s/resharding/resharding_coordinator_service_external_state.h"
#include "mongo/db/s/resharding/resharding_future_util.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/sharding_environment/sharding_logging.h"
#include "mongo/db/vector_clock/vector_clock.h"
#include "mongo/s/request_types/abort_reshard_collection_gen.h"
#include "mongo/s/request_types/flush_resharding_state_change_gen.h"
#include "mongo/s/request_types/reshard_collection_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

/**
 * Compiling this file requires an extremely large amount of compiler memory
 * and time. Therefore, we compile it in smaller parts, and these parts are
 * selected by ifdefs.
 */

// If no part-specifying macro is defined, then nothing is hidden, so
// that code editors don't gray everything out.
#if !defined(RESHARDING_COORDINATOR_PART_0) && !defined(RESHARDING_COORDINATOR_PART_1) && \
    !defined(RESHARDING_COORDINATOR_PART_2) && !defined(RESHARDING_COORDINATOR_PART_3) && \
    !defined(RESHARDING_COORDINATOR_PART_4)
#define RESHARDING_COORDINATOR_PART_0
#define RESHARDING_COORDINATOR_PART_1
#define RESHARDING_COORDINATOR_PART_2
#define RESHARDING_COORDINATOR_PART_3
#define RESHARDING_COORDINATOR_PART_4
#endif

namespace mongo {
inline namespace resharding_coordinator_detail {

inline const Backoff kExponentialBackoff(Seconds(1), Milliseconds::max());

extern FailPoint reshardingPauseCoordinatorAfterPreparingToDonate;
extern FailPoint reshardingPauseCoordinatorBeforeInitializing;
extern FailPoint reshardingPauseCoordinatorBeforeCloning;
extern FailPoint reshardingPauseCoordinatorBeforeBlockingWrites;
extern FailPoint reshardingPauseCoordinatorBeforeDecisionPersisted;
extern FailPoint reshardingPauseBeforeTellingParticipantsToCommit;
extern FailPoint reshardingPauseCoordinatorBeforeRemovingStateDoc;
extern FailPoint reshardingPauseCoordinatorBeforeCompletion;
extern FailPoint reshardingPauseCoordinatorBeforeStartingErrorFlow;
extern FailPoint reshardingPauseCoordinatorBeforePersistingStateTransition;
extern FailPoint reshardingPerformValidationAfterApplying;
extern FailPoint pauseBeforeTellDonorToRefresh;
extern FailPoint pauseAfterInsertCoordinatorDoc;
extern FailPoint pauseBeforeCTHolderInitialization;
extern FailPoint pauseAfterEngagingCriticalSection;
extern FailPoint reshardingPauseBeforeTellingRecipientsToClone;

// These failpoints are declared in all parts, but only defined in part 0.
#ifdef RESHARDING_COORDINATOR_PART_0
MONGO_FAIL_POINT_DEFINE(reshardingPauseCoordinatorAfterPreparingToDonate);
MONGO_FAIL_POINT_DEFINE(reshardingPauseCoordinatorBeforeInitializing);
MONGO_FAIL_POINT_DEFINE(reshardingPauseCoordinatorBeforeCloning);
MONGO_FAIL_POINT_DEFINE(reshardingPauseCoordinatorBeforeBlockingWrites);
MONGO_FAIL_POINT_DEFINE(reshardingPauseCoordinatorBeforeDecisionPersisted);
MONGO_FAIL_POINT_DEFINE(reshardingPauseBeforeTellingParticipantsToCommit);
MONGO_FAIL_POINT_DEFINE(reshardingPauseCoordinatorBeforeRemovingStateDoc);
MONGO_FAIL_POINT_DEFINE(reshardingPauseCoordinatorBeforeCompletion);
MONGO_FAIL_POINT_DEFINE(reshardingPauseCoordinatorBeforeStartingErrorFlow);
MONGO_FAIL_POINT_DEFINE(reshardingPauseCoordinatorBeforePersistingStateTransition);
MONGO_FAIL_POINT_DEFINE(pauseBeforeTellDonorToRefresh);
MONGO_FAIL_POINT_DEFINE(pauseAfterInsertCoordinatorDoc);
MONGO_FAIL_POINT_DEFINE(pauseBeforeCTHolderInitialization);
MONGO_FAIL_POINT_DEFINE(pauseAfterEngagingCriticalSection);
MONGO_FAIL_POINT_DEFINE(reshardingPauseBeforeTellingRecipientsToClone);
#endif  // RESHARDING_COORDINATOR_PART_0

}  // namespace resharding_coordinator_detail

#ifdef RESHARDING_COORDINATOR_PART_0
ReshardingCoordinator::ReshardingCoordinator(
    ReshardingCoordinatorService* coordinatorService,
    const ReshardingCoordinatorDocument& coordinatorDoc,
    std::shared_ptr<ReshardingCoordinatorExternalState> externalState,
    ServiceContext* serviceContext)
    : repl::PrimaryOnlyService::TypedInstance<ReshardingCoordinator>(),
      _id(BSON("_id" << coordinatorDoc.getReshardingUUID())),
      _coordinatorService(coordinatorService),
      _serviceContext(serviceContext),
      _metrics{ReshardingMetrics::initializeFrom(coordinatorDoc, _serviceContext)},
      _metadata(coordinatorDoc.getCommonReshardingMetadata()),
      _coordinatorDoc(coordinatorDoc),
      _coordinatorDao(resharding::ReshardingCoordinatorDao(coordinatorDoc.getReshardingUUID())),
      _markKilledExecutor{resharding::makeThreadPoolForMarkKilledExecutor(
          "ReshardingCoordinatorCancelableOpCtxPool")},
      _reshardingCoordinatorExternalState(externalState) {
    _reshardingCoordinatorObserver = std::make_shared<ReshardingCoordinatorObserver>();

    // If the coordinator is recovering from step-up, make sure to properly initialize the
    // promises to reflect the latest state of this resharding operation.
    if (coordinatorDoc.getState() > CoordinatorStateEnum::kInitializing) {
        _reshardingCoordinatorObserver->onReshardingParticipantTransition(coordinatorDoc);
    }

    /*
     * _originalReshardingStatus is used to return the final status of the operation
     * if set. If we are in the quiesced state here, it means we completed the
     * resharding operation on a different primary and failed over. Since we
     * completed the operation previously we do not want to report a failure status
     * from aborting (if _originalReshardingStatus is empty). Explicitly set the
     * previous status to Status:OK() unless we actually had an abort reason from
     * before the failover.
     *
     * If we are in the aborting state, we do want to preserve the original abort reason
     * to report in the final status.
     */
    if (coordinatorDoc.getAbortReason()) {
        invariant(coordinatorDoc.getState() == CoordinatorStateEnum::kQuiesced ||
                  coordinatorDoc.getState() == CoordinatorStateEnum::kAborting);
        _originalReshardingStatus.emplace(resharding::getStatusFromAbortReason(coordinatorDoc));
    } else if (coordinatorDoc.getState() == CoordinatorStateEnum::kQuiesced) {
        _originalReshardingStatus.emplace(Status::OK());
    }

    _metrics->onStateTransition(boost::none, coordinatorDoc.getState());
}

void ReshardingCoordinator::_installCoordinatorDoc(const ReshardingCoordinatorDocument& doc) {
    invariant(doc.getReshardingUUID() == _coordinatorDoc.getReshardingUUID());
    _coordinatorDoc = doc;
}

void ReshardingCoordinator::installCoordinatorDocOnStateTransition(
    OperationContext* opCtx, const ReshardingCoordinatorDocument& doc) {
    const auto previousState = _coordinatorDoc.getState();

    _installCoordinatorDoc(doc);

    BSONObjBuilder bob;
    bob.append("newState", CoordinatorState_serializer(_coordinatorDoc.getState()));
    bob.append("oldState", CoordinatorState_serializer(previousState));
    bob.append("namespace",
               NamespaceStringUtil::serialize(_coordinatorDoc.getSourceNss(),
                                              SerializationContext::stateDefault()));
    bob.append("collectionUUID", _coordinatorDoc.getSourceUUID().toString());
    bob.append("reshardingUUID", _coordinatorDoc.getReshardingUUID().toString());

    LOGV2_INFO(5343001,
               "Transitioned resharding coordinator state",
               "newState"_attr = CoordinatorState_serializer(_coordinatorDoc.getState()),
               "oldState"_attr = CoordinatorState_serializer(previousState),
               logAttrs(_coordinatorDoc.getSourceNss()),
               "collectionUUID"_attr = _coordinatorDoc.getSourceUUID(),
               "reshardingUUID"_attr = _coordinatorDoc.getReshardingUUID());

    _metrics->onStateTransition(previousState, _coordinatorDoc.getState());

    ShardingLogging::get(opCtx)->logChange(opCtx,
                                           "resharding.coordinator.transition",
                                           _coordinatorDoc.getSourceNss(),
                                           bob.obj(),
                                           resharding::kMajorityWriteConcern);
}
#endif  // RESHARDING_COORDINATOR_PART_0

inline namespace resharding_coordinator_detail {
// Declared in all parts and defined in part 0.
void markCompleted(const Status& status, ReshardingMetrics* metrics);
#ifdef RESHARDING_COORDINATOR_PART_0
void markCompleted(const Status& status, ReshardingMetrics* metrics) {
    if (status.isOK()) {
        metrics->onSuccess();
    } else if (status == ErrorCodes::ReshardCollectionAborted) {
        metrics->onCanceled();
    } else {
        metrics->onFailure();
    }
}
#endif  // RESHARDING_COORDINATOR_PART_0
}  // namespace resharding_coordinator_detail

#ifdef RESHARDING_COORDINATOR_PART_1
ExecutorFuture<void> ReshardingCoordinator::_tellAllParticipantsReshardingStarted(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kPreparingToDonate) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return resharding::WithAutomaticRetry([this, executor] {
               return ExecutorFuture<void>(**executor)
                   .then([this] {
                       // Ensure the flushes to create participant state machines don't get
                       // interrupted upon abort.
                       _cancelableOpCtxFactory.emplace(_ctHolder->getStepdownToken(),
                                                       _markKilledExecutor);
                   })
                   .then([this] {
                       return resharding::waitForMajority(_ctHolder->getStepdownToken(),
                                                          *_cancelableOpCtxFactory);
                   })
                   .then([this, executor]() {
                       pauseBeforeTellDonorToRefresh.pauseWhileSet();
                       _establishAllDonorsAsParticipants(executor);
                   })
                   .then([this, executor] { _establishAllRecipientsAsParticipants(executor); })
                   .onCompletion([this](Status status) {
                       // Swap back to using operation contexts canceled upon abort until ready to
                       // persist the decision or unrecoverable error.
                       _cancelableOpCtxFactory.emplace(_ctHolder->getAbortToken(),
                                                       _markKilledExecutor);

                       return status;
                   });
           })
        .onTransientError([](const Status& status) {
            LOGV2(5093702,
                  "Resharding coordinator encountered transient error while telling participants "
                  "to refresh",
                  "error"_attr = status);
        })
        .onUnrecoverableError([](const Status& status) {
            LOGV2(10494612,
                  "Resharding coordinator encountered unrecoverable error while telling "
                  "participants to refresh",
                  "error"_attr = status);
        })
        .until<Status>([](const Status& status) { return status.isOK(); })
        .on(**executor, _ctHolder->getStepdownToken());
}

ExecutorFuture<void> ReshardingCoordinator::_initializeCoordinator(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    return resharding::WithAutomaticRetry([this, executor] {
               return ExecutorFuture<void>(**executor)
                   .then([this] { _insertCoordDocAndChangeOrigCollEntry(); })
                   .then([this] { _calculateParticipantsAndChunksThenWriteToDisk(); });
           })
        .onTransientError([](const Status& status) {
            LOGV2(5093703,
                  "Resharding coordinator encountered transient error while initializing",
                  "error"_attr = status);
        })
        .onUnrecoverableError([](const Status& status) {
            LOGV2(10494613,
                  "Resharding coordinator encountered unrecoverable error while initializing",
                  "error"_attr = status);
        })
        .until<Status>([](const Status& status) { return status.isOK(); })
        .on(**executor, _ctHolder->getAbortToken())
        .onCompletion([this, executor](Status status) {
            if (_ctHolder->isSteppingOrShuttingDown()) {
                return ExecutorFuture<void>(**executor, status);
            }

            if (_coordinatorDoc.getState() != CoordinatorStateEnum::kPreparingToDonate) {
                return ExecutorFuture<void>(**executor, status);
            }

            // Regardless of error or non-error, guarantee that once the coordinator
            // completes its transition to kPreparingToDonate, participants are aware of
            // the resharding operation and their state machines are created.
            return _tellAllParticipantsReshardingStarted(executor);
        })
        .onError([this, executor](Status status) {
            {
                auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
                reshardingPauseCoordinatorBeforeStartingErrorFlow.pauseWhileSet(opCtx.get());
            }

            if (_ctHolder->isSteppingOrShuttingDown()) {
                return ExecutorFuture<void>(**executor, status);
            }

            if (_ctHolder->isAborted()) {
                // If the abort cancellation token was triggered, implying that a user ran the abort
                // command, override status with a resharding abort error.
                //
                // Note for debugging purposes: Ensure the original error status is recorded in the
                // logs before replacing it.
                status = {ErrorCodes::ReshardCollectionAborted, "aborted"};
            }

            auto nss = _coordinatorDoc.getSourceNss();

            // If we have an original resharding status due to a failover occurring, we want to
            // log the original abort reason from before the failover in lieu of the generic
            // ReshardCollectionAborted status.
            LOGV2(4956903,
                  "Resharding failed",
                  logAttrs(nss),
                  "newShardKeyPattern"_attr = _coordinatorDoc.getReshardingKey(),
                  "error"_attr = _originalReshardingStatus ? *_originalReshardingStatus : status);

            // Allow abort to continue except when stepped down.
            _cancelableOpCtxFactory.emplace(_ctHolder->getStepdownToken(), _markKilledExecutor);

            // If we're already quiesced here it means we failed over and need to preserve the
            // original abort reason.
            if (_coordinatorDoc.getState() == CoordinatorStateEnum::kQuiesced) {
                markCompleted(*_originalReshardingStatus, _metrics.get());
                // We must return status here, not _originalReshardingStatus, because the latter
                // may be Status::OK() and not abort the future flow.
                return ExecutorFuture<void>(**executor, status);
            } else if (_coordinatorDoc.getState() < CoordinatorStateEnum::kPreparingToDonate) {
                return _onAbortCoordinatorOnly(executor, status);
            } else {
                return _onAbortCoordinatorAndParticipants(executor, status);
            }
        });
}

ExecutorFuture<ReshardingCoordinatorDocument> ReshardingCoordinator::_runUntilReadyToCommit(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    return resharding::WithAutomaticRetry([this, executor] {
               return ExecutorFuture<void>(**executor)
                   .then([this, executor] { return _awaitAllDonorsReadyToDonate(executor); })
                   .then([this, executor] {
                       if (_coordinatorDoc.getState() == CoordinatorStateEnum::kCloning) {
                           if (resharding::gFeatureFlagReshardingCloneNoRefresh.isEnabled(
                                   serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
                               _tellAllRecipientsToClone(executor);
                           } else {
                               _tellAllRecipientsToRefresh(executor);
                           }
                           _tellAllDonorsToStartChangeStreamsMonitor(executor);
                       }
                   })
                   .then([this, executor] {
                       return _fetchAndPersistNumDocumentsToCloneFromDonors(executor);
                   })
                   .then([this, executor] { return _awaitAllRecipientsFinishedCloning(executor); })
                   .then([this, executor] {
                       if (_coordinatorDoc.getState() == CoordinatorStateEnum::kApplying) {
                           _tellAllDonorsToRefresh(executor);
                       }
                   })
                   .then([this, executor] { return _awaitAllRecipientsFinishedApplying(executor); })
                   .then([this, executor] {
                       if (_coordinatorDoc.getState() == CoordinatorStateEnum::kBlockingWrites) {
                           _tellAllDonorsToRefresh(executor);
                           _tellAllRecipientsToRefresh(executor);
                       }
                   })
                   .then([this, executor] {
                       return _fetchAndPersistNumDocumentsFinalFromDonors(executor);
                   })
                   .then([this, executor] {
                       return _awaitAllRecipientsInStrictConsistency(executor);
                   });
           })
        .onTransientError([](const Status& status) {
            LOGV2(5093704,
                  "Resharding coordinator encountered transient error",
                  "error"_attr = status);
        })
        .onUnrecoverableError([](const Status& status) {})
        .until<StatusWith<ReshardingCoordinatorDocument>>(
            [](const StatusWith<ReshardingCoordinatorDocument>& status) { return status.isOK(); })
        .on(**executor, _ctHolder->getAbortToken())
        .onCompletion([this](auto passthroughFuture) {
            _cancelableOpCtxFactory.emplace(_ctHolder->getStepdownToken(), _markKilledExecutor);
            return passthroughFuture;
        })
        .onError([this, executor](Status status) -> ExecutorFuture<ReshardingCoordinatorDocument> {
            {
                auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
                reshardingPauseCoordinatorBeforeStartingErrorFlow.pauseWhileSet(opCtx.get());
            }

            if (_ctHolder->isSteppingOrShuttingDown()) {
                return ExecutorFuture<ReshardingCoordinatorDocument>(**executor, status);
            }

            if (_ctHolder->isAborted()) {
                // If the abort cancellation token was triggered, implying that a user ran the abort
                // command, override status with a resharding abort error.
                status = {ErrorCodes::ReshardCollectionAborted, "aborted"};
            }

            auto nss = _coordinatorDoc.getSourceNss();
            LOGV2(4956902,
                  "Resharding failed",
                  logAttrs(nss),
                  "newShardKeyPattern"_attr = _coordinatorDoc.getReshardingKey(),
                  "error"_attr = status);

            invariant(_coordinatorDoc.getState() >= CoordinatorStateEnum::kPreparingToDonate);

            return _onAbortCoordinatorAndParticipants(executor, status)
                .onCompletion([](Status status) {
                    return StatusWith<ReshardingCoordinatorDocument>(status);
                });
        });
}

ExecutorFuture<void> ReshardingCoordinator::_commitAndFinishReshardOperation(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const ReshardingCoordinatorDocument& updatedCoordinatorDoc) {
    return resharding::WithAutomaticRetry([this, executor, updatedCoordinatorDoc] {
               return ExecutorFuture<void>(**executor)
                   .then([this, executor] {
                       auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
                       if (feature_flags::gFeatureFlagChangeStreamPreciseShardTargeting.isEnabled(
                               VersionContext::getDecoration(opCtx.get()),
                               serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
                           // V2 change stream readers expect to see an op entry concerning the
                           // commit before this materializes into the global catalog. (Multiple
                           // copies of this event notification are acceptable)
                           _generateCommitNotificationForChangeStreams(
                               opCtx.get(),
                               executor,
                               ChangeStreamCommitNotificationMode::BeforeWriteOnCatalog);
                       }
                   })
                   .then(
                       [this, executor, updatedCoordinatorDoc] { _commit(updatedCoordinatorDoc); });
           })
        .onTransientError([](const Status& status) {
            LOGV2(7698801,
                  "Resharding coordinator encountered transient error while committing",
                  "error"_attr = status);
        })
        .onUnrecoverableError([](const Status& status) {
            LOGV2(10494614,
                  "Resharding coordinator encountered unrecoverable error while committing",
                  "error"_attr = status);
        })
        .until<Status>([](const Status& status) { return status.isOK(); })
        .on(**executor, _ctHolder->getStepdownToken())
        .onError([this, executor](Status status) {
            if (status == ErrorCodes::TransactionTooLargeForCache) {
                return _onAbortCoordinatorAndParticipants(executor, status);
            }
            return ExecutorFuture<void>(**executor, status);
        })
        .then([this, executor, updatedCoordinatorDoc] {
            return resharding::WithAutomaticRetry([this, executor, updatedCoordinatorDoc] {
                       return ExecutorFuture<void>(**executor)
                           .then([this] {
                               return resharding::waitForMajority(_ctHolder->getStepdownToken(),
                                                                  *_cancelableOpCtxFactory);
                           })
                           .thenRunOn(**executor)
                           .then([this, executor] {
                               auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
                               if (feature_flags::gFeatureFlagChangeStreamPreciseShardTargeting
                                       .isEnabled(VersionContext::getDecoration(opCtx.get()),
                                                  serverGlobalParams.featureCompatibility
                                                      .acquireFCVSnapshot())) {
                                   // V2 change stream readers expect to see an op entry concerning
                                   // on the placement change caused by the commit after this has
                                   // been majority written on the global catalog.
                                   _generatePlacementChangeNotificationForChangeStreams(opCtx.get(),
                                                                                        executor);
                               } else {
                                   // Legacy change stream readers are only able to consume
                                   // notifications concerning the commit once this has been
                                   // majority written on the global catalog.
                                   _generateCommitNotificationForChangeStreams(
                                       opCtx.get(),
                                       executor,
                                       ChangeStreamCommitNotificationMode::
                                           AfterWriteOnCatalogLegacy);
                               }
                           })
                           .then([this, executor] {
                               _tellAllParticipantsToCommit(_coordinatorDoc.getSourceNss(),
                                                            executor);
                           })
                           .then([this] {
                               _updateChunkImbalanceMetrics(_coordinatorDoc.getSourceNss());
                           })
                           .then([this, updatedCoordinatorDoc] {
                               auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
                               resharding::removeChunkDocs(opCtx.get(),
                                                           updatedCoordinatorDoc.getSourceUUID());
                               return Status::OK();
                           })
                           .then([this, executor] {
                               return _awaitAllParticipantShardsDone(executor);
                           })
                           .then([this, executor] {
                               _metrics->setEndFor(CoordinatorStateEnum::kBlockingWrites,
                                                   resharding::getCurrentTime());

                               // Best-effort attempt to trigger a refresh on the participant shards
                               // so they see the collection metadata without reshardingFields and
                               // no longer throw ReshardCollectionInProgress. There is no guarantee
                               // this logic ever runs if the config server primary steps down after
                               // having removed the coordinator state document.
                               return _tellAllRecipientsToRefresh(executor);
                           });
                   })
                .onTransientError([](const Status& status) {
                    LOGV2(5093705,
                          "Resharding coordinator encountered transient error while committing",
                          "error"_attr = status);
                })
                .onUnrecoverableError([](const Status& status) {
                    LOGV2(10494615,
                          "Resharding coordinator encountered unrecoverable error while "
                          "committing",
                          "error"_attr = status);
                })
                .until<Status>([](const Status& status) { return status.isOK(); })
                .on(**executor, _ctHolder->getStepdownToken())
                .onError([this, executor](Status status) {
                    {
                        auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
                        reshardingPauseCoordinatorBeforeStartingErrorFlow.pauseWhileSet(
                            opCtx.get());
                    }

                    if (_ctHolder->isSteppingOrShuttingDown()) {
                        return status;
                    }

                    LOGV2_FATAL(
                        5277000,
                        "Unrecoverable error past the point resharding was guaranteed to succeed",
                        "error"_attr = redact(status));
                });
        });
}

SemiFuture<void> ReshardingCoordinator::run(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                            const CancellationToken& stepdownToken) noexcept {
    getObserver()->reshardingCoordinatorRunCalled();
    pauseBeforeCTHolderInitialization.pauseWhileSet();

    auto abortCalled = [&] {
        stdx::lock_guard<stdx::mutex> lk(_abortCalledMutex);
        _ctHolder = std::make_unique<CoordinatorCancellationTokenHolder>(stepdownToken);
        return _abortCalled;
    }();

    if (abortCalled) {
        if (abortCalled == AbortType::kAbortSkipQuiesce) {
            _ctHolder->cancelQuiescePeriod();
        }
        _ctHolder->abort();
    }

    _markKilledExecutor->startup();
    _cancelableOpCtxFactory.emplace(_ctHolder->getAbortToken(), _markKilledExecutor);

    return _isReshardingOpRedundant(executor)
        .thenRunOn(_coordinatorService->getInstanceCleanupExecutor())
        .onCompletion([this, self = shared_from_this(), executor](
                          StatusWith<bool> isOpRedundantSW) -> ExecutorFuture<void> {
            if (isOpRedundantSW.isOK() && isOpRedundantSW.getValue()) {
                this->_coordinatorService->releaseInstance(this->_id, isOpRedundantSW.getStatus());
                _coordinatorDocWrittenPromise.emplaceValue();
                _completionPromise.emplaceValue();
                _reshardingCoordinatorObserver->fulfillPromisesBeforePersistingStateDoc();
                return ExecutorFuture<void>(**executor, isOpRedundantSW.getStatus());
            } else if (!isOpRedundantSW.isOK()) {
                this->_coordinatorService->releaseInstance(this->_id, isOpRedundantSW.getStatus());
                _coordinatorDocWrittenPromise.setError(isOpRedundantSW.getStatus());
                _completionPromise.setError(isOpRedundantSW.getStatus());
                _reshardingCoordinatorObserver->interrupt(isOpRedundantSW.getStatus());
                return ExecutorFuture<void>(**executor, isOpRedundantSW.getStatus());
            }
            return _runReshardingOp(executor);
        })
        .onCompletion([this, self = shared_from_this(), executor](Status status) {
            _cancelableOpCtxFactory.emplace(_ctHolder->getStepdownToken(), _markKilledExecutor);
            return _quiesce(executor, std::move(status));
        })
        .semi();
}

ExecutorFuture<void> ReshardingCoordinator::_quiesce(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor, Status status) {
    if (_coordinatorDoc.getState() == CoordinatorStateEnum::kQuiesced) {
        return (*executor)
            ->sleepUntil(*_coordinatorDoc.getQuiescePeriodEnd(), _ctHolder->getCancelQuiesceToken())
            .onCompletion([this, self = shared_from_this(), executor, status](Status sleepStatus) {
                LOGV2_DEBUG(7760405,
                            1,
                            "Resharding coordinator quiesce period done",
                            "reshardingUUID"_attr = _coordinatorDoc.getReshardingUUID());
                if (!_ctHolder->isSteppingOrShuttingDown()) {
                    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
                    ReshardingCoordinatorDocument updatedCoordinatorDoc = _coordinatorDoc;
                    updatedCoordinatorDoc.setState(CoordinatorStateEnum::kDone);
                    resharding::executeMetadataChangesInTxn(
                        opCtx.get(),
                        [&updatedCoordinatorDoc](OperationContext* opCtx, TxnNumber txnNumber) {
                            resharding::writeToCoordinatorStateNss(
                                opCtx,
                                nullptr /* metrics have already been freed */
                                ,
                                updatedCoordinatorDoc,
                                txnNumber);
                        });
                    LOGV2_DEBUG(7760406,
                                1,
                                "Resharding coordinator removed state doc after quiesce",
                                "reshardingUUID"_attr = _coordinatorDoc.getReshardingUUID());
                }
                return status;
            })
            .thenRunOn(_coordinatorService->getInstanceCleanupExecutor())
            .onCompletion([this, self = shared_from_this(), executor, status](Status deleteStatus) {
                _quiescePeriodFinishedPromise.emplaceValue();
                return status;
            });
    }
    // No quiesce period is required.
    _quiescePeriodFinishedPromise.emplaceValue();
    return ExecutorFuture<void>(**executor, status);
}

ExecutorFuture<void> ReshardingCoordinator::_runReshardingOp(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    return _initializeCoordinator(executor)
        .then([this, executor] { return _runUntilReadyToCommit(executor); })
        .then([this, executor](const ReshardingCoordinatorDocument& updatedCoordinatorDoc) {
            return _commitAndFinishReshardOperation(executor, updatedCoordinatorDoc);
        })
        .onCompletion([this, executor](Status status) {
            auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
            reshardingPauseCoordinatorBeforeCompletion.executeIf(
                [&](const BSONObj&) {
                    reshardingPauseCoordinatorBeforeCompletion.pauseWhileSetAndNotCanceled(
                        opCtx.get(), _ctHolder->getStepdownToken());
                },
                [&](const BSONObj& data) {
                    auto ns = data.getStringField("sourceNamespace");
                    return ns.empty() ? true
                                      : std::string{ns} ==
                            NamespaceStringUtil::serialize(_coordinatorDoc.getSourceNss(),
                                                           SerializationContext::stateDefault());
                });

            {
                auto lg = stdx::lock_guard(_fulfillmentMutex);
                // reportStatus is the status reported back to the caller, which may be
                // different than the status if we interrupted the future chain because the
                // resharding was already completed on a previous primary.
                auto reportStatus = _originalReshardingStatus.value_or(status);
                if (reportStatus.isOK()) {
                    _completionPromise.emplaceValue();

                    if (!_coordinatorDocWrittenPromise.getFuture().isReady()) {
                        _coordinatorDocWrittenPromise.emplaceValue();
                    }
                } else {
                    _completionPromise.setError(reportStatus);

                    if (!_coordinatorDocWrittenPromise.getFuture().isReady()) {
                        _coordinatorDocWrittenPromise.setError(reportStatus);
                    }
                }
            }

            if (_criticalSectionTimeoutCbHandle) {
                (*executor)->cancel(*_criticalSectionTimeoutCbHandle);
            }

            return status;
        })
        .thenRunOn(_coordinatorService->getInstanceCleanupExecutor())
        .onCompletion([this](Status outerStatus) {
            // Wait for the commit monitor to halt. We ignore any ignores because the
            // ReshardingCoordinator instance is already exiting at this point.
            return _commitMonitorQuiesced
                .thenRunOn(_coordinatorService->getInstanceCleanupExecutor())
                .onCompletion([outerStatus](Status) { return outerStatus; });
        })
        .onCompletion([this, self = shared_from_this()](Status status) {
            _metrics->onStateTransition(_coordinatorDoc.getState(), boost::none);
            _logStatsOnCompletion(status.isOK());

            // Unregister metrics early so the cumulative metrics do not continue to track these
            // metrics for the lifetime of this state machine. We have future callbacks copy shared
            // pointers to this state machine that causes it to live longer than expected, and can
            // potentially overlap with a newer instance when stepping up.
            _metrics->deregisterMetrics();

            if (!status.isOK()) {
                {
                    auto lg = stdx::lock_guard(_fulfillmentMutex);
                    if (!_completionPromise.getFuture().isReady()) {
                        _completionPromise.setError(status);
                    }

                    if (!_coordinatorDocWrittenPromise.getFuture().isReady()) {
                        _coordinatorDocWrittenPromise.setError(status);
                    }
                }
                _reshardingCoordinatorObserver->interrupt(status);
            }
        });
}

ExecutorFuture<void> ReshardingCoordinator::_onAbortCoordinatorOnly(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor, const Status& status) {
    if (_coordinatorDoc.getState() == CoordinatorStateEnum::kUnused) {
        return ExecutorFuture<void>(**executor, status);
    }

    return resharding::WithAutomaticRetry([this, executor, status] {
               auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());

               // Notify metrics as the operation is now complete for external observers.
               markCompleted(status, _metrics.get());

               // The temporary collection and its corresponding entries were never created. Only
               // the coordinator document and reshardingFields require cleanup.
               _removeOrQuiesceCoordinatorDocAndRemoveReshardingFields(opCtx.get(), status);
               return status;
           })
        .onTransientError([](const Status& retryStatus) {
            LOGV2(5093706,
                  "Resharding coordinator encountered transient error while aborting",
                  "error"_attr = retryStatus);
        })
        .onUnrecoverableError([](const Status& retryStatus) {
            LOGV2(10494616,
                  "Resharding coordinator encountered unrecoverable error while aborting",
                  "error"_attr = retryStatus);
        })
        .until<Status>([](const Status& retryStatus) { return retryStatus.isOK(); })
        .on(**executor, _ctHolder->getStepdownToken())
        // Return back original status.
        .then([status] { return status; });
}

ExecutorFuture<void> ReshardingCoordinator::_onAbortCoordinatorAndParticipants(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor, const Status& status) {
    // Participants should never be waited upon to complete the abort if they were never made aware
    // of the resharding operation (the coordinator flushing its state change to
    // kPreparingToDonate).
    invariant(_coordinatorDoc.getState() >= CoordinatorStateEnum::kPreparingToDonate);

    return resharding::WithAutomaticRetry([this, executor, status] {
               return ExecutorFuture<void>(**executor)
                   .then([this, executor, status] {
                       if (_coordinatorDoc.getState() != CoordinatorStateEnum::kAborting) {
                           // The coordinator only transitions into kAborting if there are
                           // participants to wait on before transitioning to kDone.
                           _updateCoordinatorDocStateAndCatalogEntries(
                               [=, this](OperationContext* opCtx, TxnNumber txnNumber) {
                                   auto previousPhase = _coordinatorDao.getPhase(opCtx, txnNumber);
                                   auto now = resharding::getCurrentTime();
                                   auto updatedDocument = _coordinatorDao.transitionToAbortingPhase(
                                       opCtx, now, status, txnNumber);
                                   _metrics->setEndFor(previousPhase, now);
                                   return updatedDocument;
                               });
                       }
                   })
                   .then([this] {
                       return resharding::waitForMajority(_ctHolder->getStepdownToken(),
                                                          *_cancelableOpCtxFactory);
                   })
                   .thenRunOn(**executor)
                   .then([this, executor, status] {
                       _tellAllParticipantsToAbort(executor,
                                                   status == ErrorCodes::ReshardCollectionAborted);

                       // Wait for all participants to acknowledge the operation reached an
                       // unrecoverable error.
                       return future_util::withCancellation(
                           _awaitAllParticipantShardsDone(executor), _ctHolder->getStepdownToken());
                   });
           })
        .onTransientError([](const Status& retryStatus) {
            LOGV2(5093707,
                  "Resharding coordinator encountered transient error while aborting all "
                  "participants",
                  "error"_attr = retryStatus);
        })
        .onUnrecoverableError([](const Status& retryStatus) {
            LOGV2(10494617,
                  "Resharding coordinator encountered unrecoverable error while aborting all "
                  "participants",
                  "error"_attr = retryStatus);
        })
        .until<Status>([](const Status& retryStatus) { return retryStatus.isOK(); })
        .on(**executor, _ctHolder->getStepdownToken())
        // Return back the original status.
        .then([status] { return status; });
}

void ReshardingCoordinator::abort(bool skipQuiescePeriod) {
    auto ctHolderInitialized = [&] {
        stdx::lock_guard<stdx::mutex> lk(_abortCalledMutex);
        skipQuiescePeriod = skipQuiescePeriod || _abortCalled == AbortType::kAbortSkipQuiesce;
        _abortCalled =
            skipQuiescePeriod ? AbortType::kAbortSkipQuiesce : AbortType::kAbortWithQuiesce;
        return !(_ctHolder == nullptr);
    }();

    if (ctHolderInitialized) {
        if (skipQuiescePeriod)
            _ctHolder->cancelQuiescePeriod();
        _ctHolder->abort();
    }
}

boost::optional<BSONObj> ReshardingCoordinator::reportForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode,
    MongoProcessInterface::CurrentOpSessionsMode) noexcept {
    return _metrics->reportForCurrentOp();
}

std::shared_ptr<ReshardingCoordinatorObserver> ReshardingCoordinator::getObserver() {
    return _reshardingCoordinatorObserver;
}

void ReshardingCoordinator::onOkayToEnterCritical() {
    _fulfillOkayToEnterCritical(Status::OK());
}

void ReshardingCoordinator::_fulfillOkayToEnterCritical(Status status) {
    auto lg = stdx::lock_guard(_fulfillmentMutex);
    if (_canEnterCritical.getFuture().isReady())
        return;

    if (status.isOK()) {
        LOGV2(5391601, "Marking resharding operation okay to enter critical section");
        _canEnterCritical.emplaceValue();
    } else {
        _canEnterCritical.setError(std::move(status));
    }
}

ExecutorFuture<bool> ReshardingCoordinator::_isReshardingOpRedundant(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    // We only check for redundancy when the resharding op first starts as it would be unsafe to
    // skip the remainder of the cleanup for the resharding operation if there was a primary
    // failover after the CoordinatorStateEnum::kCommitting state had been reached.
    if (_coordinatorDoc.getState() != CoordinatorStateEnum::kUnused) {
        return ExecutorFuture<bool>(**executor, false);
    }

    return resharding::WithAutomaticRetry([this, executor] {
               auto cancelableOpCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
               auto opCtx = cancelableOpCtx.get();

               const auto cm = uassertStatusOK(
                   RoutingInformationCache::get(opCtx)->getCollectionPlacementInfoWithRefresh(
                       opCtx, _coordinatorDoc.getSourceNss()));

               uassert(ErrorCodes::NamespaceNotFound,
                       fmt::format("Expected collection '{}' to be tracked on cluster catalog",
                                   _coordinatorDoc.getSourceNss().toStringForErrorMsg()),
                       cm.hasRoutingTable());

               if (resharding::isMoveCollection(_metadata.getProvenance())) {
                   // Verify if the moveCollection is redundant by checking if the operation is
                   // attempting to move to the same shard.
                   std::set<ShardId> shardIdsSet;
                   cm.getAllShardIds(&shardIdsSet);
                   const auto toShard =
                       _coordinatorDoc.getShardDistribution().get().front().getShard();
                   return shardIdsSet.find(toShard) != shardIdsSet.end();
               } else if (_metadata.getProvenance() &&
                          _metadata.getProvenance().get() ==
                              ReshardingProvenanceEnum::kUnshardCollection) {
                   std::set<ShardId> shardIdsSet;
                   cm.getAllShardIds(&shardIdsSet);
                   const auto toShard =
                       _coordinatorDoc.getShardDistribution().get().front().getShard();
                   return !cm.isSharded() && shardIdsSet.find(toShard) != shardIdsSet.end();
               }

               const auto currentShardKey = cm.getShardKeyPattern().getKeyPattern();
               // Verify if there is any work to be done by the resharding operation by checking
               // if the existing shard key matches the desired new shard key.
               bool isOpRedundant = SimpleBSONObjComparator::kInstance.evaluate(
                   currentShardKey.toBSON() == _coordinatorDoc.getReshardingKey().toBSON());

               // If forceRedistribution is true, still do resharding.
               if (isOpRedundant && _coordinatorDoc.getForceRedistribution() &&
                   *_coordinatorDoc.getForceRedistribution()) {
                   return false;
               }

               // If this is not forced same-key resharding, set forceRedistribution to false so
               // we can identify forced same-key resharding by this field later.
               _coordinatorDoc.setForceRedistribution(false);
               return isOpRedundant;
           })
        .onTransientError([](const StatusWith<bool>& status) {
            LOGV2(7074600,
                  "Resharding coordinator encountered transient error refreshing routing info",
                  "error"_attr = status.getStatus());
        })
        .onUnrecoverableError([](const StatusWith<bool>& status) {
            LOGV2(10494618,
                  "Resharding coordinator encountered unrecoverable error refreshing routing info",
                  "error"_attr = status.getStatus());
        })
        .until<StatusWith<bool>>([](const StatusWith<bool>& status) { return status.isOK(); })
        .on(**executor, _ctHolder->getAbortToken())
        .onError(([this, executor](StatusWith<bool> status) {
            if (_ctHolder->isAborted()) {
                // If the abort cancellation token was triggered, implying that a user ran the
                // abort command, override status with a resharding abort error.
                //
                // Note for debugging purposes: Ensure the original error status is recorded in
                // the logs before replacing it.
                status = {ErrorCodes::ReshardCollectionAborted, "aborted"};
            }
            return status;
        }));
}

void ReshardingCoordinator::_insertCoordDocAndChangeOrigCollEntry() {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kUnused) {
        if (!_coordinatorDocWrittenPromise.getFuture().isReady()) {
            _coordinatorDocWrittenPromise.emplaceValue();
        }

        if (_coordinatorDoc.getState() == CoordinatorStateEnum::kAborting ||
            _coordinatorDoc.getState() == CoordinatorStateEnum::kQuiesced) {
            _ctHolder->abort();
        }
    } else {
        auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
        reshardingPauseCoordinatorBeforeInitializing.pauseWhileSetAndNotCanceled(
            opCtx.get(), _ctHolder->getStepdownToken());
        ReshardingCoordinatorDocument updatedCoordinatorDoc = _coordinatorDoc;
        updatedCoordinatorDoc.setState(CoordinatorStateEnum::kInitializing);
        resharding::insertCoordDocAndChangeOrigCollEntry(
            opCtx.get(), _metrics.get(), updatedCoordinatorDoc);
        installCoordinatorDocOnStateTransition(opCtx.get(), updatedCoordinatorDoc);

        _coordinatorDocWrittenPromise.emplaceValue();
    }

    const bool isSameKeyResharding =
        _coordinatorDoc.getForceRedistribution() && *_coordinatorDoc.getForceRedistribution();
    _metrics->setIsSameKeyResharding(isSameKeyResharding);
    _metrics->onStarted();

    pauseAfterInsertCoordinatorDoc.pauseWhileSet();
}

void ReshardingCoordinator::_calculateParticipantsAndChunksThenWriteToDisk() {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kInitializing) {
        return;
    }
    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    auto provenance = _coordinatorDoc.getCommonReshardingMetadata().getProvenance();

    std::vector<ReshardingZoneType> zones;
    if (resharding::isUnshardCollection(provenance)) {
        // Since the resulting collection of an unshardCollection operation cannot have zones, we do
        // not need to account for existing zones in the original collection. Existing zones from
        // the original collection will be deleted after the unsharding operation commits.
        uassert(ErrorCodes::InvalidOptions,
                "Cannot specify zones when unsharding a collection.",
                !_coordinatorDoc.getZones());
    } else {
        if (_coordinatorDoc.getZones()) {
            zones = *_coordinatorDoc.getZones();
        } else if (_coordinatorDoc.getForceRedistribution() &&
                   *_coordinatorDoc.getForceRedistribution()) {
            // If zones are not provided by the user for same-key resharding, we should use the
            // existing zones for this resharding operation.
            zones = resharding::getZonesFromExistingCollection(opCtx.get(),
                                                               _coordinatorDoc.getSourceNss());
        }
    }

    auto shardsAndChunks = _reshardingCoordinatorExternalState->calculateParticipantShardsAndChunks(
        opCtx.get(), _coordinatorDoc, zones);

    auto isUnsplittable = _reshardingCoordinatorExternalState->getIsUnsplittable(
                              opCtx.get(), _coordinatorDoc.getSourceNss()) ||
        (provenance && provenance.get() == ReshardingProvenanceEnum::kUnshardCollection);

    resharding::PhaseTransitionFn phaseTransitionFn = [=, this](OperationContext* opCtx,
                                                                TxnNumber txnNumber) {
        auto updatedDocument =
            _coordinatorDao.transitionToPreparingToDonatePhase(opCtx, shardsAndChunks, txnNumber);
        return updatedDocument;
    };

    resharding::writeParticipantShardsAndTempCollInfo(opCtx.get(),
                                                      _metrics.get(),
                                                      _coordinatorDoc,
                                                      std::move(phaseTransitionFn),
                                                      std::move(shardsAndChunks.initialChunks),
                                                      std::move(zones),
                                                      isUnsplittable);
    installCoordinatorDocOnStateTransition(
        opCtx.get(),
        resharding::getCoordinatorDoc(opCtx.get(), _coordinatorDoc.getReshardingUUID()));

    reshardingPauseCoordinatorAfterPreparingToDonate.pauseWhileSetAndNotCanceled(
        opCtx.get(), _ctHolder->getAbortToken());
}

#endif  // RESHARDING_COORDINATOR_PART_1
#ifdef RESHARDING_COORDINATOR_PART_2

namespace {
ReshardingApproxCopySize computeApproxCopySize(OperationContext* opCtx,
                                               ReshardingCoordinatorDocument& coordinatorDoc) {
    const auto cm =
        uassertStatusOK(RoutingInformationCache::get(opCtx)->getCollectionPlacementInfoWithRefresh(
            opCtx, coordinatorDoc.getTempReshardingNss()));
    const auto numRecipientsToCopy = cm.getNShardsOwningChunks();
    iassert(ErrorCodes::BadValue,
            "Expected to find at least one recipient for the collection",
            numRecipientsToCopy > 0);

    // Compute the aggregate for the number of documents and bytes to copy.
    long aggBytesToCopy = 0, aggDocumentsToCopy = 0;
    for (auto donor : coordinatorDoc.getDonorShards()) {
        if (const auto bytesToClone = donor.getMutableState().getBytesToClone()) {
            aggBytesToCopy += *bytesToClone;
        }

        if (const auto documentsToClone = donor.getMutableState().getDocumentsToClone()) {
            aggDocumentsToCopy += *documentsToClone;
        }
    }

    // Calculate the approximate number of documents and bytes that each recipient will clone.
    ReshardingApproxCopySize approxCopySize;
    approxCopySize.setApproxBytesToCopy(aggBytesToCopy / numRecipientsToCopy);
    approxCopySize.setApproxDocumentsToCopy(aggDocumentsToCopy / numRecipientsToCopy);
    return approxCopySize;
}
}  // namespace

ExecutorFuture<void> ReshardingCoordinator::_awaitAllDonorsReadyToDonate(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    if (_coordinatorDao.getPhase(opCtx.get()) > CoordinatorStateEnum::kPreparingToDonate) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return future_util::withCancellation(
               _reshardingCoordinatorObserver->awaitAllDonorsReadyToDonate(),
               _ctHolder->getAbortToken())
        .thenRunOn(**executor)
        .then([this](ReshardingCoordinatorDocument coordinatorDocChangedOnDisk) {
            {
                auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
                reshardingPauseCoordinatorBeforeCloning.pauseWhileSetAndNotCanceled(
                    opCtx.get(), _ctHolder->getAbortToken());
            }

            auto highestMinFetchTimestamp = resharding::getHighestMinFetchTimestamp(
                coordinatorDocChangedOnDisk.getDonorShards());
            auto approxCopySize = [&] {
                auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
                return computeApproxCopySize(opCtx.get(), coordinatorDocChangedOnDisk);
            }();

            _updateCoordinatorDocStateAndCatalogEntries(
                [=, this](OperationContext* opCtx, TxnNumber txnNumber) {
                    auto now = resharding::getCurrentTime();
                    auto updatedDocument = _coordinatorDao.transitionToCloningPhase(
                        opCtx, now, highestMinFetchTimestamp, approxCopySize, txnNumber);
                    _metrics->setStartFor(CoordinatorStateEnum::kCloning, now);
                    return updatedDocument;
                });
        })
        .then([this] {
            return resharding::waitForMajority(_ctHolder->getAbortToken(),
                                               *_cancelableOpCtxFactory);
        });
}

void ReshardingCoordinator::_updateCoordinatorDocDonorShardEntriesNumDocuments(
    OperationContext* opCtx,
    const std::map<ShardId, int64_t>& values,
    std::function<void(DonorShardEntry& donorShard, int64_t value)> setter) {
    auto updatedCoordinatorDoc = _coordinatorDoc;
    auto& donorShards = updatedCoordinatorDoc.getDonorShards();

    invariant(values.size() == donorShards.size());
    for (auto& donorShard : donorShards) {
        auto it = values.find(donorShard.getId());
        invariant(it != values.end());
        setter(donorShard, it->second);
    }

    resharding::executeMetadataChangesInTxn(
        opCtx, [&updatedCoordinatorDoc](OperationContext* opCtx, TxnNumber txnNumber) {
            // Metrics are null because we are not transitioning state and do not want to
            // write any metrics.
            resharding::writeToCoordinatorStateNss(
                opCtx, nullptr, updatedCoordinatorDoc, txnNumber);
        });
    // We call _installCoordinatorDoc instead of installCoordinatorDocOnStateTransition
    // because we do not to udpate any metrics or print any logs indicating we transitioned
    // states.
    _installCoordinatorDoc(updatedCoordinatorDoc);
}

ExecutorFuture<void> ReshardingCoordinator::_fetchAndPersistNumDocumentsToCloneFromDonors(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    // The exact numbers of documents to copy are only need for verification so don't fetch them if
    // verification is not enabled. Also, if they have already fetched, don't fetch again. We only
    // need to check 'documentsToCopy' on the first entry because it will either be set on all
    // entries or on none of them.
    bool needToFetch = (_coordinatorDoc.getState() == CoordinatorStateEnum::kCloning) &&
        _metadata.getPerformVerification() &&
        !_coordinatorDoc.getDonorShards().front().getDocumentsToCopy().has_value();
    if (!needToFetch) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    LOGV2(9858100,
          "Start fetching the number of documents to copy from all donor shards",
          "reshardingUUID"_attr = _coordinatorDoc.getReshardingUUID());

    invariant(_coordinatorDoc.getCloneTimestamp());

    return resharding::WithAutomaticRetry([this, executor] {
               return ExecutorFuture<void>(**executor)
                   .then([this, anchor = shared_from_this(), executor] {
                       auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());

                       // If running in "relaxed" mode, instruct the receiving shards to ignore
                       // collection uuid mismatches between the local and sharding catalogs.
                       boost::optional<RouterRelaxCollectionUUIDConsistencyCheckBlock>
                           routerRelaxCollectionUUIDConsistencyCheckBlock(
                               boost::in_place_init_if, _coordinatorDoc.getRelaxed(), opCtx.get());

                       std::map<ShardId, ShardVersion> donorShardVersions;
                       {
                           auto cri =
                               uassertStatusOK(RoutingInformationCache::get(opCtx.get())
                                                   ->getCollectionRoutingInfoAt(
                                                       opCtx.get(),
                                                       _coordinatorDoc.getSourceNss(),
                                                       _coordinatorDoc.getCloneTimestamp().get()));
                           for (const auto& donorShard : _coordinatorDoc.getDonorShards()) {
                               donorShardVersions.emplace(donorShard.getId(),
                                                          cri.getShardVersion(donorShard.getId()));
                           }
                       }

                       return _reshardingCoordinatorExternalState->getDocumentsToCopyFromDonors(
                           opCtx.get(),
                           **executor,
                           _ctHolder->getAbortToken(),
                           _coordinatorDoc.getReshardingUUID(),
                           _coordinatorDoc.getSourceNss(),
                           _coordinatorDoc.getCloneTimestamp().get(),
                           donorShardVersions);
                   })
                   .then([this](std::map<ShardId, int64_t> documentsToCopy) {
                       auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
                       _updateCoordinatorDocDonorShardEntriesNumDocuments(
                           opCtx.get(),
                           documentsToCopy,
                           [](DonorShardEntry& donorShard, int64_t value) {
                               donorShard.setDocumentsToCopy(value);
                           });

                       LOGV2(9858106,
                             "Finished fetching the number of documents to copy from all donor "
                             "shards",
                             "reshardingUUID"_attr = _coordinatorDoc.getReshardingUUID());
                       return Status::OK();
                   });
           })
        .onTransientError([](const Status& status) {
            LOGV2(1003571,
                  "Resharding coordinator encountered transient error while fetching the number of "
                  "documents to copy from donor shards",
                  "error"_attr = status);
        })
        .onUnrecoverableError([](const Status& status) {
            LOGV2(10494619,
                  "Resharding coordinator encountered unrecoverable error while fetching the "
                  "number of "
                  "documents to copy from donor shards",
                  "error"_attr = status);
        })
        .until<Status>([](const Status& status) { return status.isOK(); })
        .on(**executor, _ctHolder->getAbortToken())
        .then([this] {
            // Wait for the update to the coordinator doc to be majority committed before moving to
            // the next step.
            return resharding::waitForMajority(_ctHolder->getAbortToken(),
                                               *_cancelableOpCtxFactory);
        });
}

ExecutorFuture<void> ReshardingCoordinator::_awaitAllRecipientsFinishedCloning(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    if (_coordinatorDao.getPhase(opCtx.get()) > CoordinatorStateEnum::kCloning) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return future_util::withCancellation(
               _reshardingCoordinatorObserver->awaitAllRecipientsFinishedCloning(),
               _ctHolder->getAbortToken())
        .thenRunOn(**executor)
        .then([this, executor](ReshardingCoordinatorDocument coordinatorDocChangedOnDisk) {
            if (_metadata.getPerformVerification()) {
                auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
                // Fetch the coordinator doc from disk since the 'coordinatorDocChangedOnDisk' above
                // came from the OpObserver and may not reflect the latest version coordinator doc
                // because the write to populate donor 'documentsToCopy' metrics (to be used for
                // verification) may occur after all recipients have finished cloning and updated
                // the coordinator doc.
                coordinatorDocChangedOnDisk = resharding::getCoordinatorDoc(
                    opCtx.get(), coordinatorDocChangedOnDisk.getReshardingUUID());
                _reshardingCoordinatorExternalState->verifyClonedCollection(
                    opCtx.get(),
                    **executor,
                    _ctHolder->getAbortToken(),
                    coordinatorDocChangedOnDisk);
            }
        })
        .then([this] {
            this->_updateCoordinatorDocStateAndCatalogEntries(
                [=, this](OperationContext* opCtx, TxnNumber txnNumber) {
                    auto now = resharding::getCurrentTime();
                    auto updatedDocument =
                        _coordinatorDao.transitionToApplyingPhase(opCtx, now, txnNumber);

                    _metrics->setEndFor(CoordinatorStateEnum::kCloning, now);
                    _metrics->setStartFor(CoordinatorStateEnum::kApplying, now);
                    return updatedDocument;
                });
        })
        .then([this] {
            return resharding::waitForMajority(_ctHolder->getAbortToken(),
                                               *_cancelableOpCtxFactory);
        });
}

void ReshardingCoordinator::_startCommitMonitor(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_commitMonitor) {
        return;
    }

    _commitMonitor = std::make_shared<resharding::CoordinatorCommitMonitor>(
        _metrics,
        _coordinatorDoc.getSourceNss(),
        resharding::extractShardIdsFromParticipantEntries(_coordinatorDoc.getDonorShards()),
        resharding::extractShardIdsFromParticipantEntries(_coordinatorDoc.getRecipientShards()),
        **executor,
        _ctHolder->getCommitMonitorToken(),
        _coordinatorDoc.getDemoMode()
            ? 0
            : resharding::gReshardingDelayBeforeRemainingOperationTimeQueryMillis.load());

    _commitMonitorQuiesced = _commitMonitor->waitUntilRecipientsAreWithinCommitThreshold()
                                 .thenRunOn(**executor)
                                 .onCompletion([this](Status status) {
                                     _fulfillOkayToEnterCritical(status);
                                     return status;
                                 })
                                 .share();
}

#endif  // RESHARDING_COORDINATOR_PART_2
#ifdef RESHARDING_COORDINATOR_PART_3

ExecutorFuture<void> ReshardingCoordinator::_awaitAllRecipientsFinishedApplying(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kApplying) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return ExecutorFuture<void>(**executor)
        .then([this, executor] {
            _startCommitMonitor(executor);

            LOGV2(5391602, "Resharding operation waiting for an okay to enter critical section");

            // The _reshardingCoordinatorObserver->awaitAllRecipientsInStrictConsistency() future is
            // used for reporting recipient shard errors encountered during the Applying phase and
            // in turn aborting the resharding operation.
            // For all other cases, the _canEnterCritical.getFuture() resolves first and the
            // operation can then proceed to entering the critical section depending on the status
            // returned.
            return future_util::withCancellation(
                       whenAny(
                           _canEnterCritical.getFuture().thenRunOn(**executor),
                           _reshardingCoordinatorObserver->awaitAllRecipientsInStrictConsistency()
                               .thenRunOn(**executor)
                               .ignoreValue()),
                       _ctHolder->getAbortToken())
                .thenRunOn(**executor)
                .then([](auto result) { return result.result; })
                .onCompletion([this](Status status) {
                    _ctHolder->cancelCommitMonitor();
                    if (status.isOK()) {
                        LOGV2(5391603, "Resharding operation is okay to enter critical section");
                    }
                    return status;
                });
        })
        .then([this, executor] {
            {
                auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
                reshardingPauseCoordinatorBeforeBlockingWrites.pauseWhileSetAndNotCanceled(
                    opCtx.get(), _ctHolder->getAbortToken());
            }

            // set the criticalSectionExpiresAt on the coordinator doc
            const auto now = (*executor)->now();
            const auto criticalSectionTimeout =
                Milliseconds(resharding::gReshardingCriticalSectionTimeoutMillis.load());
            const auto criticalSectionExpiresAt = now + criticalSectionTimeout;

            _updateCoordinatorDocStateAndCatalogEntries(
                [=, this](OperationContext* opCtx, TxnNumber txnNumber) {
                    auto updatedDocument = _coordinatorDao.transitionToBlockingWritesPhase(
                        opCtx, now, criticalSectionExpiresAt, txnNumber);
                    _metrics->setStartFor(CoordinatorStateEnum::kBlockingWrites, now);
                    return updatedDocument;
                });
        })
        .then([this] {
            return resharding::waitForMajority(_ctHolder->getAbortToken(),
                                               *_cancelableOpCtxFactory);
        })
        .thenRunOn(**executor)
        .then([this, executor] {
            const auto criticalSectionTimeout =
                Milliseconds(resharding::gReshardingCriticalSectionTimeoutMillis.load());
            const auto criticalSectionExpiresAt = (*executor)->now() + criticalSectionTimeout;

            LOGV2_INFO(
                5573001, "Engaging critical section", "timeoutAt"_attr = criticalSectionExpiresAt);

            _setCriticalSectionTimeoutCallback(executor, criticalSectionExpiresAt);

            pauseAfterEngagingCriticalSection.pauseWhileSet();
        });
}

ExecutorFuture<void> ReshardingCoordinator::_fetchAndPersistNumDocumentsFinalFromDonors(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    // The final numbers of documents to copy are only need for verification so don't fetch them if
    // verification is not enabled. Also, if they have already fetched, don't fetch again. We only
    // need to check 'documentsFinal' on the first entry because it will either be set on all
    // entries or on none of them.
    bool needToFetch = (_coordinatorDoc.getState() == CoordinatorStateEnum::kBlockingWrites) &&
        _metadata.getPerformVerification() &&
        !_coordinatorDoc.getDonorShards().front().getDocumentsFinal().has_value();
    if (!needToFetch) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    LOGV2(1003581,
          "Start fetching the change in the number of documents from all donor shards",
          "reshardingUUID"_attr = _coordinatorDoc.getReshardingUUID());

    return resharding::WithAutomaticRetry([this, executor] {
               return ExecutorFuture<void>(**executor)
                   .then([this, anchor = shared_from_this(), executor] {
                       auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());

                       return _reshardingCoordinatorExternalState->getDocumentsDeltaFromDonors(
                           opCtx.get(),
                           **executor,
                           _ctHolder->getAbortToken(),
                           _coordinatorDoc.getReshardingUUID(),
                           _coordinatorDoc.getSourceNss(),
                           resharding::extractShardIdsFromParticipantEntries(
                               _coordinatorDoc.getDonorShards()));
                   })
                   .then([this](std::map<ShardId, int64_t> documentsDelta) {
                       auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
                       _updateCoordinatorDocDonorShardEntriesNumDocuments(
                           opCtx.get(),
                           documentsDelta,
                           [](DonorShardEntry& donorShard, int64_t value) {
                               auto documentsToCopy = donorShard.getDocumentsToCopy();
                               tassert(1003582,
                                       str::stream()
                                           << "Expected the number of documents to copy from the "
                                              "donor shard '"
                                           << donorShard.getId() << "' to have been set",
                                       documentsToCopy);
                               donorShard.setDocumentsFinal(*documentsToCopy + value);
                           });

                       LOGV2(1003583,
                             "Finished fetching the change in the number of documents from all "
                             "donor shards",
                             "reshardingUUID"_attr = _coordinatorDoc.getReshardingUUID());
                   });
           })
        .onTransientError([](const Status& status) {
            LOGV2(1003584,
                  "Resharding coordinator encountered transient error while fetching the final "
                  "number of documents from donor shards",
                  "error"_attr = status);
        })
        .onUnrecoverableError([](const Status& status) {
            LOGV2(10494620,
                  "Resharding coordinator encountered unrecoverable error while fetching the final "
                  "number of documents from donor shards",
                  "error"_attr = status);
        })
        .until<Status>([](const Status& status) { return status.isOK(); })
        .on(**executor, _ctHolder->getAbortToken())
        .then([this] {
            // Wait for the update to the coordinator doc to be majority committed before moving to
            // the next step.
            return resharding::waitForMajority(_ctHolder->getAbortToken(),
                                               *_cancelableOpCtxFactory);
        });
}

ExecutorFuture<ReshardingCoordinatorDocument>
ReshardingCoordinator::_awaitAllRecipientsInStrictConsistency(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kBlockingWrites) {
        // If in recovery, just return the existing _stateDoc.
        return ExecutorFuture<ReshardingCoordinatorDocument>(**executor, _coordinatorDoc);
    }

    // ensure that the critical section timeout handler is still set
    if (!_criticalSectionTimeoutCbHandle &&
        _coordinatorDoc.getCriticalSectionExpiresAt().has_value()) {
        _setCriticalSectionTimeoutCallback(executor,
                                           _coordinatorDoc.getCriticalSectionExpiresAt().value());
    }
    return future_util::withCancellation(
               _reshardingCoordinatorObserver->awaitAllRecipientsInStrictConsistency(),
               _ctHolder->getAbortToken())
        .thenRunOn(**executor)
        .then([this, executor](ReshardingCoordinatorDocument coordinatorDocChangedOnDisk) {
            if (_metadata.getPerformVerification()) {
                auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
                // Fetch the coordinator doc from disk since the 'coordinatorDocChangedOnDisk'
                // above came from the OpObserver and may not reflect the latest version
                // coordinator doc because the write to populate donor 'documentsFinal' metrics
                // (to be used for verification) may occur after all recipients have reached
                // strict consistency and updated the coordinator doc.
                coordinatorDocChangedOnDisk = resharding::getCoordinatorDoc(
                    opCtx.get(), coordinatorDocChangedOnDisk.getReshardingUUID());

                _reshardingCoordinatorExternalState->verifyFinalCollection(
                    opCtx.get(), coordinatorDocChangedOnDisk);
            }

            return coordinatorDocChangedOnDisk;
        });
}

void ReshardingCoordinator::_setCriticalSectionTimeoutCallback(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    Date_t criticalSectionExpiresAt) {
    auto swCbHandle = (*executor)->scheduleWorkAt(
        criticalSectionExpiresAt, [this](const executor::TaskExecutor::CallbackArgs& cbData) {
            if (!cbData.status.isOK()) {
                return;
            }
            _reshardingCoordinatorObserver->onCriticalSectionTimeout();
        });

    if (!swCbHandle.isOK()) {
        _reshardingCoordinatorObserver->interrupt(swCbHandle.getStatus());
    }

    _criticalSectionTimeoutCbHandle = swCbHandle.getValue();
}


void ReshardingCoordinator::_commit(const ReshardingCoordinatorDocument& coordinatorDoc) {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kBlockingWrites) {
        invariant(_coordinatorDoc.getState() != CoordinatorStateEnum::kAborting);
        return;
    }

    ReshardingCoordinatorDocument updatedCoordinatorDoc = coordinatorDoc;
    updatedCoordinatorDoc.setState(CoordinatorStateEnum::kCommitting);

    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    reshardingPauseCoordinatorBeforeDecisionPersisted.pauseWhileSetAndNotCanceled(
        opCtx.get(), _ctHolder->getAbortToken());

    // The new epoch and timestamp to use for the resharded collection to indicate that the
    // collection is a new incarnation of the namespace
    auto newCollectionEpoch = OID::gen();
    auto newCollectionTimestamp = [&] {
        const auto now = VectorClock::get(opCtx.get())->getTime();
        return now.clusterTime().asTimestamp();
    }();

    // Retrieve the exact placement of the resharded collection from the routing table.
    // The 'recipientShards' field of the coordinator doc cannot be used for this purpose as it
    // always includes the primary shard for the parent database (even when it doesn't own any chunk
    // under the new key pattern).
    auto reshardedCollectionPlacement = [&] {
        std::set<ShardId> collectionPlacement;
        std::vector<ShardId> collectionPlacementAsVector;

        const auto cm =
            uassertStatusOK(RoutingInformationCache::get(opCtx.get())
                                ->getCollectionPlacementInfoWithRefresh(
                                    opCtx.get(), coordinatorDoc.getTempReshardingNss()));

        cm.getAllShardIds(&collectionPlacement);

        collectionPlacementAsVector.reserve(collectionPlacement.size());
        for (auto& elem : collectionPlacement) {
            collectionPlacementAsVector.emplace_back(elem);
        }
        return collectionPlacementAsVector;
    }();

    resharding::writeDecisionPersistedState(opCtx.get(),
                                            _metrics.get(),
                                            updatedCoordinatorDoc,
                                            std::move(newCollectionEpoch),
                                            std::move(newCollectionTimestamp),
                                            reshardedCollectionPlacement);

    // Update the in memory state
    installCoordinatorDocOnStateTransition(opCtx.get(), updatedCoordinatorDoc);
}

void ReshardingCoordinator::_generateCommitNotificationForChangeStreams(
    OperationContext* opCtx,
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    ChangeStreamCommitNotificationMode mode) {

    if (mode == ChangeStreamCommitNotificationMode::BeforeWriteOnCatalog &&
        _coordinatorDoc.getState() >= CoordinatorStateEnum::kCommitting) {
        return;
    }

    CollectionResharded eventNotification(_coordinatorDoc.getSourceNss(),
                                          _coordinatorDoc.getSourceUUID(),
                                          _coordinatorDoc.getReshardingUUID(),
                                          _coordinatorDoc.getReshardingKey().toBSON());
    eventNotification.setSourceKey(_coordinatorDoc.getSourceKey());
    eventNotification.setNumInitialChunks(_coordinatorDoc.getNumInitialChunks());
    eventNotification.setUnique(_coordinatorDoc.getUnique());
    eventNotification.setCollation(_coordinatorDoc.getCollation());
    // Set the identity of the collection that is currently associated to the set of zones
    // created during resharding, based on whether this notification is being generated
    // before of after committing on the global catalog.
    eventNotification.setReferenceToZoneList(
        mode == ChangeStreamCommitNotificationMode::BeforeWriteOnCatalog
            ? _coordinatorDoc.getTempReshardingNss()
            : _coordinatorDoc.getSourceNss());
    if (const auto& provenance = _metadata.getProvenance()) {
        eventNotification.setProvenance(provenance);
    }
    ShardsvrNotifyShardingEventRequest request(notify_sharding_event::kCollectionResharded,
                                               eventNotification.toBSON());

    const auto& notifierShard = _getChangeStreamNotifierShardId();

    // In case the recipient is running a legacy binary, swallow the error.
    try {
        generic_argument_util::setMajorityWriteConcern(request, &resharding::kMajorityWriteConcern);
        const auto opts =
            std::make_shared<async_rpc::AsyncRPCOptions<ShardsvrNotifyShardingEventRequest>>(
                **executor, _ctHolder->getStepdownToken(), request);
        opts->cmd.setDbName(DatabaseName::kAdmin);
        resharding::sendCommandToShards(opCtx, opts, {notifierShard});
    } catch (const ExceptionFor<ErrorCodes::UnsupportedShardingEventNotification>& e) {
        LOGV2_WARNING(7403100,
                      "Unable to generate op entry on reshardCollection commit",
                      "error"_attr = redact(e.toStatus()));
    }
}

void ReshardingCoordinator::_generatePlacementChangeNotificationForChangeStreams(
    OperationContext* opCtx, const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {

    const auto placementChangeTime = [&] {
        const auto reshardedColl =
            ShardingCatalogManager::get(opCtx)->localCatalogClient()->getCollection(
                opCtx, _coordinatorDoc.getSourceNss());
        return reshardedColl.getTimestamp();
    }();
    NamespacePlacementChanged eventNotification(_coordinatorDoc.getSourceNss(),
                                                placementChangeTime);
    ShardsvrNotifyShardingEventRequest request(notify_sharding_event::kNamespacePlacementChanged,
                                               eventNotification.toBSON());

    const auto& notifierShard = _getChangeStreamNotifierShardId();

    // In case the recipient is running a legacy binary, swallow the error.
    try {
        generic_argument_util::setMajorityWriteConcern(request, &resharding::kMajorityWriteConcern);
        const auto opts =
            std::make_shared<async_rpc::AsyncRPCOptions<ShardsvrNotifyShardingEventRequest>>(
                **executor, _ctHolder->getStepdownToken(), request);
        opts->cmd.setDbName(DatabaseName::kAdmin);
        resharding::sendCommandToShards(opCtx, opts, {notifierShard});
    } catch (const ExceptionFor<ErrorCodes::UnsupportedShardingEventNotification>& e) {
        tasserted(
            10674000,
            str::stream() << "Unable to generate op entry on namespacePlacementChanged commit:"
                          << redact(e.toStatus()));
    }
}

ExecutorFuture<void> ReshardingCoordinator::_awaitAllParticipantShardsDone(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    std::vector<ExecutorFuture<ReshardingCoordinatorDocument>> futures;
    futures.emplace_back(
        _reshardingCoordinatorObserver->awaitAllRecipientsDone().thenRunOn(**executor));
    futures.emplace_back(
        _reshardingCoordinatorObserver->awaitAllDonorsDone().thenRunOn(**executor));

    // We only allow the stepdown token to cancel operations after progressing past
    // kCommitting.
    return future_util::withCancellation(whenAllSucceed(std::move(futures)),
                                         _ctHolder->getStepdownToken())
        .thenRunOn(**executor)
        .then([this, executor](const auto& coordinatorDocsChangedOnDisk) {
            auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
            auto& coordinatorDoc = coordinatorDocsChangedOnDisk[1];

            boost::optional<Status> abortReason;
            if (coordinatorDoc.getAbortReason()) {
                abortReason = resharding::getStatusFromAbortReason(coordinatorDoc);
            }

            if (!abortReason) {
                // (SERVER-54231) Ensure every catalog entry referring the source uuid is
                // cleared out on every shard.
                const auto allShardIds =
                    Grid::get(opCtx.get())->shardRegistry()->getAllShardIds(opCtx.get());
                const auto& nss = coordinatorDoc.getSourceNss();
                const auto& notMatchingThisUUID = coordinatorDoc.getReshardingUUID();
                auto cmd = ShardsvrDropCollectionIfUUIDNotMatchingWithWriteConcernRequest(
                    nss, notMatchingThisUUID);

                generic_argument_util::setMajorityWriteConcern(cmd,
                                                               &resharding::kMajorityWriteConcern);
                auto opts = std::make_shared<async_rpc::AsyncRPCOptions<
                    ShardsvrDropCollectionIfUUIDNotMatchingWithWriteConcernRequest>>(
                    **executor, _ctHolder->getStepdownToken(), cmd);
                resharding::sendCommandToShards(opCtx.get(), opts, allShardIds);
            }

            reshardingPauseCoordinatorBeforeRemovingStateDoc.pauseWhileSetAndNotCanceled(
                opCtx.get(), _ctHolder->getStepdownToken());

            // Notify metrics as the operation is now complete for external observers.
            markCompleted(abortReason ? *abortReason : Status::OK(), _metrics.get());
            _removeOrQuiesceCoordinatorDocAndRemoveReshardingFields(opCtx.get(), abortReason);
        });
}

void ReshardingCoordinator::_updateCoordinatorDocStateAndCatalogEntries(
    CoordinatorStateEnum nextState,
    ReshardingCoordinatorDocument coordinatorDoc,
    boost::optional<Timestamp> cloneTimestamp,
    boost::optional<ReshardingApproxCopySize> approxCopySize,
    boost::optional<Status> abortReason) {
    // Build new state doc for coordinator state update
    ReshardingCoordinatorDocument updatedCoordinatorDoc = coordinatorDoc;
    updatedCoordinatorDoc.setState(nextState);
    resharding::emplaceApproxBytesToCopyIfExists(updatedCoordinatorDoc, std::move(approxCopySize));
    resharding::emplaceCloneTimestampIfExists(updatedCoordinatorDoc, std::move(cloneTimestamp));
    resharding::emplaceTruncatedAbortReasonIfExists(updatedCoordinatorDoc, abortReason);

    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    resharding::writeStateTransitionAndCatalogUpdatesThenBumpCollectionPlacementVersions(
        opCtx.get(), _metrics.get(), updatedCoordinatorDoc, boost::none);

    // Update in-memory coordinator doc
    installCoordinatorDocOnStateTransition(
        opCtx.get(),
        resharding::getCoordinatorDoc(opCtx.get(), _coordinatorDoc.getReshardingUUID()));
}

void ReshardingCoordinator::_updateCoordinatorDocStateAndCatalogEntries(
    resharding::PhaseTransitionFn phaseTransitionFn) {
    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    resharding::writeStateTransitionAndCatalogUpdatesThenBumpCollectionPlacementVersions(
        opCtx.get(), _metrics.get(), _coordinatorDoc, std::move(phaseTransitionFn));

    installCoordinatorDocOnStateTransition(
        opCtx.get(),
        resharding::getCoordinatorDoc(opCtx.get(), _coordinatorDoc.getReshardingUUID()));
}

void ReshardingCoordinator::_removeOrQuiesceCoordinatorDocAndRemoveReshardingFields(
    OperationContext* opCtx, boost::optional<Status> abortReason) {
    auto updatedCoordinatorDoc = resharding::removeOrQuiesceCoordinatorDocAndRemoveReshardingFields(
        opCtx,
        _metrics.get(),
        resharding::tryGetCoordinatorDoc(opCtx, _coordinatorDoc.getReshardingUUID())
            .value_or(_coordinatorDoc),
        abortReason);

    // Update in-memory coordinator doc.
    installCoordinatorDocOnStateTransition(opCtx, updatedCoordinatorDoc);
}

#endif  // RESHARDING_COORDINATOR_PART_3
#ifdef RESHARDING_COORDINATOR_PART_4

template <typename CommandType>
void ReshardingCoordinator::_sendCommandToAllParticipants(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    std::shared_ptr<async_rpc::AsyncRPCOptions<CommandType>> opts) {
    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    auto donorShardIds =
        resharding::extractShardIdsFromParticipantEntries(_coordinatorDoc.getDonorShards());
    auto recipientShardIds =
        resharding::extractShardIdsFromParticipantEntries(_coordinatorDoc.getRecipientShards());
    std::set<ShardId> participantShardIds{donorShardIds.begin(), donorShardIds.end()};
    participantShardIds.insert(recipientShardIds.begin(), recipientShardIds.end());

    resharding::sendCommandToShards(
        opCtx.get(), opts, {participantShardIds.begin(), participantShardIds.end()});
}

template <typename CommandType>
void ReshardingCoordinator::_sendCommandToAllDonors(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    std::shared_ptr<async_rpc::AsyncRPCOptions<CommandType>> opts) {
    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    auto donorShardIds =
        resharding::extractShardIdsFromParticipantEntries(_coordinatorDoc.getDonorShards());

    resharding::sendCommandToShards(
        opCtx.get(), opts, {donorShardIds.begin(), donorShardIds.end()});
}

void ReshardingCoordinator::_sendRecipientCloneCmdToShards(
    OperationContext* opCtx,
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    ShardsvrReshardRecipientClone cmd,
    std::set<ShardId> recipientShardIds) {
    auto opts = std::make_shared<async_rpc::AsyncRPCOptions<ShardsvrReshardRecipientClone>>(
        **executor, _ctHolder->getStepdownToken(), cmd);

    generic_argument_util::setMajorityWriteConcern(opts->cmd, &resharding::kMajorityWriteConcern);
    opts->cmd.setDbName(DatabaseName::kAdmin);

    resharding::sendCommandToShards(
        opCtx, opts, {recipientShardIds.begin(), recipientShardIds.end()});
}

void ReshardingCoordinator::_establishAllDonorsAsParticipants(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    invariant(_coordinatorDoc.getState() == CoordinatorStateEnum::kPreparingToDonate);
    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());

    _reshardingCoordinatorExternalState->establishAllDonorsAsParticipants(
        opCtx.get(),
        _coordinatorDoc.getSourceNss(),
        _coordinatorDoc.getDonorShards(),
        **executor,
        _ctHolder->getStepdownToken());
}

void ReshardingCoordinator::_establishAllRecipientsAsParticipants(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    invariant(_coordinatorDoc.getState() == CoordinatorStateEnum::kPreparingToDonate);
    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());

    _reshardingCoordinatorExternalState->establishAllRecipientsAsParticipants(
        opCtx.get(),
        _coordinatorDoc.getTempReshardingNss(),
        _coordinatorDoc.getRecipientShards(),
        **executor,
        _ctHolder->getStepdownToken());
}

void ReshardingCoordinator::_tellAllRecipientsToClone(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    // TODO (SERVER-99772): Remove failpoint.
    reshardingPauseBeforeTellingRecipientsToClone.pauseWhileSetAndNotCanceled(
        opCtx.get(), _ctHolder->getAbortToken());

    auto [shardsOwningChunks, shardsNotOwningChunks] =
        resharding::computeRecipientChunkOwnership(opCtx.get(), _coordinatorDoc);

    auto recipientFields = resharding::constructRecipientFields(_coordinatorDoc);
    ShardsvrReshardRecipientClone cmd(_coordinatorDoc.getReshardingUUID());
    cmd.setCloneTimestamp(recipientFields.getCloneTimestamp().get());
    cmd.setDonorShards(recipientFields.getDonorShards());
    cmd.setApproxCopySize(recipientFields.getReshardingApproxCopySizeStruct());

    _sendRecipientCloneCmdToShards(opCtx.get(), executor, cmd, shardsOwningChunks);

    if (shardsNotOwningChunks.size() > 0) {
        ReshardingApproxCopySize approxCopySize;
        approxCopySize.setApproxBytesToCopy(0);
        approxCopySize.setApproxDocumentsToCopy(0);
        cmd.setApproxCopySize(approxCopySize);

        _sendRecipientCloneCmdToShards(opCtx.get(), executor, cmd, shardsNotOwningChunks);
    }
}

void ReshardingCoordinator::_tellAllRecipientsToRefresh(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    NamespaceString nssToRefresh;
    // Refresh the temporary namespace if the coordinator is in a state prior to 'kCommitting'.
    // A refresh of recipients while in 'kCommitting' should be accompanied by a refresh of
    // all participants for the original namespace to ensure correctness.
    if (_coordinatorDoc.getState() < CoordinatorStateEnum::kCommitting) {
        nssToRefresh = _coordinatorDoc.getTempReshardingNss();
    } else {
        nssToRefresh = _coordinatorDoc.getSourceNss();
    }

    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    _reshardingCoordinatorExternalState->tellAllRecipientsToRefresh(
        opCtx.get(),
        nssToRefresh,
        _coordinatorDoc.getReshardingUUID(),
        _coordinatorDoc.getRecipientShards(),
        **executor,
        _ctHolder->getStepdownToken());
}

void ReshardingCoordinator::_tellAllDonorsToRefresh(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());

    _reshardingCoordinatorExternalState->tellAllDonorsToRefresh(opCtx.get(),
                                                                _coordinatorDoc.getSourceNss(),
                                                                _coordinatorDoc.getReshardingUUID(),
                                                                _coordinatorDoc.getDonorShards(),
                                                                **executor,
                                                                _ctHolder->getStepdownToken());
}

void ReshardingCoordinator::_tellAllDonorsToStartChangeStreamsMonitor(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    // The donors do not need to monitor the changes to the collection being resharded if
    // verification is not enabled.
    if (!_metadata.getPerformVerification()) {
        return;
    }
    invariant(_coordinatorDoc.getCloneTimestamp());
    ShardsvrReshardingDonorStartChangeStreamsMonitor cmd(_coordinatorDoc.getSourceNss(),
                                                         _coordinatorDoc.getReshardingUUID(),
                                                         *_coordinatorDoc.getCloneTimestamp());
    auto opts = std::make_shared<
        async_rpc::AsyncRPCOptions<ShardsvrReshardingDonorStartChangeStreamsMonitor>>(
        **executor, _ctHolder->getStepdownToken(), cmd);
    opts->cmd.setDbName(DatabaseName::kAdmin);
    _sendCommandToAllDonors(executor, opts);
}

namespace {
std::shared_ptr<async_rpc::AsyncRPCOptions<ShardsvrCommitReshardCollection>>
createShardsvrCommitReshardCollectionOptions(const NamespaceString& nss,
                                             const UUID& reshardingUUID,
                                             const std::shared_ptr<executor::TaskExecutor>& exec,
                                             CancellationToken token) {
    ShardsvrCommitReshardCollection cmd(nss);
    cmd.setDbName(DatabaseName::kAdmin);
    cmd.setReshardingUUID(reshardingUUID);
    generic_argument_util::setMajorityWriteConcern(cmd, &resharding::kMajorityWriteConcern);
    auto opts = std::make_shared<async_rpc::AsyncRPCOptions<ShardsvrCommitReshardCollection>>(
        exec, token, cmd);
    return opts;
}
}  // namespace

void ReshardingCoordinator::_tellAllParticipantsToCommit(
    const NamespaceString& nss, const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    {
        auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
        reshardingPauseBeforeTellingParticipantsToCommit.pauseWhileSetAndNotCanceled(
            opCtx.get(), _ctHolder->getAbortToken());
    }

    auto opts = createShardsvrCommitReshardCollectionOptions(
        nss, _coordinatorDoc.getReshardingUUID(), **executor, _ctHolder->getStepdownToken());
    opts->cmd.setDbName(DatabaseName::kAdmin);
    _sendCommandToAllParticipants(executor, opts);
}

void ReshardingCoordinator::_tellAllParticipantsToAbort(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor, bool isUserAborted) {
    ShardsvrAbortReshardCollection abortCmd(_coordinatorDoc.getReshardingUUID(), isUserAborted);
    abortCmd.setDbName(DatabaseName::kAdmin);
    generic_argument_util::setMajorityWriteConcern(abortCmd, &resharding::kMajorityWriteConcern);
    auto opts = std::make_shared<async_rpc::AsyncRPCOptions<ShardsvrAbortReshardCollection>>(
        **executor, _ctHolder->getStepdownToken(), abortCmd);
    _sendCommandToAllParticipants(executor, opts);
}

void ReshardingCoordinator::_updateChunkImbalanceMetrics(const NamespaceString& nss) {
    auto cancellableOpCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    auto opCtx = cancellableOpCtx.get();

    try {
        const auto routingInfo = uassertStatusOK(
            RoutingInformationCache::get(opCtx)->getCollectionPlacementInfoWithRefresh(opCtx, nss));

        const auto catalogClient = ShardingCatalogManager::get(opCtx)->localCatalogClient();
        const auto collectionZones =
            uassertStatusOK(catalogClient->getTagsForCollection(opCtx, nss));

        const auto& keyPattern = routingInfo.getShardKeyPattern().getKeyPattern();

        ZoneInfo zoneInfo;
        for (const auto& tag : collectionZones) {
            uassertStatusOK(zoneInfo.addRangeToZone(
                ZoneRange(keyPattern.extendRangeBound(tag.getMinKey(), false),
                          keyPattern.extendRangeBound(tag.getMaxKey(), false),
                          tag.getTag())));
        }

        const auto allShardsWithOpTime =
            catalogClient->getAllShards(opCtx, repl::ReadConcernLevel::kLocalReadConcern);

        auto imbalanceCount =
            getMaxChunkImbalanceCount(routingInfo, allShardsWithOpTime.value, zoneInfo);

        _metrics->setLastOpEndingChunkImbalance(imbalanceCount);
    } catch (const DBException& ex) {
        LOGV2_WARNING(5543000,
                      "Encountered error while trying to update resharding chunk imbalance metrics",
                      logAttrs(nss),
                      "error"_attr = redact(ex.toStatus()));
    }
}

void ReshardingCoordinator::_logStatsOnCompletion(bool success) {
    BSONObjBuilder builder;
    BSONObjBuilder statsBuilder;
    BSONObjBuilder totalsBuilder;
    BSONObjBuilder criticalSectionBuilder;
    builder.append("uuid", _coordinatorDoc.getReshardingUUID().toBSON());
    if (const auto& userUuid = _coordinatorDoc.getUserReshardingUUID()) {
        builder.append("userSuppliedUUID", userUuid->toBSON());
    }
    builder.append("status", success ? "success" : "failed");
    if (!success) {
        if (const auto& abortReason = _coordinatorDoc.getAbortReason()) {
            builder.append("failureReason", *abortReason);
        }
    }
    statsBuilder.append("ns", toStringForLogging(_coordinatorDoc.getSourceNss()));
    statsBuilder.append("provenance",
                        ReshardingProvenance_serializer(_coordinatorDoc.getProvenance().value_or(
                            ReshardingProvenanceEnum::kReshardCollection)));
    statsBuilder.append("sourceUUID", _coordinatorDoc.getSourceUUID().toBSON());
    if (success) {
        statsBuilder.append("newUUID", _coordinatorDoc.getReshardingUUID().toBSON());
    }
    if (_coordinatorDoc.getSourceKey()) {
        statsBuilder.append("oldShardKey", _coordinatorDoc.getSourceKey()->toString());
    }
    statsBuilder.append("newShardKey", _coordinatorDoc.getReshardingKey().toString());
    if (_coordinatorDoc.getStartTime()) {
        auto startTime = *_coordinatorDoc.getStartTime();
        statsBuilder.append("startTime", startTime);

        auto endTime = resharding::getCurrentTime();
        statsBuilder.append("endTime", endTime);

        auto elapsedMillis = (endTime - startTime).count();
        statsBuilder.append("operationDurationMs", elapsedMillis);
    } else {
        statsBuilder.append("endTime", resharding::getCurrentTime());
    }
    _metrics->reportPhaseDurations(&totalsBuilder);

    auto numDestinationShards = 0;
    if (const auto& shardDistribution = _coordinatorDoc.getShardDistribution()) {
        std::set<ShardId> destinationShards;
        for (const auto& shardDist : *shardDistribution) {
            destinationShards.emplace(shardDist.getShard());
        }
        numDestinationShards = destinationShards.size();
    } else {
        numDestinationShards = _coordinatorDoc.getRecipientShards().size();
    }
    statsBuilder.append("numberOfSourceShards",
                        static_cast<int64_t>(_coordinatorDoc.getDonorShards().size()));
    statsBuilder.append("numberOfDestinationShards", static_cast<int64_t>(numDestinationShards));

    BSONArrayBuilder donors;
    int64_t maxDonorIndexes = 0;
    int64_t totalDocuments = 0;
    int64_t totalBytes = 0;
    int64_t totalWritesDuringCriticalSection = 0;
    for (auto donor : _coordinatorDoc.getDonorShards()) {
        BSONObjBuilder shardBuilder;
        auto& state = donor.getMutableState();
        shardBuilder.append("shardName", donor.getId());
        auto bytes = state.getBytesToClone().value_or(0);
        auto docs = state.getDocumentsToClone().value_or(0);
        shardBuilder.append("bytesToClone", bytes);
        shardBuilder.append("documentsToClone", docs);
        totalBytes += bytes;
        totalDocuments += docs;
        auto indexes = state.getIndexCount().value_or(0);
        shardBuilder.append("indexCount", indexes);
        maxDonorIndexes = std::max(maxDonorIndexes, indexes);
        if (const auto& phaseDurations = state.getPhaseDurations()) {
            shardBuilder.append("phaseDurations", *phaseDurations);
        }
        if (const auto& interval = state.getCriticalSectionInterval()) {
            shardBuilder.append("criticalSectionInterval", interval->toBSON());
        }
        auto writes = state.getWritesDuringCriticalSection().value_or(0);
        totalWritesDuringCriticalSection += writes;
        shardBuilder.append("writesDuringCriticalSection", writes);
        donors.append(shardBuilder.obj());
    }
    statsBuilder.append("donors", donors.obj());
    totalsBuilder.append("totalBytesToClone", totalBytes);
    totalsBuilder.append("totalDocumentsToClone", totalDocuments);
    totalsBuilder.append("averageDocSize", totalDocuments > 0 ? (totalBytes / totalDocuments) : 0);

    int64_t maxRecipientIndexes = 0;
    int64_t totalBytesCloned = 0;
    int64_t totalDocumentsCloned = 0;
    int64_t totalOplogsFetched = 0;
    int64_t totalOplogsApplied = 0;
    BSONArrayBuilder recipients;
    for (auto recipient : _coordinatorDoc.getRecipientShards()) {
        BSONObjBuilder shardBuilder;
        auto& state = recipient.getMutableState();
        shardBuilder.append("shardName", recipient.getId());
        auto bytes = state.getBytesCopied().value_or(0);
        auto docs = state.getTotalNumDocuments().value_or(0);
        auto fetched = state.getOplogFetched().value_or(0);
        auto applied = state.getOplogApplied().value_or(0);
        shardBuilder.append("bytesCloned", bytes);
        shardBuilder.append("documentsCloned", docs);
        shardBuilder.append("oplogsFetched", fetched);
        shardBuilder.append("oplogsApplied", applied);
        totalBytesCloned += bytes;
        totalDocumentsCloned += docs;
        totalOplogsFetched += fetched;
        totalOplogsApplied += applied;
        auto indexes = state.getNumOfIndexes().value_or(0);
        shardBuilder.append("indexCount", indexes);
        maxRecipientIndexes = std::max(maxRecipientIndexes, indexes);
        if (const auto& phaseDurations = state.getPhaseDurations()) {
            shardBuilder.append("phaseDurations", *phaseDurations);
        }
        recipients.append(shardBuilder.obj());
    }
    statsBuilder.append("recipients", recipients.obj());
    totalsBuilder.append("totalBytesCloned", totalBytesCloned);
    totalsBuilder.append("totalDocumentsCloned", totalDocumentsCloned);
    totalsBuilder.append("totalOplogsFetched", totalOplogsFetched);
    totalsBuilder.append("totalOplogsApplied", totalOplogsApplied);
    totalsBuilder.append("maxDonorIndexes", maxDonorIndexes);
    totalsBuilder.append("maxRecipientIndexes", maxRecipientIndexes);
    totalsBuilder.append("numberOfIndexesDelta", maxRecipientIndexes - maxDonorIndexes);
    statsBuilder.append("totals", totalsBuilder.obj());

    bool hadCriticalSection = false;
    if (auto criticalSectionInterval =
            _metrics->getIntervalFor(resharding_metrics::TimedPhase::kCriticalSection)) {
        criticalSectionBuilder.append("interval", criticalSectionInterval->toBSON());
        hadCriticalSection = true;
    }
    if (auto expiration = _coordinatorDoc.getCriticalSectionExpiresAt()) {
        criticalSectionBuilder.append("expiration", *expiration);
        hadCriticalSection = true;
    }
    if (hadCriticalSection) {
        criticalSectionBuilder.append("totalWritesDuringCriticalSection",
                                      totalWritesDuringCriticalSection);
        statsBuilder.append("criticalSection", criticalSectionBuilder.obj());
    }

    builder.append("statistics", statsBuilder.obj());
    LOGV2(7763800, "Resharding complete", "info"_attr = builder.obj());
}

const ShardId& ReshardingCoordinator::_getChangeStreamNotifierShardId() const {
    // Change stream readers expect to receive pre & post commit event notifications
    // from one of the shards holding data before the beginning of the resharding.
    return _coordinatorDoc.getDonorShards().front().getId();
}
#endif  // RESHARDING_COORDINATOR_PART_4

}  // namespace mongo
#undef MONGO_LOGV2_DEFAULT_COMPONENT
