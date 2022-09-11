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

#include <fmt/format.h>

#include "mongo/db/persistent_task_store.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/storage/duplicate_key_error_info.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/unordered_set.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

namespace mongo {
namespace resharding {

using DonorStateMachine = ReshardingDonorService::DonorStateMachine;
using RecipientStateMachine = ReshardingRecipientService::RecipientStateMachine;

namespace {
using namespace fmt::literals;

const Backoff kExponentialBackoff(Seconds(1), Milliseconds::max());

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
        StateMachine::insertStateDocument(opCtx, doc);

        auto registry = repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext());
        auto service = registry->lookupServiceByName(Service::kServiceName);
        StateMachine::getOrCreate(opCtx, service, doc.toBSON());
    } catch (const ExceptionForCat<ErrorCategory::NotPrimaryError>&) {
        // resharding::processReshardingFieldsForCollection() is called on both primary and
        // secondary nodes as part of the shard version being refreshed. Due to the RSTL lock not
        // being held throughout the shard version refresh, it is also possible for the node to
        // arbitrarily step down and step up during the shard version refresh. Rather than
        // attempt to prevent replica set member state transitions during the shard version refresh,
        // we instead swallow the NotPrimaryError exception. This is safe because there is no work a
        // secondary (or primary which stepped down) must do for an active resharding operation upon
        // refreshing its shard version. The primary is solely responsible for advancing the
        // participant state as a result of the shard version refresh.
    } catch (const ExceptionFor<ErrorCodes::DuplicateKey>& ex) {
        // Similar to the ErrorCategory::NotPrimaryError clause above, it is theoretically possible
        // for a series of stepdowns and step-ups to lead a scenario where a stale but now
        // re-elected primary attempts to insert the state document when another node which was
        // primary had already done so. Again, rather than attempt to prevent replica set member
        // state transitions during the shard version refresh, we instead swallow the DuplicateKey
        // exception. This is safe because PrimaryOnlyService::onStepUp() will have constructed a
        // new instance of the resharding state machine.
        auto dupeKeyInfo = ex.extraInfo<DuplicateKeyErrorInfo>();
        invariant(dupeKeyInfo->getDuplicatedKeyValue().binaryEqual(
            BSON("_id" << doc.getReshardingUUID())));
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

    auto donorDoc = constructDonorDocumentFromReshardingFields(nss, metadata, reshardingFields);
    createReshardingStateMachine<ReshardingDonorService,
                                 DonorStateMachine,
                                 ReshardingDonorDocument>(opCtx, donorDoc);
}

bool isCurrentShardPrimary(OperationContext* opCtx, const CollectionMetadata& metadata) {
    return metadata.getChunkManager()->dbPrimary() == ShardingState::get(opCtx)->shardId();
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
        recipientStateMachine->get()->onReshardingFieldsChanges(opCtx, reshardingFields);
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
    if (!isCurrentShardPrimary(opCtx, metadata) && !metadata.currentShardHasAnyChunks()) {
        return;
    }

    auto recipientDoc =
        constructRecipientDocumentFromReshardingFields(opCtx, nss, metadata, reshardingFields);
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
    donorDoc.setCommonReshardingMetadata(std::move(commonMetadata));

    return donorDoc;
}

ReshardingRecipientDocument constructRecipientDocumentFromReshardingFields(
    OperationContext* opCtx,
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

    ReshardingRecipientMetrics metrics;
    metrics.setApproxDocumentsToCopy(recipientFields->getApproxDocumentsToCopy());
    metrics.setApproxBytesToCopy(recipientFields->getApproxBytesToCopy());
    recipientDoc.setMetrics(std::move(metrics));

    recipientDoc.setCommonReshardingMetadata(std::move(commonMetadata));

    return recipientDoc;
}

void processReshardingFieldsForCollection(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const CollectionMetadata& metadata,
                                          const ReshardingFields& reshardingFields) {
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

        AutoGetCollection autoColl(opCtx, nss, MODE_IX);
        CollectionShardingRuntime::get(opCtx, nss)->clearFilteringMetadata(opCtx);

        if (!scheduleAsyncRefresh) {
            continue;
        }

        AsyncTry([svcCtx = opCtx->getServiceContext(), nss] {
            ThreadClient tc("TriggerReshardingRecovery", svcCtx);
            {
                stdx::lock_guard<Client> lk(*tc.get());
                tc->setSystemOperationKillableByStepdown(lk);
            }

            auto opCtx = tc->makeOperationContext();
            onShardVersionMismatch(opCtx.get(), nss, boost::none /* shardVersionReceived */);
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
