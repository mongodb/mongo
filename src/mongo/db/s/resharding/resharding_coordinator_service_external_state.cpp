// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/resharding/resharding_coordinator_service_external_state.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/router_role/routing_cache/routing_information_cache.h"
#include "mongo/db/s/config/initial_split_policy.h"
#include "mongo/db/s/primary_only_service_helpers/participant_causality_barrier.h"
#include "mongo/db/s/resharding/recipient_resume_document_gen.h"
#include "mongo/db/s/resharding/resharding_coordinator_service_util.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/s/resharding/shardsvr_resharding_commands_gen.h"
#include "mongo/db/topology/vector_clock/vector_clock.h"
#include "mongo/logv2/log.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"
#include "mongo/util/duration.h"

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
          "reshardingUUID"_attr = coordinatorDoc.getReshardingUUID(),
          "documentsToCopy"_attr = reportBuilder.obj());

    return docsToCopy;
}

/**
 * Builds a BSON object mapping each shard id to the string form of its error, for use in a
 * summary log line.
 */
BSONObj errorsToBSON(const std::map<ShardId, Status>& errors) {
    BSONObjBuilder errorsBuilder;
    for (const auto& [shardId, error] : errors) {
        errorsBuilder.append(shardId.toString(), error.toString());
    }
    return errorsBuilder.obj();
}

}  // namespace

ChunkVersion ReshardingCoordinatorExternalState::calculateChunkVersionForInitialChunks(
    OperationContext* opCtx) {
    const auto now = VectorClock::get(opCtx)->getTime();
    const auto timestamp = now.clusterTime().asTimestamp();
    return ChunkVersion({OID::gen(), timestamp}, {1, 0});
}

bool ReshardingCoordinatorExternalState::getIsUnsplittable(OperationContext* opCtx,
                                                           const NamespaceString& nss) {
    const auto cri =
        uassertStatusOK(RoutingInformationCache::get(opCtx)->getCollectionRoutingInfo(opCtx, nss));
    return cri.getChunkManager().isUnsplittable();
}

resharding::ParticipantShardsAndChunks
ReshardingCoordinatorExternalStateImpl::calculateParticipantShardsAndChunks(
    OperationContext* opCtx,
    const ReshardingCoordinatorDocument& coordinatorDoc,
    const std::vector<ReshardingZoneType> rawZones) {
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
                                                  repl::ReadConcernArgs::kMajority)
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
            auto& newChunk = initialChunks.back();
            newChunk.setOnCurrentShardSince(version.getTimestamp());
            newChunk.setHistory({ChunkHistory(*newChunk.getOnCurrentShardSince(),
                                              reshardedChunk.getRecipientShardId())});
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

        boost::optional<std::vector<mongo::TagsType>> parsedZones;
        if (rawZones.size() != 0) {
            parsedZones.emplace();
            parsedZones->reserve(rawZones.size());

            for (const auto& zone : rawZones) {
                ChunkRange range(zone.getMin(), zone.getMax());
                TagsType tag(
                    coordinatorDoc.getTempReshardingNss(), std::string{zone.getZone()}, range);

                parsedZones->push_back(tag);
            }
        }

        InitialSplitPolicy::ShardCollectionConfig splitResult;

        // Note: The resharding initial split policy doesn't care about what is the real
        // primary shard, so just pass in a random shard.
        const SplitPolicyParams splitParams{coordinatorDoc.getReshardingUUID(),
                                            *donorShardIds.begin()};

        if (shardKey.hasHashedPrefix()) {
            auto initialSplitter = SplitPointsBasedSplitPolicy(boost::none /* availableShardIds */);
            splitResult = initialSplitter.createFirstChunks(opCtx, shardKey, splitParams);
        } else if (const auto& shardDistribution = coordinatorDoc.getShardDistribution()) {
            uassert(ErrorCodes::InvalidOptions,
                    "ShardDistribution should not be empty if provided",
                    shardDistribution->size() > 0);

            // If shardDistribution is specified with min/max, use shardDistributionSplitPolicy to
            // create chunks based on the shard min/max. If not, do sampling based split on limited
            // shards.
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
            auto initialSplitter =
                SamplingBasedSplitPolicy::make(opCtx,
                                               coordinatorDoc.getSourceNss(),
                                               shardKey,
                                               numInitialChunks,
                                               std::move(parsedZones),
                                               boost::none /* availableShardIds */,
                                               numSamplesPerChunk);
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

bool ReshardingCoordinatorExternalStateImpl::searchIndexExistsForCollection(
    OperationContext* opCtx, const NamespaceString& nss) {

    // $listSearchIndex can be run on any shard that owns at least part of the collection
    // we are interested in. Here we get the shard which owns the MinKey chunk to make the
    // command more deterministic and easier to test.
    const auto cri =
        uassertStatusOK(RoutingInformationCache::get(opCtx)->getCollectionRoutingInfo(opCtx, nss));
    const auto minKeyShardId = cri.getChunkManager().getMinKeyShardIdWithSimpleCollation();

    const auto shardPtr =
        uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, minKeyShardId));

    std::vector<BSONObj> pipeline{};
    pipeline.emplace_back(BSON("$listSearchIndexes" << BSONObj{}));
    pipeline.emplace_back(BSON("$limit" << 1));

    AggregateCommandRequest aggRequest(nss, pipeline);
    aggRequest.setWriteConcern(WriteConcernOptions());
    aggRequest.setCursor(SimpleCursorOptions{});

    try {
        auto indexes = uassertStatusOK(
            shardPtr->runAggregationWithResult(opCtx, aggRequest, Shard::RetryPolicy::kIdempotent));
        return !indexes.empty();
    } catch (const ExceptionFor<ErrorCodes::SearchNotEnabled>&) {
        // If search is not enabled, no search indexes could exist.
        return false;
    }
}

void ReshardingCoordinatorExternalStateImpl::tellAllDonorsToRefresh(
    OperationContext* opCtx,
    const NamespaceString& sourceNss,
    const UUID& reshardingUUID,
    const std::vector<mongo::DonorShardEntry>& donorShards,
    const std::shared_ptr<executor::TaskExecutor>& executor,
    CancellationToken token) {
    auto donorShardIds = resharding::extractShardIdsFromParticipantEntries(donorShards);
    resharding::sendFlushReshardingStateChangeToShards(
        opCtx, sourceNss, reshardingUUID, donorShardIds, executor, token);
}

void ReshardingCoordinatorExternalStateImpl::tellAllRecipientsToRefresh(
    OperationContext* opCtx,
    const NamespaceString& nssToRefresh,
    const UUID& reshardingUUID,
    const std::vector<mongo::RecipientShardEntry>& recipientShards,
    const std::shared_ptr<executor::TaskExecutor>& executor,
    CancellationToken token) {
    auto recipientShardIds = resharding::extractShardIdsFromParticipantEntries(recipientShards);
    resharding::sendFlushReshardingStateChangeToShards(
        opCtx, nssToRefresh, reshardingUUID, recipientShardIds, executor, token);
}

void ReshardingCoordinatorExternalStateImpl::establishAllDonorsAsParticipants(
    OperationContext* opCtx,
    const NamespaceString& sourceNss,
    const std::vector<mongo::DonorShardEntry>& donorShards,
    const std::shared_ptr<executor::TaskExecutor>& executor,
    CancellationToken token) {
    auto donorShardIds = resharding::extractShardIdsFromParticipantEntries(donorShards);
    resharding::sendFlushRoutingTableCacheUpdatesToShards(
        opCtx, sourceNss, donorShardIds, executor, token);
}

void ReshardingCoordinatorExternalStateImpl::establishAllRecipientsAsParticipants(
    OperationContext* opCtx,
    const NamespaceString& tempNss,
    const std::vector<mongo::RecipientShardEntry>& recipientShards,
    const std::shared_ptr<executor::TaskExecutor>& executor,
    CancellationToken token) {
    auto recipientShardIds = resharding::extractShardIdsFromParticipantEntries(recipientShards);
    resharding::sendFlushRoutingTableCacheUpdatesToShards(
        opCtx, tempNss, recipientShardIds, executor, token);
}

std::map<ShardId, int64_t> ReshardingCoordinatorExternalStateImpl::getDocumentsToCopyFromDonors(
    OperationContext* opCtx,
    const std::shared_ptr<executor::TaskExecutor>& executor,
    CancellationToken token,
    const UUID& reshardingUUID,
    const NamespaceString& nss,
    const Timestamp& cloneTimestamp,
    const std::map<ShardId, ShardVersion>& shardVersions) {
    ShardsvrReshardingDonorGetCloneCount cmd(nss);
    cmd.setReshardingUUID(reshardingUUID);
    cmd.setCloneTimestamp(cloneTimestamp);
    cmd.setDbName(DatabaseName::kAdmin);
    cmd.setReadConcern(repl::ReadConcernArgs::snapshot(LogicalTime(cloneTimestamp)));
    auto readPref = ReadPreferenceSetting{ReadPreference::SecondaryPreferred};
    cmd.setReadPreference(readPref);

    auto fetchStart = resharding::getCurrentTime();

    const auto opts =
        std::make_shared<async_rpc::AsyncRPCOptions<ShardsvrReshardingDonorGetCloneCount>>(
            executor, token, cmd);
    auto responses = resharding::sendCommandToShards(
        opCtx, opts, shardVersions, readPref, false /* throwOnError */);

    std::map<ShardId, int64_t> docsToCopy;
    std::map<ShardId, Status> errors;

    for (auto&& response : responses) {
        const auto& donorShardId = response.shardId;

        auto status = AsyncRequestsSender::Response::getEffectiveStatus(response);
        if (!status.isOK()) {
            errors.emplace(donorShardId, status);
            continue;
        }
        auto reply = ShardsvrReshardingDonorGetCloneCountResponse::parse(
            response.swResponse.getValue().data, IDLParserContext("getDocumentsToCopyFromDonors"));
        int64_t count = reply.getDocumentsToCopy();
        docsToCopy.emplace(donorShardId, count);

        LOGV2(9858107,
              "Fetched documents to copy from donor shard",
              "reshardingUUID"_attr = reshardingUUID,
              "shardId"_attr = donorShardId,
              "documentsToCopy"_attr = count);
    }

    if (!errors.empty()) {
        LOGV2(12992507,
              "Failed to fetch documents to copy from one or more donor shards",
              "reshardingUUID"_attr = reshardingUUID,
              "errors"_attr = errorsToBSON(errors));
        uassertStatusOK(errors.begin()->second);
    }

    LOGV2(12992501,
          "Completed RPC fetch of documents to copy from donor shards",
          "reshardingUUID"_attr = reshardingUUID,
          "numDonorShards"_attr = docsToCopy.size(),
          "durationMillis"_attr =
              durationCount<Milliseconds>(resharding::getCurrentTime() - fetchStart));

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
    pipeline.push_back(BSON(
        "$match" << BSON((std::string{ReshardingRecipientResumeData::kIdFieldName} + "." +
                          std::string{ReshardingRecipientResumeDataId::kReshardingUUIDFieldName})
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
            "_id"
            << BSONNULL << "pairs"
            << BSON("$push" << BSON(
                        "k" << ("$" + std::string{ReshardingRecipientResumeData::kIdFieldName} +
                                "." +
                                std::string{ReshardingRecipientResumeDataId::kShardIdFieldName})
                            << "v"
                            << ("$" +
                                std::string{
                                    ReshardingRecipientResumeData::kDocumentsCopiedFieldName}))))));
    // Transform the array of pairs into an object.
    // {
    //    documentsCopied: {
    //        <donorShardId>: <documentsCopied>,
    //        ...
    //    }
    // }
    pipeline.push_back(BSON(
        "$project" << BSON("_id" << 0 << "documentsCopied" << BSON("$arrayToObject" << "$pairs"))));

    AggregateCommandRequest aggRequest(NamespaceString::kRecipientReshardingResumeDataNamespace,
                                       pipeline);
    aggRequest.setWriteConcern(WriteConcernOptions());
    aggRequest.setReadConcern(repl::ReadConcernArgs::kMajority);
    aggRequest.setUnwrappedReadPref(BSON(
        "$readPreference" << ReadPreferenceSetting{ReadPreference::PrimaryOnly}.toInnerBSON()));

    const auto opts = std::make_shared<async_rpc::AsyncRPCOptions<AggregateCommandRequest>>(
        executor, token, aggRequest);
    opts->cmd.setDbName(DatabaseName::kConfig);
    auto fetchStart = resharding::getCurrentTime();
    auto responses = resharding::sendCommandToShards(opCtx, opts, shardIds);

    std::map<ShardId, int64_t> docsCopied;

    for (auto&& response : responses) {
        const auto& recipientShardId = response.shardId;

        uassertStatusOK(AsyncRequestsSender::Response::getEffectiveStatus(response));
        auto cursorReply =
            CursorInitialReply::parse(response.swResponse.getValue().data,
                                      IDLParserContext("_getDocumentsCopiedFromRecipients"));
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
            ShardId donorShardId(std::string{fieldName});

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
              "reshardingUUID"_attr = reshardingUUID,
              "shardId"_attr = recipientShardId,
              "documentsCopied"_attr = obj);
    }

    LOGV2(12992506,
          "Completed RPC fetch of documents copied from recipient shards",
          "reshardingUUID"_attr = reshardingUUID,
          "numRecipientShards"_attr = shardIds.size(),
          "durationMillis"_attr =
              durationCount<Milliseconds>(resharding::getCurrentTime() - fetchStart));

    return docsCopied;
}

std::map<ShardId, int64_t> ReshardingCoordinatorExternalStateImpl::getDocumentsDeltaFromDonors(
    OperationContext* opCtx,
    const std::shared_ptr<executor::TaskExecutor>& executor,
    CancellationToken token,
    const UUID& reshardingUUID,
    const NamespaceString& nss,
    const std::vector<ShardId>& shardIds) {
    ShardsvrReshardingDonorFetchFinalCollectionStats cmd(nss, reshardingUUID);
    const auto opts = std::make_shared<
        async_rpc::AsyncRPCOptions<ShardsvrReshardingDonorFetchFinalCollectionStats>>(
        executor, token, cmd);
    opts->cmd.setDbName(DatabaseName::kAdmin);
    // Fetch from all donors without throwing on the first failure so that every per-shard error
    // can be logged before the fetch is retried.
    auto responses =
        resharding::sendCommandToShards(opCtx, opts, shardIds, false /* throwOnError */);

    std::map<ShardId, int64_t> docsDelta;
    std::map<ShardId, Status> errors;

    for (auto&& response : responses) {
        const auto& donorShardId = response.shardId;

        auto status = AsyncRequestsSender::Response::getEffectiveStatus(response);
        if (!status.isOK()) {
            errors.emplace(donorShardId, status);
            continue;
        }
        auto collStatsResponse = ShardsvrReshardingDonorFetchFinalCollectionStatsResponse::parse(
            response.swResponse.getValue().data, IDLParserContext("getDocumentsDeltaFromDonors"));

        int64_t delta = collStatsResponse.getDocumentsDelta();
        docsDelta.emplace(donorShardId, delta);

        LOGV2(12992503,
              "Fetched documents delta from donor shard",
              "reshardingUUID"_attr = reshardingUUID,
              "shardId"_attr = donorShardId,
              "documentsDelta"_attr = delta);
    }

    if (!errors.empty()) {
        LOGV2(12992508,
              "Failed to fetch documents delta from one or more donor shards",
              "reshardingUUID"_attr = reshardingUUID,
              "errors"_attr = errorsToBSON(errors));
        uassertStatusOK(errors.begin()->second);
    }

    return docsDelta;
}

std::map<ShardId, int64_t> ReshardingCoordinatorExternalStateImpl::getDocumentsDeltaFromRecipients(
    OperationContext* opCtx,
    const std::shared_ptr<executor::TaskExecutor>& executor,
    CancellationToken token,
    const UUID& reshardingUUID,
    const NamespaceString& nss,
    const std::vector<ShardId>& shardIds) {
    ShardsvrReshardingRecipientFetchFinalCollectionStats cmd(nss, reshardingUUID);
    const auto opts = std::make_shared<
        async_rpc::AsyncRPCOptions<ShardsvrReshardingRecipientFetchFinalCollectionStats>>(
        executor, token, cmd);
    opts->cmd.setDbName(DatabaseName::kAdmin);
    auto responses =
        resharding::sendCommandToShards(opCtx, opts, shardIds, false /* throwOnError */);

    std::map<ShardId, int64_t> docsDelta;
    std::map<ShardId, Status> errors;

    for (auto&& response : responses) {
        const auto& recipientShardId = response.shardId;

        auto status = AsyncRequestsSender::Response::getEffectiveStatus(response);
        if (!status.isOK()) {
            errors.emplace(recipientShardId, status);
            continue;
        }
        auto collStatsResponse =
            ShardsvrReshardingRecipientFetchFinalCollectionStatsResponse::parse(
                response.swResponse.getValue().data,
                IDLParserContext("getDocumentsDeltaFromRecipients"));

        int64_t delta = collStatsResponse.getDocumentsDelta();
        docsDelta.emplace(recipientShardId, delta);

        LOGV2(12992505,
              "Fetched documents delta from recipient shard",
              "reshardingUUID"_attr = reshardingUUID,
              "shardId"_attr = recipientShardId,
              "documentsDelta"_attr = delta);
    }

    if (!errors.empty()) {
        LOGV2(12992509,
              "Failed to fetch documents delta from one or more recipient shards",
              "reshardingUUID"_attr = reshardingUUID,
              "errors"_attr = errorsToBSON(errors));
        uassertStatusOK(errors.begin()->second);
    }

    return docsDelta;
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
    BSONObjBuilder donorReportBuilder;
    for (const auto& donorEntry : coordinatorDoc.getDonorShards()) {
        uassert(ErrorCodes::ReshardingValidationIncompleteData,
                str::stream() << "Expected the coordinator document to have the "
                                 "final number of documents on the donor shard '"
                              << donorEntry.getId() << "'",
                donorEntry.getDocumentsFinal());
        numDocsOriginal += *donorEntry.getDocumentsFinal();
        donorReportBuilder.append(donorEntry.getId(), *donorEntry.getDocumentsFinal());
    }

    int64_t numDocsTemporary = 0;
    BSONObjBuilder recipientReportBuilder;
    for (const auto& recipientEntry : coordinatorDoc.getRecipientShards()) {
        auto finalCount = recipientEntry.getDocumentsFinal();
        uassert(ErrorCodes::ReshardingValidationIncompleteData,
                str::stream() << "Expected the coordinator document to have the "
                                 "final number of documents on the recipient shard '"
                              << recipientEntry.getId() << "'",
                finalCount);
        numDocsTemporary += *finalCount;
        recipientReportBuilder.append(recipientEntry.getId(), *finalCount);
    }

    LOGV2(9858601,
          "Verifying the temporary resharding collection after reaching strict consistency",
          "reshardingUUID"_attr = coordinatorDoc.getReshardingUUID(),
          "donorDocumentsFinal"_attr = donorReportBuilder.obj(),
          "recipientDocumentsFinal"_attr = recipientReportBuilder.obj());

    uassert(
        9929906,
        str::stream() << "The number of documents in the original collection is " << numDocsOriginal
                      << " but the number of documents in the resharding temporary collection is "
                      << numDocsTemporary,
        numDocsOriginal == numDocsTemporary);

    LOGV2(
        9929913,
        "Finished verifying the temporary resharding collection after reaching strict consistency",
        "reshardingUUID"_attr = coordinatorDoc.getReshardingUUID(),
        "donorDocumentsFinal"_attr = numDocsOriginal,
        "recipientDocumentsFinal"_attr = numDocsTemporary);
}

namespace {
AuthoritativeMetadataAccessLevelEnum convert(
    ReshardingAuthoritativeMetadataAccessLevelEnum reshardingVersion) {
    switch (reshardingVersion) {
        case ReshardingAuthoritativeMetadataAccessLevelEnum::kNone:
            return AuthoritativeMetadataAccessLevelEnum::kNone;
        case ReshardingAuthoritativeMetadataAccessLevelEnum::kWritesAllowed:
            return AuthoritativeMetadataAccessLevelEnum::kWritesAllowed;
        case ReshardingAuthoritativeMetadataAccessLevelEnum::kWritesAndReadsAllowed:
            return AuthoritativeMetadataAccessLevelEnum::kWritesAndReadsAllowed;
        default:
            MONGO_UNREACHABLE;
    }
}
}  // namespace

void ReshardingCoordinatorExternalStateImpl::stopMigrations(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const UUID& expectedCollectionUUID,
    ReshardingAuthoritativeMetadataAccessLevelEnum authoritativeMetadataLevel,
    std::function<OperationSessionInfo()> osiGenerator) {
    sharding_ddl_util::stopMigrations(
        opCtx, nss, expectedCollectionUUID, osiGenerator, convert(authoritativeMetadataLevel));
}

void ReshardingCoordinatorExternalStateImpl::resumeMigrations(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const UUID& expectedCollectionUUID,
    ReshardingAuthoritativeMetadataAccessLevelEnum authoritativeMetadataLevel,
    std::function<OperationSessionInfo()> osiGenerator) {
    sharding_ddl_util::resumeMigrations(
        opCtx, nss, expectedCollectionUUID, osiGenerator, convert(authoritativeMetadataLevel));
}

std::unique_ptr<CausalityBarrier> ReshardingCoordinatorExternalStateImpl::buildCausalityBarrier(
    std::vector<ShardId> participants,
    std::shared_ptr<executor::TaskExecutor> executor,
    CancellationToken token) {
    return std::make_unique<ParticipantCausalityBarrier>(
        std::move(participants), std::move(executor), std::move(token));
}

}  // namespace mongo
