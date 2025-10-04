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

#include "mongo/db/s/resharding/resharding_donor_recipient_common.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/client.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_sharding_runtime.h"
#include "mongo/db/local_catalog/shard_role_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/resharding/resharding_donor_service.h"
#include "mongo/db/s/resharding/resharding_recipient_service.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/vector_clock/vector_clock_mutable.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/logv2/log.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/future_util.h"
#include "mongo/util/time_support.h"

#include <initializer_list>
#include <string>
#include <type_traits>
#include <utility>

#include <absl/container/node_hash_set.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

namespace mongo {
namespace resharding {

using DonorStateMachine = ReshardingDonorService::DonorStateMachine;
using RecipientStateMachine = ReshardingRecipientService::RecipientStateMachine;

namespace {
MONGO_FAIL_POINT_DEFINE(reshardingInterruptAfterInsertStateMachineDocument);

const Backoff kExponentialBackoff(Seconds(1), Milliseconds::max());

template <class StateMachine, class ReshardingDocument>
void ensureStateDocumentInserted(OperationContext* opCtx, const ReshardingDocument& doc) {
    try {
        StateMachine::insertStateDocument(opCtx, doc);
    } catch (const ExceptionFor<ErrorCodes::DuplicateKey>& ex) {
        // It's possible that the state document was already previously inserted in the following
        // cases:
        // 1. The document was inserted previously, but the opCtx was interrupted before the
        // state machine was started in-memory with getOrCreate(), e.g. due to a chunk migration
        // (see SERVER-74647)
        // 2. Similar to the ErrorCategory::NotPrimaryError clause below, it is
        // theoretically possible for a series of stepdowns and step-ups to lead a scenario where a
        // stale but now re-elected primary attempts to insert the state document when another node
        // which was primary had already done so. Again, rather than attempt to prevent replica set
        // member state transitions during the shard version refresh, we instead swallow the
        // DuplicateKey exception. This is safe because PrimaryOnlyService::onStepUp() will have
        // constructed a new instance of the resharding state machine.
        auto dupeKeyInfo = ex.extraInfo<DuplicateKeyErrorInfo>();
        invariant(dupeKeyInfo->getDuplicatedKeyValue().binaryEqual(
            BSON("_id" << doc.getReshardingUUID())));
    }
}

/*
 * Creates a ReshardingStateMachine if this node is primary and the ReshardingStateMachine doesn't
 * already exist.
 *
 * It is safe to call this function when this node is actually a secondary.
 */
template <class Service, class StateMachine, class ReshardingDocument>
void createReshardingStateMachine(OperationContext* opCtx, const ReshardingDocument& doc) {
    try {
        // Inserting the resharding state document must happen synchronously with the shard version
        // refresh for the w:majority wait from the resharding coordinator to mean that this replica
        // set shard cannot forget about being a participant.
        ensureStateDocumentInserted<StateMachine>(opCtx, doc);

        reshardingInterruptAfterInsertStateMachineDocument.execute(
            [&opCtx](const BSONObj& data) { opCtx->markKilled(); });

        auto registry = repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext());
        auto service = registry->lookupServiceByName(Service::kServiceName);
        StateMachine::getOrCreate(opCtx, service, doc.toBSON());
    } catch (const ExceptionFor<ErrorCategory::NotPrimaryError>&) {
        // resharding::processReshardingFieldsForCollection() is called on both primary and
        // secondary nodes as part of the shard version being refreshed. Due to the RSTL lock not
        // being held throughout the shard version refresh, it is also possible for the node to
        // arbitrarily step down and step up during the shard version refresh. Rather than
        // attempt to prevent replica set member state transitions during the shard version refresh,
        // we instead swallow the NotPrimaryError exception. This is safe because there is no work a
        // secondary (or primary which stepped down) must do for an active resharding operation upon
        // refreshing its shard version. The primary is solely responsible for advancing the
        // participant state as a result of the shard version refresh.
    }
}

/*
 * Either constructs a new ReshardingDonorStateMachine with 'reshardingFields' or passes
 * 'reshardingFields' to an already-existing ReshardingDonorStateMachine.
 */
void processReshardingFieldsForDonorCollection(OperationContext* opCtx,
                                               const NamespaceString& nss,
                                               const CollectionMetadata& metadata,
                                               const ReshardingFields& reshardingFields) {
    if (auto donorStateMachine = tryGetReshardingStateMachine<ReshardingDonorService,
                                                              DonorStateMachine,
                                                              ReshardingDonorDocument>(
            opCtx, reshardingFields.getReshardingUUID())) {
        donorStateMachine->get()->onReshardingFieldsChanges(opCtx, reshardingFields);

        const auto coordinatorState = reshardingFields.getState();
        if (coordinatorState == CoordinatorStateEnum::kBlockingWrites) {
            (*donorStateMachine)->awaitCriticalSectionAcquired().wait(opCtx);
        }

        return;
    }

    // If a resharding operation is BEFORE state kPreparingToDonate, then the config.collections
    // entry won't have yet been created for the temporary resharding collection. The
    // DonorStateMachine refreshes the temporary resharding collection immediately after being
    // constructed. Accordingly, we avoid constructing the DonorStateMachine until the collection
    // entry for the temporary resharding collection is known to exist.
    //
    // If a resharding operation is PAST state kPreparingToDonate, but does not currently have a
    // donor document in-memory, this means that the document will be recovered by the
    // ReshardingDonorService. Accordingly, at time-of-recovery, the latest instance of
    // 'reshardingFields' will be read. Return no-op.
    if (reshardingFields.getState() != CoordinatorStateEnum::kPreparingToDonate) {
        return;
    }

    // This could be a shard not indicated as a donor that's trying to refresh the source
    // collection. In this case, we don't want to create a donor machine.
    if (!metadata.currentShardHasAnyChunks()) {
        return;
    }

    // We clear the routing information for the temporary resharding namespace to ensure this donor
    // shard primary will refresh from the config server and see the chunk distribution for the new
    // resharding operation.
    auto* catalogCache = Grid::get(opCtx)->catalogCache();
    catalogCache->invalidateCollectionEntry_LINEARIZABLE(
        reshardingFields.getDonorFields()->getTempReshardingNss());

    auto donorDoc = constructDonorDocumentFromReshardingFields(
        VersionContext::getDecoration(opCtx), nss, metadata, reshardingFields);
    createReshardingStateMachine<ReshardingDonorService,
                                 DonorStateMachine,
                                 ReshardingDonorDocument>(opCtx, donorDoc);
}

bool isCurrentShardPrimary(OperationContext* opCtx, const NamespaceString& nss) {
    auto dbInfo = [&] {
        // At this point of resharding execution, the coordinator is holding a DDL lock which means
        // the DB primary is stable and we have gossiped-in the `configTime` which created that
        // coordinator. Therefore if we force a routing cache refresh thorugh
        // onStaleDatabaseVersion() we have the guarantee to fetch the most-up-to date DB info.
        // primary.
        //
        // TODO SERVER-96115 Use the CatalogClient to fetch DB info and avoid this forced refresh
        const auto& catalogCache = Grid::get(opCtx)->catalogCache();
        catalogCache->onStaleDatabaseVersion(nss.dbName(), boost::none /* wantedVersion */);
        return uassertStatusOK(catalogCache->getDatabase(opCtx, nss.dbName()));
    }();
    return dbInfo->getPrimary() == ShardingState::get(opCtx)->shardId();
}

/*
 * Either constructs a new ReshardingRecipientStateMachine with 'reshardingFields' or passes
 * 'reshardingFields' to an already-existing ReshardingRecipientStateMachine.
 */
void processReshardingFieldsForRecipientCollection(OperationContext* opCtx,
                                                   const NamespaceString& nss,
                                                   const CollectionMetadata& metadata,
                                                   const ReshardingFields& reshardingFields) {
    if (auto recipientStateMachine = tryGetReshardingStateMachine<ReshardingRecipientService,
                                                                  RecipientStateMachine,
                                                                  ReshardingRecipientDocument>(
            opCtx, reshardingFields.getReshardingUUID())) {
        recipientStateMachine->get()->onReshardingFieldsChanges(
            opCtx, reshardingFields, !metadata.currentShardHasAnyChunks() /* noChunksToCopy */);
        return;
    }

    // If a resharding operation is past state kPreparingToDonate but does not currently have a
    // recipient document in-memory, this means that the document will be recovered by the
    // ReshardingRecipientService, and at that time the latest instance of 'reshardingFields'
    // will be read. Return no-op.
    //
    // We construct the RecipientStateMachine in the kPreparingToDonate state (which is the same
    // state as when we would construct the DonorStateMachine) so the resharding coordinator can
    // rely on all of the state machines being constructed as part of the same state transition.
    if (reshardingFields.getState() != CoordinatorStateEnum::kPreparingToDonate) {
        return;
    }

    // This could be a shard not indicated as a recipient that's trying to refresh the temporary
    // collection. In this case, we don't want to create a recipient machine.
    if (!isCurrentShardPrimary(opCtx, nss) && !metadata.currentShardHasAnyChunks()) {
        return;
    }

    auto recipientDoc = constructRecipientDocumentFromReshardingFields(
        VersionContext::getDecoration(opCtx), nss, metadata, reshardingFields);
    createReshardingStateMachine<ReshardingRecipientService,
                                 RecipientStateMachine,
                                 ReshardingRecipientDocument>(opCtx, recipientDoc);
}

/**
 * Checks that presence/absence of 'donorShards' and 'recipientShards' fields in the
 * reshardingFields are consistent with the 'state' field.
 */
void verifyValidReshardingFields(const ReshardingFields& reshardingFields) {
    auto coordinatorState = reshardingFields.getState();

    if (coordinatorState < CoordinatorStateEnum::kPreparingToDonate) {
        // Prior to the state CoordinatorStateEnum::kPreparingToDonate, the source collection's
        // config.collections entry won't have "donorFields". Additionally, the temporary resharding
        // collection's config.collections entry won't exist yet.
        uassert(5498100,
                fmt::format("reshardingFields must not contain donorFields or recipientFields when"
                            " the coordinator is in state {}. Got reshardingFields {}",
                            CoordinatorState_serializer(reshardingFields.getState()),
                            reshardingFields.toBSON().toString()),
                !reshardingFields.getDonorFields() && !reshardingFields.getRecipientFields());
    } else if (coordinatorState < CoordinatorStateEnum::kCommitting) {
        // Prior to the state CoordinatorStateEnum::kCommitting, only the source
        // collection's config.collections entry should have donorFields, and only the
        // temporary resharding collection's entry should have recipientFields.
        uassert(5274201,
                fmt::format("reshardingFields must contain exactly one of donorFields and"
                            " recipientFields when the coordinator is in state {}. Got"
                            " reshardingFields {}",
                            CoordinatorState_serializer(reshardingFields.getState()),
                            reshardingFields.toBSON().toString()),
                bool(reshardingFields.getDonorFields()) !=
                    bool(reshardingFields.getRecipientFields()));
    } else {
        // At and after state CoordinatorStateEnum::kCommitting, the temporary
        // resharding collection's config.collections entry has been removed, and so the
        // source collection's entry should have both donorFields and recipientFields.
        uassert(5274202,
                fmt::format("reshardingFields must contain both donorFields and recipientFields "
                            "when the coordinator's state is greater than or equal to "
                            "CoordinatorStateEnum::kCommitting. Got reshardingFields {}",
                            reshardingFields.toBSON().toString()),
                reshardingFields.getDonorFields() && reshardingFields.getRecipientFields());
    }
}

}  // namespace

ReshardingDonorDocument constructDonorDocumentFromReshardingFields(
    const VersionContext& vCtx,
    const NamespaceString& nss,
    const CollectionMetadata& metadata,
    const ReshardingFields& reshardingFields) {
    DonorShardContext donorCtx;
    donorCtx.setState(DonorStateEnum::kPreparingToDonate);

    auto donorDoc = ReshardingDonorDocument{
        std::move(donorCtx), reshardingFields.getDonorFields()->getRecipientShardIds()};

    auto sourceUUID = metadata.getChunkManager()->getUUID();
    auto commonMetadata =
        CommonReshardingMetadata(reshardingFields.getReshardingUUID(),
                                 nss,
                                 sourceUUID,
                                 reshardingFields.getDonorFields()->getTempReshardingNss(),
                                 reshardingFields.getDonorFields()->getReshardingKey().toBSON());
    commonMetadata.setStartTime(reshardingFields.getStartTime());
    commonMetadata.setProvenance(reshardingFields.getProvenance());
    resharding::validatePerformVerification(vCtx, reshardingFields.getPerformVerification());
    commonMetadata.setPerformVerification(reshardingFields.getPerformVerification());

    donorDoc.setCommonReshardingMetadata(std::move(commonMetadata));

    return donorDoc;
}

ReshardingRecipientDocument constructRecipientDocumentFromReshardingFields(
    const VersionContext& vCtx,
    const NamespaceString& nss,
    const CollectionMetadata& metadata,
    const ReshardingFields& reshardingFields) {
    const auto& recipientFields = reshardingFields.getRecipientFields();
    // The recipient state machines are created before the donor shards are prepared to donate but
    // will remain idle until the donor shards are prepared to donate.
    invariant(!recipientFields->getCloneTimestamp());

    RecipientShardContext recipientCtx;
    recipientCtx.setState(RecipientStateEnum::kAwaitingFetchTimestamp);

    auto recipientDoc =
        ReshardingRecipientDocument{std::move(recipientCtx),
                                    recipientFields->getDonorShards(),
                                    recipientFields->getMinimumOperationDurationMillis()};

    auto sourceNss = recipientFields->getSourceNss();
    auto sourceUUID = recipientFields->getSourceUUID();
    auto commonMetadata = CommonReshardingMetadata(reshardingFields.getReshardingUUID(),
                                                   sourceNss,
                                                   sourceUUID,
                                                   nss,
                                                   metadata.getShardKeyPattern().toBSON());
    commonMetadata.setStartTime(reshardingFields.getStartTime());
    commonMetadata.setProvenance(reshardingFields.getProvenance());
    resharding::validatePerformVerification(vCtx, reshardingFields.getPerformVerification());
    commonMetadata.setPerformVerification(reshardingFields.getPerformVerification());

    ReshardingRecipientMetrics metrics;
    metrics.setApproxDocumentsToCopy(recipientFields->getApproxDocumentsToCopy());
    metrics.setApproxBytesToCopy(recipientFields->getApproxBytesToCopy());
    recipientDoc.setMetrics(std::move(metrics));

    recipientDoc.setCommonReshardingMetadata(std::move(commonMetadata));

    if (resharding::gFeatureFlagReshardingSkipCloningAndApplyingIfApplicable.isEnabled(
            vCtx, serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) &&
        !metadata.currentShardHasAnyChunks()) {
        recipientDoc.setSkipCloningAndApplying(true);
    }
    if (resharding::gFeatureFlagReshardingStoreOplogFetcherProgress.isEnabled(
            vCtx, serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        recipientDoc.setStoreOplogFetcherProgress(true);
    }

    recipientDoc.setOplogBatchTaskCount(recipientFields->getOplogBatchTaskCount());

    recipientDoc.setRelaxed(recipientFields->getRelaxed());

    return recipientDoc;
}

void processReshardingFieldsForCollection(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const CollectionMetadata& metadata,
                                          const ReshardingFields& reshardingFields) {
    // Persist the config time to ensure that in case of stepdown next filtering metadata refresh on
    // the new primary will always fetch the latest information.
    auto* const replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (!replCoord->getSettings().isReplSet() || replCoord->getMemberState().primary()) {
        VectorClockMutable::get(opCtx)->waitForDurableConfigTime().get(opCtx);
    }

    if (reshardingFields.getState() == CoordinatorStateEnum::kAborting) {
        // The coordinator encountered an unrecoverable error, both donors and recipients should be
        // made aware.
        processReshardingFieldsForDonorCollection(opCtx, nss, metadata, reshardingFields);
        processReshardingFieldsForRecipientCollection(opCtx, nss, metadata, reshardingFields);
        return;
    }

    verifyValidReshardingFields(reshardingFields);

    if (reshardingFields.getDonorFields()) {
        processReshardingFieldsForDonorCollection(opCtx, nss, metadata, reshardingFields);
    }

    if (reshardingFields.getRecipientFields()) {
        processReshardingFieldsForRecipientCollection(opCtx, nss, metadata, reshardingFields);
    }
}

void clearFilteringMetadata(OperationContext* opCtx, bool scheduleAsyncRefresh) {
    stdx::unordered_set<NamespaceString> namespacesToRefresh;
    for (const NamespaceString& homeToReshardingDocs :
         {NamespaceString::kDonorReshardingOperationsNamespace,
          NamespaceString::kRecipientReshardingOperationsNamespace}) {
        PersistentTaskStore<CommonReshardingMetadata> store(homeToReshardingDocs);

        store.forEach(opCtx, BSONObj{}, [&](CommonReshardingMetadata reshardingDoc) -> bool {
            namespacesToRefresh.insert(reshardingDoc.getSourceNss());
            namespacesToRefresh.insert(reshardingDoc.getTempReshardingNss());

            return true;
        });
    }
    clearFilteringMetadata(opCtx, namespacesToRefresh, scheduleAsyncRefresh);
}

void clearFilteringMetadata(OperationContext* opCtx,
                            stdx::unordered_set<NamespaceString> namespacesToRefresh,
                            bool scheduleAsyncRefresh) {
    auto* catalogCache = Grid::get(opCtx)->catalogCache();

    for (const auto& nss : namespacesToRefresh) {
        if (nss.isTemporaryReshardingCollection()) {
            // We clear the routing information for the temporary resharding namespace to ensure all
            // new donor shard primaries will refresh from the config server and see the chunk
            // distribution for the ongoing resharding operation.
            catalogCache->invalidateCollectionEntry_LINEARIZABLE(nss);
        }

        const auto acquisition = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kWrite),
            MODE_IX);

        CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx, nss)
            ->clearFilteringMetadata(opCtx);

        if (!scheduleAsyncRefresh) {
            continue;
        }

        AsyncTry([svcCtx = opCtx->getServiceContext(), nss] {
            ThreadClient tc("TriggerReshardingRecovery",
                            svcCtx->getService(ClusterRole::ShardServer));
            auto opCtx = tc->makeOperationContext();
            uassertStatusOK(FilteringMetadataCache::get(opCtx.get())
                                ->onCollectionPlacementVersionMismatch(
                                    opCtx.get(), nss, boost::none /* chunkVersionReceived */));
        })
            .until([](const Status& status) {
                if (!status.isOK()) {
                    LOGV2_WARNING(5498101,
                                  "Error on deferred shardVersion recovery execution",
                                  "error"_attr = redact(status));
                }
                return status.isOK();
            })
            .withBackoffBetweenIterations(kExponentialBackoff)
            .on(Grid::get(opCtx)->getExecutorPool()->getFixedExecutor(),
                CancellationToken::uncancelable())
            .getAsync([](auto) {});
    }
}

}  // namespace resharding

}  // namespace mongo
