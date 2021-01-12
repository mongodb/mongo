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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

#include "mongo/db/s/resharding/resharding_coordinator_service.h"

#include "mongo/db/auth/authorization_session_impl.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/logical_session_cache.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/db/vector_clock.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_id.h"
#include "mongo/s/sharded_collections_ddl_parameters_gen.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_util.h"
#include "mongo/util/string_map.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {

using namespace fmt::literals;

MONGO_FAIL_POINT_DEFINE(reshardingPauseCoordinatorInSteadyState);
MONGO_FAIL_POINT_DEFINE(reshardingPauseCoordinatorBeforeDecisionPersisted);

void assertNumDocsModifiedMatchesExpected(const BatchedCommandRequest& request,
                                          const BSONObj& response,
                                          int expected) {
    auto numDocsModified = response.getIntField("n");
    uassert(5030401,
            str::stream() << "Expected to match " << expected << " docs, but only matched "
                          << numDocsModified << " for write request " << request.toString(),
            expected == numDocsModified);
}

void appendShardEntriesToSetBuilder(const ReshardingCoordinatorDocument& coordinatorDoc,
                                    BSONObjBuilder& setBuilder) {
    BSONArrayBuilder donorShards(
        setBuilder.subarrayStart(ReshardingCoordinatorDocument::kDonorShardsFieldName));
    for (const auto& donorShard : coordinatorDoc.getDonorShards()) {
        donorShards.append(donorShard.toBSON());
    }
    donorShards.doneFast();

    BSONArrayBuilder recipientShards(
        setBuilder.subarrayStart(ReshardingCoordinatorDocument::kRecipientShardsFieldName));
    for (const auto& recipientShard : coordinatorDoc.getRecipientShards()) {
        recipientShards.append(recipientShard.toBSON());
    }
    recipientShards.doneFast();
}

void unsetInitializingFields(BSONObjBuilder& updateBuilder) {
    BSONObjBuilder unsetBuilder(updateBuilder.subobjStart("$unset"));
    unsetBuilder.append(ReshardingCoordinatorDocument::kPresetReshardedChunksFieldName, "");
    unsetBuilder.append(ReshardingCoordinatorDocument::kZonesFieldName, "");
    unsetBuilder.doneFast();
}

void writeToCoordinatorStateNss(OperationContext* opCtx,
                                const ReshardingCoordinatorDocument& coordinatorDoc,
                                TxnNumber txnNumber) {
    BatchedCommandRequest request([&] {
        auto nextState = coordinatorDoc.getState();
        switch (nextState) {
            case CoordinatorStateEnum::kInitializing:
                // Insert the new coordinator document.
                return BatchedCommandRequest::buildInsertOp(
                    NamespaceString::kConfigReshardingOperationsNamespace,
                    std::vector<BSONObj>{coordinatorDoc.toBSON()});
            case CoordinatorStateEnum::kDone:
                // Remove the coordinator document.
                return BatchedCommandRequest::buildDeleteOp(
                    NamespaceString::kConfigReshardingOperationsNamespace,
                    BSON("_id" << coordinatorDoc.get_id()),  // query
                    false                                    // multi
                );
            default: {
                // Partially update the coordinator document.
                BSONObjBuilder updateBuilder;
                {
                    BSONObjBuilder setBuilder(updateBuilder.subobjStart("$set"));

                    // Always update the state field.
                    setBuilder.append(ReshardingCoordinatorDocument::kStateFieldName,
                                      CoordinatorState_serializer(coordinatorDoc.getState()));

                    if (auto fetchTimestamp = coordinatorDoc.getFetchTimestamp()) {
                        // If the fetchTimestamp exists, include it in the update.
                        setBuilder.append(ReshardingCoordinatorDocument::kFetchTimestampFieldName,
                                          *fetchTimestamp);
                    }

                    if (auto abortReason = coordinatorDoc.getAbortReason()) {
                        // If the abortReason exists, include it in the update.
                        setBuilder.append(ReshardingCoordinatorDocument::kAbortReasonFieldName,
                                          *abortReason);
                    }

                    if (nextState == CoordinatorStateEnum::kPreparingToDonate) {
                        appendShardEntriesToSetBuilder(coordinatorDoc, setBuilder);
                        setBuilder.doneFast();
                        unsetInitializingFields(updateBuilder);
                    }
                }

                return BatchedCommandRequest::buildUpdateOp(
                    NamespaceString::kConfigReshardingOperationsNamespace,
                    BSON("_id" << coordinatorDoc.get_id()),
                    updateBuilder.obj(),
                    false,  // upsert
                    false   // multi
                );
            }
        }
    }());

    auto expectedNumModified = (request.getBatchType() == BatchedCommandRequest::BatchType_Insert)
        ? boost::none
        : boost::make_optional(1);
    auto res = ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
        opCtx, NamespaceString::kConfigReshardingOperationsNamespace, request, txnNumber);

    if (expectedNumModified) {
        assertNumDocsModifiedMatchesExpected(request, res, *expectedNumModified);
    }
}

/**
 * Extracts the ShardId from each Donor/RecipientShardEntry in participantShardEntries.
 */
template <class T>
std::vector<ShardId> extractShardIds(const std::vector<T>& participantShardEntries) {
    std::vector<ShardId> shardIds(participantShardEntries.size());
    std::transform(participantShardEntries.begin(),
                   participantShardEntries.end(),
                   shardIds.begin(),
                   [](auto& shardEntry) { return shardEntry.getId(); });
    return shardIds;
}

/**
 * Creates reshardingFields.recipientFields for the resharding operation. Note: these should not
 * change once the operation has begun.
 */
TypeCollectionRecipientFields constructRecipientFields(
    const ReshardingCoordinatorDocument& coordinatorDoc) {
    auto donorShardIds = extractShardIds(coordinatorDoc.getDonorShards());
    TypeCollectionRecipientFields recipientFields(
        std::move(donorShardIds), coordinatorDoc.getExistingUUID(), coordinatorDoc.getNss());
    emplaceFetchTimestampIfExists(recipientFields, coordinatorDoc.getFetchTimestamp());
    return recipientFields;
}

BSONObj createReshardingFieldsUpdateForOriginalNss(
    OperationContext* opCtx,
    const ReshardingCoordinatorDocument& coordinatorDoc,
    boost::optional<OID> newCollectionEpoch,
    boost::optional<Timestamp> newCollectionTimestamp) {
    auto nextState = coordinatorDoc.getState();
    switch (nextState) {
        case CoordinatorStateEnum::kInitializing: {
            // Append 'reshardingFields' to the config.collections entry for the original nss
            TypeCollectionReshardingFields originalEntryReshardingFields(coordinatorDoc.get_id());
            originalEntryReshardingFields.setState(coordinatorDoc.getState());
            TypeCollectionDonorFields donorField(coordinatorDoc.getReshardingKey());
            originalEntryReshardingFields.setDonorFields(donorField);

            return BSON("$set" << BSON(CollectionType::kReshardingFieldsFieldName
                                       << originalEntryReshardingFields.toBSON()
                                       << CollectionType::kUpdatedAtFieldName
                                       << opCtx->getServiceContext()->getPreciseClockSource()->now()
                                       << CollectionType::kAllowMigrationsFieldName << false));
        }
        case CoordinatorStateEnum::kDecisionPersisted: {
            // Update the config.collections entry for the original nss to reflect
            // the new sharded collection. Set 'uuid' to the reshardingUUID, 'key' to the new shard
            // key, 'lastmodEpoch' to newCollectionEpoch, and 'timestamp' to
            // newCollectionTimestamp (if newCollectionTimestamp has a value; i.e. when the
            // gShardingFullDDLSupportTimestampedVersion feature flag is enabled). Also update the
            // 'state' field and add the 'recipientFields' to the 'reshardingFields' section.
            auto recipientFields = constructRecipientFields(coordinatorDoc);
            BSONObj setFields =
                BSON("uuid" << coordinatorDoc.get_id() << "key"
                            << coordinatorDoc.getReshardingKey().toBSON() << "lastmodEpoch"
                            << newCollectionEpoch.get() << "lastmod"
                            << opCtx->getServiceContext()->getPreciseClockSource()->now()
                            << "reshardingFields.state"
                            << CoordinatorState_serializer(coordinatorDoc.getState()).toString()
                            << "reshardingFields.recipientFields" << recipientFields.toBSON());
            if (newCollectionTimestamp.has_value()) {
                setFields = setFields.addFields(BSON("timestamp" << newCollectionTimestamp.get()));
            }

            return BSON("$set" << setFields);
        }
        case mongo::CoordinatorStateEnum::kDone:
            // Remove 'reshardingFields' from the config.collections entry
            return BSON(
                "$unset" << BSON(CollectionType::kReshardingFieldsFieldName
                                 << "" << CollectionType::kAllowMigrationsFieldName << "")
                         << "$set"
                         << BSON(CollectionType::kUpdatedAtFieldName
                                 << opCtx->getServiceContext()->getPreciseClockSource()->now()));
        default: {
            // Update the 'state' field, and 'abortReason' field if it exists, in the
            // 'reshardingFields' section.
            BSONObjBuilder updateBuilder;
            {
                BSONObjBuilder setBuilder(updateBuilder.subobjStart("$set"));

                setBuilder.append("reshardingFields.state",
                                  CoordinatorState_serializer(nextState).toString());
                setBuilder.append("lastmod",
                                  opCtx->getServiceContext()->getPreciseClockSource()->now());

                if (auto abortReason = coordinatorDoc.getAbortReason()) {
                    // If the abortReason exists, include it in the update.
                    setBuilder.append("reshardingFields.abortReason", *abortReason);
                }
            }

            return updateBuilder.obj();
        }
    }
}

void updateConfigCollectionsForOriginalNss(OperationContext* opCtx,
                                           const ReshardingCoordinatorDocument& coordinatorDoc,
                                           boost::optional<OID> newCollectionEpoch,
                                           boost::optional<Timestamp> newCollectionTimestamp,
                                           TxnNumber txnNumber) {
    auto writeOp = createReshardingFieldsUpdateForOriginalNss(
        opCtx, coordinatorDoc, newCollectionEpoch, newCollectionTimestamp);

    auto request = BatchedCommandRequest::buildUpdateOp(
        CollectionType::ConfigNS,
        BSON(CollectionType::kNssFieldName << coordinatorDoc.getNss().ns()),  // query
        writeOp,
        false,  // upsert
        false   // multi
    );

    auto res = ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
        opCtx, CollectionType::ConfigNS, request, txnNumber);

    assertNumDocsModifiedMatchesExpected(request, res, 1 /* expected */);
}

void writeToConfigCollectionsForTempNss(OperationContext* opCtx,
                                        const ReshardingCoordinatorDocument& coordinatorDoc,
                                        boost::optional<ChunkVersion> chunkVersion,
                                        boost::optional<const BSONObj&> collation,
                                        TxnNumber txnNumber) {
    BatchedCommandRequest request([&] {
        auto nextState = coordinatorDoc.getState();
        switch (nextState) {
            case CoordinatorStateEnum::kPreparingToDonate: {
                // Insert new entry for the temporary nss into config.collections
                auto collType = resharding::createTempReshardingCollectionType(
                    opCtx, coordinatorDoc, chunkVersion.get(), collation.get());
                return BatchedCommandRequest::buildInsertOp(
                    CollectionType::ConfigNS, std::vector<BSONObj>{collType.toBSON()});
            }
            case CoordinatorStateEnum::kCloning:
                // Update the 'state' and 'fetchTimestamp' fields in the
                // 'reshardingFields.recipient' section
                return BatchedCommandRequest::buildUpdateOp(
                    CollectionType::ConfigNS,
                    BSON(CollectionType::kNssFieldName
                         << coordinatorDoc.getTempReshardingNss().ns()),
                    BSON("$set" << BSON(
                             "reshardingFields.state"
                             << CoordinatorState_serializer(nextState).toString()
                             << "reshardingFields.recipientFields.fetchTimestamp"
                             << coordinatorDoc.getFetchTimestamp().get() << "lastmod"
                             << opCtx->getServiceContext()->getPreciseClockSource()->now())),
                    false,  // upsert
                    false   // multi
                );
            case CoordinatorStateEnum::kDecisionPersisted:
                // Remove the entry for the temporary nss
                return BatchedCommandRequest::buildDeleteOp(
                    CollectionType::ConfigNS,
                    BSON(CollectionType::kNssFieldName
                         << coordinatorDoc.getTempReshardingNss().ns()),
                    false  // multi
                );
            default: {
                // Update the 'state' field, and 'abortReason' field if it exists, in the
                // 'reshardingFields' section.
                BSONObjBuilder updateBuilder;
                {
                    BSONObjBuilder setBuilder(updateBuilder.subobjStart("$set"));

                    setBuilder.append("reshardingFields.state",
                                      CoordinatorState_serializer(nextState).toString());
                    setBuilder.append("lastmod",
                                      opCtx->getServiceContext()->getPreciseClockSource()->now());

                    if (auto abortReason = coordinatorDoc.getAbortReason()) {
                        setBuilder.append("reshardingFields.abortReason", *abortReason);
                    }
                }

                return BatchedCommandRequest::buildUpdateOp(
                    CollectionType::ConfigNS,
                    BSON(CollectionType::kNssFieldName
                         << coordinatorDoc.getTempReshardingNss().ns()),
                    updateBuilder.obj(),
                    false,  // upsert
                    false   // multi
                );
            }
        }
    }());

    auto expectedNumModified = (request.getBatchType() == BatchedCommandRequest::BatchType_Insert)
        ? boost::none
        : boost::make_optional(1);

    auto res = ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
        opCtx, CollectionType::ConfigNS, request, txnNumber);

    if (expectedNumModified) {
        assertNumDocsModifiedMatchesExpected(request, res, *expectedNumModified);
    }
}  // namespace

void insertChunkAndTagDocsForTempNss(OperationContext* opCtx,
                                     std::vector<ChunkType> initialChunks,
                                     std::vector<BSONObj> newZones,
                                     TxnNumber txnNumber) {
    // Insert new initial chunk documents for temp nss
    std::vector<BSONObj> initialChunksBSON(initialChunks.size());
    std::transform(initialChunks.begin(),
                   initialChunks.end(),
                   initialChunksBSON.begin(),
                   [](ChunkType chunk) { return chunk.toConfigBSON(); });

    ShardingCatalogManager::get(opCtx)->insertConfigDocumentsInTxn(
        opCtx, ChunkType::ConfigNS, std::move(initialChunksBSON), txnNumber);

    ShardingCatalogManager::get(opCtx)->insertConfigDocumentsInTxn(
        opCtx, TagsType::ConfigNS, newZones, txnNumber);
}

void removeChunkAndTagsDocsForOriginalNss(OperationContext* opCtx,
                                          const ReshardingCoordinatorDocument& coordinatorDoc,
                                          boost::optional<Timestamp> newCollectionTimestamp,
                                          TxnNumber txnNumber) {
    // Remove all chunk documents for the original nss. We do not know how many chunk docs currently
    // exist, so cannot pass a value for expectedNumModified
    const auto chunksQuery = [&]() {
        if (newCollectionTimestamp) {
            return BSON(ChunkType::collectionUUID() << coordinatorDoc.getExistingUUID());
        } else {
            return BSON(ChunkType::ns(coordinatorDoc.getNss().ns()));
        }
    }();
    ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
        opCtx,
        ChunkType::ConfigNS,
        BatchedCommandRequest::buildDeleteOp(ChunkType::ConfigNS,
                                             chunksQuery,
                                             true  // multi
                                             ),
        txnNumber);

    // Remove all tag documents for the original nss. We do not know how many tag docs currently
    // exist, so cannot pass a value for expectedNumModified
    ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
        opCtx,
        TagsType::ConfigNS,
        BatchedCommandRequest::buildDeleteOp(
            TagsType::ConfigNS,
            BSON(ChunkType::ns(coordinatorDoc.getNss().ns())),  // query
            true                                                // multi
            ),
        txnNumber);
}

void updateChunkAndTagsDocsForTempNss(OperationContext* opCtx,
                                      const ReshardingCoordinatorDocument& coordinatorDoc,
                                      OID newCollectionEpoch,
                                      boost::optional<Timestamp> newCollectionTimestamp,
                                      TxnNumber txnNumber) {
    // Update all chunk documents that currently have 'ns' as the temporary collection namespace
    // such that 'ns' is now the original collection namespace and 'lastmodEpoch' is
    // newCollectionEpoch.
    const auto chunksQuery = [&]() {
        if (newCollectionTimestamp) {
            return BSON(ChunkType::collectionUUID() << coordinatorDoc.get_id());
        } else {
            return BSON(ChunkType::ns(coordinatorDoc.getTempReshardingNss().ns()));
        }
    }();
    const auto chunksUpdate = [&]() {
        if (newCollectionTimestamp) {
            return BSON("$set" << BSON("lastmodEpoch" << newCollectionEpoch));
        } else {
            return BSON("$set" << BSON("ns" << coordinatorDoc.getNss().ns() << "lastmodEpoch"
                                            << newCollectionEpoch));
        }
    }();
    auto chunksRequest = BatchedCommandRequest::buildUpdateOp(ChunkType::ConfigNS,
                                                              chunksQuery,   // query
                                                              chunksUpdate,  // update
                                                              false,         // upsert
                                                              true           // multi
    );

    auto chunksRes = ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
        opCtx, ChunkType::ConfigNS, chunksRequest, txnNumber);

    auto tagsRequest = BatchedCommandRequest::buildUpdateOp(
        TagsType::ConfigNS,
        BSON(TagsType::ns(coordinatorDoc.getTempReshardingNss().ns())),  // query
        BSON("$set" << BSON("ns" << coordinatorDoc.getNss().ns())),      // update
        false,                                                           // upsert
        true                                                             // multi
    );

    // Update the 'ns' field to be the original collection namespace for all tags documents that
    // currently have 'ns' as the temporary collection namespace
    auto tagsRes = ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
        opCtx, TagsType::ConfigNS, tagsRequest, txnNumber);
}

//
// Helper methods for ensuring donors/ recipients are able to notice when certain state transitions
// occur.
//
// Donors/recipients learn when to transition states by noticing a change in shard versions for one
// of the two collections involved in the resharding operations.
//
// Before the resharding operation persists the decision whether to succeed or fail:
// * Donors are notified when the original resharding collection's shard versions are incremented.
// * Recipients are notified when the temporary resharding collection's shard versions are
//   incremented.
//
// After the resharding operation persists its decision:
// * Both donors and recipients are notified when the original resharding collection's shard
//   versions are incremented.
//

/**
 * Maps which participants are to be notified when the coordinator transitions into a given state.
 */
enum class ParticipantsToNotifyEnum {
    kDonors,
    kRecipients,
    kAllParticipantsPostDecisionPersisted,
    kNone
};
stdx::unordered_map<CoordinatorStateEnum, ParticipantsToNotifyEnum> notifyForStateTransition{
    {CoordinatorStateEnum::kUnused, ParticipantsToNotifyEnum::kNone},
    {CoordinatorStateEnum::kInitializing, ParticipantsToNotifyEnum::kNone},
    {CoordinatorStateEnum::kPreparingToDonate, ParticipantsToNotifyEnum::kDonors},
    {CoordinatorStateEnum::kCloning, ParticipantsToNotifyEnum::kRecipients},
    {CoordinatorStateEnum::kApplying, ParticipantsToNotifyEnum::kDonors},
    {CoordinatorStateEnum::kMirroring, ParticipantsToNotifyEnum::kDonors},
    {CoordinatorStateEnum::kDecisionPersisted, ParticipantsToNotifyEnum::kNone},
    {CoordinatorStateEnum::kDone, ParticipantsToNotifyEnum::kNone},
    {CoordinatorStateEnum::kError, ParticipantsToNotifyEnum::kNone},
};

/**
 * Executes metadata changes in a transaction.
 */
void executeMetadataChangesInTxn(
    OperationContext* opCtx,
    unique_function<void(OperationContext*, TxnNumber)> changeMetadataFunc) {
    ShardingCatalogManager::withTransaction(opCtx,
                                            NamespaceString::kConfigReshardingOperationsNamespace,
                                            [&](OperationContext* opCtx, TxnNumber txnNumber) {
                                                changeMetadataFunc(opCtx, txnNumber);
                                            });
}

/**
 * Runs resharding metadata changes in a transaction.
 *
 * This function should only be called if donor and recipient shards DO NOT need to be informed of
 * the updatedCoordinatorDoc's state transition. If donor or recipient shards need to be informed,
 * instead call bumpShardVersionsThenExecuteStateTransitionAndMetadataChangesInTxn().
 */
void executeStateTransitionAndMetadataChangesInTxn(
    OperationContext* opCtx,
    const ReshardingCoordinatorDocument& updatedCoordinatorDoc,
    unique_function<void(OperationContext*, TxnNumber)> changeMetadataFunc) {
    const auto& state = updatedCoordinatorDoc.getState();
    invariant(notifyForStateTransition.find(state) != notifyForStateTransition.end());
    invariant(notifyForStateTransition[state] == ParticipantsToNotifyEnum::kNone);

    // Neither donors nor recipients need to be informed of the transition to
    // updatedCoordinatorDoc's state.
    executeMetadataChangesInTxn(opCtx, std::move(changeMetadataFunc));
}

/**
 * In a single transaction, bumps the shard version for each shard spanning the corresponding
 * resharding collection and executes changeMetadataFunc.
 *
 * This function should only be called if donor or recipient shards need to be informed of the
 * updatedCoordinatorDoc's state transition. If donor or recipient shards do not need to be
 * informed, instead call executeStateTransitionAndMetadataChangesInTxn().
 */
void bumpShardVersionsThenExecuteStateTransitionAndMetadataChangesInTxn(
    OperationContext* opCtx,
    const ReshardingCoordinatorDocument& updatedCoordinatorDoc,
    unique_function<void(OperationContext*, TxnNumber)> changeMetadataFunc) {
    const auto& state = updatedCoordinatorDoc.getState();
    invariant(notifyForStateTransition.find(state) != notifyForStateTransition.end());
    invariant(notifyForStateTransition[state] != ParticipantsToNotifyEnum::kNone);

    auto participantsToNotify = notifyForStateTransition[state];
    if (participantsToNotify == ParticipantsToNotifyEnum::kDonors) {
        // Bump the donor shard versions for the original namespace along with updating the
        // metadata.
        ShardingCatalogManager::get(opCtx)->bumpCollShardVersionsAndChangeMetadataInTxn(
            opCtx,
            updatedCoordinatorDoc.getNss(),
            extractShardIds(updatedCoordinatorDoc.getDonorShards()),
            std::move(changeMetadataFunc));
    } else if (participantsToNotify == ParticipantsToNotifyEnum::kRecipients) {
        // Bump the recipient shard versions for the temporary resharding namespace along with
        // updating the metadata.
        ShardingCatalogManager::get(opCtx)->bumpCollShardVersionsAndChangeMetadataInTxn(
            opCtx,
            updatedCoordinatorDoc.getTempReshardingNss(),
            extractShardIds(updatedCoordinatorDoc.getRecipientShards()),
            std::move(changeMetadataFunc));
    } else if (participantsToNotify ==
               ParticipantsToNotifyEnum::kAllParticipantsPostDecisionPersisted) {
        // Bump the recipient shard versions for the original resharding namespace along with
        // updating the metadata. Only the recipient shards will have chunks for the namespace after
        // the coordinator is in state kDecisionPersisted, bumping chunk versions on the donor
        // shards would not apply.
        ShardingCatalogManager::get(opCtx)->bumpCollShardVersionsAndChangeMetadataInTxn(
            opCtx,
            updatedCoordinatorDoc.getNss(),
            extractShardIds(updatedCoordinatorDoc.getRecipientShards()),
            std::move(changeMetadataFunc));
    }
}
}  // namespace

namespace resharding {
CollectionType createTempReshardingCollectionType(
    OperationContext* opCtx,
    const ReshardingCoordinatorDocument& coordinatorDoc,
    const ChunkVersion& chunkVersion,
    const BSONObj& collation) {
    CollectionType collType(coordinatorDoc.getTempReshardingNss(),
                            chunkVersion.epoch(),
                            opCtx->getServiceContext()->getPreciseClockSource()->now(),
                            coordinatorDoc.get_id());
    collType.setKeyPattern(coordinatorDoc.getReshardingKey());
    collType.setDefaultCollation(collation);
    collType.setUnique(false);
    collType.setTimestamp(chunkVersion.getTimestamp());

    TypeCollectionReshardingFields tempEntryReshardingFields(coordinatorDoc.get_id());
    tempEntryReshardingFields.setState(coordinatorDoc.getState());

    auto recipientFields = constructRecipientFields(coordinatorDoc);
    tempEntryReshardingFields.setRecipientFields(std::move(recipientFields));
    collType.setReshardingFields(std::move(tempEntryReshardingFields));
    collType.setAllowMigrations(false);
    return collType;
}

void insertCoordDocAndChangeOrigCollEntry(OperationContext* opCtx,
                                          const ReshardingCoordinatorDocument& coordinatorDoc) {
    auto originalCollType = Grid::get(opCtx)->catalogClient()->getCollection(
        opCtx, coordinatorDoc.getNss(), repl::ReadConcernLevel::kMajorityReadConcern);
    const auto collation = originalCollType.getDefaultCollation();

    executeMetadataChangesInTxn(opCtx, [&](OperationContext* opCtx, TxnNumber txnNumber) {
        // Insert the coordinator document to config.reshardingOperations.
        writeToCoordinatorStateNss(opCtx, coordinatorDoc, txnNumber);

        // Update the config.collections entry for the original collection to include
        // 'reshardingFields'
        updateConfigCollectionsForOriginalNss(
            opCtx, coordinatorDoc, boost::none, boost::none, txnNumber);
    });
}

ChunkVersion calculateChunkVersionForInitialChunks(OperationContext* opCtx) {
    boost::optional<Timestamp> timestamp;
    if (feature_flags::gShardingFullDDLSupportTimestampedVersion.isEnabled(
            serverGlobalParams.featureCompatibility)) {
        const auto now = VectorClock::get(opCtx)->getTime();
        timestamp = now.clusterTime().asTimestamp();
    }

    return ChunkVersion(1, 0, OID::gen(), timestamp);
}

std::vector<DonorShardEntry> constructDonorShardEntries(const std::set<ShardId>& donorShardIds) {
    std::vector<DonorShardEntry> donorShards;
    std::transform(donorShardIds.begin(),
                   donorShardIds.end(),
                   std::back_inserter(donorShards),
                   [](const ShardId& shardId) -> DonorShardEntry {
                       DonorShardEntry entry{shardId};
                       entry.setState(DonorStateEnum::kUnused);
                       return entry;
                   });
    return donorShards;
}

std::vector<RecipientShardEntry> constructRecipientShardEntries(
    const std::set<ShardId>& recipientShardIds) {
    std::vector<RecipientShardEntry> recipientShards;
    std::transform(recipientShardIds.begin(),
                   recipientShardIds.end(),
                   std::back_inserter(recipientShards),
                   [](const ShardId& shardId) -> RecipientShardEntry {
                       RecipientShardEntry entry{shardId};
                       entry.setState(RecipientStateEnum::kUnused);
                       return entry;
                   });
    return recipientShards;
}

ParticipantShardsAndChunks calculateParticipantShardsAndChunks(
    OperationContext* opCtx, const ReshardingCoordinatorDocument& coordinatorDoc) {
    const auto cm = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfoWithRefresh(
            opCtx, coordinatorDoc.getNss()));

    std::set<ShardId> donorShardIds;
    cm.getAllShardIds(&donorShardIds);

    std::set<ShardId> recipientShardIds;
    std::vector<ChunkType> initialChunks;

    auto version = calculateChunkVersionForInitialChunks(opCtx);

    if (const auto& chunks = coordinatorDoc.getPresetReshardedChunks()) {
        // Use the provided shardIds from presetReshardedChunks to construct the
        // recipient list.
        for (const BSONObj& obj : *chunks) {
            recipientShardIds.emplace(
                obj.getStringField(ReshardedChunk::kRecipientShardIdFieldName));

            auto reshardedChunk =
                ReshardedChunk::parse(IDLParserErrorContext("ReshardedChunk"), obj);
            if (version.getTimestamp()) {
                initialChunks.emplace_back(
                    coordinatorDoc.get_id(),
                    ChunkRange{reshardedChunk.getMin(), reshardedChunk.getMax()},
                    version,
                    reshardedChunk.getRecipientShardId());
            } else {
                initialChunks.emplace_back(
                    coordinatorDoc.getTempReshardingNss(),
                    ChunkRange{reshardedChunk.getMin(), reshardedChunk.getMax()},
                    version,
                    reshardedChunk.getRecipientShardId());
            }
            version.incMinor();
        }
    } else {
        // No presetReshardedChunks were provided, make the recipients list be the same as
        // the donors list by default.
        recipientShardIds = donorShardIds;

        cm.forEachChunk([&](const auto& chunk) {
            // TODO SERVER-49526 Change the range to refer to the new shard key pattern.
            if (version.getTimestamp()) {
                initialChunks.emplace_back(coordinatorDoc.get_id(),
                                           ChunkRange{chunk.getMin(), chunk.getMax()},
                                           version,
                                           chunk.getShardId());
            } else {
                initialChunks.emplace_back(coordinatorDoc.getTempReshardingNss(),
                                           ChunkRange{chunk.getMin(), chunk.getMax()},
                                           version,
                                           chunk.getShardId());
            }
            version.incMinor();
            return true;
        });
    }

    return {constructDonorShardEntries(donorShardIds),
            constructRecipientShardEntries(recipientShardIds),
            initialChunks};
}

void writeParticipantShardsAndTempCollInfo(
    OperationContext* opCtx,
    const ReshardingCoordinatorDocument& updatedCoordinatorDoc,
    std::vector<ChunkType> initialChunks,
    std::vector<BSONObj> zones) {
    bumpShardVersionsThenExecuteStateTransitionAndMetadataChangesInTxn(
        opCtx, updatedCoordinatorDoc, [&](OperationContext* opCtx, TxnNumber txnNumber) {
            // Update on-disk state to reflect latest state transition.
            writeToCoordinatorStateNss(opCtx, updatedCoordinatorDoc, txnNumber);
            updateConfigCollectionsForOriginalNss(
                opCtx, updatedCoordinatorDoc, boost::none, boost::none, txnNumber);

            // Insert the config.collections entry for the temporary resharding collection. The
            // chunks all have the same epoch, so picking the last chunk here is arbitrary.
            auto chunkVersion = initialChunks.back().getVersion();
            writeToConfigCollectionsForTempNss(
                opCtx, updatedCoordinatorDoc, chunkVersion, CollationSpec::kSimpleSpec, txnNumber);

            insertChunkAndTagDocsForTempNss(opCtx, std::move(initialChunks), zones, txnNumber);
        });
}

void writeDecisionPersistedState(OperationContext* opCtx,
                                 const ReshardingCoordinatorDocument& coordinatorDoc,
                                 OID newCollectionEpoch,
                                 boost::optional<Timestamp> newCollectionTimestamp) {
    executeStateTransitionAndMetadataChangesInTxn(
        opCtx, coordinatorDoc, [&](OperationContext* opCtx, TxnNumber txnNumber) {
            // Update the config.reshardingOperations entry
            writeToCoordinatorStateNss(opCtx, coordinatorDoc, txnNumber);

            // Remove the config.collections entry for the temporary collection
            writeToConfigCollectionsForTempNss(
                opCtx, coordinatorDoc, boost::none, boost::none, txnNumber);

            // Update the config.collections entry for the original namespace to reflect the new
            // shard key, new epoch, and new UUID
            updateConfigCollectionsForOriginalNss(
                opCtx, coordinatorDoc, newCollectionEpoch, newCollectionTimestamp, txnNumber);

            // Remove all chunk and tag documents associated with the original collection, then
            // update the chunk and tag docs currently associated with the temp nss to be associated
            // with the original nss
            removeChunkAndTagsDocsForOriginalNss(
                opCtx, coordinatorDoc, newCollectionTimestamp, txnNumber);
            updateChunkAndTagsDocsForTempNss(
                opCtx, coordinatorDoc, newCollectionEpoch, newCollectionTimestamp, txnNumber);
        });
}

void writeStateTransitionAndCatalogUpdatesThenBumpShardVersions(
    OperationContext* opCtx, const ReshardingCoordinatorDocument& coordinatorDoc) {
    // Run updates to config.reshardingOperations and config.collections in a transaction
    auto nextState = coordinatorDoc.getState();
    invariant(notifyForStateTransition.find(nextState) != notifyForStateTransition.end());
    // TODO SERVER-51800 Remove special casing for kError.
    invariant(nextState == CoordinatorStateEnum::kError ||
                  notifyForStateTransition[nextState] != ParticipantsToNotifyEnum::kNone,
              "failed to write state transition with nextState {}"_format(
                  CoordinatorState_serializer(nextState)));

    // Resharding metadata changes to be executed.
    auto changeMetadataFunc = [&](OperationContext* opCtx, TxnNumber txnNumber) {
        // Update the config.reshardingOperations entry
        writeToCoordinatorStateNss(opCtx, coordinatorDoc, txnNumber);

        // Update the config.collections entry for the original collection
        updateConfigCollectionsForOriginalNss(
            opCtx, coordinatorDoc, boost::none, boost::none, txnNumber);

        // Update the config.collections entry for the temporary resharding collection. If we've
        // already persisted the decision that the operation will succeed, we've removed the entry
        // for the temporary collection and updated the entry with original namespace to have the
        // new shard key, UUID, and epoch
        if (nextState < CoordinatorStateEnum::kDecisionPersisted ||
            nextState == CoordinatorStateEnum::kError) {
            writeToConfigCollectionsForTempNss(
                opCtx, coordinatorDoc, boost::none, boost::none, txnNumber);
        }
    };

    // TODO SERVER-51800 Remove special casing for kError.
    if (nextState == CoordinatorStateEnum::kError) {
        executeStateTransitionAndMetadataChangesInTxn(
            opCtx, coordinatorDoc, std::move(changeMetadataFunc));
        return;
    }

    bumpShardVersionsThenExecuteStateTransitionAndMetadataChangesInTxn(
        opCtx, coordinatorDoc, std::move(changeMetadataFunc));
}

void removeCoordinatorDocAndReshardingFields(OperationContext* opCtx,
                                             const ReshardingCoordinatorDocument& coordinatorDoc) {
    invariant(coordinatorDoc.getState() == CoordinatorStateEnum::kDecisionPersisted);

    ReshardingCoordinatorDocument updatedCoordinatorDoc = coordinatorDoc;
    updatedCoordinatorDoc.setState(CoordinatorStateEnum::kDone);

    executeStateTransitionAndMetadataChangesInTxn(
        opCtx, updatedCoordinatorDoc, [&](OperationContext* opCtx, TxnNumber txnNumber) {
            // Remove entry for this resharding operation from config.reshardingOperations
            writeToCoordinatorStateNss(opCtx, updatedCoordinatorDoc, txnNumber);

            // Remove the resharding fields from the config.collections entry
            updateConfigCollectionsForOriginalNss(
                opCtx, updatedCoordinatorDoc, boost::none, boost::none, txnNumber);
        });
}
}  // namespace resharding

std::shared_ptr<repl::PrimaryOnlyService::Instance> ReshardingCoordinatorService::constructInstance(
    BSONObj initialState) const {
    return std::make_shared<ReshardingCoordinator>(std::move(initialState));
}

ReshardingCoordinatorService::ReshardingCoordinator::ReshardingCoordinator(const BSONObj& state)
    : PrimaryOnlyService::TypedInstance<ReshardingCoordinator>(),
      _id(state["_id"].wrap().getOwned()),
      _coordinatorDoc(ReshardingCoordinatorDocument::parse(
          IDLParserErrorContext("ReshardingCoordinatorStateDoc"), state)) {
    _reshardingCoordinatorObserver = std::make_shared<ReshardingCoordinatorObserver>();
}

ReshardingCoordinatorService::ReshardingCoordinator::~ReshardingCoordinator() {
    stdx::lock_guard<Latch> lg(_mutex);
    invariant(_completionPromise.getFuture().isReady());
}

void ReshardingCoordinatorService::ReshardingCoordinator::installCoordinatorDoc(
    const ReshardingCoordinatorDocument& doc) {
    invariant(doc.get_id() == _coordinatorDoc.get_id());

    LOGV2_INFO(5343001,
               "Transitioned resharding coordinator state",
               "newState"_attr = CoordinatorState_serializer(doc.getState()),
               "oldState"_attr = CoordinatorState_serializer(_coordinatorDoc.getState()),
               "ns"_attr = doc.getNss(),
               "collectionUUID"_attr = doc.getCommonReshardingMetadata().getExistingUUID(),
               "reshardingUUID"_attr = doc.get_id());

    _coordinatorDoc = doc;
}

SemiFuture<void> ReshardingCoordinatorService::ReshardingCoordinator::run(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancelationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this, executor] { _insertCoordDocAndChangeOrigCollEntry(); })
        .then([this, executor] { _calculateParticipantsAndChunksThenWriteToDisk(); })
        .then([this, executor] { _tellAllDonorsToRefresh(executor); })
        .then([this, executor] { return _awaitAllDonorsReadyToDonate(executor); })
        .then([this, executor] { _tellAllRecipientsToRefresh(executor); })
        .then([this, executor] { return _awaitAllRecipientsFinishedCloning(executor); })
        .then([this, executor] { _tellAllDonorsToRefresh(executor); })
        .then([this, executor] { return _awaitAllRecipientsFinishedApplying(executor); })
        .then([this, executor] { _tellAllDonorsToRefresh(executor); })
        .then([this, executor] { return _awaitAllRecipientsInStrictConsistency(executor); })
        .then([this](const ReshardingCoordinatorDocument& updatedCoordinatorDoc) {
            return _persistDecision(updatedCoordinatorDoc);
        })
        .then([this, executor] { _tellAllParticipantsToRefresh(executor); })
        .then([this, self = shared_from_this(), executor] {
            // The shared_ptr maintaining the ReshardingCoordinatorService Instance object gets
            // deleted from the PrimaryOnlyService's map. Thus, shared_from_this() is necessary to
            // keep 'this' pointer alive for the remaining callbacks.
            return _awaitAllParticipantShardsRenamedOrDroppedOriginalCollection(executor);
        })
        .onError([this, self = shared_from_this(), executor](Status status) {
            stdx::lock_guard<Latch> lg(_mutex);
            if (_completionPromise.getFuture().isReady()) {
                // interrupt() was called before we got here.
                return status;
            }

            LOGV2(4956902,
                  "Resharding failed",
                  "namespace"_attr = _coordinatorDoc.getNss().ns(),
                  "newShardKeyPattern"_attr = _coordinatorDoc.getReshardingKey(),
                  "error"_attr = status);

            _updateCoordinatorDocStateAndCatalogEntries(
                CoordinatorStateEnum::kError, _coordinatorDoc, boost::none, status);

            // TODO wait for donors and recipients to abort the operation and clean up state
            _tellAllRecipientsToRefresh(executor);
            _tellAllParticipantsToRefresh(executor);

            return status;
        })
        .onCompletion([this, self = shared_from_this()](Status status) {
            stdx::lock_guard<Latch> lg(_mutex);
            if (_completionPromise.getFuture().isReady()) {
                // interrupt() was called before we got here.
                return;
            }

            if (status.isOK()) {
                _completionPromise.emplaceValue();
            } else {
                _completionPromise.setError(status);
            }
        })
        .semi();
}

void ReshardingCoordinatorService::ReshardingCoordinator::interrupt(Status status) {
    // Resolve any unresolved promises to avoid hanging.
    stdx::lock_guard<Latch> lg(_mutex);

    _reshardingCoordinatorObserver->interrupt(status);

    if (!_completionPromise.getFuture().isReady()) {
        _completionPromise.setError(status);
    }
}

std::shared_ptr<ReshardingCoordinatorObserver>
ReshardingCoordinatorService::ReshardingCoordinator::getObserver() {
    return _reshardingCoordinatorObserver;
}

void ReshardingCoordinatorService::ReshardingCoordinator::_insertCoordDocAndChangeOrigCollEntry() {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kUnused) {
        return;
    }

    auto opCtx = cc().makeOperationContext();
    ReshardingCoordinatorDocument updatedCoordinatorDoc = _coordinatorDoc;
    updatedCoordinatorDoc.setState(CoordinatorStateEnum::kInitializing);

    resharding::insertCoordDocAndChangeOrigCollEntry(opCtx.get(), updatedCoordinatorDoc);
    installCoordinatorDoc(updatedCoordinatorDoc);
}

void ReshardingCoordinatorService::ReshardingCoordinator::
    _calculateParticipantsAndChunksThenWriteToDisk() {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kInitializing) {
        return;
    }

    auto opCtx = cc().makeOperationContext();
    ReshardingCoordinatorDocument updatedCoordinatorDoc = _coordinatorDoc;

    auto shardsAndChunks =
        resharding::calculateParticipantShardsAndChunks(opCtx.get(), updatedCoordinatorDoc);

    updatedCoordinatorDoc.setDonorShards(std::move(shardsAndChunks.donorShards));
    updatedCoordinatorDoc.setRecipientShards(std::move(shardsAndChunks.recipientShards));
    updatedCoordinatorDoc.setState(CoordinatorStateEnum::kPreparingToDonate);

    // Remove the presetReshardedChunks and zones from the coordinator document to reduce
    // the possibility of the document reaching the BSONObj size constraint.
    std::vector<BSONObj> zones;
    if (updatedCoordinatorDoc.getZones()) {
        zones = std::move(updatedCoordinatorDoc.getZones().get());
    }
    updatedCoordinatorDoc.setPresetReshardedChunks(boost::none);
    updatedCoordinatorDoc.setZones(boost::none);

    resharding::writeParticipantShardsAndTempCollInfo(opCtx.get(),
                                                      updatedCoordinatorDoc,
                                                      std::move(shardsAndChunks.initialChunks),
                                                      std::move(zones));
    installCoordinatorDoc(updatedCoordinatorDoc);
};

ExecutorFuture<void>
ReshardingCoordinatorService::ReshardingCoordinator::_awaitAllDonorsReadyToDonate(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kPreparingToDonate) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return _reshardingCoordinatorObserver->awaitAllDonorsReadyToDonate()
        .thenRunOn(**executor)
        .then([this](ReshardingCoordinatorDocument coordinatorDocChangedOnDisk) {
            auto highestMinFetchTimestamp =
                getHighestMinFetchTimestamp(coordinatorDocChangedOnDisk.getDonorShards());
            _updateCoordinatorDocStateAndCatalogEntries(CoordinatorStateEnum::kCloning,
                                                        coordinatorDocChangedOnDisk,
                                                        highestMinFetchTimestamp);
        });
}

ExecutorFuture<void>
ReshardingCoordinatorService::ReshardingCoordinator::_awaitAllRecipientsFinishedCloning(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kCloning) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return _reshardingCoordinatorObserver->awaitAllRecipientsFinishedCloning()
        .thenRunOn(**executor)
        .then([this](ReshardingCoordinatorDocument coordinatorDocChangedOnDisk) {
            this->_updateCoordinatorDocStateAndCatalogEntries(CoordinatorStateEnum::kApplying,
                                                              coordinatorDocChangedOnDisk);
        });
}

ExecutorFuture<void>
ReshardingCoordinatorService::ReshardingCoordinator::_awaitAllRecipientsFinishedApplying(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kApplying) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return _reshardingCoordinatorObserver->awaitAllRecipientsFinishedApplying()
        .thenRunOn(**executor)
        .then([this](ReshardingCoordinatorDocument coordinatorDocChangedOnDisk) {
            {
                auto opCtx = cc().makeOperationContext();
                reshardingPauseCoordinatorInSteadyState.pauseWhileSet(opCtx.get());
            }

            this->_updateCoordinatorDocStateAndCatalogEntries(CoordinatorStateEnum::kMirroring,
                                                              coordinatorDocChangedOnDisk);
        });
}

SharedSemiFuture<ReshardingCoordinatorDocument>
ReshardingCoordinatorService::ReshardingCoordinator::_awaitAllRecipientsInStrictConsistency(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kMirroring) {
        // If in recovery, just return the existing _stateDoc.
        return _coordinatorDoc;
    }

    return _reshardingCoordinatorObserver->awaitAllRecipientsInStrictConsistency();
}

Future<void> ReshardingCoordinatorService::ReshardingCoordinator::_persistDecision(
    const ReshardingCoordinatorDocument& coordinatorDoc) {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kMirroring) {
        return Status::OK();
    }

    ReshardingCoordinatorDocument updatedCoordinatorDoc = coordinatorDoc;
    updatedCoordinatorDoc.setState(CoordinatorStateEnum::kDecisionPersisted);

    auto opCtx = cc().makeOperationContext();
    reshardingPauseCoordinatorBeforeDecisionPersisted.pauseWhileSet(opCtx.get());

    // The new epoch and timestamp to use for the resharded collection to indicate that the
    // collection is a new incarnation of the namespace
    auto newCollectionEpoch = OID::gen();
    boost::optional<Timestamp> newCollectionTimestamp;
    if (feature_flags::gShardingFullDDLSupportTimestampedVersion.isEnabled(
            serverGlobalParams.featureCompatibility)) {
        auto now = VectorClock::get(opCtx.get())->getTime();
        newCollectionTimestamp = now.clusterTime().asTimestamp();
    }

    resharding::writeDecisionPersistedState(
        opCtx.get(), updatedCoordinatorDoc, newCollectionEpoch, newCollectionTimestamp);

    // Update the in memory state
    installCoordinatorDoc(updatedCoordinatorDoc);

    return Status::OK();
};

ExecutorFuture<void> ReshardingCoordinatorService::ReshardingCoordinator::
    _awaitAllParticipantShardsRenamedOrDroppedOriginalCollection(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kDecisionPersisted) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    std::vector<ExecutorFuture<ReshardingCoordinatorDocument>> futures;
    futures.emplace_back(
        _reshardingCoordinatorObserver->awaitAllRecipientsRenamedCollection().thenRunOn(
            **executor));
    futures.emplace_back(
        _reshardingCoordinatorObserver->awaitAllDonorsDroppedOriginalCollection().thenRunOn(
            **executor));

    return whenAllSucceed(std::move(futures))
        .thenRunOn(**executor)
        .then([executor](const auto& coordinatorDocsChangedOnDisk) {
            auto opCtx = cc().makeOperationContext();
            resharding::removeCoordinatorDocAndReshardingFields(opCtx.get(),
                                                                coordinatorDocsChangedOnDisk[1]);
        });
}

void ReshardingCoordinatorService::ReshardingCoordinator::
    _updateCoordinatorDocStateAndCatalogEntries(CoordinatorStateEnum nextState,
                                                ReshardingCoordinatorDocument coordinatorDoc,
                                                boost::optional<Timestamp> fetchTimestamp,
                                                boost::optional<Status> abortReason) {
    // Build new state doc for coordinator state update
    ReshardingCoordinatorDocument updatedCoordinatorDoc = coordinatorDoc;
    updatedCoordinatorDoc.setState(nextState);
    emplaceFetchTimestampIfExists(updatedCoordinatorDoc, std::move(fetchTimestamp));
    emplaceAbortReasonIfExists(updatedCoordinatorDoc, abortReason);

    auto opCtx = cc().makeOperationContext();
    resharding::writeStateTransitionAndCatalogUpdatesThenBumpShardVersions(opCtx.get(),
                                                                           updatedCoordinatorDoc);

    // Update in-memory coordinator doc
    installCoordinatorDoc(updatedCoordinatorDoc);
}

void ReshardingCoordinatorService::ReshardingCoordinator::_tellAllRecipientsToRefresh(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    auto opCtx = cc().makeOperationContext();
    auto recipientIds = extractShardIds(_coordinatorDoc.getRecipientShards());

    NamespaceString nssToRefresh;
    // Refresh the temporary namespace if the coordinator is in state 'kError' just in case the
    // previous state was before 'kDecisionPersisted'. A refresh of recipients while in
    // 'kDecisionPersisted' should be accompanied by a refresh of all participants for the original
    // namespace to ensure correctness.
    if (_coordinatorDoc.getState() < CoordinatorStateEnum::kDecisionPersisted ||
        _coordinatorDoc.getState() == CoordinatorStateEnum::kError) {
        nssToRefresh = _coordinatorDoc.getTempReshardingNss();
    } else {
        nssToRefresh = _coordinatorDoc.getNss();
    }

    tellShardsToRefresh(opCtx.get(), recipientIds, nssToRefresh, **executor);
}

void ReshardingCoordinatorService::ReshardingCoordinator::_tellAllDonorsToRefresh(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    auto opCtx = cc().makeOperationContext();
    auto donorIds = extractShardIds(_coordinatorDoc.getDonorShards());
    tellShardsToRefresh(opCtx.get(), donorIds, _coordinatorDoc.getNss(), **executor);
}

void ReshardingCoordinatorService::ReshardingCoordinator::_tellAllParticipantsToRefresh(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    auto opCtx = cc().makeOperationContext();

    auto donorShardIds = extractShardIds(_coordinatorDoc.getDonorShards());
    auto recipientShardIds = extractShardIds(_coordinatorDoc.getRecipientShards());
    std::set<ShardId> participantShardIds{donorShardIds.begin(), donorShardIds.end()};
    participantShardIds.insert(recipientShardIds.begin(), recipientShardIds.end());

    tellShardsToRefresh(opCtx.get(),
                        {participantShardIds.begin(), participantShardIds.end()},
                        _coordinatorDoc.getNss(),
                        **executor);
}

}  // namespace mongo
