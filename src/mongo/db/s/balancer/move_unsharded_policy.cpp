/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/s/balancer/move_unsharded_policy.h"

#include "mongo/bson/json.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/database_name.h"
#include "mongo/db/list_collections_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/balancer/balancer_chunk_selection_policy.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/sharding_feature_flags_gen.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

namespace {

template <class T>
int64_t getRandomIndex(const std::vector<T>& items) {
    static std::default_random_engine gen(time(nullptr));
    size_t max = items.size();
    std::uniform_int_distribution<int64_t> dist(0, max - 1);
    return dist(gen);
}

/**
 *  Returns a list of collections that are present on the given shard for the given database.
 *
 *  These collections can be either sharded or unsharded.
 *  Collections that can never be tracked are not included.
 */
std::map<NamespaceString, ListCollectionsReplyItem> getCollectionsFromShard(
    OperationContext* opCtx, const ShardId& shardId, const DatabaseName& dbName) {
    const auto& shardRegistry = Grid::get(opCtx)->shardRegistry();
    const auto shard = uassertStatusOK(shardRegistry->getShard(opCtx, shardId));
    const auto listCollResponse = uassertStatusOK(
        shard->runExhaustiveCursorCommand(opCtx,
                                          ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                          dbName,
                                          BSON("listCollections" << 1),
                                          Milliseconds(-1)));

    std::map<NamespaceString, ListCollectionsReplyItem> localColls;
    for (auto&& replyItemBson : listCollResponse.docs) {
        auto replyItem =
            ListCollectionsReplyItem::parse(IDLParserContext("ListCollectionReply"), replyItemBson);
        if (replyItem.getType() != "collection") {
            // This entry is not a collection (e.g. view)
            continue;
        }
        auto nss = NamespaceStringUtil::deserialize(dbName, replyItem.getName());
        if (nss.isNamespaceAlwaysUntracked()) {
            // This collection can never be tracked so we skip it
            continue;
        }

        localColls.emplace(std::move(nss), std::move(replyItem));
    }
    return localColls;
}

/**
 *  Returns the list of untracked collections for the given database.
 */
std::vector<std::pair<NamespaceString, ListCollectionsReplyItem>> getUntrackedCollections(
    OperationContext* opCtx, const DatabaseName& dbName, const ShardId& shardId) {
    // get all collections from the local catalog of the shard
    auto localCollsMap = getCollectionsFromShard(opCtx, shardId, dbName);

    // from the local collections filter out the one that are tracked on the global catalog
    auto trackedColls = [&] {
        const auto& localCatalogClient = ShardingCatalogManager::get(opCtx)->localCatalogClient();
        BSONObj noSort{};
        return localCatalogClient->getCollections(
            opCtx, dbName, repl::ReadConcernLevel::kMajorityReadConcern, noSort);
    }();

    for (const auto& trackedColl : trackedColls) {
        localCollsMap.erase(trackedColl.getNss());
    }

    std::vector<std::pair<NamespaceString, ListCollectionsReplyItem>> localColls;
    for (auto&& [nss, listCollEntry] : localCollsMap) {
        localColls.emplace_back(std::move(nss), std::move(listCollEntry));
    }

    return localColls;
}

/*
 * Returns a random untracked collection on the given shard
 *
 * In case no collection can be found returns boost::none.
 */
boost::optional<std::pair<NamespaceString, ChunkType>> getRandomUntrackedCollectionOnShard(
    OperationContext* opCtx, const ShardId& shardId) {

    const auto shuffledShardDatabases = [&] {
        const auto& localCatalogClient = ShardingCatalogManager::get(opCtx)->localCatalogClient();
        auto dbs = uassertStatusOK(localCatalogClient->getDatabasesForShard(opCtx, shardId));
        std::shuffle(dbs.begin(), dbs.end(), opCtx->getClient()->getPrng().urbg());
        return dbs;
    }();

    for (const auto& dbName : shuffledShardDatabases) {
        auto untrackedCollections = getUntrackedCollections(opCtx, dbName, shardId);
        if (untrackedCollections.empty()) {
            continue;
        }
        const auto rndIdx = getRandomIndex(untrackedCollections);
        auto& coll = untrackedCollections.at(rndIdx);
        auto& collectionUUID = coll.second.getInfo()->getUuid().get();
        ChunkType dummyChunk{collectionUUID,
                             ChunkRange(BSON("_id" << MINKEY), BSON("_id" << MAXKEY)),
                             ChunkVersion::UNSHARDED(),
                             shardId};
        return std::make_pair(std::move(coll.first), dummyChunk);
    }

    return boost::none;
}

/*
 * Returns a list of tracked unsharded collections that are currently placed on
 * the give shard.
 */
std::vector<std::pair<NamespaceString, ChunkType>> getTrackedUnshardedCollectionsOnShard(
    OperationContext* opCtx, const ShardId& shardId) {
    static constexpr auto chunkFieldName = "chunk"_sd;

    std::vector<BSONObj> rawPipelineStages{
        // Match only unsplittable collections
        // {
        //     $match: {
        //         {unsplittable: true}
        //     }
        // }
        BSON("$match" << BSON(CollectionType::kUnsplittableFieldName << true)),

        // Add chunk object to the collection entry using lookup + unwind stage
        // "$lookup": {
        //    "from": "chunks",
        //    "localField": "uuid",
        //    "foreignField": "uuid",
        //    "pipeline": [{
        //          "$match": {
        //             "shard": <SHARD>
        //          }
        //       }, {
        //           "$limit": 1
        //       }
        //     ],
        //     "as": "chunks",
        // }
        BSON("$lookup" << BSON("from"
                               << "chunks"
                               << "localField" << ChunkType::collectionUUID.name() << "foreignField"
                               << CollectionType::kUuidFieldName << "pipeline"
                               << BSON_ARRAY(
                                      BSON("$match" << BSON(ChunkType::shard.name() << shardId))
                                      << BSON("$limit" << 1))
                               << "as" << chunkFieldName)),

        // This stage has two purposes:
        //   - Promote the chunk object to top level field in every collection entry.
        //   - Filter out all the collection that do not have a chunk on the given shard.
        fromjson(R"({
           "$unwind": {
              "path": "$chunk",
              "preserveNullAndEmptyArrays": false
           }
        })")};

    const auto& localCatalogClient = ShardingCatalogManager::get(opCtx)->localCatalogClient();
    AggregateCommandRequest aggRequest{NamespaceString::kConfigsvrCollectionsNamespace,
                                       rawPipelineStages};
    auto aggResult = localCatalogClient->runCatalogAggregation(
        opCtx, aggRequest, {repl::ReadConcernLevel::kSnapshotReadConcern});

    std::vector<std::pair<NamespaceString, ChunkType>> movableCollections;
    for (auto&& resEntry : aggResult) {
        CollectionType coll{resEntry};
        if (!balancer_policy_utils::canBalanceCollection(coll)) {
            // balancing for this collection is disabled
            continue;
        }
        auto chunk = uassertStatusOK(ChunkType::parseFromConfigBSON(
            resEntry.getObjectField(chunkFieldName), coll.getEpoch(), coll.getTimestamp()));
        movableCollections.emplace_back(coll.getNss(), std::move(chunk));
    }
    return movableCollections;
}

}  // namespace

MoveUnshardedPolicy::MoveUnshardedPolicy()
    : fpBalancerShouldReturnRandomMigrations(
          globalFailPointRegistry().find("balancerShouldReturnRandomMigrations")) {
    tassert(8245244,
            "balancerShouldReturnRandomMigrations failpoint is not registered",
            fpBalancerShouldReturnRandomMigrations != nullptr);
}

void MoveUnshardedPolicy::applyActionResult(OperationContext* opCtx,
                                            const BalancerStreamAction& action,
                                            const BalancerStreamActionResponse& response) {

    const auto& moveAction = get<MigrateInfo>(action);
    const auto& moveResponse = get<Status>(response);

    if (!moveResponse.isOK()) {
        auto isAcceptableError = [](const Status& status) {
            // Categories covering stepdown, crashes, network issues, slow machines, stale routing
            // info
            const bool isErrorInAcceptableCategory = status.isA<ErrorCategory::ShutdownError>() ||
                status.isA<ErrorCategory::NetworkError>() ||
                status.isA<ErrorCategory::RetriableError>() ||
                status.isA<ErrorCategory::Interruption>() ||
                status.isA<ErrorCategory::ExceededTimeLimitError>() ||
                status.isA<ErrorCategory::WriteConcernError>() ||
                status.isA<ErrorCategory::NeedRetargettingError>() ||
                status.isA<ErrorCategory::NotPrimaryError>();
            if (isErrorInAcceptableCategory) {
                return true;
            }

            switch (status.code()) {
                case ErrorCodes::BackgroundOperationInProgressForNamespace:
                // TODO SERVER-89892 Investigate CannotCreateIndex error
                case ErrorCodes::CannotCreateIndex:
                // TODO SERVER-90002 Investigate CannotInsertTimeseriesBucketsWithMixedSchema error
                case ErrorCodes::CannotInsertTimeseriesBucketsWithMixedSchema:
                case ErrorCodes::CommandNotSupported:
                case ErrorCodes::DuplicateKey:
                case ErrorCodes::FailedToSatisfyReadPreference:
                // TODO SERVER-89826 Investigate IllegalOperation error
                case ErrorCodes::IllegalOperation:
                case ErrorCodes::LockBusy:
                case ErrorCodes::NamespaceNotFound:
                case ErrorCodes::NotImplemented:
                case ErrorCodes::OplogOperationUnsupported:
                case ErrorCodes::OplogQueryMinTsMissing:
                case ErrorCodes::ReshardCollectionAborted:
                case ErrorCodes::ReshardCollectionInProgress:
                case ErrorCodes::ReshardCollectionTruncatedError:
                case ErrorCodes::SnapshotTooOld:
                case ErrorCodes::StaleDbVersion:
                case ErrorCodes::ConflictingOperationInProgress:
                case ErrorCodes::UserWritesBlocked:
                    return true;
                default:
                    return false;
            }
        };
        tassert(8959500,
                str::stream()
                    << "An unexpected error occured while moving a random unsharded collection"
                    << ", from: " << moveAction.from << ", to: " << moveAction.to
                    << ", nss: " << moveAction.nss.toStringForErrorMsg()
                    << ", error: " << moveResponse.toString(),
                isAcceptableError(moveResponse));
    }
}

// Returns a MigrateInfo for a moveCollection of an unsharded collection from one of the given
// donors to one of the given recipients. Returns boost::none if there are no eligible collections
// to move or no eligibile recipients. Handles overlaps in the given donors and recipients.
boost::optional<MigrateInfo> selectUnsplittableCollectionToMove(
    OperationContext* opCtx,
    stdx::unordered_set<ShardId>* availableShards,
    const std::vector<ShardId>& availableDonors,
    const std::vector<ShardId>& availableRecipients) {
    auto collectionAndChunks = [&]() -> boost::optional<std::pair<NamespaceString, ChunkType>> {
        const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();

        if (!feature_flags::gTrackUnshardedCollectionsUponCreation.isEnabled(fcvSnapshot) &&
            !feature_flags::gTrackUnshardedCollectionsUponMoveCollection.isEnabled(fcvSnapshot)) {
            return boost::none;
        }

        for (const auto& shardId : availableDonors) {
            if (!feature_flags::gTrackUnshardedCollectionsUponCreation.isEnabled(fcvSnapshot)) {
                auto randomUntrackedColl = getRandomUntrackedCollectionOnShard(opCtx, shardId);
                if (randomUntrackedColl) {
                    return randomUntrackedColl;
                }
            }

            auto trackedUnshardedCollections =
                getTrackedUnshardedCollectionsOnShard(opCtx, shardId);
            if (!trackedUnshardedCollections.empty()) {
                auto rndIdx = getRandomIndex(trackedUnshardedCollections);
                auto& randomTrackedUnshardedColl = trackedUnshardedCollections.at(rndIdx);
                return randomTrackedUnshardedColl;
            }
        }

        return boost::none;
    }();
    if (!collectionAndChunks) {
        return boost::none;
    }

    NamespaceString& collectionToMove = collectionAndChunks->first;
    ChunkType& chunkToMove = collectionAndChunks->second;
    const ShardId& sourceShardId = collectionAndChunks->second.getShard();

    // Pick a destination
    boost::optional<ShardId> destinationShardId = [&]() -> boost::optional<ShardId> {
        for (const auto& availableShard : availableRecipients) {
            if (availableShard != sourceShardId) {
                return availableShard;
            }
        }

        return boost::none;
    }();
    if (!destinationShardId) {
        return boost::none;
    }

    // Remove source and destination shards from the available shards set to not use them again
    // on the same balancer round.
    tassert(8701800,
            "Source shard does not exist in available shards",
            availableShards->erase(sourceShardId));
    tassert(8701801,
            "Target shard does not exist in available shards",
            availableShards->erase(*destinationShardId));

    return MigrateInfo(
        *destinationShardId, collectionToMove, chunkToMove, ForceJumbo::kDoNotForce, boost::none);
}

MigrateInfoVector MoveUnshardedPolicy::selectCollectionsToMove(
    OperationContext* opCtx,
    const std::vector<ClusterStatistics::ShardStatistics>& allShards,
    stdx::unordered_set<ShardId>* availableShards) {
    MigrateInfoVector result;

    if (auto sfp = fpBalancerShouldReturnRandomMigrations->scoped();
        MONGO_unlikely(sfp.isActive())) {
        if (availableShards->size() < 2) {
            return result;
        }

        // Separate draining shards so they can be handled separately.
        std::vector<ShardId> randomizedAvailableShards, randomizedDrainingShards;
        for (const auto& shardStat : allShards) {
            if (!availableShards->contains(shardStat.shardId)) {
                continue;
            }
            if (shardStat.isDraining) {
                randomizedDrainingShards.emplace_back(shardStat.shardId);
            } else {
                randomizedAvailableShards.emplace_back(shardStat.shardId);
            }
        }
        std::shuffle(randomizedDrainingShards.begin(),
                     randomizedDrainingShards.end(),
                     opCtx->getClient()->getPrng().urbg());
        std::shuffle(randomizedAvailableShards.begin(),
                     randomizedAvailableShards.end(),
                     opCtx->getClient()->getPrng().urbg());

        // Try to move collections off draining shards first.
        auto drainingShardMigration = selectUnsplittableCollectionToMove(
            opCtx, availableShards, randomizedDrainingShards, randomizedAvailableShards);
        if (drainingShardMigration) {
            result.emplace_back(*drainingShardMigration);
            return result;
        }

        // Choosing to move a collection between shards removes them from the available shards,
        // which prevents them from being chosen for a random chunk migration, so allow skipping a
        // random moveCollection to avoid "starving" random chunk migrations with few shards. Note
        // safeNumberDouble() defaults to 0 if the field is missing.
        auto skipMoveCollectionThreshold =
            sfp.getData()["skipMoveCollectionThreshold"].safeNumberDouble();
        if (opCtx->getClient()->getPrng().nextCanonicalDouble() < skipMoveCollectionThreshold) {
            return result;
        }

        auto migration = selectUnsplittableCollectionToMove(
            opCtx, availableShards, randomizedAvailableShards, randomizedAvailableShards);
        if (migration) {
            result.emplace_back(*migration);
        }
    }

    return result;
}
}  // namespace mongo
