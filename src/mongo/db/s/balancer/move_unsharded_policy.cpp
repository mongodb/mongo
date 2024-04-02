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

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
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

boost::optional<std::pair<NamespaceString, ChunkType>> getRandomUnshardedUntrackedCollection(
    OperationContext* opCtx, const DatabaseName& dbName, const ShardId& shardId) {

    // Query the Catalog for a list of tracked collections.
    const auto catalogClient = ShardingCatalogManager::get(opCtx)->localCatalogClient();
    BSONObj noSort;
    const auto collsNsCatalogResp = catalogClient->getCollectionNamespacesForDb(
        opCtx, dbName, repl::ReadConcernLevel::kMajorityReadConcern, noSort);
    std::set<NamespaceString> trackedColls(collsNsCatalogResp.begin(), collsNsCatalogResp.end());

    // Obtain collections (tracked + untracked) that are placed on a given shard.
    const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
    const auto toShard = uassertStatusOK(shardRegistry->getShard(opCtx, shardId));
    const auto listCommand = [&] {
        BSONObjBuilder commandBuilder;
        commandBuilder.append("listCollections", 1);
        return commandBuilder.obj();
    }();
    const auto listResponse = uassertStatusOK(
        toShard->runExhaustiveCursorCommand(opCtx,
                                            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                                            dbName,
                                            listCommand,
                                            Milliseconds(-1)));

    // Compute a list of unsharded and untracked collections. Those are collections that
    // are listed by a shard, but not tracked in the catalog.
    std::vector<std::pair<NamespaceString, UUID>> unshardedColls;
    for (const auto& bsonColl : listResponse.docs) {
        std::string collName;
        uassertStatusOK(bsonExtractStringField(bsonColl, "name", &collName));
        const auto ns = NamespaceStringUtil::deserialize(dbName, collName);
        if (ns.isNamespaceAlwaysUntracked()) {
            continue;
        }
        const auto uid = uassertStatusOK(UUID::parse(bsonColl["info"]["uuid"]));
        if (trackedColls.find(ns) == trackedColls.end()) {
            unshardedColls.emplace_back(std::make_pair(ns, uid));
        }
    }

    if (unshardedColls.empty())
        return {};

    auto selectedIndex = getRandomIndex(unshardedColls);
    auto selectedCollection = unshardedColls[selectedIndex];

    // Unsharded untracked collections don't have chunks associated with them.
    // Create a dummy representation, to be compatible with balancer migration interface.
    ChunkType dummyChunk(selectedCollection.second,
                         ChunkRange(BSON("_id" << MINKEY), BSON("_id" << MAXKEY)),
                         ChunkVersion::UNSHARDED(),
                         shardId);

    return std::make_pair(selectedCollection.first, dummyChunk);
}

boost::optional<std::pair<NamespaceString, ChunkType>> getRandomUnsplittableCollection(
    OperationContext* opCtx, const DatabaseName& dbName) {
    const auto catalogClient = ShardingCatalogManager::get(opCtx)->localCatalogClient();
    BSONObj noSort;
    std::vector<CollectionType> collections = catalogClient->getCollections(
        opCtx, dbName, repl::ReadConcernLevel::kMajorityReadConcern, noSort);
    std::vector<CollectionType*> unsplittableCollections;
    for (auto& collection : collections) {
        if (collection.getUnsplittable()) {
            unsplittableCollections.emplace_back(&collection);
        }
    }

    while (!unsplittableCollections.empty()) {
        auto selectedIndex = getRandomIndex(unsplittableCollections);
        auto selectedCollection = unsplittableCollections[selectedIndex];
        auto swChunks = catalogClient->getChunks(
            opCtx,
            BSON(ChunkType::collectionUUID() << selectedCollection->getUuid()) /*query*/,
            noSort,
            boost::none /*limit*/,
            nullptr /*opTime*/,
            selectedCollection->getEpoch(),
            selectedCollection->getTimestamp(),
            repl::ReadConcernLevel::kMajorityReadConcern,
            boost::none);
        if (!swChunks.isOK()) {
            LOGV2_WARNING(8544101,
                          "Could not find the corresponding chunks in the catalog for the selected "
                          "unsplitable collection",
                          logAttrs(selectedCollection->getNss()));
            return {};
        }
        auto& chunks = swChunks.getValue();
        if (chunks.size() == 1) {
            // Successfully selected a collection
            return std::make_pair(selectedCollection->getNss(), chunks[0]);
        } else {
            // The collection was deleted or changed to sharded while we were fetching additional
            // chunk information. We simply look for another one
            std::swap(unsplittableCollections[selectedIndex], unsplittableCollections.back());
            unsplittableCollections.pop_back();
        }
    }
    // There are no unsplittable collections
    return {};
}
}  // namespace

MoveUnshardedPolicy::MoveUnshardedPolicy()
    : fpBalancerShouldReturnRandomMigrations(
          globalFailPointRegistry().find("balancerShouldReturnRandomMigrations")) {
    tassert(8245244,
            "balancerShouldReturnRandomMigrations failpoint is not registered",
            fpBalancerShouldReturnRandomMigrations != nullptr);
}


MigrateInfoVector MoveUnshardedPolicy::selectCollectionsToMove(
    OperationContext* opCtx,
    const std::vector<ClusterStatistics::ShardStatistics>& allShards,
    stdx::unordered_set<ShardId>* availableShards) {

    MigrateInfoVector result;

    if (MONGO_unlikely(fpBalancerShouldReturnRandomMigrations->shouldFail())) {
        if (availableShards->size() < 2) {
            return result;
        }

        const auto catalogClient = ShardingCatalogManager::get(opCtx)->localCatalogClient();

        const std::vector<const ShardId*> randomizedAvailableShards = [&] {
            std::vector<const ShardId*> candidateShards;
            for (auto& availableShard : *availableShards) {
                candidateShards.emplace_back(&availableShard);
            }
            std::shuffle(candidateShards.begin(),
                         candidateShards.end(),
                         opCtx->getClient()->getPrng().urbg());
            return candidateShards;
        }();

        auto collectionAndChunks = [&]() -> boost::optional<std::pair<NamespaceString, ChunkType>> {
            auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();

            for (auto& availableShard : randomizedAvailableShards) {
                auto databases = catalogClient->getDatabasesForShard(opCtx, *availableShard);
                if (!databases.isOK()) {
                    continue;
                }
                for (auto& database : databases.getValue()) {

                    boost::optional<std::pair<NamespaceString, ChunkType>> collectionAndChunks;

                    if (feature_flags::gTrackUnshardedCollectionsUponCreation.isEnabled(
                            fcvSnapshot)) {
                        collectionAndChunks = getRandomUnsplittableCollection(opCtx, database);
                    } else if (feature_flags::gTrackUnshardedCollectionsUponMoveCollection
                                   .isEnabled(fcvSnapshot)) {
                        collectionAndChunks =
                            getRandomUnshardedUntrackedCollection(opCtx, database, *availableShard);
                        // If no unsharded, untracked collection found, try finding an unsharded
                        // collection that is already tracked.
                        if (!collectionAndChunks) {
                            collectionAndChunks = getRandomUnsplittableCollection(opCtx, database);
                        }
                    }

                    if (collectionAndChunks) {
                        return collectionAndChunks;
                    }
                }
            }
            return {};
        }();
        if (!collectionAndChunks) {
            return result;
        }

        NamespaceString& collectionToMove = collectionAndChunks->first;
        ChunkType& chunkToMove = collectionAndChunks->second;
        const ShardId& sourceShardId = collectionAndChunks->second.getShard();

        // Pick a destination
        boost::optional<ShardId> destinationShardId = [&]() -> boost::optional<ShardId> {
            for (auto& availableShard : randomizedAvailableShards) {
                if (*availableShard != sourceShardId) {
                    return *availableShard;
                }
            }
            tasserted(8245245, "Destination does not exist");

            return {};
        }();

        result.emplace_back(MigrateInfo(*destinationShardId,
                                        collectionToMove,
                                        chunkToMove,
                                        ForceJumbo::kDoNotForce,
                                        boost::none));

        // Remove source and destination shards from the available shards set to not use them again
        // on the same balancer round.
        tassert(8701800,
                "Source shard does not exist in available shards",
                availableShards->erase(sourceShardId));
        tassert(8701801,
                "Target shard does not exist in available shards",
                availableShards->erase(*destinationShardId));
    }

    return result;
}
}  // namespace mongo
