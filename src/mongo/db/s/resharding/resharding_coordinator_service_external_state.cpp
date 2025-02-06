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

#include "mongo/db/s/resharding/resharding_coordinator_service_external_state.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/s/config/initial_split_policy.h"
#include "mongo/db/s/resharding/recipient_resume_document_gen.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/vector_clock.h"
#include "mongo/logv2/log.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"
#include "mongo/s/routing_information_cache.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

namespace mongo {
namespace {

const int kReshardingNumInitialChunksDefault = 90;

std::vector<DonorShardEntry> constructDonorShardEntries(const std::set<ShardId>& donorShardIds) {
    std::vector<DonorShardEntry> donorShards;
    std::transform(donorShardIds.begin(),
                   donorShardIds.end(),
                   std::back_inserter(donorShards),
                   [](const ShardId& shardId) -> DonorShardEntry {
                       DonorShardContext donorCtx;
                       donorCtx.setState(DonorStateEnum::kUnused);
                       return DonorShardEntry{shardId, std::move(donorCtx)};
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
                       RecipientShardContext recipientCtx;
                       recipientCtx.setState(RecipientStateEnum::kUnused);
                       return RecipientShardEntry{shardId, std::move(recipientCtx)};
                   });
    return recipientShards;
}

/**
 * Returns a map from each the donor shard id to the number of documents to copy from that donor
 * shard based on the metrics in the coordinator document.
 */
std::map<ShardId, int64_t> extractDocumentsToCopy(
    const ReshardingCoordinatorDocument& coordinatorDoc) {
    std::map<ShardId, int64_t> docsToCopy;
    // This is used for logging.
    BSONObjBuilder reportBuilder;

    for (const auto& donorEntry : coordinatorDoc.getDonorShards()) {
        uassert(9929907,
                str::stream() << "Expected the coordinator document to have the "
                                 "number of documents to copy from the donor shard '"
                              << donorEntry.getId() << "'",
                donorEntry.getDocumentsToCopy());

        docsToCopy[donorEntry.getId()] = *donorEntry.getDocumentsToCopy();
        reportBuilder.append(donorEntry.getId(), *donorEntry.getDocumentsToCopy());
    }

    LOGV2(9929908,
          "Fetched cloning metrics for donor shards",
          "documentsToCopy"_attr = reportBuilder.obj());

    return docsToCopy;
}

}  // namespace

ChunkVersion ReshardingCoordinatorExternalState::calculateChunkVersionForInitialChunks(
    OperationContext* opCtx) {
    const auto now = VectorClock::get(opCtx)->getTime();
    const auto timestamp = now.clusterTime().asTimestamp();
    return ChunkVersion({OID::gen(), timestamp}, {1, 0});
}

boost::optional<CollectionIndexes> ReshardingCoordinatorExternalState::getCatalogIndexVersion(
    OperationContext* opCtx, const NamespaceString& nss, const UUID& uuid) {
    auto [_, optSii] =
        uassertStatusOK(RoutingInformationCache::get(opCtx)->getCollectionRoutingInfo(opCtx, nss));
    if (optSii) {
        VectorClock::VectorTime vt = VectorClock::get(opCtx)->getTime();
        auto time = vt.clusterTime().asTimestamp();
        return CollectionIndexes{uuid, time};
    }
    return boost::none;
}

bool ReshardingCoordinatorExternalState::getIsUnsplittable(OperationContext* opCtx,
                                                           const NamespaceString& nss) {
    auto [cm, _] =
        uassertStatusOK(RoutingInformationCache::get(opCtx)->getCollectionRoutingInfo(opCtx, nss));
    return cm.isUnsplittable();
}

boost::optional<CollectionIndexes>
ReshardingCoordinatorExternalState::getCatalogIndexVersionForCommit(OperationContext* opCtx,
                                                                    const NamespaceString& nss) {
    auto [_, optSii] =
        uassertStatusOK(RoutingInformationCache::get(opCtx)->getCollectionRoutingInfo(opCtx, nss));
    if (optSii) {
        return optSii->getCollectionIndexes();
    }
    return boost::none;
}

ReshardingCoordinatorExternalState::ParticipantShardsAndChunks
ReshardingCoordinatorExternalStateImpl::calculateParticipantShardsAndChunks(
    OperationContext* opCtx, const ReshardingCoordinatorDocument& coordinatorDoc) {

    const auto cm =
        uassertStatusOK(RoutingInformationCache::get(opCtx)->getCollectionPlacementInfoWithRefresh(
            opCtx, coordinatorDoc.getSourceNss()));

    std::set<ShardId> donorShardIds;
    cm.getAllShardIds(&donorShardIds);

    std::set<ShardId> recipientShardIds;
    std::vector<ChunkType> initialChunks;

    // The database primary must always be a recipient to ensure it ends up with consistent
    // collection metadata.
    const auto dbPrimaryShard = Grid::get(opCtx)
                                    ->catalogClient()
                                    ->getDatabase(opCtx,
                                                  coordinatorDoc.getSourceNss().dbName(),
                                                  repl::ReadConcernLevel::kMajorityReadConcern)
                                    .getPrimary();

    recipientShardIds.emplace(dbPrimaryShard);

    if (const auto& chunks = coordinatorDoc.getPresetReshardedChunks()) {
        auto version = calculateChunkVersionForInitialChunks(opCtx);

        // Use the provided shardIds from presetReshardedChunks to construct the
        // recipient list.
        for (const auto& reshardedChunk : *chunks) {
            recipientShardIds.emplace(reshardedChunk.getRecipientShardId());

            initialChunks.emplace_back(coordinatorDoc.getReshardingUUID(),
                                       ChunkRange{reshardedChunk.getMin(), reshardedChunk.getMax()},
                                       version,
                                       reshardedChunk.getRecipientShardId());
            version.incMinor();
        }
    } else {
        int numInitialChunks = coordinatorDoc.getNumInitialChunks()
            ? *coordinatorDoc.getNumInitialChunks()
            : kReshardingNumInitialChunksDefault;

        int numSamplesPerChunk = coordinatorDoc.getNumSamplesPerChunk()
            ? *coordinatorDoc.getNumSamplesPerChunk()
            : SamplingBasedSplitPolicy::kDefaultSamplesPerChunk;

        ShardKeyPattern shardKey(coordinatorDoc.getReshardingKey());
        const auto tempNs = coordinatorDoc.getTempReshardingNss();

        boost::optional<std::vector<mongo::TagsType>> parsedZones;
        auto rawBSONZones = coordinatorDoc.getZones();
        if (rawBSONZones && rawBSONZones->size() != 0) {
            parsedZones.emplace();
            parsedZones->reserve(rawBSONZones->size());

            for (const auto& zone : *rawBSONZones) {
                ChunkRange range(zone.getMin(), zone.getMax());
                TagsType tag(
                    coordinatorDoc.getTempReshardingNss(), zone.getZone().toString(), range);

                parsedZones->push_back(tag);
            }
        }

        InitialSplitPolicy::ShardCollectionConfig splitResult;

        // If shardDistribution is specified with min/max, use ShardDistributionSplitPolicy.
        if (const auto& shardDistribution = coordinatorDoc.getShardDistribution()) {
            uassert(ErrorCodes::InvalidOptions,
                    "Resharding improvements is not enabled, should not have "
                    "shardDistribution in coordinatorDoc",
                    resharding::gFeatureFlagReshardingImprovements.isEnabled(
                        serverGlobalParams.featureCompatibility.acquireFCVSnapshot()));
            uassert(ErrorCodes::InvalidOptions,
                    "ShardDistribution should not be empty if provided",
                    shardDistribution->size() > 0);
            const SplitPolicyParams splitParams{coordinatorDoc.getReshardingUUID(),
                                                *donorShardIds.begin()};
            // If shardDistribution is specified with min/max, create chunks based on the shard
            // min/max. If not, do sampling based split on limited shards.
            if ((*shardDistribution)[0].getMin()) {
                auto initialSplitter = ShardDistributionSplitPolicy::make(
                    opCtx, shardKey, *shardDistribution, std::move(parsedZones));
                splitResult = initialSplitter.createFirstChunks(opCtx, shardKey, splitParams);
            } else {
                std::vector<ShardId> availableShardIds;
                for (const auto& shardDist : *shardDistribution) {
                    availableShardIds.emplace_back(shardDist.getShard());
                }
                auto initialSplitter = SamplingBasedSplitPolicy::make(opCtx,
                                                                      coordinatorDoc.getSourceNss(),
                                                                      shardKey,
                                                                      numInitialChunks,
                                                                      std::move(parsedZones),
                                                                      availableShardIds,
                                                                      numSamplesPerChunk);
                splitResult = initialSplitter.createFirstChunks(opCtx, shardKey, splitParams);
            }
        } else {
            auto initialSplitter = SamplingBasedSplitPolicy::make(opCtx,
                                                                  coordinatorDoc.getSourceNss(),
                                                                  shardKey,
                                                                  numInitialChunks,
                                                                  std::move(parsedZones),
                                                                  boost::none /*availableShardIds*/,
                                                                  numSamplesPerChunk);
            // Note: The resharding initial split policy doesn't care about what is the real
            // primary shard, so just pass in a random shard.
            const SplitPolicyParams splitParams{coordinatorDoc.getReshardingUUID(),
                                                *donorShardIds.begin()};
            splitResult = initialSplitter.createFirstChunks(opCtx, shardKey, splitParams);
        }

        initialChunks = std::move(splitResult.chunks);

        for (const auto& chunk : initialChunks) {
            recipientShardIds.insert(chunk.getShard());
        }
    }

    if (recipientShardIds.size() != 1 || donorShardIds != recipientShardIds) {
        sharding_ddl_util::assertDataMovementAllowed();
    }

    return {constructDonorShardEntries(donorShardIds),
            constructRecipientShardEntries(recipientShardIds),
            initialChunks};
}

std::map<ShardId, int64_t> ReshardingCoordinatorExternalStateImpl::getDocumentsToCopyFromDonors(
    OperationContext* opCtx,
    const std::shared_ptr<executor::TaskExecutor>& executor,
    CancellationToken token,
    const UUID& reshardingUUID,
    const NamespaceString& nss,
    const Timestamp& cloneTimestamp,
    const std::map<ShardId, ShardVersion>& shardVersions) {
    std::vector<BSONObj> pipeline;
    pipeline.push_back(BSON("$count"
                            << "count"));
    AggregateCommandRequest aggRequest(nss, pipeline);
    BSONObj hint = BSON("_id" << 1);
    aggRequest.setHint(hint);

    aggRequest.setWriteConcern(WriteConcernOptions());
    aggRequest.setReadConcern(repl::ReadConcernArgs::snapshot(LogicalTime(cloneTimestamp)));

    auto readPref = ReadPreferenceSetting{ReadPreference::SecondaryPreferred};
    aggRequest.setUnwrappedReadPref(readPref.toContainingBSON());

    const auto opts = std::make_shared<async_rpc::AsyncRPCOptions<AggregateCommandRequest>>(
        executor, token, aggRequest);
    opts->cmd.setDbName(nss.dbName());
    auto responses = sendCommandToShards(opCtx, opts, shardVersions, readPref);

    std::map<ShardId, int64_t> docsToCopy;

    for (auto&& response : responses) {
        const auto& donorShardId = response.shardId;

        uassertStatusOK(AsyncRequestsSender::Response::getEffectiveStatus(response));
        auto cursorReply = CursorInitialReply::parse(
            IDLParserContext("getDocumentsToCopyFromDonors"), response.swResponse.getValue().data);
        auto firstBatch = cursorReply.getCursor()->getFirstBatch();

        uassert(9858102,
                str::stream() << "The aggregation result from fetching the number of "
                                 "documents from the donor shard '"
                              << donorShardId
                              << "' should contain at most one document but it contains "
                              << firstBatch.size() << " documents .",
                firstBatch.size() <= 1);

        int64_t count = [&] {
            // If there are no documents in the collection, the count aggregation would not
            // return any documents.
            if (firstBatch.size() == 0) {
                return 0LL;
            }
            auto doc = firstBatch[0];
            uassert(9858103,
                    str::stream() << "The aggregation result from fetching the number of "
                                     "documents from the donor shard '"
                                  << donorShardId << "' does not have the field 'count' set.",
                    doc.hasField("count"));
            return doc["count"].numberLong();
        }();
        docsToCopy.emplace(donorShardId, count);

        LOGV2(9858107,
              "Fetched documents to copy from donor shard",
              "shardId"_attr = donorShardId,
              "documentsToCopy"_attr = count);
    }

    return docsToCopy;
}

std::map<ShardId, int64_t>
ReshardingCoordinatorExternalStateImpl::_getDocumentsCopiedFromRecipients(
    OperationContext* opCtx,
    const std::shared_ptr<executor::TaskExecutor>& executor,
    CancellationToken token,
    const UUID& reshardingUUID,
    const std::vector<ShardId>& shardIds) {
    std::vector<BSONObj> pipeline;
    // For each recipient, get the per-donor collection cloner resume data docs for this resharding
    // operation.
    // [
    //    {_id: {reshardingUUID: <uuid>, shardId: <string>}, documentsCopied: <long>, ...},
    //    ...
    // ]
    pipeline.push_back(
        BSON("$match" << BSON((ReshardingRecipientResumeData::kIdFieldName + "." +
                               ReshardingRecipientResumeDataId::kReshardingUUIDFieldName)
                              << reshardingUUID)));
    // Combine the docs into one doc with an array of donorShardId and documentsCopied pairs. This
    // is to avoid needing to run getMore commands when there are more than 'batchSize' (defaults to
    // 100) donor shards.
    // [
    //    {k: <donorShardId>, v: <documentsCopied>},
    //    ...,
    // ]
    pipeline.push_back(BSON(
        "$group" << BSON(
            "_id" << BSONNULL << "pairs"
                  << BSON(
                         "$push" << BSON(
                             "k" << ("$" + ReshardingRecipientResumeData::kIdFieldName + "." +
                                     ReshardingRecipientResumeDataId::kShardIdFieldName)
                                 << "v"
                                 << ("$" +
                                     ReshardingRecipientResumeData::kDocumentsCopiedFieldName))))));
    // Transform the array of pairs into an object.
    // {
    //    documentsCopied: {
    //        <donorShardId>: <documentsCopied>,
    //        ...
    //    }
    // }
    pipeline.push_back(BSON("$project" << BSON("_id" << 0 << "documentsCopied"
                                                     << BSON("$arrayToObject"
                                                             << "$pairs"))));

    AggregateCommandRequest aggRequest(NamespaceString::kRecipientReshardingResumeDataNamespace,
                                       pipeline);
    aggRequest.setWriteConcern(WriteConcernOptions());
    aggRequest.setReadConcern(repl::ReadConcernArgs::kMajority);
    aggRequest.setUnwrappedReadPref(BSON(
        "$readPreference" << ReadPreferenceSetting{ReadPreference::PrimaryOnly}.toInnerBSON()));

    const auto opts = std::make_shared<async_rpc::AsyncRPCOptions<AggregateCommandRequest>>(
        executor, token, aggRequest);
    opts->cmd.setDbName(DatabaseName::kConfig);
    auto responses = sendCommandToShards(opCtx, opts, shardIds);

    std::map<ShardId, int64_t> docsCopied;

    for (auto&& response : responses) {
        const auto& recipientShardId = response.shardId;

        uassertStatusOK(AsyncRequestsSender::Response::getEffectiveStatus(response));
        auto cursorReply =
            CursorInitialReply::parse(IDLParserContext("_getDocumentsCopiedFromRecipients"),
                                      response.swResponse.getValue().data);
        auto firstBatch = cursorReply.getCursor()->getFirstBatch();

        uassert(1003561,
                str::stream() << "The aggregation result from fetching the number of "
                                 "documents copied from the recipient shard '"
                              << recipientShardId
                              << "' should contain at most one document but it contains "
                              << firstBatch.size() << " documents .",
                firstBatch.size() <= 1);

        if (firstBatch.empty()) {
            LOGV2_WARNING(9929910,
                          "Could not find the collection cloner resume data documents on a "
                          "recipient shard. This is expected when there are no documents for this "
                          "recipient to copy.",
                          "reshardingUUID"_attr = reshardingUUID,
                          "shardId"_attr = recipientShardId);
            continue;
        }

        uassert(1003562,
                str::stream() << "The aggregation result from fetching the number of "
                                 "documents from the recipient shard '"
                              << recipientShardId
                              << "' does not have the field 'documentsCopied' set.",
                firstBatch[0].hasField("documentsCopied"));

        auto obj = firstBatch[0].getObjectField("documentsCopied");
        for (const auto& element : obj) {
            const auto fieldName = element.fieldNameStringData();
            ShardId donorShardId(fieldName.toString());

            uassert(
                9929909,
                str::stream() << "Expected the recipient collection cloner resume data document "
                                 "for the donor shard '"
                              << donorShardId << "' to have the number of documents copied",
                !element.isNull());

            if (docsCopied.find(donorShardId) == docsCopied.end()) {
                docsCopied.emplace(donorShardId, 0);
            }
            docsCopied[donorShardId] += element.numberLong();
        }

        LOGV2(9929911,
              "Fetched cloning metrics for recipient shard",
              "shardId"_attr = recipientShardId,
              "documentsCopied"_attr = obj);
    }

    return docsCopied;
}

void ReshardingCoordinatorExternalStateImpl::verifyClonedCollection(
    OperationContext* opCtx,
    const std::shared_ptr<executor::TaskExecutor>& executor,
    CancellationToken token,
    const ReshardingCoordinatorDocument& coordinatorDoc) {
    LOGV2(9929900,
          "Start verifying the temporary resharding collection after cloning",
          "reshardingUUID"_attr = coordinatorDoc.getReshardingUUID());

    auto docsToCopy = extractDocumentsToCopy(coordinatorDoc);

    auto recipientShardIds =
        resharding::extractShardIdsFromParticipantEntries(coordinatorDoc.getRecipientShards());
    auto docsCopied = _getDocumentsCopiedFromRecipients(
        opCtx, executor, token, coordinatorDoc.getReshardingUUID(), recipientShardIds);

    for (auto donorIter = docsToCopy.begin(); donorIter != docsToCopy.end(); ++donorIter) {
        auto donorShardId = donorIter->first;
        auto donorDocsToCopy = donorIter->second;
        auto donorDocsCopied =
            (docsCopied.find(donorShardId) != docsCopied.end()) ? docsCopied[donorShardId] : 0;

        uassert(9929901,
                str::stream() << "The number of documents to copy from the donor shard '"
                              << donorShardId.toString() << "' is " << donorDocsToCopy
                              << " but the number of documents copied is " << donorDocsCopied,
                donorDocsToCopy == donorDocsCopied);
    }

    for (auto donorIter = docsCopied.begin(); donorIter != docsCopied.end(); ++donorIter) {
        auto donorShardId = donorIter->first;
        auto donorDocsCopied = donorIter->second;

        uassert(9929902,
                str::stream() << donorDocsCopied << " documents were copied from the shard '"
                              << donorShardId.toString()
                              << "' which is not expected to be a donor shard ",
                docsToCopy.find(donorShardId) != docsToCopy.end());
    }

    LOGV2(9929912,
          "Finished verifying the temporary resharding collection after cloning",
          "reshardingUUID"_attr = coordinatorDoc.getReshardingUUID());
}

void ReshardingCoordinatorExternalStateImpl::verifyFinalCollection(
    OperationContext* opCtx, const ReshardingCoordinatorDocument& coordinatorDoc) {
    LOGV2(9929903,
          "Start verifying the temporary resharding collection after reaching strict consistency",
          "reshardingUUID"_attr = coordinatorDoc.getReshardingUUID());

    int64_t numDocsOriginal = 0;
    for (const auto& donorEntry : coordinatorDoc.getDonorShards()) {
        uassert(9929904,
                str::stream() << "Expected the coordinator document to have the "
                                 "final number of documents on the donor shard '"
                              << donorEntry.getId() << "'",
                donorEntry.getDocumentsFinal());
        numDocsOriginal += *donorEntry.getDocumentsFinal();
    }

    int64_t numDocsTemporary = 0;
    for (const auto& recipientEntry : coordinatorDoc.getRecipientShards()) {
        auto mutableState = recipientEntry.getMutableState();
        uassert(9929905,
                str::stream() << "Expected the coordinator document to have the "
                                 "final number of documents on the recipient shard '"
                              << recipientEntry.getId() << "'",
                mutableState.getTotalNumDocuments());
        numDocsTemporary += *mutableState.getTotalNumDocuments();
    }

    uassert(
        9929906,
        str::stream() << "The number of documents in the original collection is " << numDocsOriginal
                      << " but the number of documents in the resharding temporary collection is "
                      << numDocsTemporary,
        numDocsOriginal == numDocsTemporary);

    LOGV2(
        9929913,
        "Finished verifying the temporary resharding collection after reaching strict consistency",
        "reshardingUUID"_attr = coordinatorDoc.getReshardingUUID());
}

}  // namespace mongo
