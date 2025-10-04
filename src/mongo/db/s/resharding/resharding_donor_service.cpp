/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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


#include "mongo/db/s/resharding/resharding_donor_service.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/ddl/sharding_recovery_service.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/index_builds/index_builds_coordinator.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_runtime.h"
#include "mongo/db/local_catalog/shard_role_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/query/write_ops/delete.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/resharding/coordinator_document_gen.h"
#include "mongo/db/s/resharding/resharding_change_event_o2_field_gen.h"
#include "mongo/db/s/resharding/resharding_data_copy_util.h"
#include "mongo/db/s/resharding/resharding_donor_recipient_common.h"
#include "mongo/db/s/resharding/resharding_future_util.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/server_options.h"
#include "mongo/db/session/kill_sessions_local.h"
#include "mongo/db/session/session_killer.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/user_write_block/write_block_bypass.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/future_util.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <tuple>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding


namespace mongo {

MONGO_FAIL_POINT_DEFINE(reshardingPauseDonorBeforeCatalogCacheRefresh);
MONGO_FAIL_POINT_DEFINE(reshardingPauseDonorAfterBlockingReads);
MONGO_FAIL_POINT_DEFINE(reshardingDonorFailsAfterTransitionToDonatingOplogEntries);
MONGO_FAIL_POINT_DEFINE(removeDonorDocFailpoint);
MONGO_FAIL_POINT_DEFINE(reshardingDonorFailsBeforeObtainingTimestamp);
MONGO_FAIL_POINT_DEFINE(reshardingDonorFailsUpdatingChangeStreamsMonitorProgress);

namespace {

const WriteConcernOptions kNoWaitWriteConcern{1, WriteConcernOptions::SyncMode::UNSET, Seconds(0)};

Timestamp generateMinFetchTimestamp(OperationContext* opCtx, const NamespaceString& sourceNss) {
    // Do a no-op write and use the OpTime as the minFetchTimestamp
    writeConflictRetry(
        opCtx, "resharding donor minFetchTimestamp", NamespaceString::kRsOplogNamespace, [&] {
            AutoGetDb db(opCtx, sourceNss.dbName(), MODE_IX);
            Lock::CollectionLock collLock(opCtx, sourceNss, MODE_S);

            AutoGetOplogFastPath oplogWrite(opCtx, OplogAccessMode::kWrite);

            const std::string msg = str::stream()
                << "All future oplog entries on the namespace " << sourceNss.toStringForErrorMsg()
                << " must include a 'destinedRecipient' field";
            WriteUnitOfWork wuow(opCtx);
            opCtx->getClient()->getServiceContext()->getOpObserver()->onInternalOpMessage(
                opCtx,
                NamespaceString::kForceOplogBatchBoundaryNamespace,
                boost::none,
                BSON("msg" << msg),
                boost::none,
                boost::none,
                boost::none,
                boost::none,
                boost::none);
            wuow.commit();
        });

    auto generatedOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    return generatedOpTime.getTimestamp();
}

/**
 * Fulfills the promise if it is not already. Otherwise, does nothing.
 */
void ensureFulfilledPromise(WithLock lk, SharedPromise<void>& sp) {
    if (!sp.getFuture().isReady()) {
        sp.emplaceValue();
    }
}

template <typename Type>
void ensureFulfilledPromise(WithLock lk, SharedPromise<Type>& sp, Type value) {
    if (!sp.getFuture().isReady()) {
        sp.emplaceValue(value);
    }
}

template <typename Type>
void ensureFulfilledPromise(WithLock lk, SharedPromise<Type>& sp, Status error) {
    if (!sp.getFuture().isReady()) {
        sp.setError(error);
    }
}

/**
 * Returns whether it is possible for the donor to be in 'state' when resharding will indefinitely
 * abort.
 */
bool inPotentialAbortScenario(const DonorStateEnum& state) {
    // Regardless of whether resharding will abort or commit, the donor will eventually reach state
    // kDone. Additionally, if the donor is in state kError, it is guaranteed that the coordinator
    // will eventually begin the abort process.
    return state == DonorStateEnum::kError || state == DonorStateEnum::kDone;
}

class ExternalStateImpl : public ReshardingDonorService::DonorStateMachineExternalState {
public:
    ShardId myShardId(ServiceContext* serviceContext) const override {
        return ShardingState::get(serviceContext)->shardId();
    }

    void waitForCollectionFlush(OperationContext* opCtx, const NamespaceString& nss) override {
        FilteringMetadataCache::get(opCtx)->waitForCollectionFlush(opCtx, nss);
    }

    void updateCoordinatorDocument(OperationContext* opCtx,
                                   const BSONObj& query,
                                   const BSONObj& update) override {
        auto catalogClient = Grid::get(opCtx)->catalogClient();
        auto docWasModified = uassertStatusOK(catalogClient->updateConfigDocument(
            opCtx,
            NamespaceString::kConfigReshardingOperationsNamespace,
            query,
            update,
            false, /* upsert */
            resharding::kMajorityWriteConcern,
            Milliseconds::max()));

        if (!docWasModified) {
            LOGV2_DEBUG(
                5543400,
                1,
                "Resharding coordinator document was not modified by the donor's update; this is "
                "expected when the update had previously been interrupted due to a stepdown",
                "query"_attr = query,
                "update"_attr = update);
        }
    }

    void refreshCollectionPlacementInfo(OperationContext* opCtx,
                                        const NamespaceString& sourceNss) override {
        uassertStatusOK(FilteringMetadataCache::get(opCtx)->onCollectionPlacementVersionMismatch(
            opCtx, sourceNss, boost::none));
    }

    std::unique_ptr<ShardingRecoveryService::BeforeReleasingCustomAction>
    getOnReleaseCriticalSectionCustomAction() override {
        return std::make_unique<ShardingRecoveryService::FilteringMetadataClearer>();
    }

    void abortUnpreparedTransactionIfNecessary(OperationContext* opCtx) override {
        if (resharding::gFeatureFlagReshardingAbortUnpreparedTransactionsUponPreparingToBlockWrites
                .isEnabled(VersionContext::getDecoration(opCtx),
                           serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) &&
            resharding::gReshardingAbortUnpreparedTransactionsUponPreparingToBlockWrites.load()) {
            // Unless explicitly opted out, abort any unprepared transactions that may be running on
            // the donor shard. This helps prevent the donor from not being to acquire the critical
            // section within the critical section timeout when there are long-running transactions.
            //
            // TODO (SERVER-106990): Abort unprepared transactions after enqueuing the collection
            // lock request instead. Until we do SERVER-106990, this is best-effort only since a new
            // transaction may start between this step and the step for acquiring the critical
            // section. Please note that InterruptedDueToReshardingCriticalSection is a transient
            // error code so any aborted transactions would get retried by the driver.

            SessionKiller::Matcher matcherAllSessions(
                KillAllSessionsByPatternSet{makeKillAllSessionsByPattern(opCtx)});
            killSessionsAbortUnpreparedTransactions(
                opCtx, matcherAllSessions, ErrorCodes::InterruptedDueToReshardingCriticalSection);
        }
    }
};

}  // namespace

ThreadPool::Limits ReshardingDonorService::getThreadPoolLimits() const {
    ThreadPool::Limits threadPoolLimit;
    threadPoolLimit.maxThreads = resharding::gReshardingDonorServiceMaxThreadCount;
    return threadPoolLimit;
}

std::shared_ptr<repl::PrimaryOnlyService::Instance> ReshardingDonorService::constructInstance(
    BSONObj initialState) {
    return std::make_shared<DonorStateMachine>(
        this,
        ReshardingDonorDocument::parse(initialState, IDLParserContext{"DonorStateMachine"}),
        std::make_unique<ExternalStateImpl>(),
        _serviceContext);
}

ReshardingDonorService::DonorStateMachine::DonorStateMachine(
    const ReshardingDonorService* donorService,
    const ReshardingDonorDocument& donorDoc,
    std::unique_ptr<DonorStateMachineExternalState> externalState,
    ServiceContext* serviceContext)
    : repl::PrimaryOnlyService::TypedInstance<DonorStateMachine>(),
      _donorService(donorService),
      _serviceContext(serviceContext),
      _metrics{ReshardingMetrics::initializeFrom(donorDoc, _serviceContext)},
      _metadata{donorDoc.getCommonReshardingMetadata()},
      _recipientShardIds{donorDoc.getRecipientShards()},
      _donorCtx{donorDoc.getMutableState()},
      _changeStreamsMonitorCtx{donorDoc.getChangeStreamsMonitor()},
      _donorMetricsToRestore{donorDoc.getMetrics() ? donorDoc.getMetrics().value()
                                                   : ReshardingDonorMetrics()},
      _externalState{std::move(externalState)},
      _markKilledExecutor{
          resharding::makeThreadPoolForMarkKilledExecutor("ReshardingDonorCancelableOpCtxPool")},
      _critSecReason(BSON("command"
                          << "resharding_donor"
                          << "collection"
                          << NamespaceStringUtil::serialize(_metadata.getSourceNss(),
                                                            SerializationContext::stateDefault()))),
      _isAlsoRecipient([&] {
          auto myShardId = _externalState->myShardId(_serviceContext);
          return std::find(_recipientShardIds.begin(), _recipientShardIds.end(), myShardId) !=
              _recipientShardIds.end();
      }()) {
    invariant(_externalState);

    if (_changeStreamsMonitorCtx) {
        invariant(_metadata.getPerformVerification());

        stdx::lock_guard<stdx::mutex> lk(_mutex);
        ensureFulfilledPromise(lk,
                               _changeStreamMonitorStartTimeSelected,
                               _changeStreamsMonitorCtx->getStartAtOperationTime());
        if (_changeStreamsMonitorCtx->getCompleted()) {
            ensureFulfilledPromise(lk, _changeStreamsMonitorStarted);
            ensureFulfilledPromise(
                lk, _changeStreamsMonitorCompleted, _changeStreamsMonitorCtx->getDocumentsDelta());
        }
    }

    _metrics->onStateTransition(boost::none, _donorCtx.getState());
}

ExecutorFuture<void> ReshardingDonorService::DonorStateMachine::_runUntilBlockingWritesOrErrored(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& abortToken) {
    return _retryingCancelableOpCtxFactory
        ->withAutomaticRetry([this, executor, abortToken](const auto& factory) {
            return ExecutorFuture(**executor)
                .then([this, &factory] {
                    _onPreparingToDonateCalculateTimestampThenTransitionToDonatingInitialData(
                        factory);
                })
                .then([this, executor, abortToken, &factory] {
                    return _awaitAllRecipientsDoneCloningThenTransitionToDonatingOplogEntries(
                        executor, abortToken, factory);
                })
                .then([this, executor, abortToken, &factory] {
                    return _createAndStartChangeStreamsMonitor(executor, abortToken, factory);
                })
                .then([this, executor, abortToken, &factory] {
                    return _awaitAllRecipientsDoneApplyingThenTransitionToPreparingToBlockWrites(
                        executor, abortToken, factory);
                })
                .then([this, &factory] {
                    _writeTransactionOplogEntryThenTransitionToBlockingWrites(factory);
                })
                .then([this, executor, abortToken] {
                    return _awaitChangeStreamsMonitorCompleted(executor, abortToken);
                });
        })
        .onTransientError([](const Status& status) {
            LOGV2(5633603,
                  "Donor _runUntilBlockingWritesOrErrored encountered transient error",
                  "error"_attr = status);
        })
        .onUnrecoverableError([](const Status& status) {
            LOGV2(10494609,
                  "Donor _runUntilBlockingWritesOrErrored encountered unrecoverable error",
                  "error"_attr = status);
        })
        .until<Status>([](const Status& status) { return status.isOK(); })
        .on(**executor, abortToken)
        .onError([this, executor, abortToken](Status status) {
            if (abortToken.isCanceled()) {
                return ExecutorFuture<void>(**executor, status);
            }

            LOGV2(4956400,
                  "Resharding operation donor state machine failed",
                  logAttrs(_metadata.getSourceNss()),
                  "reshardingUUID"_attr = _metadata.getReshardingUUID(),
                  "error"_attr = status);

            {
                stdx::lock_guard<stdx::mutex> lk(_mutex);
                ensureFulfilledPromise(lk, _changeStreamsMonitorStarted, status);
                ensureFulfilledPromise(lk, _changeStreamsMonitorCompleted, status);
                ensureFulfilledPromise(lk, _critSecWasAcquired, status);
                ensureFulfilledPromise(lk, _critSecWasPromoted, status);
            }

            return _retryingCancelableOpCtxFactory
                ->withAutomaticRetry([this, status](const auto& factory) {
                    // It is illegal to transition into kError if the state is in or has already
                    // surpassed kBlockingWrites.
                    invariant(_donorCtx.getState() < DonorStateEnum::kBlockingWrites);
                    _transitionToError(status, factory);

                    // Intentionally swallow the error - by transitioning to kError, the donor
                    // effectively recovers from encountering the error and should continue
                    // running in the future chain.
                })
                .onTransientError([](const Status& status) {
                    LOGV2(5633601,
                          "Donor _runUntilBlockingWritesOrErrored encountered transient "
                          "error while transitioning to state kError",
                          "error"_attr = status);
                })
                .onUnrecoverableError([](const Status& status) {
                    LOGV2(10494610,
                          "Donor _runUntilBlockingWritesOrErrored encountered unrecoverable "
                          "error while transitioning to state kError",
                          "error"_attr = status);
                })
                .until<Status>([](const Status& status) { return status.isOK(); })
                .on(**executor, abortToken);
        })
        .onCompletion([this, executor, abortToken](Status status) {
            if (abortToken.isCanceled()) {
                return ExecutorFuture<void>(**executor, status);
            }

            {
                // The donor is done with all local transitions until the coordinator makes its
                // decision.
                stdx::lock_guard<stdx::mutex> lk(_mutex);
                invariant(_donorCtx.getState() >= DonorStateEnum::kError);
                ensureFulfilledPromise(lk, _inBlockingWritesOrError);
            }
            return ExecutorFuture<void>(**executor, status);
        });
}

ExecutorFuture<void> ReshardingDonorService::DonorStateMachine::_notifyCoordinatorAndAwaitDecision(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& abortToken) {
    if (_donorCtx.getState() == DonorStateEnum::kDone) {
        {
            stdx::lock_guard<stdx::mutex> lk(_mutex);
            ensureFulfilledPromise(lk, _critSecWasPromoted);
        }
        return ExecutorFuture(**executor);
    }

    return _retryingCancelableOpCtxFactory
        ->withAutomaticRetry([this, executor](const auto& factory) {
            auto opCtx = factory.makeOperationContext(&cc());
            return _updateCoordinator(opCtx.get(), executor, factory);
        })
        .onTransientError([](const Status& status) {
            LOGV2(5633602,
                  "Transient error while notifying the coordinator and awaiting decision",
                  "error"_attr = status);
        })
        .onUnrecoverableError([](const Status& status) {
            LOGV2(10494611,
                  "Unrecoverable error while notifying the coordinator and awaiting decision",
                  "error"_attr = status);
        })
        .until<Status>([](const Status& status) { return status.isOK(); })
        .on(**executor, abortToken)
        .then([this, abortToken] {
            stdx::lock_guard<stdx::mutex> lk(_mutex);
            return future_util::withCancellation(_coordinatorHasDecisionPersisted.getFuture(),
                                                 abortToken);
        });
}

ExecutorFuture<void> ReshardingDonorService::DonorStateMachine::_finishReshardingOperation(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& stepdownToken,
    bool aborted) {
    return _retryingCancelableOpCtxFactory
        ->withAutomaticRetry([this, executor, stepdownToken, aborted](const auto& factory) {
            if (!aborted) {
                // If a failover occured after the donor transitioned to done locally, but
                // before it notified the coordinator, it will already be in state done here.
                // Otherwise, it must be in blocking-writes before transitioning to done.
                invariant(_donorCtx.getState() == DonorStateEnum::kBlockingWrites ||
                          _donorCtx.getState() == DonorStateEnum::kDone);

                _dropOriginalCollectionThenTransitionToDone(factory);
            } else if (_donorCtx.getState() != DonorStateEnum::kDone) {
                {
                    // Unblock the RecoverRefreshThread as quickly as possible when aborting.
                    stdx::lock_guard<stdx::mutex> lk(_mutex);
                    ensureFulfilledPromise(
                        lk, _critSecWasAcquired, {ErrorCodes::ReshardCollectionAborted, "aborted"});
                    ensureFulfilledPromise(
                        lk, _critSecWasPromoted, {ErrorCodes::ReshardCollectionAborted, "aborted"});
                }

                // If aborted, the donor must be allowed to transition to done from any
                // state.
                _transitionToDone(aborted, factory);
            }

            {
                auto opCtx = factory.makeOperationContext(&cc());

                // Clear filtering metadata for the temp resharding namespace;
                // We force a refresh to make sure that the placement information is updated
                // in cache after abort decision before the donor state document is deleted.
                {
                    const auto coll = acquireCollection(
                        opCtx.get(),
                        CollectionAcquisitionRequest::fromOpCtx(opCtx.get(),
                                                                _metadata.getTempReshardingNss(),
                                                                AcquisitionPrerequisites::kWrite),
                        MODE_IX);
                    CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(
                        opCtx.get(), _metadata.getTempReshardingNss())
                        ->clearFilteringMetadata(opCtx.get());
                }

                const auto onReleaseCriticalSectionAction =
                    _externalState->getOnReleaseCriticalSectionCustomAction();
                ShardingRecoveryService::get(opCtx.get())
                    ->releaseRecoverableCriticalSection(
                        opCtx.get(),
                        _metadata.getSourceNss(),
                        _critSecReason,
                        ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
                        *onReleaseCriticalSectionAction);

                _metrics->setEndFor(ReshardingMetrics::TimedPhase::kCriticalSection,
                                    resharding::getCurrentTime());

                // We force a refresh to make sure that the placement information is updated
                // in cache after abort decision before the donor state document is deleted.
                std::initializer_list<NamespaceString> namespacesToRefresh{
                    _metadata.getSourceNss(), _metadata.getTempReshardingNss()};
                for (const auto& nss : namespacesToRefresh) {
                    _externalState->refreshCollectionPlacementInfo(opCtx.get(), nss);
                    _externalState->waitForCollectionFlush(opCtx.get(), nss);
                }
            }

            auto opCtx = factory.makeOperationContext(&cc());
            return _updateCoordinator(opCtx.get(), executor, factory)
                .then([this, aborted, stepdownToken, &factory] {
                    {
                        auto opCtx = factory.makeOperationContext(&cc());
                        removeDonorDocFailpoint.pauseWhileSet(opCtx.get());
                    }
                    _removeDonorDocument(stepdownToken, aborted, factory);
                });
        })
        .onTransientError([](const Status& status) {
            LOGV2(5633600,
                  "Transient error while finishing resharding operation",
                  "error"_attr = status);
        })
        .onUnrecoverableError([](const Status& status) {})
        .until<Status>([](const Status& status) { return status.isOK(); })
        .on(**executor, stepdownToken);
}

ExecutorFuture<void> ReshardingDonorService::DonorStateMachine::_runMandatoryCleanup(
    Status status, const CancellationToken& stepdownToken) {
    return ExecutorFuture(_donorService->getInstanceCleanupExecutor())
        .then([this, self = shared_from_this()] {
            if (_changeStreamsMonitor) {
                LOGV2(9858410, "Waiting for the change streams monitor to clean up");
            }
            return _changeStreamsMonitorQuiesced.thenRunOn(
                _donorService->getInstanceCleanupExecutor());
        })
        .onCompletion([this, anchor = shared_from_this(), status, stepdownToken](
                          Status changeStreamsMonitorStatus) {
            LOGV2(9858411, "Performing cleanup");

            _metrics->onStateTransition(_donorCtx.getState(), boost::none);

            // Unregister metrics early so the cumulative metrics do not continue to track these
            // metrics for the lifetime of this state machine. We have future callbacks copy shared
            // pointers to this state machine that causes it to live longer than expected, and can
            // potentially overlap with a newer instance when stepping up.
            _metrics->deregisterMetrics();

            if (!status.isOK()) {
                // If the stepdownToken was triggered, it takes priority in order to make sure that
                // the promise is set with an error that can be retried with. If it ran into an
                // unrecoverable error, it would have fasserted earlier.
                auto statusForPromise = stepdownToken.isCanceled()
                    ? Status{ErrorCodes::InterruptedDueToReplStateChange,
                             "Resharding operation donor state machine interrupted due to replica "
                             "set stepdown"}
                    : status;

                stdx::lock_guard<stdx::mutex> lk(_mutex);

                ensureFulfilledPromise(lk, _changeStreamsMonitorStarted, statusForPromise);
                ensureFulfilledPromise(lk, _changeStreamsMonitorCompleted, statusForPromise);
                ensureFulfilledPromise(lk, _critSecWasAcquired, statusForPromise);
                ensureFulfilledPromise(lk, _critSecWasPromoted, statusForPromise);
                ensureFulfilledPromise(lk, _completionPromise, statusForPromise);
            }

            return status;
        });
}

SemiFuture<void> ReshardingDonorService::DonorStateMachine::run(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& stepdownToken) noexcept {
    auto abortToken = _initAbortSource(stepdownToken);
    _markKilledExecutor->startup();
    _retryingCancelableOpCtxFactory.emplace(
        abortToken,
        _markKilledExecutor,
        resharding::kRetryabilityPredicateIncludeWriteConcernTimeout);

    return ExecutorFuture(**executor)
        .then([this, executor, abortToken] {
            return _runUntilBlockingWritesOrErrored(executor, abortToken);
        })
        .then([this, executor, abortToken] {
            return _notifyCoordinatorAndAwaitDecision(executor, abortToken);
        })
        .onCompletion([this, executor, stepdownToken, abortToken](Status status) {
            _retryingCancelableOpCtxFactory.emplace(
                stepdownToken,
                _markKilledExecutor,
                resharding::kRetryabilityPredicateIncludeWriteConcernTimeout);
            if (stepdownToken.isCanceled()) {
                // Propagate any errors from the donor stepping down.
                return ExecutorFuture<bool>(**executor, status);
            }

            if (!status.isOK() && !abortToken.isCanceled()) {
                // Propagate any errors from the donor failing to notify the coordinator.
                return ExecutorFuture<bool>(**executor, status);
            }

            return ExecutorFuture(**executor, abortToken.isCanceled());
        })
        .then([this, executor, stepdownToken](bool aborted) {
            return _finishReshardingOperation(executor, stepdownToken, aborted);
        })
        .onError([this, stepdownToken](Status status) {
            if (stepdownToken.isCanceled()) {
                return status;
            }

            LOGV2_FATAL(5160600,
                        "Unrecoverable error occurred past the point donor was prepared to "
                        "complete the resharding operation",
                        "error"_attr = redact(status));
        })
        // The shared_ptr stored in the PrimaryOnlyService's map for the ReshardingDonorService
        // Instance is removed when the donor state document tied to the instance is deleted. It is
        // necessary to use shared_from_this() to extend the lifetime so the all earlier code can
        // safely finish executing.
        .onCompletion([anchor = shared_from_this()](Status status) { return status; })
        .thenRunOn(_donorService->getInstanceCleanupExecutor())
        .onCompletion([this, anchor = shared_from_this(), stepdownToken](Status status) {
            // On stepdown or shutdown, the _scopedExecutor may have already been shut down.
            // Everything in this function runs on the instance's cleanup executor, and will
            // execute regardless of any work on _scopedExecutor ever running.
            return _runMandatoryCleanup(status, stepdownToken);
        })
        .semi();
}

void ReshardingDonorService::DonorStateMachine::interrupt(Status status) {}

boost::optional<BSONObj> ReshardingDonorService::DonorStateMachine::reportForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode connMode,
    MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept {
    return _metrics->reportForCurrentOp();
}

void ReshardingDonorService::DonorStateMachine::onReshardingFieldsChanges(
    OperationContext* opCtx, const TypeCollectionReshardingFields& reshardingFields) {
    if (reshardingFields.getState() == CoordinatorStateEnum::kAborting) {
        abort(reshardingFields.getUserCanceled().value());
        return;
    }

    const CoordinatorStateEnum coordinatorState = reshardingFields.getState();
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        if (coordinatorState >= CoordinatorStateEnum::kApplying) {
            ensureFulfilledPromise(lk, _allRecipientsDoneCloning);
        }

        if (coordinatorState >= CoordinatorStateEnum::kBlockingWrites) {
            ensureFulfilledPromise(lk, _allRecipientsDoneApplying);
        }

        if (coordinatorState >= CoordinatorStateEnum::kCommitting) {
            ensureFulfilledPromise(lk, _coordinatorHasDecisionPersisted);
        }
    }
}

void ReshardingDonorService::DonorStateMachine::onWriteDuringCriticalSection() {
    _metrics->onWriteDuringCriticalSection();
}

void ReshardingDonorService::DonorStateMachine::onReadDuringCriticalSection() {
    _metrics->onReadDuringCriticalSection();
}

SharedSemiFuture<void> ReshardingDonorService::DonorStateMachine::awaitCriticalSectionAcquired() {
    return _critSecWasAcquired.getFuture();
}

SharedSemiFuture<void> ReshardingDonorService::DonorStateMachine::awaitCriticalSectionPromoted() {
    return _critSecWasPromoted.getFuture();
}

void ReshardingDonorService::DonorStateMachine::
    _onPreparingToDonateCalculateTimestampThenTransitionToDonatingInitialData(
        const CancelableOperationContextFactory& factory) {
    if (_donorCtx.getState() > DonorStateEnum::kPreparingToDonate) {
        if (!inPotentialAbortScenario(_donorCtx.getState())) {
            // The invariants won't hold if an unrecoverable error is encountered before the donor
            // makes enough progress to transition to kDonatingInitialData and then a failover
            // occurs.
            invariant(_donorCtx.getMinFetchTimestamp());
            invariant(_donorCtx.getBytesToClone());
            invariant(_donorCtx.getDocumentsToClone());
        }
        return;
    }

    int64_t bytesToClone = 0;
    int64_t documentsToClone = 0;
    int64_t indexCount = 0;

    {
        auto opCtx = factory.makeOperationContext(&cc());
        auto rawOpCtx = opCtx.get();

        const auto coll = acquireCollection(
            rawOpCtx,
            CollectionAcquisitionRequest::fromOpCtx(
                rawOpCtx, _metadata.getSourceNss(), AcquisitionPrerequisites::kRead),
            MODE_IS);
        if (auto optionalCount = resharding::getIndexCount(rawOpCtx, coll)) {
            indexCount = *optionalCount;
        }

        if (coll.exists()) {
            IndexBuildsCoordinator::get(rawOpCtx)->assertNoIndexBuildInProgForCollection(
                coll.uuid());

            bytesToClone = coll.getCollectionPtr()->dataSize(rawOpCtx);
            documentsToClone = coll.getCollectionPtr()->numRecords(rawOpCtx);
        }
    }

    // Recipient shards expect to read from the donor shard's existing sharded collection and the
    // config.cache.chunks collection of the temporary resharding collection using
    // {atClusterTime: <fetchTimestamp>}. Refreshing the temporary resharding collection on the
    // donor shards causes them to create the config.cache.chunks collection. Without this refresh,
    // the {atClusterTime: <fetchTimestamp>} read on the config.cache.chunks namespace would fail
    // with a SnapshotUnavailable error response.
    {
        auto opCtx = factory.makeOperationContext(&cc());
        reshardingPauseDonorBeforeCatalogCacheRefresh.pauseWhileSet(opCtx.get());

        _externalState->refreshCollectionPlacementInfo(opCtx.get(),
                                                       _metadata.getTempReshardingNss());
        _externalState->waitForCollectionFlush(opCtx.get(), _metadata.getTempReshardingNss());
    }

    reshardingDonorFailsBeforeObtainingTimestamp.execute([&](const BSONObj& data) {
        auto errmsgElem = data["errmsg"];
        StringData errmsg = errmsgElem ? errmsgElem.checkAndGetStringData() : "Failing for test"_sd;
        uasserted(ErrorCodes::InternalError, errmsg);
    });

    Timestamp minFetchTimestamp = [this, &factory] {
        auto opCtx = factory.makeOperationContext(&cc());
        return generateMinFetchTimestamp(opCtx.get(), _metadata.getSourceNss());
    }();

    {
        auto opCtx = factory.makeOperationContext(&cc());
        auto rawOpCtx = opCtx.get();

        auto generateOplogEntry = [&]() {
            ReshardBeginChangeEventO2Field changeEvent{
                _metadata.getSourceNss(),
                _metadata.getReshardingUUID(),
            };

            repl::MutableOplogEntry oplog;
            oplog.setNss(_metadata.getSourceNss());
            oplog.setOpType(repl::OpTypeEnum::kNoop);
            oplog.setUuid(_metadata.getSourceUUID());
            oplog.setObject(BSON("msg" << "Created temporary resharding collection"));
            oplog.setObject2(changeEvent.toBSON());
            oplog.setFromMigrate(true);
            oplog.setOpTime(OplogSlot());
            oplog.setWallClockTime(opCtx->fastClockSource().now());
            return oplog;
        };

        auto oplog = generateOplogEntry();
        writeConflictRetry(
            rawOpCtx, "ReshardingBeginOplog", NamespaceString::kRsOplogNamespace, [&] {
                AutoGetOplogFastPath oplogWrite(rawOpCtx, OplogAccessMode::kWrite);
                WriteUnitOfWork wunit(rawOpCtx);
                const auto& oplogOpTime = repl::logOp(rawOpCtx, &oplog);
                uassert(5052101,
                        str::stream()
                            << "Failed to create new oplog entry for oplog with opTime: "
                            << oplog.getOpTime().toString() << ": " << redact(oplog.toBSON()),
                        !oplogOpTime.isNull());
                wunit.commit();
            });
    }

    LOGV2_DEBUG(5390702,
                2,
                "Collection being resharded now ready for recipients to begin cloning",
                logAttrs(_metadata.getSourceNss()),
                "minFetchTimestamp"_attr = minFetchTimestamp,
                "bytesToClone"_attr = bytesToClone,
                "documentsToClone"_attr = documentsToClone,
                "reshardingUUID"_attr = _metadata.getReshardingUUID());

    _transitionToDonatingInitialData(
        minFetchTimestamp, bytesToClone, documentsToClone, indexCount, factory);
}

ExecutorFuture<void> ReshardingDonorService::DonorStateMachine::
    _awaitAllRecipientsDoneCloningThenTransitionToDonatingOplogEntries(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& abortToken,
        const CancelableOperationContextFactory& factory) {
    if (_donorCtx.getState() > DonorStateEnum::kDonatingInitialData) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    auto opCtx = factory.makeOperationContext(&cc());
    return _updateCoordinator(opCtx.get(), executor, factory)
        .then([this, executor, abortToken, &factory] {
            return _createAndStartChangeStreamsMonitor(executor, abortToken, factory);
        })
        .then([this, abortToken] {
            return future_util::withCancellation(_allRecipientsDoneCloning.getFuture(), abortToken);
        })
        .thenRunOn(**executor)
        .then([this, &factory]() {
            _transitionState(DonorStateEnum::kDonatingOplogEntries, factory);
        })
        .onCompletion([=, this](Status status) {
            if (!status.isOK()) {
                LOGV2_ERROR(8639700,
                            "Failed to transition the donor shard's state to `donating`",
                            "reshardingUUID"_attr = _metadata.getReshardingUUID(),
                            "error"_attr = redact(status));
                return status;
            }

            reshardingDonorFailsAfterTransitionToDonatingOplogEntries.execute(
                [&](const BSONObj& data) {
                    auto errmsgElem = data["errmsg"];
                    StringData errmsg =
                        errmsgElem ? errmsgElem.checkAndGetStringData() : "Failing for test"_sd;
                    uasserted(ErrorCodes::InternalError, errmsg);
                });
            return status;
        });
}

ExecutorFuture<void> ReshardingDonorService::DonorStateMachine::
    _awaitAllRecipientsDoneApplyingThenTransitionToPreparingToBlockWrites(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& abortToken,
        const CancelableOperationContextFactory& factory) {
    if (_donorCtx.getState() > DonorStateEnum::kDonatingOplogEntries) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    SharedSemiFuture<void> allRecipientsDoneApplyingFuture;
    {
        stdx::lock_guard<std::mutex> lk(_mutex);
        allRecipientsDoneApplyingFuture = _allRecipientsDoneApplying.getFuture();
    }

    return future_util::withCancellation(std::move(allRecipientsDoneApplyingFuture), abortToken)
        .thenRunOn(**executor)
        .then([this, &factory] {
            _transitionState(DonorStateEnum::kPreparingToBlockWrites, factory);
        });
}

void ReshardingDonorService::DonorStateMachine::
    _writeTransactionOplogEntryThenTransitionToBlockingWrites(
        const CancelableOperationContextFactory& factory) {
    if (_donorCtx.getState() > DonorStateEnum::kPreparingToBlockWrites) {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        ensureFulfilledPromise(lk, _critSecWasAcquired);
        return;
    }

    {
        auto opCtx = factory.makeOperationContext(&cc());
        _externalState->abortUnpreparedTransactionIfNecessary(opCtx.get());

        ShardingRecoveryService::get(opCtx.get())
            ->acquireRecoverableCriticalSectionBlockWrites(
                opCtx.get(),
                _metadata.getSourceNss(),
                _critSecReason,
                ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter());

        _metrics->setStartFor(ReshardingMetrics::TimedPhase::kCriticalSection,
                              resharding::getCurrentTime());
    }

    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        ensureFulfilledPromise(lk, _critSecWasAcquired);
    }

    {
        auto opCtx = factory.makeOperationContext(&cc());
        auto rawOpCtx = opCtx.get();

        auto generateOplogEntry = [&](ShardId destinedRecipient) {
            ReshardBlockingWritesChangeEventO2Field changeEvent{
                _metadata.getSourceNss(),
                _metadata.getReshardingUUID(),
                std::string{resharding::kReshardFinalOpLogType},
            };

            repl::MutableOplogEntry oplog;
            oplog.setNss(_metadata.getSourceNss());
            oplog.setOpType(repl::OpTypeEnum::kNoop);
            oplog.setUuid(_metadata.getSourceUUID());
            oplog.setDestinedRecipient(destinedRecipient);
            oplog.setObject(
                BSON("msg" << fmt::format(
                         "Writes to {} are temporarily blocked for resharding.",
                         NamespaceStringUtil::serialize(_metadata.getSourceNss(),
                                                        SerializationContext::stateDefault()))));
            oplog.setObject2(changeEvent.toBSON());
            oplog.setOpTime(OplogSlot());
            oplog.setWallClockTime(opCtx->fastClockSource().now());
            return oplog;
        };

        try {
            Timer latency;

            for (const auto& recipient : _recipientShardIds) {
                auto oplog = generateOplogEntry(recipient);
                writeConflictRetry(
                    rawOpCtx,
                    "ReshardingBlockWritesOplog",
                    NamespaceString::kRsOplogNamespace,
                    [&] {
                        AutoGetOplogFastPath oplogWrite(rawOpCtx, OplogAccessMode::kWrite);
                        WriteUnitOfWork wunit(rawOpCtx);
                        const auto& oplogOpTime = repl::logOp(rawOpCtx, &oplog);
                        uassert(5279507,
                                str::stream()
                                    << "Failed to create new oplog entry for oplog with opTime: "
                                    << oplog.getOpTime().toString() << ": "
                                    << redact(oplog.toBSON()),
                                !oplogOpTime.isNull());
                        wunit.commit();
                    });
            }

            LOGV2_DEBUG(5279504,
                        0,
                        "Committed oplog entries to temporarily block writes for resharding",
                        logAttrs(_metadata.getSourceNss()),
                        "reshardingUUID"_attr = _metadata.getReshardingUUID(),
                        "numRecipients"_attr = _recipientShardIds.size(),
                        "duration"_attr = duration_cast<Milliseconds>(latency.elapsed()));
        } catch (const DBException& e) {
            const auto& status = e.toStatus();
            LOGV2_ERROR(5279508,
                        "Exception while writing resharding final oplog entries",
                        "reshardingUUID"_attr = _metadata.getReshardingUUID(),
                        "error"_attr = status);
            uassertStatusOK(status);
        }
    }

    _transitionState(DonorStateEnum::kBlockingWrites, factory);
}

void ReshardingDonorService::DonorStateMachine::_dropOriginalCollectionThenTransitionToDone(
    const CancelableOperationContextFactory& factory) {
    if (_donorCtx.getState() > DonorStateEnum::kBlockingWrites) {
        {
            stdx::lock_guard<stdx::mutex> lk(_mutex);
            ensureFulfilledPromise(lk, _critSecWasPromoted);
        }
        return;
    }
    {
        auto opCtx = factory.makeOperationContext(&cc());
        ShardingRecoveryService::get(opCtx.get())
            ->promoteRecoverableCriticalSectionToBlockAlsoReads(
                opCtx.get(),
                _metadata.getSourceNss(),
                _critSecReason,
                ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter());
        reshardingPauseDonorAfterBlockingReads.pauseWhileSet(opCtx.get());
    }

    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        ensureFulfilledPromise(lk, _critSecWasPromoted);
    }

    if (_isAlsoRecipient) {
        auto opCtx = factory.makeOperationContext(&cc());
        // Allow bypassing user write blocking. The check has already been performed on the
        // db-primary shard's ReshardCollectionCoordinator.
        WriteBlockBypass::get(opCtx.get()).set(true);
        resharding::data_copy::ensureTemporaryReshardingCollectionRenamed(opCtx.get(), _metadata);
    } else {
        auto opCtx = factory.makeOperationContext(&cc());
        // Allow bypassing user write blocking. The check has already been performed on the
        // db-primary shard's ReshardCollectionCoordinator.
        WriteBlockBypass::get(opCtx.get()).set(true);

        resharding::data_copy::ensureCollectionDropped(
            opCtx.get(), _metadata.getSourceNss(), _metadata.getSourceUUID());
    }

    _transitionToDone(false, factory /* aborted */);
}

SharedSemiFuture<void>
ReshardingDonorService::DonorStateMachine::createAndStartChangeStreamsMonitor(
    const Timestamp& cloneTimestamp) {
    if (!_metadata.getPerformVerification()) {
        return Status{ErrorCodes::IllegalOperation,
                      "Cannot start the change streams monitor when verification is not enabled"};
    }

    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        auto startAtOperationTime = cloneTimestamp + 1;
        ensureFulfilledPromise(lk, _changeStreamMonitorStartTimeSelected, startAtOperationTime);
    }
    return _changeStreamsMonitorStarted.getFuture();
}

SharedSemiFuture<void>
ReshardingDonorService::DonorStateMachine::awaitChangeStreamsMonitorStarted() {
    if (!_metadata.getPerformVerification()) {
        return Status{ErrorCodes::IllegalOperation,
                      "Cannot wait for the change streams monitor to start when verification is "
                      "not enabled. The monitor only exists when verification is enabled"};
    }
    return _changeStreamsMonitorStarted.getFuture();
}

SharedSemiFuture<int64_t>
ReshardingDonorService::DonorStateMachine::awaitChangeStreamsMonitorCompleted() {
    if (!_metadata.getPerformVerification()) {
        return Status{ErrorCodes::IllegalOperation,
                      "Cannot wait for the change streams monitor to complete when verification is "
                      "not enabled. The monitor only exists when verification is enabled"};
    }

    return _changeStreamsMonitorCompleted.getFuture();
}

ExecutorFuture<void> ReshardingDonorService::DonorStateMachine::_createAndStartChangeStreamsMonitor(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& abortToken,
    const CancelableOperationContextFactory& factory) {
    if (!_metadata.getPerformVerification() || _changeStreamsMonitorStarted.getFuture().isReady()) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    SharedSemiFuture<Timestamp> changeStreamMonitorStartTimeSelectedFuture;
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        changeStreamMonitorStartTimeSelectedFuture =
            _changeStreamMonitorStartTimeSelected.getFuture();
    }

    return future_util::withCancellation(std::move(changeStreamMonitorStartTimeSelectedFuture),
                                         abortToken)
        .thenRunOn(**executor)
        .then([this, executor, abortToken, &factory](const Timestamp& startAtOperationTime) {
            if (!_changeStreamsMonitorCtx) {
                LOGV2(9858400,
                      "Initializing the change streams monitor",
                      "reshardingUUID"_attr = _metadata.getReshardingUUID(),
                      "startAtOperationTime"_attr = startAtOperationTime);
                auto opCtx = factory.makeOperationContext(&cc());

                ChangeStreamsMonitorContext changeStreamsMonitorCtx(startAtOperationTime,
                                                                    0 /* documentsDelta */);
                _updateDonorDocument(opCtx.get(), std::move(changeStreamsMonitorCtx));

                // Wait for the write above to be majority committed to make sure it cannot be
                // rolled back after this donor acknowledges to the coordinator that it has started
                // the change streams monitor.
                auto clientOpTime =
                    repl::ReplClientInfo::forClient(opCtx.get()->getClient()).getLastOp();
                return WaitForMajorityService::get(opCtx.get()->getServiceContext())
                    .waitUntilMajorityForWrite(clientOpTime, abortToken)
                    .thenRunOn(**executor);
            }

            return ExecutorFuture<void>(**executor, Status::OK());
        })
        .then([this, executor, abortToken, &factory] {
            auto batchCallback = [this, factory = factory, anchor = shared_from_this()](
                                     const auto& batch) {
                LOGV2(9858404,
                      "Persisting change streams monitor's progress",
                      "reshardingUUID"_attr = _metadata.getReshardingUUID(),
                      "documentsDelta"_attr = batch.getDocumentsDelta(),
                      "completed"_attr = batch.containsFinalEvent());

                auto changeStreamsMonitorCtx = _changeStreamsMonitorCtx.get();
                changeStreamsMonitorCtx.setResumeToken(batch.getResumeToken().getOwned());
                changeStreamsMonitorCtx.setDocumentsDelta(
                    changeStreamsMonitorCtx.getDocumentsDelta() + batch.getDocumentsDelta());
                changeStreamsMonitorCtx.setCompleted(batch.containsFinalEvent());

                if (MONGO_unlikely(
                        reshardingDonorFailsUpdatingChangeStreamsMonitorProgress.shouldFail())) {
                    uasserted(ErrorCodes::InternalError,
                              "Simulating an unrecoverable error for testing inside the donor's "
                              "changeStreamsMonitor callback");
                }

                auto opCtx = factory.makeOperationContext(&cc());
                _updateDonorDocument(opCtx.get(), std::move(changeStreamsMonitorCtx));
            };

            _changeStreamsMonitor = std::make_shared<ReshardingChangeStreamsMonitor>(
                _metadata.getReshardingUUID(),
                _metadata.getSourceNss(),
                _changeStreamsMonitorCtx->getStartAtOperationTime(),
                _changeStreamsMonitorCtx->getResumeToken(),
                batchCallback);

            LOGV2(9858401,
                  "Starting the change streams monitor",
                  "reshardingUUID"_attr = _metadata.getReshardingUUID());
            _changeStreamsMonitorQuiesced =
                _changeStreamsMonitor
                    ->startMonitoring(**executor,
                                      _donorService->getInstanceCleanupExecutor(),
                                      abortToken,
                                      factory)
                    .share();
            _changeStreamsMonitorStarted.emplaceValue();
        });
}

ExecutorFuture<void> ReshardingDonorService::DonorStateMachine::_awaitChangeStreamsMonitorCompleted(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& abortToken) {
    if (!_metadata.getPerformVerification() ||
        _changeStreamsMonitorCompleted.getFuture().isReady()) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    invariant(_changeStreamsMonitor);
    return future_util::withCancellation(_changeStreamsMonitor->awaitFinalChangeEvent(), abortToken)
        .thenRunOn(**executor)
        .onCompletion([this](Status status) {
            LOGV2(9858402,
                  "The change streams monitor completed",
                  "reshardingUUID"_attr = _metadata.getReshardingUUID(),
                  "status"_attr = status);

            stdx::lock_guard<stdx::mutex> lk(_mutex);
            if (status.isOK()) {
                ensureFulfilledPromise(lk,
                                       _changeStreamsMonitorCompleted,
                                       _changeStreamsMonitorCtx->getDocumentsDelta());
            } else {
                ensureFulfilledPromise(lk, _changeStreamsMonitorCompleted, status);
            }
        });
}

void ReshardingDonorService::DonorStateMachine::_transitionState(
    DonorStateEnum newState, const CancelableOperationContextFactory& factory) {
    invariant(newState != DonorStateEnum::kDonatingInitialData &&
              newState != DonorStateEnum::kError && newState != DonorStateEnum::kDone);

    auto newDonorCtx = _donorCtx;
    newDonorCtx.setState(newState);
    _transitionState(std::move(newDonorCtx), factory);
}

void ReshardingDonorService::DonorStateMachine::_transitionState(
    DonorShardContext&& newDonorCtx, const CancelableOperationContextFactory& factory) {
    // For logging purposes.
    auto oldState = _donorCtx.getState();
    auto newState = newDonorCtx.getState();

    // The donor state machine enters the kError state on unrecoverable errors and so we don't
    // expect it to ever transition from kError except to kDone.
    invariant(oldState != DonorStateEnum::kError || newState == DonorStateEnum::kDone);

    _updateDonorDocument(std::move(newDonorCtx), factory);

    _metrics->onStateTransition(oldState, newState);

    LOGV2_INFO(5279505,
               "Transitioned resharding donor state",
               "newState"_attr = DonorState_serializer(newState),
               "oldState"_attr = DonorState_serializer(oldState),
               logAttrs(_metadata.getSourceNss()),
               "collectionUUID"_attr = _metadata.getSourceUUID(),
               "reshardingUUID"_attr = _metadata.getReshardingUUID());
}

void ReshardingDonorService::DonorStateMachine::_transitionToDonatingInitialData(
    Timestamp minFetchTimestamp,
    int64_t bytesToClone,
    int64_t documentsToClone,
    int64_t indexCount,
    const CancelableOperationContextFactory& factory) {
    auto newDonorCtx = _donorCtx;
    newDonorCtx.setState(DonorStateEnum::kDonatingInitialData);
    newDonorCtx.setMinFetchTimestamp(minFetchTimestamp);
    newDonorCtx.setBytesToClone(bytesToClone);
    newDonorCtx.setDocumentsToClone(documentsToClone);
    newDonorCtx.setIndexCount(indexCount);
    _transitionState(std::move(newDonorCtx), factory);
}

void ReshardingDonorService::DonorStateMachine::_transitionToError(
    Status abortReason, const CancelableOperationContextFactory& factory) {
    auto newDonorCtx = _donorCtx;
    newDonorCtx.setState(DonorStateEnum::kError);
    resharding::emplaceTruncatedAbortReasonIfExists(newDonorCtx, abortReason);
    _transitionState(std::move(newDonorCtx), factory);
}

void ReshardingDonorService::DonorStateMachine::_transitionToDone(
    bool aborted, const CancelableOperationContextFactory& factory) {
    auto newDonorCtx = _donorCtx;
    newDonorCtx.setState(DonorStateEnum::kDone);
    if (aborted) {
        resharding::emplaceTruncatedAbortReasonIfExists(newDonorCtx,
                                                        resharding::coordinatorAbortedError());
    }
    _transitionState(std::move(newDonorCtx), factory);
}

/**
 * Returns a query filter of the form
 * {
 *     _id: <reshardingUUID>,
 *     donorShards: {$elemMatch: {
 *         id: <this donor's ShardId>,
 *         "mutableState.state: {$in: [ <list of valid current states> ]},
 *     }},
 * }
 */
BSONObj ReshardingDonorService::DonorStateMachine::_makeQueryForCoordinatorUpdate(
    const ShardId& shardId, DonorStateEnum newState) {
    // The donor only updates the coordinator when it transitions to states which the coordinator
    // depends on for its own transitions. The table maps the donor states which could be updated on
    // the coordinator to the only states the donor could have already persisted to the current
    // coordinator document in order for its transition to the newState to be valid.
    static const stdx::unordered_map<DonorStateEnum, std::vector<DonorStateEnum>>
        validPreviousStateMap = {
            {DonorStateEnum::kDonatingInitialData, {DonorStateEnum::kUnused}},
            {DonorStateEnum::kError,
             {DonorStateEnum::kUnused, DonorStateEnum::kDonatingInitialData}},
            {DonorStateEnum::kBlockingWrites, {DonorStateEnum::kDonatingInitialData}},
            {DonorStateEnum::kDone,
             {DonorStateEnum::kUnused,
              DonorStateEnum::kDonatingInitialData,
              DonorStateEnum::kError,
              DonorStateEnum::kBlockingWrites}},
        };

    auto it = validPreviousStateMap.find(newState);
    invariant(it != validPreviousStateMap.end());

    // The network isn't perfectly reliable so it is possible for update commands sent by
    // _updateCoordinator() to be received out of order by the coordinator. To overcome this
    // behavior, the donor shard includes the list of valid current states as part of the
    // update to transition to the next state. This way, the update from a delayed message
    // won't match the document if it or any later state transitions have already occurred.
    BSONObjBuilder queryBuilder;
    {
        _metadata.getReshardingUUID().appendToBuilder(
            &queryBuilder, ReshardingCoordinatorDocument::kReshardingUUIDFieldName);

        BSONObjBuilder donorShardsBuilder(
            queryBuilder.subobjStart(ReshardingCoordinatorDocument::kDonorShardsFieldName));
        {
            BSONObjBuilder elemMatchBuilder(donorShardsBuilder.subobjStart("$elemMatch"));
            {
                elemMatchBuilder.append(DonorShardEntry::kIdFieldName, shardId);

                BSONObjBuilder mutableStateBuilder(
                    elemMatchBuilder.subobjStart(DonorShardEntry::kMutableStateFieldName + "." +
                                                 DonorShardContext::kStateFieldName));
                {
                    BSONArrayBuilder inBuilder(mutableStateBuilder.subarrayStart("$in"));
                    for (const auto& state : it->second) {
                        inBuilder.append(DonorState_serializer(state));
                    }
                }
            }
        }
    }

    return queryBuilder.obj();
}

ExecutorFuture<void> ReshardingDonorService::DonorStateMachine::_updateCoordinator(
    OperationContext* opCtx,
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancelableOperationContextFactory& factory) {
    repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
    auto clientOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    return WaitForMajorityService::get(opCtx->getServiceContext())
        .waitUntilMajorityForWrite(clientOpTime, CancellationToken::uncancelable())
        .thenRunOn(**executor)
        .then([this, &factory] {
            auto localOpCtx = factory.makeOperationContext(&cc());
            auto shardId = _externalState->myShardId(localOpCtx->getServiceContext());
            _metrics->updateDonorCtx(_donorCtx);

            BSONObjBuilder updateBuilder;
            {
                BSONObjBuilder setBuilder(updateBuilder.subobjStart("$set"));
                {
                    setBuilder.append(ReshardingCoordinatorDocument::kDonorShardsFieldName + ".$." +
                                          DonorShardEntry::kMutableStateFieldName,
                                      _donorCtx.toBSON());
                }
            }

            _externalState->updateCoordinatorDocument(
                localOpCtx.get(),
                _makeQueryForCoordinatorUpdate(shardId, _donorCtx.getState()),
                updateBuilder.done());
        });
}

void ReshardingDonorService::DonorStateMachine::insertStateDocument(
    OperationContext* opCtx, const ReshardingDonorDocument& donorDoc) {
    PersistentTaskStore<ReshardingDonorDocument> store(
        NamespaceString::kDonorReshardingOperationsNamespace);
    store.add(opCtx, donorDoc, kNoWaitWriteConcern);
}

void ReshardingDonorService::DonorStateMachine::commit() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    tassert(ErrorCodes::ReshardCollectionInProgress,
            "Attempted to commit the resharding operation in an incorrect state",
            _donorCtx.getState() >= DonorStateEnum::kBlockingWrites);

    if (!_coordinatorHasDecisionPersisted.getFuture().isReady()) {
        _coordinatorHasDecisionPersisted.emplaceValue();
    }
}

void ReshardingDonorService::DonorStateMachine::_updateDonorDocument(OperationContext* opCtx,
                                                                     const BSONObj& updateMod) {
    const auto& nss = NamespaceString::kDonorReshardingOperationsNamespace;

    writeConflictRetry(opCtx, "_updateDonorDocument", nss, [&] {
        auto coll = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest(nss,
                                         PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                         repl::ReadConcernArgs::get(opCtx),
                                         AcquisitionPrerequisites::kWrite),
            MODE_X);

        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << nss.toStringForErrorMsg() << " does not exist",
                coll.exists());

        WriteUnitOfWork wuow(opCtx);
        Helpers::update(opCtx,
                        coll,
                        BSON(ReshardingDonorDocument::kReshardingUUIDFieldName
                             << _metadata.getReshardingUUID()),
                        updateMod);
        wuow.commit();
    });
}

void ReshardingDonorService::DonorStateMachine::_updateDonorDocument(
    DonorShardContext&& newDonorCtx, const CancelableOperationContextFactory& factory) {
    auto opCtx = factory.makeOperationContext(&cc());
    _updateDonorDocument(opCtx.get(),
                         BSON("$set" << BSON(ReshardingDonorDocument::kMutableStateFieldName
                                             << newDonorCtx.toBSON())));
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _donorCtx = newDonorCtx;
}

void ReshardingDonorService::DonorStateMachine::_updateDonorDocument(
    OperationContext* opCtx, ChangeStreamsMonitorContext&& newChangeStreamsMonitorCtx) {
    _updateDonorDocument(opCtx,
                         BSON("$set" << BSON(ReshardingDonorDocument::kChangeStreamsMonitorFieldName
                                             << newChangeStreamsMonitorCtx.toBSON())));
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _changeStreamsMonitorCtx = newChangeStreamsMonitorCtx;
}

void ReshardingDonorService::DonorStateMachine::_removeDonorDocument(
    const CancellationToken& stepdownToken,
    bool aborted,
    const CancelableOperationContextFactory& factory) {
    auto opCtx = factory.makeOperationContext(&cc());

    const auto& nss = NamespaceString::kDonorReshardingOperationsNamespace;
    writeConflictRetry(opCtx.get(), "DonorStateMachine::_removeDonorDocument", nss, [&] {
        const auto coll = acquireCollection(
            opCtx.get(),
            CollectionAcquisitionRequest(nss,
                                         PlacementConcern{boost::none, ShardVersion::UNSHARDED()},
                                         repl::ReadConcernArgs::get(opCtx.get()),
                                         AcquisitionPrerequisites::kWrite),
            MODE_X);

        if (!coll.exists()) {
            return;
        }

        WriteUnitOfWork wuow(opCtx.get());

        shard_role_details::getRecoveryUnit(opCtx.get())
            ->onCommit(
                [this, stepdownToken, aborted](OperationContext*, boost::optional<Timestamp>) {
                    stdx::lock_guard<stdx::mutex> lk(_mutex);
                    _completionPromise.emplaceValue();
                });

        deleteObjects(opCtx.get(),
                      coll,
                      BSON(ReshardingDonorDocument::kReshardingUUIDFieldName
                           << _metadata.getReshardingUUID()),
                      true /* justOne */);

        wuow.commit();
    });
}

CancellationToken ReshardingDonorService::DonorStateMachine::_initAbortSource(
    const CancellationToken& stepdownToken) {
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _abortSource = CancellationSource(stepdownToken);
    }

    if (_donorCtx.getState() == DonorStateEnum::kDone && _donorCtx.getAbortReason()) {
        // A donor in state kDone with an abortReason is indication that the coordinator
        // has persisted the decision and called abort on all participants. Canceling the
        // _abortSource to avoid repeating the future chain.
        _abortSource->cancel();
    }

    if (auto future = _coordinatorHasDecisionPersisted.getFuture(); future.isReady()) {
        if (auto status = future.getNoThrow(); !status.isOK()) {
            // onReshardingFieldsChanges() missed canceling _abortSource because _initAbortSource()
            // hadn't been called yet. We used an error status stored in
            // _coordinatorHasDecisionPersisted as an indication that an abort had been received.
            // Canceling _abortSource immediately allows callers to use the returned abortToken as a
            // definitive means of checking whether the operation has been aborted.
            _abortSource->cancel();
        }
    }

    return _abortSource->token();
}

void ReshardingDonorService::DonorStateMachine::abort(bool isUserCancelled) {
    auto abortSource = [&]() -> boost::optional<CancellationSource> {
        stdx::lock_guard<stdx::mutex> lk(_mutex);

        if (_abortSource) {
            return _abortSource;
        } else {
            // run() hasn't been called, notify the operation should be aborted by setting an
            // error. Abort can be retried, so only set error if future is not ready.
            if (!_coordinatorHasDecisionPersisted.getFuture().isReady()) {
                _coordinatorHasDecisionPersisted.setError(
                    {ErrorCodes::ReshardCollectionAborted, "aborted"});
            }

            return boost::none;
        }
    }();

    if (abortSource) {
        abortSource->cancel();
    }
}

}  // namespace mongo
