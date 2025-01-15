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

#include "mongo/db/s/config/initial_split_policy.h"
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

}  // namespace mongo
