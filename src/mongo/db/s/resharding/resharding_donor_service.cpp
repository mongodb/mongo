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
#include "mongo/db/global_catalog/ddl/sharding_recovery_service.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/index_builds/index_builds_coordinator.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/query/write_ops/delete.h"
#include "mongo/db/repl/change_stream_oplog_notification.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/s/resharding/coordinator_document_gen.h"
#include "mongo/db/s/resharding/resharding_data_copy_util.h"
#include "mongo/db/s/resharding/resharding_donor_recipient_common.h"
#include "mongo/db/s/resharding/resharding_future_util.h"
#include "mongo/db/s/resharding/resharding_promise_util.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/server_options.h"
#include "mongo/db/session/kill_sessions_local.h"
#include "mongo/db/session/session_killer.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/commit_collection_metadata_locally.h"
#include "mongo/db/shard_role/shard_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/topology/user_write_block/user_write_block_bypass.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/otel/traces/span/span.h"
#include "mongo/otel/traces/span/span_names.h"
#include "mongo/otel/traces/telemetry_context_serialization.h"
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
MONGO_FAIL_POINT_DEFINE(reshardingPauseDonorBeforeInitCancelState);
MONGO_FAIL_POINT_DEFINE(reshardingPauseDonorAfterInitCancelState);
MONGO_FAIL_POINT_DEFINE(reshardingPauseDonorInAbortBeforePromiseSet);

namespace {

const WriteConcernOptions kNoWaitWriteConcern{1, WriteConcernOptions::SyncMode::UNSET, Seconds(0)};

using otel::traces::SpanNames;
using resharding::ensureFulfilledPromise;

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
    getOnReleaseCriticalSectionCustomAction(bool mustClearFilteringMetadata) override {
        if (!mustClearFilteringMetadata) {
            return std::make_unique<ShardingRecoveryService::NoCustomAction>();
        } else {
            return std::make_unique<ShardingRecoveryService::FilteringMetadataClearer>();
        }
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

auto ReshardingDonorService::getThreadPoolLimits() const -> ThreadPoolLimits {
    return {.maxThreads = static_cast<size_t>(resharding::gReshardingDonorServiceMaxThreadCount)};
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
      _forwardableOpMetadata{
          _metadata.getForwardableOpMetadata().map([](const ForwardableOperationMetadata& f) {
              return f.withVersionContextPropagation_UNSAFE();
          })},
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
    }

    _fulfillPromisesOnStepup(donorDoc);

    _metrics->onStateTransition(boost::none, _donorCtx.getState());
}

CancelableOperationContext ReshardingDonorService::DonorStateMachine::_makeOperationContext(
    std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory) const {
    auto state = [this] {
        std::lock_guard<std::mutex> lk(_mutex);
        return _donorCtx.getState();
    }();
    return resharding::makeReshardingOperationContext(
        *factory, state >= DonorStateEnum::kBlockingWrites, _forwardableOpMetadata);
}

ExecutorFuture<void> ReshardingDonorService::DonorStateMachine::_runUntilBlockingWritesOrErrored(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    std::shared_ptr<otel::TelemetryContext> telemetryCtx) {
    return _retryingCancelableOpCtxFactory
        ->withAutomaticRetry([this, executor, telemetryCtx = telemetryCtx->clone()](auto factory) {
            return ExecutorFuture(**executor)
                .then([this, factory, telemetryCtx = telemetryCtx->clone()]() mutable {
                    auto span = _startSpan(
                        telemetryCtx,
                        SpanNames::
                            kReshardingDonorOnPreparingToDonateCalculateTimestampThenTransitionToDonatingInitialData);
                    _onPreparingToDonateCalculateTimestampThenTransitionToDonatingInitialData(
                        factory);
                })
                .then([this, executor, factory, telemetryCtx = telemetryCtx->clone()]() mutable {
                    auto span = _startSpan(
                        telemetryCtx,
                        SpanNames::
                            kReshardingDonorAwaitAllRecipientsDoneCloningThenTransitionToDonatingOplogEntries);
                    return _awaitAllRecipientsDoneCloningThenTransitionToDonatingOplogEntries(
                        executor, factory);
                })
                .then([this, executor, factory, telemetryCtx = telemetryCtx->clone()]() mutable {
                    auto span =
                        _startSpan(telemetryCtx,
                                   SpanNames::kReshardingDonorCreateAndStartChangeStreamsMonitor);
                    return _createAndStartChangeStreamsMonitor(executor, factory);
                })
                .then([this, executor, factory, telemetryCtx = telemetryCtx->clone()]() mutable {
                    auto span = _startSpan(
                        telemetryCtx,
                        SpanNames::
                            kReshardingDonorAwaitAllRecipientsDoneApplyingThenTransitionToPreparingToBlockWrites);
                    return _awaitAllRecipientsDoneApplyingThenTransitionToPreparingToBlockWrites(
                        executor, factory);
                })
                .then([this, factory, telemetryCtx = telemetryCtx->clone()]() mutable {
                    auto span = _startSpan(
                        telemetryCtx,
                        SpanNames::
                            kReshardingDonorWriteTransactionOplogEntryThenTransitionToBlockingWrites);
                    _writeTransactionOplogEntryThenTransitionToBlockingWrites(factory);
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
        .runOn(**executor, _cancelState->getAbortOrStepdownToken())
        .onError([this, executor](Status status) {
            if (_cancelState->isAbortedOrSteppingDown()) {
                return ExecutorFuture<void>(**executor, status);
            }

            LOGV2(4956400,
                  "Resharding operation donor state machine failed",
                  logAttrs(_metadata.getSourceNss()),
                  "reshardingUUID"_attr = _metadata.getReshardingUUID(),
                  "error"_attr = status);

            {
                std::lock_guard<std::mutex> lk(_mutex);
                ensureFulfilledPromise(lk, _changeStreamsMonitorStarted, status);
                ensureFulfilledPromise(lk, _changeStreamsMonitorCompleted, status);
            }

            return _retryingCancelableOpCtxFactory
                ->withAutomaticRetry([this, status](auto factory) {
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
                .runOn(**executor, _cancelState->getAbortOrStepdownToken());
        })
        .then([this] {
            // The donor is done with all local transitions until the coordinator makes its
            // decision.
            std::lock_guard<std::mutex> lk(_mutex);
            tassert(12559801,
                    "Donor state must be at least kError upon completion of "
                    "_runUntilBlockingWritesOrErrored",
                    _donorCtx.getState() >= DonorStateEnum::kError);
        });
}

ExecutorFuture<void> ReshardingDonorService::DonorStateMachine::_notifyCoordinatorAndAwaitDecision(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_donorCtx.getState() == DonorStateEnum::kDone) {
        return ExecutorFuture(**executor);
    }

    return _retryingCancelableOpCtxFactory
        ->withAutomaticRetry([this, executor](auto factory) {
            auto opCtx = _makeOperationContext(factory);
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
        .runOn(**executor, _cancelState->getAbortOrStepdownToken())
        .then([this] {
            std::lock_guard<std::mutex> lk(_mutex);
            return future_util::withCancellation(_coordinatorHasDecisionPersisted.getFuture(),
                                                 _cancelState->getAbortOrStepdownToken());
        });
}

ExecutorFuture<void> ReshardingDonorService::DonorStateMachine::_finishReshardingOperation(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    return _retryingCancelableOpCtxFactory
        ->withAutomaticRetry([this, executor](auto factory) {
            if (!_cancelState->isAbortedOrSteppingDown()) {
                // If a failover occured after the donor transitioned to done locally, but
                // before it notified the coordinator, it will already be in state done here.
                // Otherwise, it must be in blocking-writes before transitioning to done.
                invariant(_donorCtx.getState() == DonorStateEnum::kBlockingWrites ||
                          _donorCtx.getState() == DonorStateEnum::kDone);

                _dropOriginalCollectionThenTransitionToDone(factory);
            } else if (_donorCtx.getState() != DonorStateEnum::kDone) {
                {
                    // Unblock the RecoverRefreshThread as quickly as possible when aborting.
                    std::lock_guard<std::mutex> lk(_mutex);
                    Status abortedStatus{ErrorCodes::ReshardCollectionAborted, "aborted"};
                    _promises.setError(lk, abortedStatus);
                }

                // If aborted, the donor must be allowed to transition to done from any
                // state.
                _transitionToDone(factory);
            }

            {
                auto opCtx = _makeOperationContext(factory);

                const bool mustClearMetadata = !feature_flags::gAuthoritativeShardsDDL.isEnabled(
                    resharding::getVersionContextOrDefault(_forwardableOpMetadata),
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot());

                // Clear filtering metadata for the temp resharding namespace;
                // We force a refresh to make sure that the placement information is updated
                // in cache after abort decision before the donor state document is deleted.
                if (mustClearMetadata) {
                    auto scopedCsr = CollectionShardingRuntime::acquireExclusive(
                        opCtx.get(), _metadata.getTempReshardingNss());
                    scopedCsr->clearFilteringMetadata_nonAuthoritative(opCtx.get());
                }

                const auto onReleaseCriticalSectionAction =
                    _externalState->getOnReleaseCriticalSectionCustomAction(mustClearMetadata);

                ShardingRecoveryService::get(opCtx.get())
                    ->releaseRecoverableCriticalSection(
                        opCtx.get(),
                        _metadata.getSourceNss(),
                        _critSecReason,
                        ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
                        *onReleaseCriticalSectionAction,
                        false /*throwIfReasonDiffers*/);

                _metrics->setEndFor(ReshardingMetrics::TimedPhase::kCriticalSection,
                                    resharding::getCurrentTime());

                if (mustClearMetadata) {
                    // We force a refresh to make sure that the placement information is updated
                    // in cache after abort decision before the donor state document is deleted.
                    std::initializer_list<NamespaceString> namespacesToRefresh{
                        _metadata.getSourceNss(), _metadata.getTempReshardingNss()};
                    for (const auto& nss : namespacesToRefresh) {
                        _externalState->refreshCollectionPlacementInfo(opCtx.get(), nss);
                        _externalState->waitForCollectionFlush(opCtx.get(), nss);
                    }
                }
            }

            auto opCtx = _makeOperationContext(factory);
            return _updateCoordinator(opCtx.get(), executor, factory).then([this, factory] {
                {
                    auto opCtx = _makeOperationContext(factory);
                    removeDonorDocFailpoint.pauseWhileSet(opCtx.get());
                }
                _removeDonorDocument(factory);
            });
        })
        .onTransientError([](const Status& status) {
            LOGV2(5633600,
                  "Transient error while finishing resharding operation",
                  "error"_attr = status);
        })
        .onUnrecoverableError([](const Status& status) {})
        .runOn(**executor, _cancelState->getStepdownToken());
}

ExecutorFuture<void> ReshardingDonorService::DonorStateMachine::_runMandatoryCleanup(
    Status status) {
    return ExecutorFuture(_donorService->getInstanceCleanupExecutor())
        .then([this, self = shared_from_this()] {
            if (_changeStreamsMonitor) {
                LOGV2(9858410, "Waiting for the change streams monitor to clean up");
            }
            return _changeStreamsMonitorQuiesced.thenRunOn(
                _donorService->getInstanceCleanupExecutor());
        })
        .onCompletion([this, anchor = shared_from_this(), status](
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
                auto statusForPromise = _cancelState->isSteppingDown()
                    ? Status{ErrorCodes::InterruptedDueToReplStateChange,
                             "Resharding operation donor state machine interrupted due to replica "
                             "set stepdown"}
                    : status;

                std::lock_guard<std::mutex> lk(_mutex);
                _promises.setError(lk, statusForPromise);
                ensureFulfilledPromise(lk, _coordinatorHasDecisionPersisted, statusForPromise);
                ensureFulfilledPromise(lk, _changeStreamMonitorStartTimeSelected, statusForPromise);
                ensureFulfilledPromise(lk, _changeStreamsMonitorStarted, statusForPromise);
                ensureFulfilledPromise(lk, _changeStreamsMonitorCompleted, statusForPromise);
                ensureFulfilledPromise(lk, _completionPromise, statusForPromise);
            }

            return status;
        });
}

SemiFuture<void> ReshardingDonorService::DonorStateMachine::run(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& stepdownToken) noexcept {
    auto telemetryCtx = _metadata.getTelemetryContext()
        ? otel::traces::TelemetryContextSerializer::fromBSON(*_metadata.getTelemetryContext())
        : otel::traces::Span::createTelemetryContext();
    auto span = _startSpan(telemetryCtx, otel::traces::SpanNames::kReshardingDonorStateMachineRun);

    _initCancelState(stepdownToken);
    _markKilledExecutor->startup();
    _retryingCancelableOpCtxFactory.emplace(
        _cancelState->getAbortOrStepdownToken(),
        _markKilledExecutor,
        resharding::kRetryabilityPredicateIncludeWriteConcernTimeout);

    return ExecutorFuture(**executor)
        .then([this, executor, telemetryCtx = telemetryCtx->clone()]() mutable {
            auto span = _startSpan(telemetryCtx,
                                   SpanNames::kReshardingDonorRunUntilBlockingWritesOrErrored);
            return _runUntilBlockingWritesOrErrored(executor, telemetryCtx);
        })
        .then([this, executor, telemetryCtx = telemetryCtx->clone()]() mutable {
            auto span = _startSpan(telemetryCtx,
                                   SpanNames::kReshardingDonorNotifyCoordinatorAndAwaitDecision);
            return _notifyCoordinatorAndAwaitDecision(executor);
        })
        .onCompletion([this, executor](Status status) {
            _retryingCancelableOpCtxFactory.emplace(
                _cancelState->getStepdownToken(),
                _markKilledExecutor,
                resharding::kRetryabilityPredicateIncludeWriteConcernTimeout);
            if (_cancelState->isSteppingDown()) {
                // Propagate any errors from the donor stepping down.
                return ExecutorFuture<void>(**executor, status);
            }

            if (!status.isOK() && !_cancelState->isAbortedOrSteppingDown()) {
                // Propagate any errors from the donor failing to notify the coordinator.
                return ExecutorFuture<void>(**executor, status);
            }

            return ExecutorFuture(**executor);
        })
        .then([this, executor]() { return _finishReshardingOperation(executor); })
        .onError([this](Status status) {
            if (_cancelState->isSteppingDown()) {
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
        .onCompletion([this, anchor = shared_from_this()](Status status) {
            // On stepdown or shutdown, the _scopedExecutor may have already been shut down.
            // Everything in this function runs on the instance's cleanup executor, and will
            // execute regardless of any work on _scopedExecutor ever running.
            return _runMandatoryCleanup(status);
        })
        .semi();
}

void ReshardingDonorService::DonorStateMachine::interrupt(Status status) {
    // TODO: SERVER-125961
    // Guard against PrimaryOnlyService invoking interrupt() before run() (e.g. interrupted
    // mid-stepup), in which case _runMandatoryCleanup never runs to fulfill _completionPromise.
    std::lock_guard<std::mutex> lk(_mutex);
    ensureFulfilledPromise(lk, _completionPromise, status);
}

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

    std::lock_guard<std::mutex> lk(_mutex);
    _onCoordinatorStateAdvanced(lk, reshardingFields.getState());
}

void ReshardingDonorService::DonorStateMachine::_fulfillPromisesOnStepup(
    const ReshardingDonorDocument& donorDoc) {
    std::lock_guard<std::mutex> lk(_mutex);

    _promises.recover(lk, donorDoc);

    auto donorState = donorDoc.getMutableState();
    if (donorState.getState() == DonorStateEnum::kDone) {
        if (donorState.getAbortReason()) {
            ensureFulfilledPromise(lk,
                                   _coordinatorHasDecisionPersisted,
                                   resharding::getStatusFromAbortReason(donorState));
        } else {
            ensureFulfilledPromise(lk, _coordinatorHasDecisionPersisted);
        }
    }

    if (auto monitor = donorDoc.getChangeStreamsMonitor()) {
        ensureFulfilledPromise(
            lk, _changeStreamMonitorStartTimeSelected, monitor->getStartAtOperationTime());
        if (monitor->getCompleted()) {
            ensureFulfilledPromise(lk, _changeStreamsMonitorStarted);
            ensureFulfilledPromise(
                lk, _changeStreamsMonitorCompleted, monitor->getDocumentsDelta());
        }
    }
}

void ReshardingDonorService::DonorStateMachine::onCoordinatorStateAdvanced(
    CoordinatorStateEnum newState) {
    std::lock_guard<std::mutex> lk(_mutex);
    _onCoordinatorStateAdvanced(lk, newState);
}

void ReshardingDonorService::DonorStateMachine::_onCoordinatorStateAdvanced(
    WithLock lk, CoordinatorStateEnum newState) {
    _promises.onCoordinatorStateAdvanced(lk, newState);

    if (newState >= CoordinatorStateEnum::kCommitting) {
        ensureFulfilledPromise(lk, _coordinatorHasDecisionPersisted);
    }
}

void ReshardingDonorService::DonorStateMachine::onWriteDuringCriticalSection() {
    _metrics->onWriteDuringCriticalSection();
}

void ReshardingDonorService::DonorStateMachine::onReadDuringCriticalSection() {
    _metrics->onReadDuringCriticalSection();
}

SharedSemiFuture<void> ReshardingDonorService::DonorStateMachine::awaitCriticalSectionAcquired() {
    return _promises.getCritSecWasAcquiredFuture();
}

SharedSemiFuture<void> ReshardingDonorService::DonorStateMachine::awaitCriticalSectionPromoted() {
    return _promises.getCritSecWasPromotedFuture();
}

void ReshardingDonorService::DonorStateMachine::
    _onPreparingToDonateCalculateTimestampThenTransitionToDonatingInitialData(
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory) {
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
        auto opCtx = _makeOperationContext(factory);
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
        auto opCtx = _makeOperationContext(factory);
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

    Timestamp minFetchTimestamp = [this, factory] {
        auto opCtx = _makeOperationContext(factory);
        return generateMinFetchTimestamp(opCtx.get(), _metadata.getSourceNss());
    }();

    {
        auto opCtx = _makeOperationContext(factory);
        notifyChangeStreamsOnReshardCollectionBegin(opCtx.get(),
                                                    _metadata.getSourceNss(),
                                                    _metadata.getSourceUUID(),
                                                    _metadata.getReshardingUUID());
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
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory) {
    if (_donorCtx.getState() > DonorStateEnum::kDonatingInitialData) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    auto opCtx = _makeOperationContext(factory);
    return _updateCoordinator(opCtx.get(), executor, factory)
        .then([this, executor, factory] {
            return _createAndStartChangeStreamsMonitor(executor, factory);
        })
        .then([this] {
            return future_util::withCancellation(_promises.getAllRecipientsDoneCloningFuture(),
                                                 _cancelState->getAbortOrStepdownToken());
        })
        .thenRunOn(**executor)
        .then(
            [this, factory]() { _transitionState(DonorStateEnum::kDonatingOplogEntries, factory); })
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
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory) {
    if (_donorCtx.getState() > DonorStateEnum::kDonatingOplogEntries) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    SharedSemiFuture<void> allRecipientsDoneApplyingFuture;
    {
        std::lock_guard<std::mutex> lk(_mutex);
        allRecipientsDoneApplyingFuture = _promises.getAllRecipientsDoneApplyingFuture();
    }

    return future_util::withCancellation(std::move(allRecipientsDoneApplyingFuture),
                                         _cancelState->getAbortOrStepdownToken())
        .thenRunOn(**executor)
        .then([this, factory] {
            _transitionState(DonorStateEnum::kPreparingToBlockWrites, factory);
        });
}

void ReshardingDonorService::DonorStateMachine::
    _writeTransactionOplogEntryThenTransitionToBlockingWrites(
        std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory) {
    if (_donorCtx.getState() > DonorStateEnum::kPreparingToBlockWrites) {
        std::lock_guard<std::mutex> lk(_mutex);
        _promises.emplaceCritSecWasAcquired(lk);
        return;
    }

    {
        auto opCtx = _makeOperationContext(factory);
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
        std::lock_guard<std::mutex> lk(_mutex);
        _promises.emplaceCritSecWasAcquired(lk);
    }

    {
        auto opCtx = _makeOperationContext(factory);

        try {
            Timer latency;

            notifyChangeStreamsOnReshardCollectionBlockingWrites(opCtx.get(),
                                                                 _metadata.getSourceNss(),
                                                                 _metadata.getSourceUUID(),
                                                                 _metadata.getReshardingUUID(),
                                                                 _recipientShardIds);

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
    std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory) {
    if (_donorCtx.getState() > DonorStateEnum::kBlockingWrites) {
        std::lock_guard<std::mutex> lk(_mutex);
        _promises.emplaceCritSecWasPromoted(lk);
        return;
    }
    {
        auto opCtx = _makeOperationContext(factory);
        ShardingRecoveryService::get(opCtx.get())
            ->promoteRecoverableCriticalSectionToBlockAlsoReads(
                opCtx.get(),
                _metadata.getSourceNss(),
                _critSecReason,
                ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter());
        reshardingPauseDonorAfterBlockingReads.pauseWhileSet(opCtx.get());
    }

    {
        std::lock_guard<std::mutex> lk(_mutex);
        _promises.emplaceCritSecWasPromoted(lk);
    }

    if (_isAlsoRecipient) {
        auto opCtx = _makeOperationContext(factory);
        // Allow bypassing user write blocking. The check has already been performed on the
        // db-primary shard's ReshardCollectionCoordinator.
        WriteBlockBypass::get(opCtx.get()).set(true);
        resharding::data_copy::ensureTemporaryReshardingCollectionRenamed(opCtx.get(), _metadata);

        if (feature_flags::gAuthoritativeShardsDDL.isEnabled(
                resharding::getVersionContextOrDefault(_metadata.getForwardableOpMetadata()),
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
            // TODO SERVER-127215: Mark this as not upgrading once rename recovers from disk once it
            // finishes.
            //
            // Force the rename to act as if it is upgrading in order to force a recovery at the
            // end. Ideally this should be false since:
            // 1. Resharding cannot commit during an FCV upgrade
            // 2. The shard already contains the metadata for both "from" and "to" collections and
            //    thus can assume it's living in a fully authoritative world.
            bool isUpgrading = true;

            shard_catalog_commit_for_resharding::commitRenameOfTemporaryCollection(
                opCtx.get(),
                _metadata.getTempReshardingNss(),
                _metadata.getReshardingUUID(),
                _metadata.getSourceNss(),
                _metadata.getSourceUUID(),
                isUpgrading,
                _metadata.getPrimaryShardId() == ShardingState::get(opCtx.get())->shardId());
        }
    } else {
        auto opCtx = _makeOperationContext(factory);
        // Allow bypassing user write blocking. The check has already been performed on the
        // db-primary shard's ReshardCollectionCoordinator.
        WriteBlockBypass::get(opCtx.get()).set(true);

        resharding::data_copy::ensureCollectionDropped(
            opCtx.get(), _metadata.getSourceNss(), _metadata.getSourceUUID());

        if (feature_flags::gAuthoritativeShardsDDL.isEnabled(
                resharding::getVersionContextOrDefault(_forwardableOpMetadata),
                serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
            shard_catalog_commit_for_resharding::commitDropCollection(
                opCtx.get(), _metadata.getSourceNss(), _metadata.getSourceUUID());
            // Make sure to also clear out the state of the temporary resharding as it now no longer
            // exists since it got renamed to the final namespace.
            shard_catalog_commit_for_resharding::commitDropCollection(
                opCtx.get(), _metadata.getTempReshardingNss(), _metadata.getReshardingUUID());
        }
    }

    _transitionToDone(factory);
}

SharedSemiFuture<void>
ReshardingDonorService::DonorStateMachine::createAndStartChangeStreamsMonitor(
    const Timestamp& cloneTimestamp) {
    if (!_metadata.getPerformVerification()) {
        return Status{ErrorCodes::IllegalOperation,
                      "Cannot start the change streams monitor when verification is not enabled"};
    }

    {
        std::lock_guard<std::mutex> lk(_mutex);
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
    std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory) {
    if (!_metadata.getPerformVerification() || _changeStreamsMonitorStarted.getFuture().isReady()) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    SharedSemiFuture<Timestamp> changeStreamMonitorStartTimeSelectedFuture;
    {
        std::lock_guard<std::mutex> lk(_mutex);
        changeStreamMonitorStartTimeSelectedFuture =
            _changeStreamMonitorStartTimeSelected.getFuture();
    }

    return future_util::withCancellation(std::move(changeStreamMonitorStartTimeSelectedFuture),
                                         _cancelState->getAbortOrStepdownToken())
        .thenRunOn(**executor)
        .then([this, executor, factory](const Timestamp& startAtOperationTime) {
            if (!_changeStreamsMonitorCtx) {
                LOGV2(9858400,
                      "Initializing the change streams monitor",
                      "reshardingUUID"_attr = _metadata.getReshardingUUID(),
                      "startAtOperationTime"_attr = startAtOperationTime);
                auto opCtx = _makeOperationContext(factory);

                ChangeStreamsMonitorContext changeStreamsMonitorCtx(startAtOperationTime,
                                                                    0 /* documentsDelta */);
                _updateDonorDocument(opCtx.get(), std::move(changeStreamsMonitorCtx));

                // Wait for the write above to be majority committed to make sure it cannot be
                // rolled back after this donor acknowledges to the coordinator that it has started
                // the change streams monitor.
                auto clientOpTime =
                    repl::ReplClientInfo::forClient(opCtx.get()->getClient()).getLastOp();
                return WaitForMajorityService::get(opCtx.get()->getServiceContext())
                    .waitUntilMajorityForWrite(clientOpTime,
                                               _cancelState->getAbortOrStepdownToken())
                    .thenRunOn(**executor);
            }

            return ExecutorFuture<void>(**executor, Status::OK());
        })
        .then([this, executor, factory] {
            _createChangeStreamsMonitor(executor, factory);
            {
                std::lock_guard<std::mutex> lk(_mutex);
                ensureFulfilledPromise(lk, _changeStreamsMonitorStarted);
            }

            // Kick off the change-streams monitor await as a fire-and-forget task. It runs in
            // the background through the remainder of the resharding operation. The monitor's
            // final change event arrives after blocking writes complete, so the chain spends
            // most of its life waiting on a pending future. The coordinator's
            // _shardsvrReshardingDonorFetchFinalCollectionStats command reads
            // _changeStreamsMonitorCompleted, which this background task fulfills.
            _awaitChangeStreamsMonitorCompleted(executor, factory)
                .getAsync([anchor = shared_from_this()](Status) {});
        });
}

void ReshardingDonorService::DonorStateMachine::_createChangeStreamsMonitor(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory) {
    auto batchCallback = [this, factory = factory, anchor = shared_from_this()](const auto& batch) {
        boost::optional<long long> lagSecs;
        if (auto clusterTimeSecs = batch.getResumeTokenClusterTimeSecs()) {
            auto replCoord = repl::ReplicationCoordinator::get(_serviceContext);
            auto lastCommittedSecs = replCoord->getLastCommittedOpTime().getTimestamp().getSecs();
            auto lagMs =
                Milliseconds(std::max<int64_t>(0,
                                               static_cast<int64_t>(lastCommittedSecs) -
                                                   static_cast<int64_t>(*clusterTimeSecs)) *
                             1000);
            _metrics->setChangeStreamMonitorLag(lagMs);
            lagSecs = durationCount<Seconds>(lagMs);
        }

        LOGV2(9858404,
              "Persisting change streams monitor's progress",
              "reshardingUUID"_attr = _metadata.getReshardingUUID(),
              "documentsDelta"_attr = batch.getDocumentsDelta(),
              "completed"_attr = batch.containsFinalEvent(),
              "changeStreamMonitorLagSecs"_attr = lagSecs);

        auto changeStreamsMonitorCtx = _changeStreamsMonitorCtx.get();
        changeStreamsMonitorCtx.setResumeToken(batch.getResumeToken().getOwned());
        changeStreamsMonitorCtx.setDocumentsDelta(changeStreamsMonitorCtx.getDocumentsDelta() +
                                                  batch.getDocumentsDelta());
        changeStreamsMonitorCtx.setCompleted(batch.containsFinalEvent());

        reshardingDonorFailsUpdatingChangeStreamsMonitorProgress.execute([&](const BSONObj& data) {
            const auto& errorCode = data.getIntField("errorCode");
            uasserted(ErrorCodes::Error(errorCode),
                      "Simulating an error in donor's changeStreamsMonitor callback "
                      "via failpoint");
        });

        auto opCtx = _makeOperationContext(factory);
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
                              _cancelState->getAbortOrStepdownToken(),
                              factory)
            .share();
    _metrics->setStartFor(ReshardingMetrics::TimedPhase::kChangeStreamMonitor,
                          resharding::getCurrentTime());
}

ExecutorFuture<void> ReshardingDonorService::DonorStateMachine::_awaitChangeStreamsMonitorCompleted(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory) {
    if (!_metadata.getPerformVerification() ||
        _changeStreamsMonitorCompleted.getFuture().isReady()) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    invariant(_changeStreamsMonitor);
    return future_util::withCancellation(_changeStreamsMonitor->awaitFinalChangeEvent(),
                                         _cancelState->getAbortOrStepdownToken())
        .thenRunOn(**executor)
        .onCompletion([this, executor, factory](Status status) {
            std::lock_guard<std::mutex> lk(_mutex);
            if (status.isOK()) {
                _metrics->setEndFor(ReshardingMetrics::TimedPhase::kChangeStreamMonitor,
                                    resharding::getCurrentTime());

                LOGV2(9858402,
                      "The change streams monitor completed",
                      "reshardingUUID"_attr = _metadata.getReshardingUUID(),
                      "status"_attr = status,
                      "changeStreamMonitorTotalTimeSecs"_attr = _metrics->getElapsed<Seconds>(
                          ReshardingMetrics::TimedPhase::kChangeStreamMonitor,
                          _serviceContext->getFastClockSource()),
                      "blockingWritesToMonitorCompletionSecs"_attr =
                          _metrics->getCrossPhaseElapsed<Seconds>(
                              ReshardingMetrics::TimedPhase::kCriticalSection,
                              ReshardingMetrics::TimedPhase::kChangeStreamMonitor));

                ensureFulfilledPromise(lk,
                                       _changeStreamsMonitorCompleted,
                                       _changeStreamsMonitorCtx->getDocumentsDelta());
                return ExecutorFuture<void>(**executor, Status::OK());
            } else {
                if (_cancelState->isAbortedOrSteppingDown() ||
                    status.isA<ErrorCategory::NotPrimaryError>()) {
                    ensureFulfilledPromise(lk, _changeStreamsMonitorCompleted, status);
                    return ExecutorFuture<void>(**executor, Status::OK());
                }

                if (resharding::isRetryableChangeStreamsMonitorError(status)) {
                    LOGV2_WARNING(10903203,
                                  "Change streams monitor failed with retryable error, will "
                                  "recreate",
                                  "reshardingUUID"_attr = _metadata.getReshardingUUID(),
                                  "error"_attr = status);
                    return _changeStreamsMonitor->awaitCleanup()
                        .thenRunOn(**executor)
                        .then([this, executor, factory]() {
                            _createChangeStreamsMonitor(executor, factory);
                            return _awaitChangeStreamsMonitorCompleted(executor, factory);
                        });
                } else {
                    if (_donorCtx.getState() >= DonorStateEnum::kBlockingWrites) {
                        LOGV2_FATAL(
                            10903204,
                            "Change streams monitor failed with unrecoverable error past the "
                            "point donor was prepared to complete the resharding operation",
                            "error"_attr = status);
                    }
                    LOGV2_WARNING(10903205,
                                  "Change streams monitor failed with unrecoverable error",
                                  "reshardingUUID"_attr = _metadata.getReshardingUUID(),
                                  "error"_attr = status);
                    ensureFulfilledPromise(lk, _changeStreamsMonitorCompleted, status);
                    return ExecutorFuture<void>(**executor, status);
                }
            }
        });
}

void ReshardingDonorService::DonorStateMachine::_transitionState(
    DonorStateEnum newState,
    std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory) {
    invariant(newState != DonorStateEnum::kDonatingInitialData &&
              newState != DonorStateEnum::kError && newState != DonorStateEnum::kDone);

    auto newDonorCtx = _donorCtx;
    newDonorCtx.setState(newState);
    _transitionState(std::move(newDonorCtx), factory);
}

void ReshardingDonorService::DonorStateMachine::_transitionState(
    DonorShardContext&& newDonorCtx,
    std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory) {
    // For logging purposes.
    auto oldState = _donorCtx.getState();
    auto newState = newDonorCtx.getState();

    // The donor state machine enters the kError state on unrecoverable errors and so we don't
    // expect it to ever transition from kError except to kDone.
    invariant(oldState != DonorStateEnum::kError || newState == DonorStateEnum::kDone);

    _updateDonorDocument(std::move(newDonorCtx), factory);

    _metrics->onStateTransition(oldState, newState);

    {
        std::lock_guard<std::mutex> lk(_mutex);
        _promises.onDonorStateAdvanced(lk, newState);
    }

    LOGV2_INFO(5279505,
               "Transitioned resharding donor state",
               "newState"_attr = idl::serialize(newState),
               "oldState"_attr = idl::serialize(oldState),
               logAttrs(_metadata.getSourceNss()),
               "collectionUUID"_attr = _metadata.getSourceUUID(),
               "reshardingUUID"_attr = _metadata.getReshardingUUID());
}

void ReshardingDonorService::DonorStateMachine::_transitionToDonatingInitialData(
    Timestamp minFetchTimestamp,
    int64_t bytesToClone,
    int64_t documentsToClone,
    int64_t indexCount,
    std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory) {
    auto newDonorCtx = _donorCtx;
    newDonorCtx.setState(DonorStateEnum::kDonatingInitialData);
    newDonorCtx.setMinFetchTimestamp(minFetchTimestamp);
    newDonorCtx.setBytesToClone(bytesToClone);
    newDonorCtx.setDocumentsToClone(documentsToClone);
    newDonorCtx.setIndexCount(indexCount);
    _transitionState(std::move(newDonorCtx), factory);
}

void ReshardingDonorService::DonorStateMachine::_transitionToError(
    Status abortReason, std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory) {
    auto newDonorCtx = _donorCtx;
    newDonorCtx.setState(DonorStateEnum::kError);
    resharding::emplaceTruncatedAbortReasonIfExists(newDonorCtx, abortReason);
    _transitionState(std::move(newDonorCtx), factory);
}

void ReshardingDonorService::DonorStateMachine::_transitionToDone(
    std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory) {
    auto newDonorCtx = _donorCtx;
    newDonorCtx.setState(DonorStateEnum::kDone);
    if (_cancelState->isAbortedOrSteppingDown() && !_cancelState->isSteppingDown()) {
        resharding::emplaceTruncatedAbortReasonIfExists(newDonorCtx,
                                                        resharding::kCoordinatorAbortedError);
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

                BSONObjBuilder mutableStateBuilder(elemMatchBuilder.subobjStart(
                    std::string{DonorShardEntry::kMutableStateFieldName} + "." +
                    std::string{DonorShardContext::kStateFieldName}));
                {
                    BSONArrayBuilder inBuilder(mutableStateBuilder.subarrayStart("$in"));
                    for (const auto& state : it->second) {
                        inBuilder.append(idl::serialize(state));
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
    std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory) {
    repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
    auto clientOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    return WaitForMajorityService::get(opCtx->getServiceContext())
        .waitUntilMajorityForWrite(clientOpTime, CancellationToken::uncancelable())
        .thenRunOn(**executor)
        .then([this, factory] {
            auto localOpCtx = _makeOperationContext(factory);
            auto shardId = _externalState->myShardId(localOpCtx->getServiceContext());
            _metrics->updateDonorCtx(_donorCtx);

            BSONObjBuilder updateBuilder;
            {
                BSONObjBuilder setBuilder(updateBuilder.subobjStart("$set"));
                {
                    setBuilder.append(
                        std::string{ReshardingCoordinatorDocument::kDonorShardsFieldName} + ".$." +
                            std::string{DonorShardEntry::kMutableStateFieldName},
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
    std::lock_guard<std::mutex> lk(_mutex);
    tassert(
        ErrorCodes::ReshardCollectionInProgress,
        fmt::format("Attempted to commit the resharding operation in an incorrect donor state: {}",
                    idl::serialize(_donorCtx.getState())),
        _donorCtx.getState() >= DonorStateEnum::kBlockingWrites);

    ensureFulfilledPromise(lk, _coordinatorHasDecisionPersisted);
}

void ReshardingDonorService::DonorStateMachine::_updateDonorDocument(OperationContext* opCtx,
                                                                     const BSONObj& updateMod) {
    const auto& nss = NamespaceString::kDonorReshardingOperationsNamespace;

    writeConflictRetry(opCtx, "_updateDonorDocument", nss, [&] {
        auto coll = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest(nss,
                                         PlacementConcern{boost::none, ShardVersion::UNTRACKED()},
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
    DonorShardContext&& newDonorCtx,
    std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory) {
    auto opCtx = _makeOperationContext(factory);
    _updateDonorDocument(opCtx.get(),
                         BSON("$set" << BSON(ReshardingDonorDocument::kMutableStateFieldName
                                             << newDonorCtx.toBSON())));
    std::lock_guard<std::mutex> lk(_mutex);
    _donorCtx = newDonorCtx;
}

void ReshardingDonorService::DonorStateMachine::_updateDonorDocument(
    OperationContext* opCtx, ChangeStreamsMonitorContext&& newChangeStreamsMonitorCtx) {
    _updateDonorDocument(opCtx,
                         BSON("$set" << BSON(ReshardingDonorDocument::kChangeStreamsMonitorFieldName
                                             << newChangeStreamsMonitorCtx.toBSON())));
    std::lock_guard<std::mutex> lk(_mutex);
    _changeStreamsMonitorCtx = newChangeStreamsMonitorCtx;
}

void ReshardingDonorService::DonorStateMachine::_removeDonorDocument(
    std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory) {
    auto opCtx = _makeOperationContext(factory);

    const auto& nss = NamespaceString::kDonorReshardingOperationsNamespace;
    writeConflictRetry(opCtx.get(), "DonorStateMachine::_removeDonorDocument", nss, [&] {
        const auto coll = acquireCollection(
            opCtx.get(),
            CollectionAcquisitionRequest(nss,
                                         PlacementConcern{boost::none, ShardVersion::UNTRACKED()},
                                         repl::ReadConcernArgs::get(opCtx.get()),
                                         AcquisitionPrerequisites::kWrite),
            MODE_X);

        if (!coll.exists()) {
            return;
        }

        WriteUnitOfWork wuow(opCtx.get());

        shard_role_details::getRecoveryUnit(opCtx.get())
            ->onCommit([this](OperationContext*, boost::optional<Timestamp>) {
                std::lock_guard<std::mutex> lk(_mutex);
                ensureFulfilledPromise(lk, _completionPromise);
            });

        deleteObjects(opCtx.get(),
                      coll,
                      BSON(ReshardingDonorDocument::kReshardingUUIDFieldName
                           << _metadata.getReshardingUUID()),
                      true /* justOne */);

        wuow.commit();
    });
}

void ReshardingDonorService::DonorStateMachine::_initCancelState(
    const CancellationToken& stepdownToken) {
    reshardingPauseDonorBeforeInitCancelState.pauseWhileSet();
    {
        std::lock_guard<std::mutex> lk(_mutex);
        _cancelState = std::make_unique<primary_only_service_helpers::CancelState>(stepdownToken);
    }

    if (_donorCtx.getState() == DonorStateEnum::kDone && _donorCtx.getAbortReason()) {
        // A donor in state kDone with an abortReason is indication that the coordinator
        // has persisted the decision and called abort on all participants. Abort the
        // _cancelState to avoid repeating the future chain.
        _cancelState->abort();
    }

    if (auto future = _coordinatorHasDecisionPersisted.getFuture(); future.isReady()) {
        if (auto status = future.getNoThrow(); !status.isOK()) {
            // An abort was signaled (via _coordinatorHasDecisionPersisted error)
            // before _cancelState was initialized. Now that _cancelState exists, abort it to
            // ensure the abortToken reflects the true state and any future chains are canceled.
            _cancelState->abort();
        }
    }
    reshardingPauseDonorAfterInitCancelState.pauseWhileSet();
}

otel::traces::Span ReshardingDonorService::DonorStateMachine::_startSpan(
    std::shared_ptr<otel::TelemetryContext> telemetryCtx, otel::traces::SpanName spanName) {
    auto span = otel::traces::Span::start(telemetryCtx, spanName);
    TRACING_SPAN_ATTR(span, "reshardingUUID", _metadata.getReshardingUUID().toString());
    return span;
}

void ReshardingDonorService::DonorStateMachine::abort(bool isUserCancelled) {
    auto cancelStateInitialized = [&] {
        std::lock_guard<std::mutex> lk(_mutex);
        return _cancelState != nullptr;
    }();

    if (cancelStateInitialized) {
        _cancelState->abort();
    }

    reshardingPauseDonorInAbortBeforePromiseSet.pauseWhileSet();

    bool cancelAfterSettingPromise = false;
    {
        std::lock_guard<std::mutex> lk(_mutex);
        ensureFulfilledPromise(
            lk, _coordinatorHasDecisionPersisted, resharding::kCoordinatorAbortedError);

        // If _cancelState is initialized between our initial read and setting the
        // promise, we may miss aborting it. Re-check _cancelState here.
        if (!cancelStateInitialized && _cancelState != nullptr) {
            cancelAfterSettingPromise = true;
        }
    }
    if (cancelAfterSettingPromise) {
        _cancelState->abort();
    }
}

}  // namespace mongo
