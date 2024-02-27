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

#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

namespace {

template <class T>
const T& getRandomElement(const std::vector<T>& items) {
    static std::default_random_engine gen(time(nullptr));
    size_t max = items.size();
    std::uniform_int_distribution<int64_t> dist(0, max - 1);
    return items[dist(gen)];
}


boost::optional<std::pair<CollectionType, ChunkType>> getRandomUnsplittableCollection(
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

    if (unsplittableCollections.empty())
        return {};

    auto selectedCollection = getRandomElement(unsplittableCollections);
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
        LOGV2_WARNING(
            8544101,
            "Could not find the corresponding chunks in the catalog for the selected unsplitable "
            "collection",
            logAttrs(selectedCollection->getNss()));
        return {};
    }
    auto& chunks = swChunks.getValue();
    tassert(8245243, "Unsplittable collection has more than one chunk", chunks.size() == 1);

    return std::make_pair(*selectedCollection, chunks[0]);
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

        auto collectionAndChunks = [&]() -> boost::optional<std::pair<CollectionType, ChunkType>> {
            for (auto& availableShard : randomizedAvailableShards) {
                auto databases = catalogClient->getDatabasesForShard(opCtx, *availableShard);
                if (!databases.isOK()) {
                    continue;
                }
                for (auto& database : databases.getValue()) {
                    auto collectionAndChunks = getRandomUnsplittableCollection(opCtx, database);
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

        CollectionType& collectionToMove = collectionAndChunks->first;
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
                                        collectionToMove.getNss(),
                                        chunkToMove,
                                        ForceJumbo::kDoNotForce,
                                        boost::none));
    }

    return result;
}
}  // namespace mongo
