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

#include "mongo/db/s/resharding/resharding_coordinator_service.h"

#include "mongo/s/resharding/common_types_gen.h"
#include <absl/container/node_hash_map.h>
#include <boost/cstdint.hpp>
#include <boost/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <cstdint>
#include <fmt/format.h>
#include <mutex>
#include <set>
#include <string>
#include <tuple>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/checked_cast.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bson_field.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/notify_sharding_event_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/query/collation/collation_spec.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/balancer/balance_stats.h"
#include "mongo/db/s/balancer/balancer_policy.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/resharding/recipient_document_gen.h"
#include "mongo/db/s/resharding/resharding_coordinator_commit_monitor.h"
#include "mongo/db/s/resharding/resharding_coordinator_service_util.h"
#include "mongo/db/s/resharding/resharding_future_util.h"
#include "mongo/db/s/resharding/resharding_metrics_helpers.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/db/server_options.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/vector_clock.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/analyze_shard_key_documents_gen.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection_gen.h"
#include "mongo/s/catalog/type_index_catalog_gen.h"
#include "mongo/s/catalog/type_namespace_placement_gen.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/abort_reshard_collection_gen.h"
#include "mongo/s/request_types/commit_reshard_collection_gen.h"
#include "mongo/s/request_types/drop_collection_if_uuid_not_matching_gen.h"
#include "mongo/s/request_types/flush_resharding_state_change_gen.h"
#include "mongo/s/request_types/flush_routing_table_cache_updates_gen.h"
#include "mongo/s/resharding/resharding_coordinator_service_conflicting_op_in_progress_info.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/s/routing_information_cache.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/s/sharding_index_catalog_cache.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/functional.h"
#include "mongo/util/future_util.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

namespace mongo {
namespace {

using namespace fmt::literals;

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
MONGO_FAIL_POINT_DEFINE(reshardingPerformValidationAfterApplying);
MONGO_FAIL_POINT_DEFINE(reshardingPerformValidationAfterCloning);
MONGO_FAIL_POINT_DEFINE(pauseBeforeTellDonorToRefresh);
MONGO_FAIL_POINT_DEFINE(pauseAfterInsertCoordinatorDoc);
MONGO_FAIL_POINT_DEFINE(pauseBeforeCTHolderInitialization);
MONGO_FAIL_POINT_DEFINE(pauseAfterEngagingCriticalSection);

const std::string kReshardingCoordinatorActiveIndexName = "ReshardingCoordinatorActiveIndex";
const Backoff kExponentialBackoff(Seconds(1), Milliseconds::max());

bool shouldStopAttemptingToCreateIndex(Status status, const CancellationToken& token) {
    return status.isOK() || token.isCanceled();
}

}  // namespace

ThreadPool::Limits ReshardingCoordinatorService::getThreadPoolLimits() const {
    ThreadPool::Limits threadPoolLimit;
    threadPoolLimit.maxThreads = resharding::gReshardingCoordinatorServiceMaxThreadCount;
    return threadPoolLimit;
}

void ReshardingCoordinatorService::checkIfConflictsWithOtherInstances(
    OperationContext* opCtx,
    BSONObj initialState,
    const std::vector<const PrimaryOnlyService::Instance*>& existingInstances) {
    auto coordinatorDoc = ReshardingCoordinatorDocument::parse(
        IDLParserContext("ReshardingCoordinatorService::checkIfConflictsWithOtherInstances"),
        initialState);

    for (const auto& instance : existingInstances) {
        auto typedInstance = checked_cast<const ReshardingCoordinator*>(instance);
        // Instances which have already completed do not conflict with other instances, unless
        // their user resharding UUIDs are the same.
        const bool isUserReshardingUUIDSame =
            typedInstance->getMetadata().getUserReshardingUUID() ==
            coordinatorDoc.getUserReshardingUUID();
        if (!isUserReshardingUUIDSame && typedInstance->getCompletionFuture().isReady()) {
            LOGV2_DEBUG(7760400,
                        1,
                        "Ignoring 'conflict' with completed instance of resharding",
                        "newNss"_attr = coordinatorDoc.getSourceNss(),
                        "oldNss"_attr = typedInstance->getMetadata().getSourceNss(),
                        "newUUID"_attr = coordinatorDoc.getReshardingUUID(),
                        "oldUUID"_attr = typedInstance->getMetadata().getReshardingUUID());
            continue;
        }
        // For resharding commands with no UUID provided by the user, we will re-connect to an
        // instance with the same NS and resharding key, if that instance was originally started
        // with no user-provided UUID. If a UUID is provided by the user, we will connect only
        // to the original instance.
        const bool isNssSame =
            typedInstance->getMetadata().getSourceNss() == coordinatorDoc.getSourceNss();
        const bool isReshardingKeySame = SimpleBSONObjComparator::kInstance.evaluate(
            typedInstance->getMetadata().getReshardingKey().toBSON() ==
            coordinatorDoc.getReshardingKey().toBSON());

        const bool isProvenanceSame =
            (typedInstance->getMetadata().getProvenance() ==
             coordinatorDoc.getCommonReshardingMetadata().getProvenance());

        iassert(ErrorCodes::ConflictingOperationInProgress,
                str::stream() << "Only one resharding operation is allowed to be active at a "
                                 "time, aborting resharding op for "
                              << coordinatorDoc.getSourceNss().toStringForErrorMsg(),
                isUserReshardingUUIDSame && isNssSame && isReshardingKeySame && isProvenanceSame);

        std::string userReshardingIdMsg;
        if (coordinatorDoc.getUserReshardingUUID()) {
            userReshardingIdMsg = str::stream()
                << " and user resharding UUID " << coordinatorDoc.getUserReshardingUUID();
        }

        iasserted(ReshardingCoordinatorServiceConflictingOperationInProgressInfo(
                      typedInstance->shared_from_this()),
                  str::stream() << "Found an active resharding operation for "
                                << coordinatorDoc.getSourceNss().toStringForErrorMsg()
                                << " with resharding key "
                                << coordinatorDoc.getReshardingKey().toString()
                                << userReshardingIdMsg);
    }
}

std::shared_ptr<repl::PrimaryOnlyService::Instance> ReshardingCoordinatorService::constructInstance(
    BSONObj initialState) {
    return std::make_shared<ReshardingCoordinator>(
        this,
        ReshardingCoordinatorDocument::parse(IDLParserContext("ReshardingCoordinatorStateDoc"),
                                             initialState),
        std::make_shared<ReshardingCoordinatorExternalStateImpl>(),
        _serviceContext);
}

ExecutorFuture<void> ReshardingCoordinatorService::_rebuildService(
    std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancellationToken& token) {

    return AsyncTry([this] {
               auto nss = getStateDocumentsNS();

               AllowOpCtxWhenServiceRebuildingBlock allowOpCtxBlock(Client::getCurrent());
               auto opCtxHolder = cc().makeOperationContext();
               auto opCtx = opCtxHolder.get();
               DBDirectClient client(opCtx);
               BSONObj result;
               // We don't need a unique index on "active" any more since
               // checkIfConflictsWithOtherInstances was implemented, and once we allow quiesced
               // instances it breaks them, so don't create it.
               //
               // TODO(SERVER-67712): We create the collection only to make index creation during
               // downgrade simpler, so we can remove all of this initialization when the flag is
               // removed.
               if (!resharding::gFeatureFlagReshardingImprovements.isEnabled(
                       serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
                   client.runCommand(
                       nss.dbName(),
                       BSON("createIndexes"
                            << nss.coll().toString() << "indexes"
                            << BSON_ARRAY(BSON("key" << BSON("active" << 1) << "name"
                                                     << kReshardingCoordinatorActiveIndexName
                                                     << "unique" << true))),
                       result);
                   uassertStatusOK(getStatusFromCommandResult(result));
               } else {
                   client.runCommand(nss.dbName(), BSON("create" << nss.coll().toString()), result);
                   const auto& status = getStatusFromCommandResult(result);
                   if (status.code() != ErrorCodes::NamespaceExists)
                       uassertStatusOK(status);
               }
           })
        .until([token](Status status) { return shouldStopAttemptingToCreateIndex(status, token); })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, CancellationToken::uncancelable());
}

void ReshardingCoordinatorService::abortAllReshardCollection(OperationContext* opCtx) {
    std::vector<SharedSemiFuture<void>> reshardingCoordinatorFutures;

    for (auto& instance : getAllInstances(opCtx)) {
        auto reshardingCoordinator = checked_pointer_cast<ReshardingCoordinator>(instance);
        reshardingCoordinatorFutures.push_back(
            reshardingCoordinator->getQuiescePeriodFinishedFuture());
        reshardingCoordinator->abort(true /* skip quiesce period */);
    }

    for (auto&& future : reshardingCoordinatorFutures) {
        future.wait(opCtx);
    }
}

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

void ReshardingCoordinator::installCoordinatorDoc(
    OperationContext* opCtx, const ReshardingCoordinatorDocument& doc) noexcept {
    invariant(doc.getReshardingUUID() == _coordinatorDoc.getReshardingUUID());

    BSONObjBuilder bob;
    bob.append("newState", CoordinatorState_serializer(doc.getState()));
    bob.append("oldState", CoordinatorState_serializer(_coordinatorDoc.getState()));
    bob.append(
        "namespace",
        NamespaceStringUtil::serialize(doc.getSourceNss(), SerializationContext::stateDefault()));
    bob.append("collectionUUID", doc.getSourceUUID().toString());
    bob.append("reshardingUUID", doc.getReshardingUUID().toString());

    LOGV2_INFO(5343001,
               "Transitioned resharding coordinator state",
               "newState"_attr = CoordinatorState_serializer(doc.getState()),
               "oldState"_attr = CoordinatorState_serializer(_coordinatorDoc.getState()),
               logAttrs(doc.getSourceNss()),
               "collectionUUID"_attr = doc.getSourceUUID(),
               "reshardingUUID"_attr = doc.getReshardingUUID());

    const auto previousState = _coordinatorDoc.getState();
    _coordinatorDoc = doc;

    _metrics->onStateTransition(previousState, _coordinatorDoc.getState());

    ShardingLogging::get(opCtx)->logChange(opCtx,
                                           "resharding.coordinator.transition",
                                           doc.getSourceNss(),
                                           bob.obj(),
                                           resharding::kMajorityWriteConcern);
}

void markCompleted(const Status& status, ReshardingMetrics* metrics) {
    if (status.isOK()) {
        metrics->onSuccess();
    } else if (status == ErrorCodes::ReshardCollectionAborted) {
        metrics->onCanceled();
    } else {
        metrics->onFailure();
    }
}

std::shared_ptr<async_rpc::AsyncRPCOptions<_flushReshardingStateChange>>
createFlushReshardingStateChangeOptions(const NamespaceString& nss,
                                        const UUID& reshardingUUID,
                                        const std::shared_ptr<executor::TaskExecutor>& exec,
                                        CancellationToken token) {
    _flushReshardingStateChange cmd(nss);
    cmd.setDbName(DatabaseName::kAdmin);
    cmd.setReshardingUUID(reshardingUUID);
    auto opts =
        std::make_shared<async_rpc::AsyncRPCOptions<_flushReshardingStateChange>>(exec, token, cmd);
    return opts;
}

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
                   .then([this] { return _waitForMajority(_ctHolder->getStepdownToken()); })
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
        .onUnrecoverableError([](const Status& status) {})
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
        .onUnrecoverableError([](const Status& status) {})
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
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) noexcept {
    return resharding::WithAutomaticRetry([this, executor] {
               return ExecutorFuture<void>(**executor)
                   .then([this, executor] { return _awaitAllDonorsReadyToDonate(executor); })
                   .then([this, executor] {
                       if (_coordinatorDoc.getState() == CoordinatorStateEnum::kCloning) {
                           _tellAllRecipientsToRefresh(executor);
                       }
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
                       }
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
    const ReshardingCoordinatorDocument& updatedCoordinatorDoc) noexcept {
    return resharding::WithAutomaticRetry([this, executor, updatedCoordinatorDoc] {
               return ExecutorFuture<void>(**executor)
                   .then(
                       [this, executor, updatedCoordinatorDoc] { _commit(updatedCoordinatorDoc); });
           })
        .onTransientError([](const Status& status) {
            LOGV2(7698801,
                  "Resharding coordinator encountered transient error while committing",
                  "error"_attr = status);
        })
        .onUnrecoverableError([](const Status& status) {})
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
                           .then([this] { return _waitForMajority(_ctHolder->getStepdownToken()); })
                           .thenRunOn(**executor)
                           .then(
                               [this, executor] { _generateOpEventOnCoordinatingShard(executor); })
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
                               _metrics->setEndFor(ReshardingMetrics::TimedPhase::kCriticalSection,
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
                .onUnrecoverableError([](const Status& status) {})
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
                                      : ns.toString() ==
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
            if (resharding::gFeatureFlagReshardingImprovements.isEnabled(
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
                _logStatsOnCompletion(status.isOK());
            }

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
        .onUnrecoverableError([](const Status& retryStatus) {})
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
                               CoordinatorStateEnum::kAborting,
                               _coordinatorDoc,
                               boost::none,
                               boost::none,
                               status);
                       }
                   })
                   .then([this] { return _waitForMajority(_ctHolder->getStepdownToken()); })
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
        .onUnrecoverableError([](const Status& retryStatus) {})
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

SemiFuture<void> ReshardingCoordinator::_waitForMajority(const CancellationToken& token) {
    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    auto client = opCtx->getClient();
    repl::ReplClientInfo::forClient(client).setLastOpToSystemLastOpTime(opCtx.get());
    auto opTime = repl::ReplClientInfo::forClient(client).getLastOp();
    return WaitForMajorityService::get(client->getServiceContext())
        .waitUntilMajorityForWrite(opTime, token);
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

               // Ensure indexes are loaded in the catalog cache, along with the collection
               // placement.
               if (feature_flags::gGlobalIndexesShardingCatalog.isEnabled(
                       serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {

                   uassertStatusOK(
                       RoutingInformationCache::get(opCtx)->getCollectionIndexInfoWithRefresh(
                           opCtx, _coordinatorDoc.getSourceNss()));
               }

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
                          _metadata.getProvenance().get() == ProvenanceEnum::kUnshardCollection) {
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
        .onUnrecoverableError([](const StatusWith<bool>& status) {})
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

        return;
    }

    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    reshardingPauseCoordinatorBeforeInitializing.pauseWhileSetAndNotCanceled(
        opCtx.get(), _ctHolder->getStepdownToken());
    ReshardingCoordinatorDocument updatedCoordinatorDoc = _coordinatorDoc;
    updatedCoordinatorDoc.setState(CoordinatorStateEnum::kInitializing);
    resharding::insertCoordDocAndChangeOrigCollEntry(
        opCtx.get(), _metrics.get(), updatedCoordinatorDoc);
    installCoordinatorDoc(opCtx.get(), updatedCoordinatorDoc);

    {
        // Note: don't put blocking or interruptible code in this block.
        const bool isSameKeyResharding =
            _coordinatorDoc.getForceRedistribution() && *_coordinatorDoc.getForceRedistribution();
        _coordinatorDocWrittenPromise.emplaceValue();
        // We need to call setIsSameKeyResharding first so the metrics can count same key resharding
        // correctly.
        _metrics->setIsSameKeyResharding(isSameKeyResharding);
        _metrics->onStarted();
    }

    pauseAfterInsertCoordinatorDoc.pauseWhileSet();
}

void ReshardingCoordinator::_calculateParticipantsAndChunksThenWriteToDisk() {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kInitializing) {
        return;
    }
    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    ReshardingCoordinatorDocument updatedCoordinatorDoc = _coordinatorDoc;
    auto provenance = updatedCoordinatorDoc.getCommonReshardingMetadata().getProvenance();

    if (resharding::isUnshardCollection(provenance)) {
        // Since the resulting collection of an unshardCollection operation cannot have zones, we do
        // not need to account for existing zones in the original collection. Existing zones from
        // the original collection will be deleted after the unsharding operation commits.
        uassert(ErrorCodes::InvalidOptions,
                "Cannot specify zones when unsharding a collection.",
                !updatedCoordinatorDoc.getZones());
    } else {
        // If zones are not provided by the user, we should use the existing zones for
        // this resharding operation.
        if (updatedCoordinatorDoc.getForceRedistribution() &&
            *updatedCoordinatorDoc.getForceRedistribution() && !updatedCoordinatorDoc.getZones()) {
            auto zones = resharding::getZonesFromExistingCollection(
                opCtx.get(), updatedCoordinatorDoc.getSourceNss());
            updatedCoordinatorDoc.setZones(std::move(zones));
        }
    }

    auto shardsAndChunks = _reshardingCoordinatorExternalState->calculateParticipantShardsAndChunks(
        opCtx.get(), updatedCoordinatorDoc);

    updatedCoordinatorDoc.setDonorShards(std::move(shardsAndChunks.donorShards));
    updatedCoordinatorDoc.setRecipientShards(std::move(shardsAndChunks.recipientShards));
    updatedCoordinatorDoc.setState(CoordinatorStateEnum::kPreparingToDonate);

    // Remove the presetReshardedChunks and zones from the coordinator document to reduce
    // the possibility of the document reaching the BSONObj size constraint.
    ShardKeyPattern shardKey(updatedCoordinatorDoc.getReshardingKey());
    std::vector<BSONObj> zones;
    if (updatedCoordinatorDoc.getZones()) {
        zones = resharding::buildTagsDocsFromZones(updatedCoordinatorDoc.getTempReshardingNss(),
                                                   *updatedCoordinatorDoc.getZones(),
                                                   shardKey);
    }
    updatedCoordinatorDoc.setPresetReshardedChunks(boost::none);
    updatedCoordinatorDoc.setZones(boost::none);

    auto indexVersion = _reshardingCoordinatorExternalState->getCatalogIndexVersion(
        opCtx.get(),
        updatedCoordinatorDoc.getSourceNss(),
        updatedCoordinatorDoc.getReshardingUUID());

    auto isUnsplittable = _reshardingCoordinatorExternalState->getIsUnsplittable(
                              opCtx.get(), updatedCoordinatorDoc.getSourceNss()) ||
        (provenance && provenance.get() == ProvenanceEnum::kUnshardCollection);

    resharding::writeParticipantShardsAndTempCollInfo(opCtx.get(),
                                                      _metrics.get(),
                                                      updatedCoordinatorDoc,
                                                      std::move(shardsAndChunks.initialChunks),
                                                      std::move(zones),
                                                      std::move(indexVersion),
                                                      isUnsplittable);
    installCoordinatorDoc(opCtx.get(), updatedCoordinatorDoc);

    reshardingPauseCoordinatorAfterPreparingToDonate.pauseWhileSetAndNotCanceled(
        opCtx.get(), _ctHolder->getAbortToken());
}

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

ExecutorFuture<void> ReshardingCoordinator::_awaitAllDonorsReadyToDonate(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kPreparingToDonate) {
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
            _updateCoordinatorDocStateAndCatalogEntries(CoordinatorStateEnum::kCloning,
                                                        coordinatorDocChangedOnDisk,
                                                        highestMinFetchTimestamp,
                                                        approxCopySize);
        })
        .then([this] { return _waitForMajority(_ctHolder->getAbortToken()); });
}

ExecutorFuture<void> ReshardingCoordinator::_awaitAllRecipientsFinishedCloning(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kCloning) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return future_util::withCancellation(
               _reshardingCoordinatorObserver->awaitAllRecipientsFinishedCloning(),
               _ctHolder->getAbortToken())
        .thenRunOn(**executor)
        .then([this](const ReshardingCoordinatorDocument& coordinatorDocChangedOnDisk) {
            if (_metadata.getPerformVerification() &&
                MONGO_unlikely(reshardingPerformValidationAfterCloning.shouldFail())) {
                auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
                _reshardingCoordinatorExternalState->verifyClonedCollection(
                    opCtx.get(), coordinatorDocChangedOnDisk);
            }

            return coordinatorDocChangedOnDisk;
        })
        .then([this](ReshardingCoordinatorDocument coordinatorDocChangedOnDisk) {
            this->_updateCoordinatorDocStateAndCatalogEntries(CoordinatorStateEnum::kApplying,
                                                              coordinatorDocChangedOnDisk);
        })
        .then([this] { return _waitForMajority(_ctHolder->getAbortToken()); });
}

void ReshardingCoordinator::_startCommitMonitor(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_commitMonitor) {
        return;
    }

    _commitMonitor = std::make_shared<resharding::CoordinatorCommitMonitor>(
        _metrics,
        _coordinatorDoc.getSourceNss(),
        resharding::extractShardIdsFromParticipantEntries(_coordinatorDoc.getRecipientShards()),
        **executor,
        _ctHolder->getCommitMonitorToken(),
        resharding::gReshardingDelayBeforeRemainingOperationTimeQueryMillis.load());

    _commitMonitorQuiesced = _commitMonitor->waitUntilRecipientsAreWithinCommitThreshold()
                                 .thenRunOn(**executor)
                                 .onCompletion([this](Status status) {
                                     _fulfillOkayToEnterCritical(status);
                                     return status;
                                 })
                                 .share();
}

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
            const auto criticalSectionTimeout =
                Milliseconds(resharding::gReshardingCriticalSectionTimeoutMillis.load());
            const auto criticalSectionExpiresAt = (*executor)->now() + criticalSectionTimeout;

            ReshardingCoordinatorDocument updatedCSExpirationDoc = _coordinatorDoc;
            updatedCSExpirationDoc.setCriticalSectionExpiresAt(criticalSectionExpiresAt);
            this->_updateCoordinatorDocStateAndCatalogEntries(CoordinatorStateEnum::kBlockingWrites,
                                                              updatedCSExpirationDoc);

            _metrics->setStartFor(ReshardingMetrics::TimedPhase::kCriticalSection,
                                  resharding::getCurrentTime());
        })
        .then([this] { return _waitForMajority(_ctHolder->getAbortToken()); })
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
        .then([this, executor](const ReshardingCoordinatorDocument& coordinatorDocChangedOnDisk) {
            if (_metadata.getPerformVerification() &&
                MONGO_unlikely(reshardingPerformValidationAfterApplying.shouldFail())) {
                auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
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

    auto indexVersion = _reshardingCoordinatorExternalState->getCatalogIndexVersionForCommit(
        opCtx.get(), updatedCoordinatorDoc.getTempReshardingNss());

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
                                            std::move(indexVersion),
                                            reshardedCollectionPlacement);

    // Update the in memory state
    installCoordinatorDoc(opCtx.get(), updatedCoordinatorDoc);
}

void ReshardingCoordinator::_generateOpEventOnCoordinatingShard(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());

    CollectionResharded eventNotification(_coordinatorDoc.getSourceNss(),
                                          _coordinatorDoc.getSourceUUID(),
                                          _coordinatorDoc.getReshardingUUID(),
                                          _coordinatorDoc.getReshardingKey().toBSON());
    eventNotification.setSourceKey(_coordinatorDoc.getSourceKey());
    eventNotification.setNumInitialChunks(_coordinatorDoc.getNumInitialChunks());
    eventNotification.setUnique(_coordinatorDoc.getUnique());
    eventNotification.setCollation(_coordinatorDoc.getCollation());

    ShardsvrNotifyShardingEventRequest request(notify_sharding_event::kCollectionResharded,
                                               eventNotification.toBSON());

    const auto dbPrimaryShard = Grid::get(opCtx.get())
                                    ->catalogClient()
                                    ->getDatabase(opCtx.get(),
                                                  _coordinatorDoc.getSourceNss().dbName(),
                                                  repl::ReadConcernLevel::kMajorityReadConcern)
                                    .getPrimary();

    // In case the recipient is running a legacy binary, swallow the error.
    try {
        generic_argument_util::setMajorityWriteConcern(request, &resharding::kMajorityWriteConcern);
        const auto opts =
            std::make_shared<async_rpc::AsyncRPCOptions<ShardsvrNotifyShardingEventRequest>>(
                **executor, _ctHolder->getStepdownToken(), request);
        opts->cmd.setDbName(DatabaseName::kAdmin);
        _reshardingCoordinatorExternalState->sendCommandToShards(
            opCtx.get(), opts, {dbPrimaryShard});
    } catch (const ExceptionFor<ErrorCodes::UnsupportedShardingEventNotification>& e) {
        LOGV2_WARNING(7403100,
                      "Unable to generate op entry on reshardCollection commit",
                      "error"_attr = redact(e.toStatus()));
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
                _reshardingCoordinatorExternalState->sendCommandToShards(
                    opCtx.get(), opts, allShardIds);
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
        opCtx.get(), _metrics.get(), updatedCoordinatorDoc);

    // Update in-memory coordinator doc
    installCoordinatorDoc(opCtx.get(), updatedCoordinatorDoc);
}

void ReshardingCoordinator::_removeOrQuiesceCoordinatorDocAndRemoveReshardingFields(
    OperationContext* opCtx, boost::optional<Status> abortReason) {
    auto updatedCoordinatorDoc = resharding::removeOrQuiesceCoordinatorDocAndRemoveReshardingFields(
        opCtx, _metrics.get(), _coordinatorDoc, abortReason);

    // Update in-memory coordinator doc.
    installCoordinatorDoc(opCtx, updatedCoordinatorDoc);
}

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

    _reshardingCoordinatorExternalState->sendCommandToShards(
        opCtx.get(), opts, {participantShardIds.begin(), participantShardIds.end()});
}

template <typename CommandType>
void ReshardingCoordinator::_sendCommandToAllRecipients(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    std::shared_ptr<async_rpc::AsyncRPCOptions<CommandType>> opts) {
    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    auto recipientShardIds =
        resharding::extractShardIdsFromParticipantEntries(_coordinatorDoc.getRecipientShards());

    _reshardingCoordinatorExternalState->sendCommandToShards(
        opCtx.get(), opts, {recipientShardIds.begin(), recipientShardIds.end()});
}

template <typename CommandType>
void ReshardingCoordinator::_sendCommandToAllDonors(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    std::shared_ptr<async_rpc::AsyncRPCOptions<CommandType>> opts) {
    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    auto donorShardIds =
        resharding::extractShardIdsFromParticipantEntries(_coordinatorDoc.getDonorShards());

    _reshardingCoordinatorExternalState->sendCommandToShards(
        opCtx.get(), opts, {donorShardIds.begin(), donorShardIds.end()});
}

void ReshardingCoordinator::_establishAllDonorsAsParticipants(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    invariant(_coordinatorDoc.getState() == CoordinatorStateEnum::kPreparingToDonate);
    auto opts = resharding::makeFlushRoutingTableCacheUpdatesOptions(
        _coordinatorDoc.getSourceNss(), **executor, _ctHolder->getStepdownToken());
    opts->cmd.setDbName(DatabaseName::kAdmin);
    generic_argument_util::setMajorityWriteConcern(opts->cmd, &resharding::kMajorityWriteConcern);
    _sendCommandToAllDonors(executor, opts);
}

void ReshardingCoordinator::_establishAllRecipientsAsParticipants(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    invariant(_coordinatorDoc.getState() == CoordinatorStateEnum::kPreparingToDonate);
    auto opts = resharding::makeFlushRoutingTableCacheUpdatesOptions(
        _coordinatorDoc.getTempReshardingNss(), **executor, _ctHolder->getStepdownToken());
    opts->cmd.setDbName(DatabaseName::kAdmin);
    generic_argument_util::setMajorityWriteConcern(opts->cmd, &resharding::kMajorityWriteConcern);
    _sendCommandToAllRecipients(executor, opts);
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

    auto opts = createFlushReshardingStateChangeOptions(nssToRefresh,
                                                        _coordinatorDoc.getReshardingUUID(),
                                                        **executor,
                                                        _ctHolder->getStepdownToken());
    opts->cmd.setDbName(DatabaseName::kAdmin);
    generic_argument_util::setMajorityWriteConcern(opts->cmd, &resharding::kMajorityWriteConcern);
    _sendCommandToAllRecipients(executor, opts);
}

void ReshardingCoordinator::_tellAllDonorsToRefresh(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    auto opts = createFlushReshardingStateChangeOptions(_coordinatorDoc.getSourceNss(),
                                                        _coordinatorDoc.getReshardingUUID(),
                                                        **executor,
                                                        _ctHolder->getStepdownToken());
    generic_argument_util::setMajorityWriteConcern(opts->cmd, &resharding::kMajorityWriteConcern);
    opts->cmd.setDbName(DatabaseName::kAdmin);
    _sendCommandToAllDonors(executor, opts);
}

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

        const auto allShardsWithOpTime = uassertStatusOK(
            catalogClient->getAllShards(opCtx, repl::ReadConcernLevel::kLocalReadConcern));

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
    builder.append("uuid", _coordinatorDoc.getReshardingUUID().toBSON());
    builder.append("status", success ? "success" : "failed");
    statsBuilder.append("ns", toStringForLogging(_coordinatorDoc.getSourceNss()));
    statsBuilder.append("sourceUUID", _coordinatorDoc.getSourceUUID().toBSON());
    statsBuilder.append("newUUID", _coordinatorDoc.getReshardingUUID().toBSON());
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
        statsBuilder.append("operationDuration", elapsedMillis);
    } else {
        statsBuilder.append("endTime", resharding::getCurrentTime());
    }
    _metrics->reportOnCompletion(&statsBuilder);

    int64_t totalWritesDuringCriticalSection = 0;
    for (auto shard : _coordinatorDoc.getDonorShards()) {
        totalWritesDuringCriticalSection +=
            shard.getMutableState().getWritesDuringCriticalSection().value_or(0);
    }
    statsBuilder.append("writesDuringCriticalSection", totalWritesDuringCriticalSection);

    for (auto shard : _coordinatorDoc.getRecipientShards()) {
        BSONObjBuilder shardBuilder;
        shardBuilder.append("bytesCopied", shard.getMutableState().getBytesCopied().value_or(0));
        shardBuilder.append("oplogFetched", shard.getMutableState().getOplogFetched().value_or(0));
        shardBuilder.append("oplogApplied", shard.getMutableState().getOplogApplied().value_or(0));
        statsBuilder.append(shard.getId(), shardBuilder.obj());
    }

    int64_t totalDocuments = 0;
    int64_t docSize = 0;
    int64_t totalIndexes = 0;
    for (auto shard : _coordinatorDoc.getRecipientShards()) {
        totalDocuments += shard.getMutableState().getTotalNumDocuments().value_or(0);
        docSize += shard.getMutableState().getTotalDocumentSize().value_or(0);
        if (shard.getMutableState().getNumOfIndexes().value_or(0) > totalIndexes) {
            totalIndexes = shard.getMutableState().getNumOfIndexes().value_or(0);
        }
    }
    statsBuilder.append("numberOfTotalDocuments", totalDocuments);
    statsBuilder.append("averageDocSize", totalDocuments > 0 ? (docSize / totalDocuments) : 0);
    statsBuilder.append("numberOfIndexes", totalIndexes);

    statsBuilder.append("numberOfSourceShards", (int64_t)(_coordinatorDoc.getDonorShards().size()));

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
    statsBuilder.append("numberOfDestinationShards", (int64_t)numDestinationShards);

    builder.append("statistics", statsBuilder.obj());
    LOGV2(7763800, "Resharding complete", "info"_attr = builder.obj());
}

}  // namespace mongo
