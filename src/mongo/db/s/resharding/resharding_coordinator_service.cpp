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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/db/s/resharding/resharding_coordinator_service.h"

#include "mongo/db/auth/authorization_session_impl.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/logical_session_cache.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_id.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/string_map.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {

BatchedCommandRequest buildInsertOp(const NamespaceString& nss, std::vector<BSONObj> docs) {
    BatchedCommandRequest request([&] {
        write_ops::Insert insertOp(nss);
        insertOp.setDocuments(docs);
        return insertOp;
    }());

    return request;
}

BatchedCommandRequest buildDeleteOp(const NamespaceString& nss,
                                    const BSONObj& query,
                                    bool multiDelete) {
    BatchedCommandRequest request([&] {
        write_ops::Delete deleteOp(nss);
        deleteOp.setDeletes({[&] {
            write_ops::DeleteOpEntry entry;
            entry.setQ(query);
            entry.setMulti(multiDelete);
            return entry;
        }()});
        return deleteOp;
    }());

    return request;
}

BatchedCommandRequest buildUpdateOp(const NamespaceString& nss,
                                    const BSONObj& query,
                                    const BSONObj& update,
                                    bool upsert,
                                    bool multi) {
    BatchedCommandRequest request([&] {
        write_ops::Update updateOp(nss);
        updateOp.setUpdates({[&] {
            write_ops::UpdateOpEntry entry;
            entry.setQ(query);
            entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(update));
            entry.setUpsert(upsert);
            entry.setMulti(multi);
            return entry;
        }()});
        return updateOp;
    }());

    return request;
}

void assertNumDocsModifiedMatchesExpected(const BatchedCommandRequest& request,
                                          const BSONObj& response,
                                          int expected) {
    auto numDocsModified = response.getIntField("n");
    uassert(5030401,
            str::stream() << "Expected to match " << expected << " docs, but only matched "
                          << numDocsModified << " for write request " << request.toString(),
            expected == numDocsModified);
}

void writeToCoordinatorStateNss(OperationContext* opCtx,
                                const ReshardingCoordinatorDocument& coordinatorDoc,
                                TxnNumber txnNumber) {
    BatchedCommandRequest request([&] {
        auto nextState = coordinatorDoc.getState();
        switch (nextState) {
            case CoordinatorStateEnum::kPreparingToDonate:
                // Insert the new coordinator state document
                return buildInsertOp(NamespaceString::kConfigReshardingOperationsNamespace,
                                     std::vector<BSONObj>{coordinatorDoc.toBSON()});
            case CoordinatorStateEnum::kDone:
                // Remove the coordinator state document
                return buildDeleteOp(NamespaceString::kConfigReshardingOperationsNamespace,
                                     BSON("_id" << coordinatorDoc.get_id()),  // query
                                     false                                    // multi
                );
            default:
                // Replacement update for the coordinator state document
                return buildUpdateOp(NamespaceString::kConfigReshardingOperationsNamespace,
                                     BSON("_id" << coordinatorDoc.get_id()),
                                     coordinatorDoc.toBSON(),
                                     false,  // upsert
                                     false   // multi
                );
        }
    }());

    auto expectedNumModified = (request.getBatchType() == BatchedCommandRequest::BatchType_Insert)
        ? boost::none
        : boost::make_optional(1);
    auto res = ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
        opCtx,
        NamespaceString::kConfigReshardingOperationsNamespace,
        std::move(request),
        txnNumber);

    if (expectedNumModified) {
        assertNumDocsModifiedMatchesExpected(request, res, *expectedNumModified);
    }
}

BSONObj createReshardingFieldsUpdateForOriginalNss(
    OperationContext* opCtx,
    const ReshardingCoordinatorDocument& coordinatorDoc,
    boost::optional<OID> newCollectionEpoch) {
    auto nextState = coordinatorDoc.getState();
    switch (nextState) {
        case CoordinatorStateEnum::kPreparingToDonate: {
            // Append 'reshardingFields' to the config.collections entry for the original nss
            TypeCollectionReshardingFields originalEntryReshardingFields(coordinatorDoc.get_id());
            originalEntryReshardingFields.setState(coordinatorDoc.getState());
            TypeCollectionDonorFields donorField(coordinatorDoc.getReshardingKey());
            originalEntryReshardingFields.setDonorFields(donorField);

            return BSON(
                "$set" << BSON("reshardingFields"
                               << originalEntryReshardingFields.toBSON() << "lastmod"
                               << opCtx->getServiceContext()->getPreciseClockSource()->now()));
        }
        case CoordinatorStateEnum::kCommitted:
            // Update the config.collections entry for the original nss to reflect
            // the new sharded collection. Set 'uuid' to the reshardingUUID, 'key' to the new shard
            // key, and 'lastmodEpoch' to newCollectionEpoch. Also update the 'state' field in the
            // 'reshardingFields' section
            return BSON("$set" << BSON(
                            "uuid"
                            << coordinatorDoc.get_id() << "key" << coordinatorDoc.getReshardingKey()
                            << "lastmodEpoch" << newCollectionEpoch.get() << "lastmod"
                            << opCtx->getServiceContext()->getPreciseClockSource()->now()
                            << "reshardingFields.state"
                            << CoordinatorState_serializer(coordinatorDoc.getState()).toString()));
        case mongo::CoordinatorStateEnum::kDone:
            // Remove 'reshardingFields' from the config.collections entry
            return BSON(
                "$unset" << BSON("reshardingFields"
                                 << "")
                         << "$set"
                         << BSON("lastmod"
                                 << opCtx->getServiceContext()->getPreciseClockSource()->now()));
        default:
            // Update the 'state' field in the 'reshardingFields' section
            return BSON(
                "$set" << BSON("reshardingFields.state"
                               << CoordinatorState_serializer(nextState).toString() << "lastmod"
                               << opCtx->getServiceContext()->getPreciseClockSource()->now()));
    }
}

void updateConfigCollectionsForOriginalNss(OperationContext* opCtx,
                                           const ReshardingCoordinatorDocument& coordinatorDoc,
                                           boost::optional<OID> newCollectionEpoch,
                                           TxnNumber txnNumber) {
    auto writeOp =
        createReshardingFieldsUpdateForOriginalNss(opCtx, coordinatorDoc, newCollectionEpoch);

    auto request =
        buildUpdateOp(CollectionType::ConfigNS,
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
                return buildInsertOp(CollectionType::ConfigNS,
                                     std::vector<BSONObj>{collType.toBSON()});
            }
            case CoordinatorStateEnum::kCloning:
                // Update the 'state' and 'fetchTimestamp' fields in the
                // 'reshardingFields.recipient' section
                return buildUpdateOp(
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
            case CoordinatorStateEnum::kCommitted:
                // Remove the entry for the temporary nss
                return buildDeleteOp(CollectionType::ConfigNS,
                                     BSON(CollectionType::kNssFieldName
                                          << coordinatorDoc.getTempReshardingNss().ns()),
                                     false  // multi
                );
            default:
                // Update the 'state' field in the 'reshardingFields' section
                return buildUpdateOp(
                    CollectionType::ConfigNS,
                    BSON(CollectionType::kNssFieldName
                         << coordinatorDoc.getTempReshardingNss().ns()),
                    BSON("$set" << BSON(
                             "reshardingFields.state"
                             << CoordinatorState_serializer(nextState).toString() << "lastmod"
                             << opCtx->getServiceContext()->getPreciseClockSource()->now())),
                    false,  // upsert
                    false   // multi
                );
        }
    }());

    auto expectedNumModified = (request.getBatchType() == BatchedCommandRequest::BatchType_Insert)
        ? boost::none
        : boost::make_optional(1);

    auto res = ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
        opCtx, CollectionType::ConfigNS, std::move(request), txnNumber);

    if (expectedNumModified) {
        assertNumDocsModifiedMatchesExpected(request, res, *expectedNumModified);
    }
}

void insertChunkAndTagDocsForTempNss(OperationContext* opCtx,
                                     std::vector<ChunkType> initialChunks,
                                     std::vector<TagsType> newZones,
                                     TxnNumber txnNumber) {
    // Insert new initial chunk documents for temp nss
    std::vector<BSONObj> initialChunksBSON(initialChunks.size());
    std::transform(initialChunks.begin(),
                   initialChunks.end(),
                   initialChunksBSON.begin(),
                   [](ChunkType chunk) { return chunk.toConfigBSON(); });

    ShardingCatalogManager::get(opCtx)->insertConfigDocumentsInTxn(
        opCtx, ChunkType::ConfigNS, std::move(initialChunksBSON), txnNumber);

    // Insert tag documents for temp nss
    std::vector<BSONObj> zonesBSON(newZones.size());
    std::transform(newZones.begin(), newZones.end(), zonesBSON.begin(), [](TagsType chunk) {
        return chunk.toBSON();
    });

    ShardingCatalogManager::get(opCtx)->insertConfigDocumentsInTxn(
        opCtx, TagsType::ConfigNS, std::move(zonesBSON), txnNumber);
}

void removeChunkAndTagsDocsForOriginalNss(OperationContext* opCtx,
                                          const ReshardingCoordinatorDocument& coordinatorDoc,
                                          TxnNumber txnNumber) {
    // Remove all chunk documents for the original nss. We do not know how many chunk docs currently
    // exist, so cannot pass a value for expectedNumModified
    ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
        opCtx,
        ChunkType::ConfigNS,
        buildDeleteOp(ChunkType::ConfigNS,
                      BSON(ChunkType::ns(coordinatorDoc.getNss().ns())),  // query
                      true                                                // multi
                      ),
        txnNumber);

    // Remove all tag documents for the original nss. We do not know how many tag docs currently
    // exist, so cannot pass a value for expectedNumModified
    ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
        opCtx,
        TagsType::ConfigNS,
        buildDeleteOp(TagsType::ConfigNS,
                      BSON(ChunkType::ns(coordinatorDoc.getNss().ns())),  // query
                      true                                                // multi
                      ),
        txnNumber);
}

void updateChunkAndTagsDocsForTempNss(OperationContext* opCtx,
                                      const ReshardingCoordinatorDocument& coordinatorDoc,
                                      OID newCollectionEpoch,
                                      boost::optional<int> expectedNumChunksModified,
                                      boost::optional<int> expectedNumZonesModified,
                                      TxnNumber txnNumber) {
    // Update all chunk documents that currently have 'ns' as the temporary collection namespace
    // such that 'ns' is now the original collection namespace and 'lastmodEpoch' is
    // newCollectionEpoch.
    auto chunksRequest =
        buildUpdateOp(ChunkType::ConfigNS,
                      BSON(ChunkType::ns(coordinatorDoc.getTempReshardingNss().ns())),  // query
                      BSON("$set" << BSON("ns" << coordinatorDoc.getNss().ns() << "lastmodEpoch"
                                               << newCollectionEpoch)),  // update
                      false,                                             // upsert
                      true                                               // multi
        );

    auto chunksRes = ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
        opCtx, ChunkType::ConfigNS, chunksRequest, txnNumber);

    if (expectedNumChunksModified) {
        assertNumDocsModifiedMatchesExpected(chunksRequest, chunksRes, *expectedNumChunksModified);
    }

    auto tagsRequest =
        buildUpdateOp(TagsType::ConfigNS,
                      BSON(TagsType::ns(coordinatorDoc.getTempReshardingNss().ns())),  // query
                      BSON("$set" << BSON("ns" << coordinatorDoc.getNss().ns())),      // update
                      false,                                                           // upsert
                      true                                                             // multi
        );

    // Update the 'ns' field to be the original collection namespace for all tags documents that
    // currently have 'ns' as the temporary collection namespace
    auto tagsRes = ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
        opCtx, TagsType::ConfigNS, tagsRequest, txnNumber);

    if (expectedNumZonesModified) {
        assertNumDocsModifiedMatchesExpected(tagsRequest, tagsRes, *expectedNumZonesModified);
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

//
// Helper methods for ensuring donors/ recipients are able to notice when certain state transitions
// occur.
//
// Donors/ recipients learn when to transition states by noticing a change in shard versions for one
// of the two collections involved in the resharding operations.
//
// Donors are notified when shard versions spanning the original resharding collection are
// incremented. Recipients are notified when shard versions spanning the temporary resharding
// collection are incremented.
//

/**
 * Maps which participants are to be notified when the coordinator transitions into a given state.
 */
enum class ParticipantsToNofityEnum { kDonors, kRecipients, kNone };
stdx::unordered_map<CoordinatorStateEnum, ParticipantsToNofityEnum> notifyForStateTransition{
    {CoordinatorStateEnum::kUnused, ParticipantsToNofityEnum::kNone},
    {CoordinatorStateEnum::kInitializing, ParticipantsToNofityEnum::kNone},
    {CoordinatorStateEnum::kPreparingToDonate, ParticipantsToNofityEnum::kDonors},
    {CoordinatorStateEnum::kCloning, ParticipantsToNofityEnum::kRecipients},
    {CoordinatorStateEnum::kMirroring, ParticipantsToNofityEnum::kDonors},
    {CoordinatorStateEnum::kCommitted, ParticipantsToNofityEnum::kNone},
    {CoordinatorStateEnum::kRenaming, ParticipantsToNofityEnum::kRecipients},
    {CoordinatorStateEnum::kDropping, ParticipantsToNofityEnum::kDonors},
    {CoordinatorStateEnum::kDone, ParticipantsToNofityEnum::kNone},
    {CoordinatorStateEnum::kError, ParticipantsToNofityEnum::kNone},
};

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
    invariant(notifyForStateTransition[state] == ParticipantsToNofityEnum::kNone);

    // Neither donors nor recipients need to be informed of the transition to
    // updatedCoordinatorDoc's state.
    ShardingCatalogManager::withTransaction(opCtx,
                                            NamespaceString::kConfigReshardingOperationsNamespace,
                                            [&](OperationContext* opCtx, TxnNumber txnNumber) {
                                                changeMetadataFunc(opCtx, txnNumber);
                                            });
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
    invariant(notifyForStateTransition[state] != ParticipantsToNofityEnum::kNone);

    auto participantsToNotify = notifyForStateTransition[state];
    if (participantsToNotify == ParticipantsToNofityEnum::kDonors) {
        // Bump the donor shard versions for the original namespace along with updating the
        // metadata.
        ShardingCatalogManager::get(opCtx)->bumpCollShardVersionsAndChangeMetadataInTxn(
            opCtx,
            updatedCoordinatorDoc.getNss(),
            extractShardIds(updatedCoordinatorDoc.getDonorShards()),
            std::move(changeMetadataFunc));
    } else if (participantsToNotify == ParticipantsToNofityEnum::kRecipients) {
        // Bump the recipient shard versions for the original namespace along with updating the
        // metadata.
        ShardingCatalogManager::get(opCtx)->bumpCollShardVersionsAndChangeMetadataInTxn(
            opCtx,
            updatedCoordinatorDoc.getTempReshardingNss(),
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
    collType.setDistributionMode(CollectionType::DistributionMode::kSharded);

    TypeCollectionReshardingFields tempEntryReshardingFields(coordinatorDoc.get_id());
    tempEntryReshardingFields.setState(coordinatorDoc.getState());

    auto donorShardIds = extractShardIds(coordinatorDoc.getDonorShards());

    TypeCollectionRecipientFields recipient(
        std::move(donorShardIds), coordinatorDoc.getExistingUUID(), coordinatorDoc.getNss());
    emplaceFetchTimestampIfExists(recipient, coordinatorDoc.getFetchTimestamp());
    tempEntryReshardingFields.setRecipientFields(recipient);
    collType.setReshardingFields(std::move(tempEntryReshardingFields));
    return collType;
}

void persistInitialStateAndCatalogUpdates(OperationContext* opCtx,
                                          const ReshardingCoordinatorDocument& coordinatorDoc,
                                          std::vector<ChunkType> initialChunks,
                                          std::vector<TagsType> newZones) {
    auto originalCollType = uassertStatusOK(Grid::get(opCtx)->catalogClient()->getCollection(
        opCtx, coordinatorDoc.getNss(), repl::ReadConcernLevel::kMajorityReadConcern));
    const auto collation = originalCollType.value.getDefaultCollation();

    bumpShardVersionsThenExecuteStateTransitionAndMetadataChangesInTxn(
        opCtx, coordinatorDoc, [&](OperationContext* opCtx, TxnNumber txnNumber) {
            // Insert state doc to config.reshardingOperations.
            writeToCoordinatorStateNss(opCtx, coordinatorDoc, txnNumber);

            // Update the config.collections entry for the original collection to include
            // 'reshardingFields'
            updateConfigCollectionsForOriginalNss(opCtx, coordinatorDoc, boost::none, txnNumber);

            // Insert the config.collections entry for the temporary resharding collection. The
            // chunks all have the same epoch, so picking the last chunk here is arbitrary.
            auto chunkVersion = initialChunks.back().getVersion();
            writeToConfigCollectionsForTempNss(
                opCtx, coordinatorDoc, chunkVersion, collation, txnNumber);

            // Insert new initial chunk and tag documents.
            insertChunkAndTagDocsForTempNss(
                opCtx, std::move(initialChunks), std::move(newZones), txnNumber);
        });
}

void persistCommittedState(OperationContext* opCtx,
                           const ReshardingCoordinatorDocument& coordinatorDoc,
                           OID newCollectionEpoch,
                           boost::optional<int> expectedNumChunksModified,
                           boost::optional<int> expectedNumZonesModified) {
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
                opCtx, coordinatorDoc, newCollectionEpoch, txnNumber);

            // Remove all chunk and tag documents associated with the original collection, then
            // update the chunk and tag docs currently associated with the temp nss to be associated
            // with the original nss
            removeChunkAndTagsDocsForOriginalNss(opCtx, coordinatorDoc, txnNumber);
            updateChunkAndTagsDocsForTempNss(opCtx,
                                             coordinatorDoc,
                                             newCollectionEpoch,
                                             expectedNumChunksModified,
                                             expectedNumZonesModified,
                                             txnNumber);
        });
}

void persistStateTransitionAndCatalogUpdatesThenBumpShardVersions(
    OperationContext* opCtx, const ReshardingCoordinatorDocument& coordinatorDoc) {
    // Run updates to config.reshardingOperations and config.collections in a transaction
    auto nextState = coordinatorDoc.getState();
    invariant(notifyForStateTransition.find(nextState) != notifyForStateTransition.end());
    // TODO SERVER-51800 Remove special casing for kError.
    invariant(nextState == CoordinatorStateEnum::kError ||
              notifyForStateTransition[nextState] != ParticipantsToNofityEnum::kNone);

    // Resharding metadata changes to be executed.
    auto changeMetadataFunc = [&](OperationContext* opCtx, TxnNumber txnNumber) {
        // Update the config.reshardingOperations entry
        writeToCoordinatorStateNss(opCtx, coordinatorDoc, txnNumber);

        // Update the config.collections entry for the original collection
        updateConfigCollectionsForOriginalNss(opCtx, coordinatorDoc, boost::none, txnNumber);

        // Update the config.collections entry for the temporary resharding collection. If we've
        // already committed this operation, we've removed the entry for the temporary
        // collection and updated the entry with original namespace to have the new shard key,
        // UUID, and epoch
        if (nextState < CoordinatorStateEnum::kCommitted ||
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
    executeStateTransitionAndMetadataChangesInTxn(
        opCtx, coordinatorDoc, [&](OperationContext* opCtx, TxnNumber txnNumber) {
            // Remove entry for this resharding operation from config.reshardingOperations
            writeToCoordinatorStateNss(opCtx, coordinatorDoc, txnNumber);

            // Remove the resharding fields from the config.collections entry
            updateConfigCollectionsForOriginalNss(opCtx, coordinatorDoc, boost::none, txnNumber);
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
    invariant(_initialChunksAndZonesPromise.getFuture().isReady());
    invariant(_completionPromise.getFuture().isReady());
}

void ReshardingCoordinatorService::ReshardingCoordinator::run(
    std::shared_ptr<executor::ScopedTaskExecutor> executor) noexcept {
    ExecutorFuture<void>(**executor)
        .then([this, executor] { return _init(executor); })
        .then([this, executor] { _tellAllDonorsToRefresh(executor); })
        .then([this, executor] { return _awaitAllDonorsReadyToDonate(executor); })
        .then([this, executor] { _tellAllRecipientsToRefresh(executor); })
        .then([this, executor] { return _awaitAllRecipientsFinishedCloning(executor); })
        .then([this, executor] { _tellAllDonorsToRefresh(executor); })
        .then([this, executor] { return _awaitAllRecipientsFinishedApplying(executor); })
        .then([this, executor] { _tellAllDonorsToRefresh(executor); })
        .then([this, executor] { return _awaitAllRecipientsInStrictConsistency(executor); })
        .then([this](const ReshardingCoordinatorDocument& updatedCoordinatorDoc) {
            return _commit(updatedCoordinatorDoc);
        })
        .then([this] {
            if (_coordinatorDoc.getState() > CoordinatorStateEnum::kRenaming) {
                return;
            }

            _updateCoordinatorDocStateAndCatalogEntries(CoordinatorStateEnum::kRenaming,
                                                        _coordinatorDoc);
            return;
        })
        .then([this, executor] { _tellAllRecipientsToRefresh(executor); })
        .then([this, executor] { return _awaitAllRecipientsRenamedCollection(executor); })
        .then([this, executor] { _tellAllDonorsToRefresh(executor); })
        .then([this, executor] { return _awaitAllDonorsDroppedOriginalCollection(executor); })
        .then([this, executor] { _tellAllRecipientsToRefresh(executor); })
        .then([this, executor] { _tellAllDonorsToRefresh(executor); })
        .onError([this, executor](Status status) {
            stdx::lock_guard<Latch> lg(_mutex);
            if (_completionPromise.getFuture().isReady()) {
                // interrupt() was called before we got here.
                return status;
            }

            _updateCoordinatorDocStateAndCatalogEntries(CoordinatorStateEnum::kError,
                                                        _coordinatorDoc);

            LOGV2(4956902,
                  "Resharding failed",
                  "namespace"_attr = _coordinatorDoc.getNss().ns(),
                  "newShardKeyPattern"_attr = _coordinatorDoc.getReshardingKey(),
                  "error"_attr = status);

            // TODO wait for donors and recipients to abort the operation and clean up state
            _tellAllRecipientsToRefresh(executor);
            _tellAllDonorsToRefresh(executor);

            return status;
        })
        .getAsync([this](Status status) {
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
        });
}

void ReshardingCoordinatorService::ReshardingCoordinator::interrupt(Status status) {
    // Resolve any unresolved promises to avoid hanging.
    stdx::lock_guard<Latch> lg(_mutex);
    if (!_initialChunksAndZonesPromise.getFuture().isReady()) {
        _initialChunksAndZonesPromise.setError(status);
    }

    _reshardingCoordinatorObserver->interrupt(status);

    if (!_completionPromise.getFuture().isReady()) {
        _completionPromise.setError(status);
    }
}

void ReshardingCoordinatorService::ReshardingCoordinator::setInitialChunksAndZones(
    std::vector<ChunkType> initialChunks, std::vector<TagsType> newZones) {
    stdx::lock_guard<Latch> lg(_mutex);
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kInitializing ||
        _initialChunksAndZonesPromise.getFuture().isReady()) {
        return;
    }

    _initialChunksAndZonesPromise.emplaceValue(
        ChunksAndZones{std::move(initialChunks), std::move(newZones)});
}

std::shared_ptr<ReshardingCoordinatorObserver>
ReshardingCoordinatorService::ReshardingCoordinator::getObserver() {
    return _reshardingCoordinatorObserver;
}

ExecutorFuture<void> ReshardingCoordinatorService::ReshardingCoordinator::_init(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kInitializing) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return _initialChunksAndZonesPromise.getFuture()
        .thenRunOn(**executor)
        .then([this](ChunksAndZones initialChunksAndZones) {
            auto initialChunks = std::move(initialChunksAndZones.initialChunks);
            auto newZones = std::move(initialChunksAndZones.newZones);

            // Create state document that will be written to disk and afterward set to the in-memory
            // _coordinatorDoc
            ReshardingCoordinatorDocument updatedCoordinatorDoc = _coordinatorDoc;
            updatedCoordinatorDoc.setState(CoordinatorStateEnum::kPreparingToDonate);

            auto opCtx = cc().makeOperationContext();
            resharding::persistInitialStateAndCatalogUpdates(
                opCtx.get(), updatedCoordinatorDoc, std::move(initialChunks), std::move(newZones));

            invariant(_coordinatorDoc.getState() == CoordinatorStateEnum::kInitializing);
            _coordinatorDoc = updatedCoordinatorDoc;
        });
}

ExecutorFuture<void>
ReshardingCoordinatorService::ReshardingCoordinator::_awaitAllDonorsReadyToDonate(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kPreparingToDonate) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    // TODO SERVER-51398 Remove this call.
    interrupt({ErrorCodes::InternalError, "Early exit to support jsTesting"});

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

Future<void> ReshardingCoordinatorService::ReshardingCoordinator::_commit(
    const ReshardingCoordinatorDocument& coordinatorDoc) {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kMirroring) {
        return Status::OK();
    }

    ReshardingCoordinatorDocument updatedCoordinatorDoc = coordinatorDoc;
    updatedCoordinatorDoc.setState(CoordinatorStateEnum::kCommitted);

    // Get the number of initial chunks and new zones that we inserted during initialization in
    // order to assert we match the same number when updating below.
    boost::optional<int> expectedNumChunksModified;
    boost::optional<int> expectedNumZonesModified;
    if (_initialChunksAndZonesPromise.getFuture().isReady()) {
        auto chunksAndZones =
            uassertStatusOK(_initialChunksAndZonesPromise.getFuture().getNoThrow());
        // TODO asserting that the updates to config.chunks and config.tags (run in
        // persistCommittedState) match 'expectedNumChunksModified' and 'expectedNumZonesModified'
        // is dependent on both the balancer and auto-splitter being off for the temporary
        // collection. If we decide to leave the auto-splitter enabled, we will need to revisit this
        // assumption.
        expectedNumChunksModified = chunksAndZones.initialChunks.size();
        expectedNumZonesModified = chunksAndZones.newZones.size();
    }
    auto opCtx = cc().makeOperationContext();

    // The new epoch to use for the resharded collection to indicate that the collection is a
    // new incarnation of the namespace
    auto newCollectionEpoch = OID::gen();
    resharding::persistCommittedState(opCtx.get(),
                                      updatedCoordinatorDoc,
                                      newCollectionEpoch,
                                      expectedNumChunksModified,
                                      expectedNumZonesModified);

    // Update the in memory state
    _coordinatorDoc = updatedCoordinatorDoc;

    return Status::OK();
};

ExecutorFuture<void>
ReshardingCoordinatorService::ReshardingCoordinator::_awaitAllRecipientsRenamedCollection(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kRenaming) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return _reshardingCoordinatorObserver->awaitAllRecipientsRenamedCollection()
        .thenRunOn(**executor)
        .then([this](ReshardingCoordinatorDocument coordinatorDocChangedOnDisk) {
            _updateCoordinatorDocStateAndCatalogEntries(CoordinatorStateEnum::kDropping,
                                                        coordinatorDocChangedOnDisk);
        });
}

ExecutorFuture<void>
ReshardingCoordinatorService::ReshardingCoordinator::_awaitAllDonorsDroppedOriginalCollection(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    if (_coordinatorDoc.getState() > CoordinatorStateEnum::kDropping) {
        return ExecutorFuture<void>(**executor, Status::OK());
    }

    return _reshardingCoordinatorObserver->awaitAllDonorsDroppedOriginalCollection()
        .thenRunOn(**executor)
        .then([this](ReshardingCoordinatorDocument coordinatorDocChangedOnDisk) {
            _updateCoordinatorDocStateAndCatalogEntries(CoordinatorStateEnum::kDone,
                                                        coordinatorDocChangedOnDisk);
        });
}

void ReshardingCoordinatorService::ReshardingCoordinator::
    _updateCoordinatorDocStateAndCatalogEntries(CoordinatorStateEnum nextState,
                                                ReshardingCoordinatorDocument coordinatorDoc,
                                                boost::optional<Timestamp> fetchTimestamp) {
    // Build new state doc for coordinator state update
    ReshardingCoordinatorDocument updatedCoordinatorDoc = coordinatorDoc;
    updatedCoordinatorDoc.setState(nextState);
    emplaceFetchTimestampIfExists(updatedCoordinatorDoc, std::move(fetchTimestamp));

    auto opCtx = cc().makeOperationContext();
    resharding::persistStateTransitionAndCatalogUpdatesThenBumpShardVersions(opCtx.get(),
                                                                             updatedCoordinatorDoc);

    // Update in-memory coordinator doc
    _coordinatorDoc = updatedCoordinatorDoc;
}

void ReshardingCoordinatorService::ReshardingCoordinator::_tellAllRecipientsToRefresh(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    auto opCtx = cc().makeOperationContext();
    auto recipientIds = extractShardIds(_coordinatorDoc.getRecipientShards());
    tellShardsToRefresh(
        opCtx.get(), recipientIds, _coordinatorDoc.getTempReshardingNss(), **executor);
}

void ReshardingCoordinatorService::ReshardingCoordinator::_tellAllDonorsToRefresh(
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor) {
    auto opCtx = cc().makeOperationContext();
    auto donorIds = extractShardIds(_coordinatorDoc.getDonorShards());
    tellShardsToRefresh(opCtx.get(), donorIds, _coordinatorDoc.getNss(), **executor);
}

}  // namespace mongo
