/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include "mongo/db/keypattern.h"
#include "mongo/db/s/balancer/balancer_policy.h"
#include "mongo/platform/random.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/chunks_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using std::map;
using std::string;
using std::stringstream;
using std::vector;

using ShardStatistics = ClusterStatistics::ShardStatistics;
typedef std::map<ShardId, std::vector<ChunkType>> ShardToChunksMap;

PseudoRandom _random{SecureRandom().nextInt64()};

const auto emptyTagSet = std::set<std::string>();
const auto kConfigId = ShardId("config");
const auto kShardId0 = ShardId("shard0");
const auto kShardId1 = ShardId("shard1");
const auto kShardId2 = ShardId("shard2");
const auto kShardId3 = ShardId("shard3");
const auto kShardId4 = ShardId("shard4");
const auto kShardId5 = ShardId("shard5");
const NamespaceString kNamespace("TestDB", "TestColl");
const uint64_t kNoMaxSize = 0;
const KeyPattern kSKeyPattern(BSON("x" << 1));
const Timestamp kCollTimestamp{1, 1};
const OID kCollEpoch;

const UUID& collUUID() {
    static const UUID kCollectionUUID{UUID::gen()};
    return kCollectionUUID;
}

std::vector<ChunkType> makeChunks(const std::vector<std::pair<ShardId, ChunkRange>>& specs) {
    ChunkVersion chunkVersion{1, 0, kCollEpoch, kCollTimestamp};
    std::vector<ChunkType> chunks;
    for (const auto& [shardId, range] : specs) {
        chunks.push_back(ChunkType(collUUID(), range, chunkVersion, shardId));
        chunkVersion.incMajor();
    }
    return chunks;
}

RoutingTableHistory makeRoutingTable(const std::vector<ChunkType>& chunks) {

    return RoutingTableHistory::makeNew(kNamespace,
                                        collUUID(),
                                        kSKeyPattern,
                                        nullptr,
                                        false /* unique */,
                                        kCollEpoch,
                                        kCollTimestamp,
                                        boost::none /* timeseriesFields */,
                                        boost::none /* reshardingFields */,
                                        boost::none /* maxChunkSizeBytes */,
                                        true /* allowMigrations */,
                                        chunks);
}

ChunkManager makeChunkManager(const std::vector<ChunkType>& chunks) {
    DatabaseVersion dbVersion;
    auto rt = std::make_shared<RoutingTableHistory>(makeRoutingTable(chunks));

    return {kConfigId, std::move(dbVersion), {std::move(rt)}, boost::none /* atClusterTime */};
}

DistributionStatus makeDistStatus(const ChunkManager& cm, ZoneInfo zoneInfo = ZoneInfo()) {
    return {kNamespace, std::move(zoneInfo), cm};
}

/**
 * Constructs a shard statistics vector and a consistent mapping of chunks to shards given the
 * specified input parameters. The generated chunks have an ever increasing min value. I.e, they
 * will be in the form:
 *
 * [MinKey, 1), [1, 2), [2, 3) ... [N - 1, MaxKey)
 */
std::pair<std::pair<ShardStatisticsVector, ShardToChunksMap>, ChunkManager> generateCluster(
    const vector<std::pair<ShardStatistics, size_t>>& shardsAndNumChunks) {
    int64_t totalNumChunks = 0;
    for (const auto& entry : shardsAndNumChunks) {
        totalNumChunks += std::get<1>(entry);
    }

    ShardToChunksMap chunkMap;
    ShardStatisticsVector shardStats;

    int64_t currentChunk = 0;

    ChunkVersion chunkVersion(1, 0, kCollEpoch, kCollTimestamp);

    std::vector<ChunkType> chunks;

    for (auto it = shardsAndNumChunks.begin(); it != shardsAndNumChunks.end(); it++) {
        ShardStatistics shard = std::move(it->first);
        const size_t numChunks = it->second;

        // Ensure that an entry is created
        chunkMap[shard.shardId];

        for (size_t i = 0; i < numChunks; i++, currentChunk++) {
            ChunkType chunk;

            chunk.setCollectionUUID(collUUID());
            chunk.setMin(currentChunk == 0 ? kSKeyPattern.globalMin() : BSON("x" << currentChunk));
            chunk.setMax(currentChunk == totalNumChunks - 1 ? kSKeyPattern.globalMax()
                                                            : BSON("x" << currentChunk + 1));
            chunk.setShard(shard.shardId);
            chunk.setVersion(chunkVersion);

            chunkVersion.incMajor();

            chunkMap[shard.shardId].push_back(chunk);
            chunks.push_back(std::move(chunk));
        }

        shardStats.push_back(std::move(shard));
    }

    return std::make_pair(std::make_pair(std::move(shardStats), std::move(chunkMap)),
                          makeChunkManager(chunks));
}

stdx::unordered_set<ShardId> getAllShardIds(const ShardStatisticsVector& shardStats) {
    stdx::unordered_set<ShardId> shards;
    std::transform(shardStats.begin(),
                   shardStats.end(),
                   std::inserter(shards, shards.end()),
                   [](const ShardStatistics& shardStatistics) { return shardStatistics.shardId; });
    return shards;
}

MigrateInfosWithReason balanceChunks(const ShardStatisticsVector& shardStats,
                                     const DistributionStatus& distribution,
                                     bool shouldAggressivelyBalance,
                                     bool forceJumbo) {
    auto availableShards = getAllShardIds(shardStats);
    return BalancerPolicy::balance(
        shardStats, distribution, boost::none /* collDataSizeInfo */, &availableShards, forceJumbo);
}

void checkChunksOnShardForTag(const DistributionStatus& dist,
                              const ShardId& shardId,
                              const std::string& zoneName,
                              const std::vector<ChunkType>& expectedChunks) {
    auto expectedChunkIt = expectedChunks.cbegin();
    const auto completed =
        dist.forEachChunkOnShardInZone(shardId, zoneName, [&](const auto& chunk) {
            ASSERT(expectedChunkIt != expectedChunks.end())
                << "forEachChunkOnShardInZone loop found more chunks than expected";
            ChunkInfo expectedChunkInfo{*expectedChunkIt++};
            ASSERT_EQ(Chunk(expectedChunkInfo, boost::none /* atClusterTime */).toString(),
                      chunk.toString());
            return true;  // continue
        });
    ASSERT(completed)
        << "forEachChunkOnShardInZone loop unexpectedly returned false (did not complete)";
    ASSERT(expectedChunkIt == expectedChunks.cend())
        << "forEachChunkOnShardInZone loop did not iterate over all the expected chunks";
}

TEST(BalancerPolicy, Basic) {
    auto [cluster, cm] =
        generateCluster({{ShardStatistics(kShardId0, kNoMaxSize, 4, false, emptyTagSet), 4},
                         {ShardStatistics(kShardId1, kNoMaxSize, 0, false, emptyTagSet), 0},
                         {ShardStatistics(kShardId2, kNoMaxSize, 3, false, emptyTagSet), 3}});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, makeDistStatus(cm), false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(kShardId0, migrations[0].from);
    ASSERT_EQ(kShardId1, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMax(), *migrations[0].maxKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);
}

TEST(BalancerPolicy, SmallClusterShouldBePerfectlyBalanced) {
    auto [cluster, cm] =
        generateCluster({{ShardStatistics(kShardId0, kNoMaxSize, 1, false, emptyTagSet), 1},
                         {ShardStatistics(kShardId1, kNoMaxSize, 2, false, emptyTagSet), 2},
                         {ShardStatistics(kShardId2, kNoMaxSize, 0, false, emptyTagSet), 0}});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, makeDistStatus(cm), false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(kShardId1, migrations[0].from);
    ASSERT_EQ(kShardId2, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId1][0].getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId1][0].getMax(), *migrations[0].maxKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);
}

TEST(BalancerPolicy, SingleChunkShouldNotMove) {
    auto [cluster, cm] =
        generateCluster({{ShardStatistics(kShardId0, kNoMaxSize, 1, false, emptyTagSet), 1},
                         {ShardStatistics(kShardId1, kNoMaxSize, 0, false, emptyTagSet), 0}});
    {
        auto [migrations, reason] = balanceChunks(cluster.first, makeDistStatus(cm), true, false);
        ASSERT(migrations.empty());
        ASSERT_EQ(MigrationReason::none, reason);
    }
    {
        auto [migrations, reason] = balanceChunks(cluster.first, makeDistStatus(cm), false, false);
        ASSERT(migrations.empty());
        ASSERT_EQ(MigrationReason::none, reason);
    }
}

TEST(BalancerPolicy, BalanceThresholdObeyed) {
    auto [cluster, cm] =
        generateCluster({{ShardStatistics(kShardId0, kNoMaxSize, 2, false, emptyTagSet), 2},
                         {ShardStatistics(kShardId1, kNoMaxSize, 2, false, emptyTagSet), 2},
                         {ShardStatistics(kShardId2, kNoMaxSize, 1, false, emptyTagSet), 1},
                         {ShardStatistics(kShardId3, kNoMaxSize, 1, false, emptyTagSet), 1}});

    {
        auto [migrations, reason] = balanceChunks(cluster.first, makeDistStatus(cm), true, false);
        ASSERT(migrations.empty());
        ASSERT_EQ(MigrationReason::none, reason);
    }
    {
        auto [migrations, reason] = balanceChunks(cluster.first, makeDistStatus(cm), false, false);
        ASSERT(migrations.empty());
        ASSERT_EQ(MigrationReason::none, reason);
    }
}

TEST(BalancerPolicy, ParallelBalancing) {
    auto [cluster, cm] =
        generateCluster({{ShardStatistics(kShardId0, kNoMaxSize, 4, false, emptyTagSet), 4},
                         {ShardStatistics(kShardId1, kNoMaxSize, 4, false, emptyTagSet), 4},
                         {ShardStatistics(kShardId2, kNoMaxSize, 0, false, emptyTagSet), 0},
                         {ShardStatistics(kShardId3, kNoMaxSize, 0, false, emptyTagSet), 0}});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, makeDistStatus(cm), false, false);
    ASSERT_EQ(2U, migrations.size());

    ASSERT_EQ(kShardId0, migrations[0].from);
    ASSERT_EQ(kShardId2, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMax(), *migrations[0].maxKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);

    ASSERT_EQ(kShardId1, migrations[1].from);
    ASSERT_EQ(kShardId3, migrations[1].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId1][0].getMin(), migrations[1].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId1][0].getMax(), *migrations[1].maxKey);
}

TEST(BalancerPolicy, ParallelBalancingDoesNotPutChunksOnShardsAboveTheOptimal) {
    auto [cluster, cm] =
        generateCluster({{ShardStatistics(kShardId0, kNoMaxSize, 100, false, emptyTagSet), 100},
                         {ShardStatistics(kShardId1, kNoMaxSize, 90, false, emptyTagSet), 90},
                         {ShardStatistics(kShardId2, kNoMaxSize, 90, false, emptyTagSet), 90},
                         {ShardStatistics(kShardId3, kNoMaxSize, 80, false, emptyTagSet), 80},
                         {ShardStatistics(kShardId4, kNoMaxSize, 0, false, emptyTagSet), 0},
                         {ShardStatistics(kShardId5, kNoMaxSize, 0, false, emptyTagSet), 0}});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, makeDistStatus(cm), false, false);
    ASSERT_EQ(2U, migrations.size());

    ASSERT_EQ(kShardId0, migrations[0].from);
    ASSERT_EQ(kShardId4, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMax(), *migrations[0].maxKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);

    ASSERT_EQ(kShardId1, migrations[1].from);
    ASSERT_EQ(kShardId5, migrations[1].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId1][0].getMin(), migrations[1].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId1][0].getMax(), *migrations[1].maxKey);
}

TEST(BalancerPolicy, ParallelBalancingDoesNotMoveChunksFromShardsBelowOptimal) {
    auto [cluster, cm] =
        generateCluster({{ShardStatistics(kShardId0, kNoMaxSize, 100, false, emptyTagSet), 100},
                         {ShardStatistics(kShardId1, kNoMaxSize, 30, false, emptyTagSet), 30},
                         {ShardStatistics(kShardId2, kNoMaxSize, 5, false, emptyTagSet), 5},
                         {ShardStatistics(kShardId3, kNoMaxSize, 0, false, emptyTagSet), 0}});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, makeDistStatus(cm), false, false);
    ASSERT_EQ(1U, migrations.size());

    ASSERT_EQ(kShardId0, migrations[0].from);
    ASSERT_EQ(kShardId3, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMax(), *migrations[0].maxKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);
}

TEST(BalancerPolicy, ParallelBalancingNotSchedulingOnInUseSourceShardsWithMoveNecessary) {
    auto [cluster, cm] =
        generateCluster({{ShardStatistics(kShardId0, kNoMaxSize, 8, false, emptyTagSet), 8},
                         {ShardStatistics(kShardId1, kNoMaxSize, 4, false, emptyTagSet), 4},
                         {ShardStatistics(kShardId2, kNoMaxSize, 0, false, emptyTagSet), 0},
                         {ShardStatistics(kShardId3, kNoMaxSize, 0, false, emptyTagSet), 0}});

    // Here kShardId0 would have been selected as a donor
    auto availableShards = getAllShardIds(cluster.first);
    availableShards.erase(kShardId0);
    const auto [migrations, reason] = BalancerPolicy::balance(cluster.first,
                                                              makeDistStatus(cm),
                                                              boost::none /* collDataSizeInfo */,
                                                              &availableShards,
                                                              false);
    ASSERT_EQ(1U, migrations.size());

    ASSERT_EQ(kShardId1, migrations[0].from);
    ASSERT_EQ(kShardId2, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId1][0].getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId1][0].getMax(), *migrations[0].maxKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);
}

TEST(BalancerPolicy, ParallelBalancingNotSchedulingOnInUseSourceShardsWithMoveNotNecessary) {
    auto [cluster, cm] =
        generateCluster({{ShardStatistics(kShardId0, kNoMaxSize, 12, false, emptyTagSet), 12},
                         {ShardStatistics(kShardId1, kNoMaxSize, 4, false, emptyTagSet), 4},
                         {ShardStatistics(kShardId2, kNoMaxSize, 0, false, emptyTagSet), 0},
                         {ShardStatistics(kShardId3, kNoMaxSize, 0, false, emptyTagSet), 0}});

    // Here kShardId0 would have been selected as a donor
    auto availableShards = getAllShardIds(cluster.first);
    availableShards.erase(kShardId0);
    const auto [migrations, reason] = BalancerPolicy::balance(cluster.first,
                                                              makeDistStatus(cm),
                                                              boost::none /* collDataSizeInfo */,
                                                              &availableShards,
                                                              false);
    ASSERT_EQ(0U, migrations.size());
}

TEST(BalancerPolicy, ParallelBalancingNotSchedulingOnInUseDestinationShards) {
    auto [cluster, cm] =
        generateCluster({{ShardStatistics(kShardId0, kNoMaxSize, 4, false, emptyTagSet), 4},
                         {ShardStatistics(kShardId1, kNoMaxSize, 4, false, emptyTagSet), 4},
                         {ShardStatistics(kShardId2, kNoMaxSize, 0, false, emptyTagSet), 0},
                         {ShardStatistics(kShardId3, kNoMaxSize, 1, false, emptyTagSet), 1}});

    // Here kShardId2 would have been selected as a recipient
    auto availableShards = getAllShardIds(cluster.first);
    availableShards.erase(kShardId2);
    const auto [migrations, reason] = BalancerPolicy::balance(cluster.first,
                                                              makeDistStatus(cm),
                                                              boost::none /* collDataSizeInfo */,
                                                              &availableShards,
                                                              false);
    ASSERT_EQ(1U, migrations.size());

    ASSERT_EQ(kShardId0, migrations[0].from);
    ASSERT_EQ(kShardId3, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMax(), *migrations[0].maxKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);
}

TEST(BalancerPolicy, JumboChunksNotMovedWhileEnforcingZones) {
    auto [cluster, cm] =
        generateCluster({{ShardStatistics(kShardId0, kNoMaxSize, 3, false, emptyTagSet), 3},
                         {ShardStatistics(kShardId1, kNoMaxSize, 3, false, {"a"}), 3}});

    // construct a new chunk map where all the chunks are jumbo except this one
    const auto& jumboChunk = cluster.second[kShardId0][1];

    std::vector<ChunkType> chunks;
    cm.forEachChunk([&](const auto& chunk) {
        ChunkType ct{collUUID(), chunk.getRange(), chunk.getLastmod(), chunk.getShardId()};
        if (chunk.getLastmod() == jumboChunk.getVersion())
            ct.setJumbo(false);
        else
            ct.setJumbo(true);
        chunks.emplace_back(std::move(ct));
        return true;
    });

    ZoneInfo zoneInfo;
    ASSERT_OK(zoneInfo.addRangeToZone(
        ZoneRange(kSKeyPattern.globalMin(), kSKeyPattern.globalMax(), "a")));
    const auto distribution = makeDistStatus(makeChunkManager(chunks), std::move(zoneInfo));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(kShardId0, migrations[0].from);
    ASSERT_EQ(kShardId1, migrations[0].to);
    ASSERT_BSONOBJ_EQ(jumboChunk.getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(jumboChunk.getMax(), *migrations[0].maxKey);
    ASSERT_EQ(MigrationReason::zoneViolation, reason);
}

TEST(BalancerPolicy, JumboChunksNotMovedWhileEnforcingZonesRandom) {
    auto [cluster, cm] =
        generateCluster({{ShardStatistics(kShardId0, kNoMaxSize, 3, false, emptyTagSet), 3},
                         {ShardStatistics(kShardId1, kNoMaxSize, 3, false, {"a"}), 3}});

    // construct a new chunk map where all the chunks are jumbo except this one
    const auto jumboChunkIdx = _random.nextInt64(cluster.second[kShardId0].size());
    const auto& jumboChunk = cluster.second[kShardId0][jumboChunkIdx];

    std::vector<ChunkType> chunks;
    cm.forEachChunk([&](const auto& chunk) {
        ChunkType ct{collUUID(), chunk.getRange(), chunk.getLastmod(), chunk.getShardId()};
        if (chunk.getLastmod() == jumboChunk.getVersion())
            ct.setJumbo(false);
        else
            ct.setJumbo(true);
        chunks.emplace_back(std::move(ct));
        return true;
    });

    ZoneInfo zoneInfo;
    ASSERT_OK(zoneInfo.addRangeToZone(
        ZoneRange(kSKeyPattern.globalMin(), kSKeyPattern.globalMax(), "a")));
    const auto distribution = makeDistStatus(makeChunkManager(chunks), std::move(zoneInfo));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(kShardId0, migrations[0].from);
    ASSERT_EQ(kShardId1, migrations[0].to);
    ASSERT_BSONOBJ_EQ(jumboChunk.getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(jumboChunk.getMax(), *migrations[0].maxKey);
    ASSERT_EQ(MigrationReason::zoneViolation, reason);
}

TEST(BalancerPolicy, JumboChunksNotMoved) {
    auto [cluster, cm] =
        generateCluster({{ShardStatistics(kShardId0, kNoMaxSize, 2, false, emptyTagSet), 4},
                         {ShardStatistics(kShardId1, kNoMaxSize, 0, false, emptyTagSet), 0}});

    // construct a new chunk map where all the chunks are jumbo except this one
    const auto& jumboChunk = cluster.second[kShardId0][1];

    std::vector<ChunkType> chunks;
    cm.forEachChunk([&](const auto& chunk) {
        ChunkType ct{collUUID(), chunk.getRange(), chunk.getLastmod(), chunk.getShardId()};
        if (chunk.getLastmod() == jumboChunk.getVersion())
            ct.setJumbo(false);
        else
            ct.setJumbo(true);
        chunks.emplace_back(std::move(ct));
        return true;
    });

    const auto [migrations, reason] =
        balanceChunks(cluster.first, makeDistStatus(makeChunkManager(chunks)), false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(kShardId0, migrations[0].from);
    ASSERT_EQ(kShardId1, migrations[0].to);
    ASSERT_BSONOBJ_EQ(jumboChunk.getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(jumboChunk.getMax(), *migrations[0].maxKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);
}

TEST(BalancerPolicy, JumboChunksNotMovedRandom) {
    auto [cluster, cm] =
        generateCluster({{ShardStatistics(kShardId0, kNoMaxSize, 2, false, emptyTagSet), 4},
                         {ShardStatistics(kShardId1, kNoMaxSize, 0, false, emptyTagSet), 0}});

    // construct a new chunk map where all the chunks are jumbo except this one
    const auto jumboChunkIdx = _random.nextInt64(cluster.second[kShardId0].size());
    const auto& jumboChunk = cluster.second[kShardId0][jumboChunkIdx];

    std::vector<ChunkType> chunks;
    cm.forEachChunk([&](const auto& chunk) {
        ChunkType ct{collUUID(), chunk.getRange(), chunk.getLastmod(), chunk.getShardId()};
        if (chunk.getLastmod() == jumboChunk.getVersion())
            ct.setJumbo(false);
        else
            ct.setJumbo(true);
        chunks.emplace_back(std::move(ct));
        return true;
    });

    const auto [migrations, reason] =
        balanceChunks(cluster.first, makeDistStatus(makeChunkManager(chunks)), false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(kShardId0, migrations[0].from);
    ASSERT_EQ(kShardId1, migrations[0].to);
    ASSERT_BSONOBJ_EQ(jumboChunk.getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(jumboChunk.getMax(), *migrations[0].maxKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);
}

TEST(BalancerPolicy, JumboChunksNotMovedParallel) {
    auto [cluster, cm] =
        generateCluster({{ShardStatistics(kShardId0, kNoMaxSize, 2, false, emptyTagSet), 4},
                         {ShardStatistics(kShardId1, kNoMaxSize, 0, false, emptyTagSet), 0},
                         {ShardStatistics(kShardId2, kNoMaxSize, 2, false, emptyTagSet), 4},
                         {ShardStatistics(kShardId3, kNoMaxSize, 0, false, emptyTagSet), 0}});

    // construct a new chunk map where all the chunks are jumbo except the ones listed below
    const auto& jumboChunk0 = cluster.second[kShardId0][1];
    const auto& jumboChunk1 = cluster.second[kShardId2][2];

    std::vector<ChunkType> chunks;
    cm.forEachChunk([&](const auto& chunk) {
        ChunkType ct{collUUID(), chunk.getRange(), chunk.getLastmod(), chunk.getShardId()};
        if (chunk.getLastmod() == jumboChunk0.getVersion() ||
            chunk.getLastmod() == jumboChunk1.getVersion())
            ct.setJumbo(false);
        else
            ct.setJumbo(true);
        chunks.emplace_back(std::move(ct));
        return true;
    });

    const auto [migrations, reason] =
        balanceChunks(cluster.first, makeDistStatus(makeChunkManager(chunks)), false, false);
    ASSERT_EQ(2U, migrations.size());

    ASSERT_EQ(kShardId0, migrations[0].from);
    ASSERT_EQ(kShardId1, migrations[0].to);
    ASSERT_BSONOBJ_EQ(jumboChunk0.getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(jumboChunk0.getMax(), *migrations[0].maxKey);

    ASSERT_EQ(kShardId2, migrations[1].from);
    ASSERT_EQ(kShardId3, migrations[1].to);
    ASSERT_BSONOBJ_EQ(jumboChunk1.getMin(), migrations[1].minKey);
    ASSERT_BSONOBJ_EQ(jumboChunk1.getMax(), *migrations[1].maxKey);

    ASSERT_EQ(MigrationReason::chunksImbalance, reason);
}

TEST(BalancerPolicy, DrainingSingleChunk) {
    // shard0 is draining and chunks will go to shard1, even though it has a lot more chunks
    auto [cluster, cm] =
        generateCluster({{ShardStatistics(kShardId0, kNoMaxSize, 2, true, emptyTagSet), 1},
                         {ShardStatistics(kShardId1, kNoMaxSize, 0, false, emptyTagSet), 5}});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, makeDistStatus(cm), false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(kShardId0, migrations[0].from);
    ASSERT_EQ(kShardId1, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMax(), *migrations[0].maxKey);
    ASSERT_EQ(MigrationReason::drain, reason);
}

TEST(BalancerPolicy, DrainingSingleChunkPerShard) {
    // shard0 and shard2 are draining and chunks will go to shard1 and shard3 in parallel
    auto [cluster, cm] =
        generateCluster({{ShardStatistics(kShardId0, kNoMaxSize, 2, true, emptyTagSet), 1},
                         {ShardStatistics(kShardId1, kNoMaxSize, 0, false, emptyTagSet), 5},
                         {ShardStatistics(kShardId2, kNoMaxSize, 2, true, emptyTagSet), 1},
                         {ShardStatistics(kShardId3, kNoMaxSize, 0, false, emptyTagSet), 5}});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, makeDistStatus(cm), false, false);
    ASSERT_EQ(2U, migrations.size());

    ASSERT_EQ(kShardId0, migrations[0].from);
    ASSERT_EQ(kShardId1, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMax(), *migrations[0].maxKey);
    ASSERT_EQ(MigrationReason::drain, reason);

    ASSERT_EQ(kShardId2, migrations[1].from);
    ASSERT_EQ(kShardId3, migrations[1].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId2][0].getMin(), migrations[1].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId2][0].getMax(), *migrations[1].maxKey);
}

TEST(BalancerPolicy, DrainingWithTwoChunksFirstOneSelected) {
    // shard0 is draining and chunks will go to shard1, even though it has a lot more chunks
    auto [cluster, cm] =
        generateCluster({{ShardStatistics(kShardId0, kNoMaxSize, 2, true, emptyTagSet), 2},
                         {ShardStatistics(kShardId1, kNoMaxSize, 0, false, emptyTagSet), 5}});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, makeDistStatus(cm), false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(kShardId0, migrations[0].from);
    ASSERT_EQ(kShardId1, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMax(), *migrations[0].maxKey);
    ASSERT_EQ(MigrationReason::drain, reason);
}

TEST(BalancerPolicy, DrainingMultipleShardsFirstOneSelected) {
    // shard0 and shard1 are both draining with very little chunks in them and chunks will go to
    // shard2, even though it has a lot more chunks that the other two
    auto [cluster, cm] =
        generateCluster({{ShardStatistics(kShardId0, kNoMaxSize, 5, true, emptyTagSet), 1},
                         {ShardStatistics(kShardId1, kNoMaxSize, 5, true, emptyTagSet), 2},
                         {ShardStatistics(kShardId2, kNoMaxSize, 5, false, emptyTagSet), 16}});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, makeDistStatus(cm), false, false);

    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(kShardId0, migrations[0].from);
    ASSERT_EQ(kShardId2, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMax(), *migrations[0].maxKey);
    ASSERT_EQ(MigrationReason::drain, reason);
}

TEST(BalancerPolicy, DrainingMultipleShardsWontAcceptChunks) {
    // shard0 has many chunks, but can't move them to shard1 or shard2 because they are draining
    auto [cluster, cm] =
        generateCluster({{ShardStatistics(kShardId0, kNoMaxSize, 2, false, emptyTagSet), 4},
                         {ShardStatistics(kShardId1, kNoMaxSize, 0, true, emptyTagSet), 0},
                         {ShardStatistics(kShardId2, kNoMaxSize, 0, true, emptyTagSet), 0}});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, makeDistStatus(cm), false, false);
    ASSERT(migrations.empty());
}

TEST(BalancerPolicy, DrainingSingleAppropriateShardFoundDueToTag) {
    auto [cluster, cm] =
        generateCluster({{ShardStatistics(kShardId0, kNoMaxSize, 2, false, {"NYC"}), 4},
                         {ShardStatistics(kShardId1, kNoMaxSize, 2, false, {"LAX"}), 4},
                         {ShardStatistics(kShardId2, kNoMaxSize, 1, true, {"LAX"}), 1}});

    ZoneInfo zoneInfo;
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(
        cluster.second[kShardId2][0].getMin(), cluster.second[kShardId2][0].getMax(), "LAX")));
    const auto distribution = makeDistStatus(cm, std::move(zoneInfo));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(kShardId2, migrations[0].from);
    ASSERT_EQ(kShardId1, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId2][0].getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId2][0].getMax(), *migrations[0].maxKey);
    ASSERT_EQ(MigrationReason::drain, reason);
}

TEST(BalancerPolicy, DrainingNoAppropriateShardsFoundDueToTag) {
    auto [cluster, cm] =
        generateCluster({{ShardStatistics(kShardId0, kNoMaxSize, 2, false, {"NYC"}), 4},
                         {ShardStatistics(kShardId1, kNoMaxSize, 2, false, {"LAX"}), 4},
                         {ShardStatistics(kShardId2, kNoMaxSize, 1, true, {"SEA"}), 1}});

    ZoneInfo zoneInfo;
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(
        cluster.second[kShardId2][0].getMin(), cluster.second[kShardId2][0].getMax(), "SEA")));
    const auto distribution = makeDistStatus(cm, std::move(zoneInfo));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT(migrations.empty());
}

TEST(BalancerPolicy, DrainingSingleAppropriateShardFoundMultipleTags) {
    auto [cluster, cm] =
        generateCluster({{ShardStatistics(kShardId0, kNoMaxSize, 2, true, {}), 4},
                         {ShardStatistics(kShardId1, kNoMaxSize, 2, false, {"Zone1", "Zone2"}), 2},
                         {ShardStatistics(kShardId2, kNoMaxSize, 2, false, {"Zone1", "Zone2"}), 2},
                         {ShardStatistics(kShardId3, kNoMaxSize, 2, false, {}), 2}});

    ZoneInfo zoneInfo;
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(
        cluster.second[kShardId0][0].getMin(), cluster.second[kShardId0][0].getMax(), "Zone1")));
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(
        cluster.second[kShardId0][1].getMin(), cluster.second[kShardId0][1].getMax(), "Zone2")));
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(
        cluster.second[kShardId0][2].getMin(), cluster.second[kShardId0][2].getMax(), "Zone1")));
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(
        cluster.second[kShardId0][3].getMin(), cluster.second[kShardId0][3].getMax(), "Zone2")));
    const auto distribution = makeDistStatus(cm, std::move(zoneInfo));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(kShardId0, migrations[0].from);
    const auto& recipientShard = migrations[0].to;
    ASSERT(recipientShard == kShardId1 || recipientShard == kShardId2);
    ASSERT_EQ(MigrationReason::drain, reason);
}

TEST(BalancerPolicy, NoBalancingDueToAllNodesEitherDrainingOrMaxedOut) {
    // shard0 and shard2 are draining, shard1 is maxed out
    auto [cluster, cm] =
        generateCluster({{ShardStatistics(kShardId0, kNoMaxSize, 2, true, emptyTagSet), 1},
                         {ShardStatistics(kShardId1, 1, 1, false, emptyTagSet), 6},
                         {ShardStatistics(kShardId2, kNoMaxSize, 1, true, emptyTagSet), 1}});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, makeDistStatus(cm), false, false);
    ASSERT(migrations.empty());
}

TEST(BalancerPolicy, BalancerRespectsMaxShardSizeOnlyBalanceToNonMaxed) {
    // Note that maxSize of shard0 is 1, and it is therefore overloaded with currSize = 3. Other
    // shards have maxSize = 0 = unset. Even though the overloaded shard has the least number of
    // less chunks, we shouldn't move chunks to that shard.
    auto [cluster, cm] =
        generateCluster({{ShardStatistics(kShardId0, 1, 3, false, emptyTagSet), 2},
                         {ShardStatistics(kShardId1, kNoMaxSize, 5, false, emptyTagSet), 5},
                         {ShardStatistics(kShardId2, kNoMaxSize, 10, false, emptyTagSet), 10}});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, makeDistStatus(cm), false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(kShardId2, migrations[0].from);
    ASSERT_EQ(kShardId1, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId2][0].getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId2][0].getMax(), *migrations[0].maxKey);
}

TEST(BalancerPolicy, BalancerRespectsMaxShardSizeWhenAllBalanced) {
    // Note that maxSize of shard0 is 1, and it is therefore overloaded with currSize = 4. Other
    // shards have maxSize = 0 = unset. We check that being over the maxSize is NOT equivalent to
    // draining, we don't want to empty shards for no other reason than they are over this limit.
    auto [cluster, cm] =
        generateCluster({{ShardStatistics(kShardId0, 1, 4, false, emptyTagSet), 4},
                         {ShardStatistics(kShardId1, kNoMaxSize, 4, false, emptyTagSet), 4},
                         {ShardStatistics(kShardId2, kNoMaxSize, 4, false, emptyTagSet), 4}});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, makeDistStatus(cm), false, false);
    ASSERT(migrations.empty());
}

TEST(BalancerPolicy, BalancerRespectsTagsWhenDraining) {
    // shard1 drains the proper chunk to shard0, even though it is more loaded than shard2
    auto [cluster, cm] =
        generateCluster({{ShardStatistics(kShardId0, kNoMaxSize, 5, false, {"a"}), 6},
                         {ShardStatistics(kShardId1, kNoMaxSize, 5, true, {"a", "b"}), 1},
                         {ShardStatistics(kShardId2, kNoMaxSize, 5, false, {"b"}), 2}});

    ZoneInfo zoneInfo;
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(kSKeyPattern.globalMin(), BSON("x" << 7), "a")));
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(BSON("x" << 8), kSKeyPattern.globalMax(), "b")));
    const auto distribution = makeDistStatus(cm, std::move(zoneInfo));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(kShardId1, migrations[0].from);
    ASSERT_EQ(kShardId0, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId1][0].getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId1][0].getMax(), *migrations[0].maxKey);
    ASSERT_EQ(MigrationReason::drain, reason);
}

TEST(BalancerPolicy, BalancerRespectsTagPolicyBeforeImbalance) {
    // There is a large imbalance between shard0 and shard1, but the balancer must first fix the
    // chunks, which are on a wrong shard due to tag policy
    auto [cluster, cm] =
        generateCluster({{ShardStatistics(kShardId0, kNoMaxSize, 5, false, {"a"}), 2},
                         {ShardStatistics(kShardId1, kNoMaxSize, 5, false, {"a"}), 6},
                         {ShardStatistics(kShardId2, kNoMaxSize, 5, false, emptyTagSet), 2}});

    ZoneInfo zoneInfo;
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(kSKeyPattern.globalMin(), BSON("x" << 100), "a")));
    const auto distribution = makeDistStatus(cm, std::move(zoneInfo));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(kShardId2, migrations[0].from);
    ASSERT_EQ(kShardId0, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId2][0].getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId2][0].getMax(), *migrations[0].maxKey);
    ASSERT_EQ(MigrationReason::zoneViolation, reason);
}

TEST(BalancerPolicy, OverloadedShardCannotDonateDueToTags) {
    // There is a large imbalance between shard0 and the other shard, but shard0 can't donate chunks
    // because it would violate tags.
    auto [cluster, cm] =
        generateCluster({{ShardStatistics(kShardId0, kNoMaxSize, 5, false, {"a"}), 5},
                         {ShardStatistics(kShardId1, kNoMaxSize, 0, false, emptyTagSet), 0},
                         {ShardStatistics(kShardId2, kNoMaxSize, 0, false, emptyTagSet), 0}});

    ZoneInfo zoneInfo;
    ASSERT_OK(zoneInfo.addRangeToZone(
        ZoneRange(kSKeyPattern.globalMin(), kSKeyPattern.globalMax(), "a")));
    const auto distribution = makeDistStatus(cm, std::move(zoneInfo));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(0U, migrations.size());
}

TEST(BalancerPolicy, BalancerFixesIncorrectTagsWithCrossShardViolationOfTags) {
    // The zone policy dictates that the same shard must donate and also receive chunks. The test
    // validates that the same shard is not used as a donor and recipient as part of the same round.
    auto [cluster, cm] =
        generateCluster({{ShardStatistics(kShardId0, kNoMaxSize, 5, false, {"a"}), 3},
                         {ShardStatistics(kShardId1, kNoMaxSize, 5, false, {"a"}), 3},
                         {ShardStatistics(kShardId2, kNoMaxSize, 5, false, {"b"}), 3}});

    ZoneInfo zoneInfo;
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(kSKeyPattern.globalMin(), BSON("x" << 1), "b")));
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(BSON("x" << 8), kSKeyPattern.globalMax(), "a")));
    const auto distribution = makeDistStatus(cm, std::move(zoneInfo));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(kShardId0, migrations[0].from);
    ASSERT_EQ(kShardId2, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMax(), *migrations[0].maxKey);
    ASSERT_EQ(MigrationReason::zoneViolation, reason);
}

TEST(BalancerPolicy, BalancerFixesIncorrectTagInOtherwiseBalancedCluster) {
    // Chunks are balanced across shards, but there are wrong tags, which need to be fixed
    auto [cluster, cm] =
        generateCluster({{ShardStatistics(kShardId0, kNoMaxSize, 5, false, {"a"}), 3},
                         {ShardStatistics(kShardId1, kNoMaxSize, 5, false, {"a"}), 3},
                         {ShardStatistics(kShardId2, kNoMaxSize, 5, false, emptyTagSet), 3}});

    ZoneInfo zoneInfo;
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(kSKeyPattern.globalMin(), BSON("x" << 10), "a")));
    const auto distribution = makeDistStatus(cm, std::move(zoneInfo));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(kShardId2, migrations[0].from);
    ASSERT_EQ(kShardId0, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId2][0].getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId2][0].getMax(), *migrations[0].maxKey);
    ASSERT_EQ(MigrationReason::zoneViolation, reason);
}

TEST(BalancerPolicy, BalancerFixesIncorrectTagsInOtherwiseBalancedCluster) {
    auto [cluster, cm] =
        generateCluster({{ShardStatistics(kShardId0, kNoMaxSize, 2, false, {}), 4},
                         {ShardStatistics(kShardId1, kNoMaxSize, 2, false, {"Zone1", "Zone2"}), 2},
                         {ShardStatistics(kShardId2, kNoMaxSize, 2, false, {"Zone1", "Zone2"}), 2},
                         {ShardStatistics(kShardId3, kNoMaxSize, 2, false, {}), 2}});

    ZoneInfo zoneInfo;
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(
        cluster.second[kShardId0][0].getMin(), cluster.second[kShardId0][0].getMax(), "Zone1")));
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(
        cluster.second[kShardId0][1].getMin(), cluster.second[kShardId0][1].getMax(), "Zone2")));
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(
        cluster.second[kShardId0][2].getMin(), cluster.second[kShardId0][2].getMax(), "Zone1")));
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(
        cluster.second[kShardId0][3].getMin(), cluster.second[kShardId0][3].getMax(), "Zone2")));
    const auto distribution = makeDistStatus(cm, std::move(zoneInfo));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(kShardId0, migrations[0].from);
    const auto& recipientShard = migrations[0].to;
    ASSERT(recipientShard == kShardId1 || recipientShard == kShardId2);
    ASSERT_EQ(MigrationReason::zoneViolation, reason);
}


TEST(BalancerPolicy, BalancerTagAlreadyBalanced) {
    // Chunks are balanced across shards for the tag.
    auto [cluster, cm] =
        generateCluster({{ShardStatistics(kShardId0, kNoMaxSize, 3, false, {"a"}), 2},
                         {ShardStatistics(kShardId1, kNoMaxSize, 2, false, {"a"}), 2}});

    ZoneInfo zoneInfo;
    ASSERT_OK(zoneInfo.addRangeToZone(
        ZoneRange(kSKeyPattern.globalMin(), kSKeyPattern.globalMax(), "a")));
    const auto distribution = makeDistStatus(cm, std::move(zoneInfo));
    ASSERT(balanceChunks(cluster.first, distribution, false, false).first.empty());
}

TEST(BalancerPolicy, BalancerMostOverLoadShardHasMultipleTags) {
    // shard0 has chunks [MinKey, 1), [1, 2), [2, 3), [3, 4), [4, 5), so two chunks each
    // for tag "b" and "c". So [1, 2) is expected to be moved to shard1 in round 1.
    auto [cluster, cm] =
        generateCluster({{ShardStatistics(kShardId0, kNoMaxSize, 5, false, {"a", "b", "c"}), 5},
                         {ShardStatistics(kShardId1, kNoMaxSize, 1, false, {"b"}), 1},
                         {ShardStatistics(kShardId2, kNoMaxSize, 1, false, {"c"}), 1}});

    ZoneInfo zoneInfo;
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(kSKeyPattern.globalMin(), BSON("x" << 1), "a")));
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(BSON("x" << 1), BSON("x" << 3), "b")));
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(BSON("x" << 3), BSON("x" << 5), "c")));
    const auto distribution = makeDistStatus(cm, std::move(zoneInfo));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(1U, migrations.size());

    ASSERT_EQ(kShardId0, migrations[0].from);
    ASSERT_EQ(kShardId1, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][1].getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][1].getMax(), *migrations[0].maxKey);
}

TEST(BalancerPolicy, BalancerMostOverLoadShardHasMultipleTagsSkipTagWithShardInUse) {
    // shard0 has chunks [MinKey, 1), [1, 2), [2, 3), [3, 4), [4, 5), so two chunks each
    // for tag "b" and "c". So [3, 4) is expected to be moved to shard2 because shard1 is
    // in use.
    auto [cluster, cm] =
        generateCluster({{ShardStatistics(kShardId0, kNoMaxSize, 5, false, {"a", "b", "c"}), 5},
                         {ShardStatistics(kShardId1, kNoMaxSize, 1, false, {"b"}), 1},
                         {ShardStatistics(kShardId2, kNoMaxSize, 1, false, {"c"}), 1}});

    ZoneInfo zoneInfo;
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(kSKeyPattern.globalMin(), BSON("x" << 1), "a")));
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(BSON("x" << 1), BSON("x" << 3), "b")));
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(BSON("x" << 3), BSON("x" << 5), "c")));
    const auto distribution = makeDistStatus(cm, std::move(zoneInfo));

    auto availableShards = getAllShardIds(cluster.first);
    availableShards.erase(kShardId1);
    const auto [migrations, reason] = BalancerPolicy::balance(
        cluster.first, distribution, boost::none /* collDataSizeInfo */, &availableShards, false);
    ASSERT_EQ(1U, migrations.size());

    ASSERT_EQ(kShardId0, migrations[0].from);
    ASSERT_EQ(kShardId2, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][3].getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][3].getMax(), *migrations[0].maxKey);
}

TEST(BalancerPolicy, BalancerFixesIncorrectTagsInOtherwiseBalancedClusterParallel) {
    // Chunks are balanced across shards, but there are wrong tags, which need to be fixed
    auto [cluster, cm] =
        generateCluster({{ShardStatistics(kShardId0, kNoMaxSize, 5, false, {"a"}), 3},
                         {ShardStatistics(kShardId1, kNoMaxSize, 5, false, {"a"}), 3},
                         {ShardStatistics(kShardId2, kNoMaxSize, 5, false, emptyTagSet), 3},
                         {ShardStatistics(kShardId3, kNoMaxSize, 5, false, emptyTagSet), 3}});

    ZoneInfo zoneInfo;
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(kSKeyPattern.globalMin(), BSON("x" << 20), "a")));
    const auto distribution = makeDistStatus(cm, std::move(zoneInfo));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(2U, migrations.size());

    ASSERT_EQ(kShardId2, migrations[0].from);
    ASSERT_EQ(kShardId0, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId2][0].getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId2][0].getMax(), *migrations[0].maxKey);
    ASSERT_EQ(MigrationReason::zoneViolation, reason);

    ASSERT_EQ(kShardId3, migrations[1].from);
    ASSERT_EQ(kShardId1, migrations[1].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId3][0].getMin(), migrations[1].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId3][0].getMax(), *migrations[1].maxKey);
    ASSERT_EQ(MigrationReason::zoneViolation, reason);
}

TEST(BalancerPolicy, ChunksInNoZoneSpanOnAllShardsWithEmptyZones) {
    // Balanacer is able to move chunks in the noZone to shards with tags
    auto [cluster, cm] =
        generateCluster({{ShardStatistics(kShardId0, kNoMaxSize, 5, false, emptyTagSet), 3},
                         {ShardStatistics(kShardId1, kNoMaxSize, 5, false, {"a"}), 0}});

    ZoneInfo zoneInfo;
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(BSON("x" << 100), kSKeyPattern.globalMax(), "a")));
    const auto distribution = makeDistStatus(cm, std::move(zoneInfo));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(1U, migrations.size());

    ASSERT_EQ(kShardId0, migrations[0].from);
    ASSERT_EQ(kShardId1, migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMin(), migrations[0].minKey);
    ASSERT_BSONOBJ_EQ(cluster.second[kShardId0][0].getMax(), *migrations[0].maxKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);
}

TEST(BalancerPolicy, BalancingNoZoneIgnoreTotalShardSize) {
    // Shard1 is overloaded and contains:
    // [min, 1) [1, 2) [2, 3] -> zone("a")
    // [3, 4) [4, 5) [5, 6)   -> NoZone
    //
    // But it won't donate any chunk since the
    auto [cluster, cm] =
        generateCluster({{ShardStatistics(kShardId0, kNoMaxSize, 5, false, {"a"}), 3},
                         {ShardStatistics(kShardId1, kNoMaxSize, 5, false, {"a"}), 6},
                         {ShardStatistics(kShardId2, kNoMaxSize, 5, false, emptyTagSet), 3}});

    ZoneInfo zoneInfo;
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(kSKeyPattern.globalMin(), BSON("x" << 6), "a")));
    const auto distribution = makeDistStatus(cm, std::move(zoneInfo));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(0U, migrations.size());
}

TEST(BalancerPolicy, BalancerHandlesNoShardsWithTag) {
    auto [cluster, cm] =
        generateCluster({{ShardStatistics(kShardId0, kNoMaxSize, 5, false, emptyTagSet), 2},
                         {ShardStatistics(kShardId1, kNoMaxSize, 5, false, emptyTagSet), 2}});

    ZoneInfo zoneInfo;
    ASSERT_OK(zoneInfo.addRangeToZone(
        ZoneRange(kSKeyPattern.globalMin(), BSON("x" << 7), "NonExistentZone")));
    const auto distribution = makeDistStatus(cm, std::move(zoneInfo));

    ASSERT(balanceChunks(cluster.first, distribution, false, false).first.empty());
}

TEST(DistributionStatus, OneChunkNoZone) {
    const auto chunks =
        makeChunks({{kShardId0, {kSKeyPattern.globalMin(), kSKeyPattern.globalMax()}}});
    const auto cm = makeChunkManager(chunks);
    const auto distStatus = makeDistStatus(cm);

    ASSERT_EQ(kNamespace, distStatus.nss());

    ASSERT_EQ(1, distStatus.totalChunksWithTag(ZoneInfo::kNoZoneName));
    ASSERT_EQ(0, distStatus.totalChunksWithTag("NotExistingZone"));

    ASSERT_EQ(1, distStatus.numberOfChunksInShard(kShardId0));
    ASSERT_EQ(0, distStatus.numberOfChunksInShard(kShardId1));

    ASSERT_EQ(1, distStatus.numberOfChunksInShardWithTag(kShardId0, ZoneInfo::kNoZoneName));
    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId0, "NotExistingZone"));

    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId1, ZoneInfo::kNoZoneName));
    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId1, "NotExistingZone"));

    ASSERT_EQ(1, distStatus.getZoneInfoForShard(kShardId0).size());
    ASSERT_EQ(0, distStatus.getZoneInfoForShard(kShardId1).size());

    const auto& noZoneInfo = distStatus.getZoneInfoForShard(kShardId0).at(ZoneInfo::kNoZoneName);
    ASSERT_EQ(1, noZoneInfo.numChunks);
    ASSERT_BSONOBJ_EQ(kSKeyPattern.globalMin(), noZoneInfo.firstChunkMinKey);

    checkChunksOnShardForTag(distStatus, kShardId0, "NotExistingZone", {});
    checkChunksOnShardForTag(distStatus, kShardId1, "NotExistingZone", {});
    checkChunksOnShardForTag(distStatus, kShardId1, ZoneInfo::kNoZoneName, {});
    checkChunksOnShardForTag(distStatus, kShardId0, ZoneInfo::kNoZoneName, chunks);
}

TEST(DistributionStatus, OneChunkOneZone) {
    const auto chunks =
        makeChunks({{kShardId0, {kSKeyPattern.globalMin(), kSKeyPattern.globalMax()}}});
    const auto cm = makeChunkManager(chunks);

    ZoneInfo zoneInfo;
    ASSERT_OK(zoneInfo.addRangeToZone(
        ZoneRange(kSKeyPattern.globalMin(), kSKeyPattern.globalMax(), "ZoneA")));
    const auto distStatus = makeDistStatus(cm, std::move(zoneInfo));

    ASSERT_EQ(kNamespace, distStatus.nss());

    ASSERT_EQ(0, distStatus.totalChunksWithTag(ZoneInfo::kNoZoneName));
    ASSERT_EQ(0, distStatus.totalChunksWithTag("NotExistingZone"));
    ASSERT_EQ(1, distStatus.totalChunksWithTag("ZoneA"));

    ASSERT_EQ(1, distStatus.numberOfChunksInShard(kShardId0));
    ASSERT_EQ(0, distStatus.numberOfChunksInShard(kShardId1));

    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId0, ZoneInfo::kNoZoneName));
    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId0, "NotExistingZone"));
    ASSERT_EQ(1, distStatus.numberOfChunksInShardWithTag(kShardId0, "ZoneA"));

    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId1, ZoneInfo::kNoZoneName));
    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId1, "NotExistingZone"));
    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId1, "ZoneA"));

    ASSERT_EQ(1, distStatus.getZoneInfoForShard(kShardId0).size());
    ASSERT_EQ(0, distStatus.getZoneInfoForShard(kShardId1).size());

    const auto& shardZoneInfo = distStatus.getZoneInfoForShard(kShardId0).at("ZoneA");
    ASSERT_EQ(1, shardZoneInfo.numChunks);
    ASSERT_BSONOBJ_EQ(kSKeyPattern.globalMin(), shardZoneInfo.firstChunkMinKey);

    checkChunksOnShardForTag(distStatus, kShardId1, ZoneInfo::kNoZoneName, {});
    checkChunksOnShardForTag(distStatus, kShardId1, "NotExistingZone", {});
    checkChunksOnShardForTag(distStatus, kShardId1, "ZoneA", {});
    checkChunksOnShardForTag(distStatus, kShardId0, ZoneInfo::kNoZoneName, {});
    checkChunksOnShardForTag(distStatus, kShardId0, "NotExistingZone", {});
    checkChunksOnShardForTag(distStatus, kShardId0, "ZoneA", chunks);
}

TEST(DistributionStatus, OneChunkMultipleContiguosZones) {
    const auto chunks =
        makeChunks({{kShardId0, {kSKeyPattern.globalMin(), kSKeyPattern.globalMax()}}});
    const auto cm = makeChunkManager(chunks);

    ZoneInfo zoneInfo;
    ASSERT_OK(
        zoneInfo.addRangeToZone(ZoneRange(kSKeyPattern.globalMin(), BSON("x" << 0), "ZoneA")));
    ASSERT_OK(
        zoneInfo.addRangeToZone(ZoneRange(BSON("x" << 0), kSKeyPattern.globalMax(), "ZoneB")));
    const auto distStatus = makeDistStatus(cm, std::move(zoneInfo));

    ASSERT_EQ(kNamespace, distStatus.nss());

    ASSERT_EQ(1, distStatus.totalChunksWithTag(ZoneInfo::kNoZoneName));
    ASSERT_EQ(0, distStatus.totalChunksWithTag("NotExistingZone"));
    ASSERT_EQ(0, distStatus.totalChunksWithTag("ZoneA"));
    ASSERT_EQ(0, distStatus.totalChunksWithTag("ZoneB"));

    ASSERT_EQ(1, distStatus.numberOfChunksInShard(kShardId0));
    ASSERT_EQ(0, distStatus.numberOfChunksInShard(kShardId1));

    ASSERT_EQ(1, distStatus.numberOfChunksInShardWithTag(kShardId0, ZoneInfo::kNoZoneName));
    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId0, "NotExistingZone"));
    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId0, "ZoneA"));
    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId0, "ZoneB"));

    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId1, ZoneInfo::kNoZoneName));
    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId1, "NotExistingZone"));
    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId1, "ZoneA"));
    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId1, "ZoneB"));

    ASSERT_EQ(1, distStatus.getZoneInfoForShard(kShardId0).size());
    ASSERT_EQ(0, distStatus.getZoneInfoForShard(kShardId1).size());

    const auto& shardZoneInfo = distStatus.getZoneInfoForShard(kShardId0).at(ZoneInfo::kNoZoneName);
    ASSERT_EQ(1, shardZoneInfo.numChunks);
    ASSERT_BSONOBJ_EQ(kSKeyPattern.globalMin(), shardZoneInfo.firstChunkMinKey);

    checkChunksOnShardForTag(distStatus, kShardId1, ZoneInfo::kNoZoneName, {});
    checkChunksOnShardForTag(distStatus, kShardId1, "NotExistingZone", {});
    checkChunksOnShardForTag(distStatus, kShardId1, "ZoneA", {});
    checkChunksOnShardForTag(distStatus, kShardId0, ZoneInfo::kNoZoneName, chunks);
    checkChunksOnShardForTag(distStatus, kShardId0, "NotExistingZone", {});
    checkChunksOnShardForTag(distStatus, kShardId0, "ZoneA", {});
}

TEST(DistributionStatus, OneChunkMultipleSparseZones) {
    const auto chunks =
        makeChunks({{kShardId0, {kSKeyPattern.globalMin(), kSKeyPattern.globalMax()}}});
    const auto cm = makeChunkManager(chunks);

    ZoneInfo zoneInfo;
    ASSERT_OK(
        zoneInfo.addRangeToZone(ZoneRange(kSKeyPattern.globalMin(), BSON("x" << 0), "ZoneA")));
    ASSERT_OK(
        zoneInfo.addRangeToZone(ZoneRange(BSON("x" << 10), kSKeyPattern.globalMax(), "ZoneB")));
    const auto distStatus = makeDistStatus(cm, std::move(zoneInfo));

    ASSERT_EQ(kNamespace, distStatus.nss());

    ASSERT_EQ(1, distStatus.totalChunksWithTag(ZoneInfo::kNoZoneName));
    ASSERT_EQ(0, distStatus.totalChunksWithTag("NotExistingZone"));
    ASSERT_EQ(0, distStatus.totalChunksWithTag("ZoneA"));
    ASSERT_EQ(0, distStatus.totalChunksWithTag("ZoneB"));

    ASSERT_EQ(1, distStatus.numberOfChunksInShard(kShardId0));
    ASSERT_EQ(0, distStatus.numberOfChunksInShard(kShardId1));

    ASSERT_EQ(1, distStatus.numberOfChunksInShardWithTag(kShardId0, ZoneInfo::kNoZoneName));
    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId0, "NotExistingZone"));
    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId0, "ZoneA"));
    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId0, "ZoneB"));

    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId1, ZoneInfo::kNoZoneName));
    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId1, "NotExistingZone"));
    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId1, "ZoneA"));
    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId1, "ZoneB"));

    ASSERT_EQ(1, distStatus.getZoneInfoForShard(kShardId0).size());
    ASSERT_EQ(0, distStatus.getZoneInfoForShard(kShardId1).size());

    const auto& shardZoneInfo = distStatus.getZoneInfoForShard(kShardId0).at(ZoneInfo::kNoZoneName);
    ASSERT_EQ(1, shardZoneInfo.numChunks);
    ASSERT_BSONOBJ_EQ(kSKeyPattern.globalMin(), shardZoneInfo.firstChunkMinKey);

    checkChunksOnShardForTag(distStatus, kShardId1, ZoneInfo::kNoZoneName, {});
    checkChunksOnShardForTag(distStatus, kShardId1, "NotExistingZone", {});
    checkChunksOnShardForTag(distStatus, kShardId1, "ZoneA", {});
    checkChunksOnShardForTag(distStatus, kShardId1, "ZoneB", {});
    checkChunksOnShardForTag(distStatus, kShardId0, ZoneInfo::kNoZoneName, chunks);
    checkChunksOnShardForTag(distStatus, kShardId0, "NotExistingZone", {});
    checkChunksOnShardForTag(distStatus, kShardId0, "ZoneA", {});
    checkChunksOnShardForTag(distStatus, kShardId0, "ZoneB", {});
}

TEST(DistributionStatus, MultipleChunksNoZone) {
    const auto chunks = makeChunks({{kShardId0, {kSKeyPattern.globalMin(), BSON("x" << 0)}},
                                    {kShardId0, {BSON("x" << 0), kSKeyPattern.globalMax()}}});
    const auto cm = makeChunkManager(chunks);
    const auto distStatus = makeDistStatus(cm);

    ASSERT_EQ(kNamespace, distStatus.nss());

    ASSERT_EQ(2, distStatus.totalChunksWithTag(ZoneInfo::kNoZoneName));
    ASSERT_EQ(0, distStatus.totalChunksWithTag("NotExistingZone"));

    ASSERT_EQ(2, distStatus.numberOfChunksInShard(kShardId0));
    ASSERT_EQ(0, distStatus.numberOfChunksInShard(kShardId1));

    ASSERT_EQ(2, distStatus.numberOfChunksInShardWithTag(kShardId0, ZoneInfo::kNoZoneName));
    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId0, "NotExistingZone"));

    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId1, ZoneInfo::kNoZoneName));
    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId1, "NotExistingZone"));

    ASSERT_EQ(1, distStatus.getZoneInfoForShard(kShardId0).size());
    ASSERT_EQ(0, distStatus.getZoneInfoForShard(kShardId1).size());

    const auto& noZoneInfo = distStatus.getZoneInfoForShard(kShardId0).at(ZoneInfo::kNoZoneName);
    ASSERT_EQ(2, noZoneInfo.numChunks);
    ASSERT_BSONOBJ_EQ(kSKeyPattern.globalMin(), noZoneInfo.firstChunkMinKey);

    checkChunksOnShardForTag(distStatus, kShardId0, "NotExistingZone", {});
    checkChunksOnShardForTag(distStatus, kShardId1, "NotExistingZone", {});
    checkChunksOnShardForTag(distStatus, kShardId1, ZoneInfo::kNoZoneName, {});
    checkChunksOnShardForTag(distStatus, kShardId0, ZoneInfo::kNoZoneName, chunks);
}

TEST(DistributionStatus, MultipleChunksDistributedNoZone) {
    const auto chunks = makeChunks({{kShardId1, {kSKeyPattern.globalMin(), BSON("x" << 0)}},
                                    {kShardId0, {BSON("x" << 0), kSKeyPattern.globalMax()}}});
    const auto cm = makeChunkManager(chunks);
    const auto distStatus = makeDistStatus(cm);

    ASSERT_EQ(kNamespace, distStatus.nss());

    ASSERT_EQ(2, distStatus.totalChunksWithTag(ZoneInfo::kNoZoneName));
    ASSERT_EQ(0, distStatus.totalChunksWithTag("NotExistingZone"));

    ASSERT_EQ(1, distStatus.numberOfChunksInShard(kShardId0));
    ASSERT_EQ(1, distStatus.numberOfChunksInShard(kShardId1));

    ASSERT_EQ(1, distStatus.numberOfChunksInShardWithTag(kShardId0, ZoneInfo::kNoZoneName));
    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId0, "NotExistingZone"));

    ASSERT_EQ(1, distStatus.numberOfChunksInShardWithTag(kShardId1, ZoneInfo::kNoZoneName));
    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId1, "NotExistingZone"));

    ASSERT_EQ(1, distStatus.getZoneInfoForShard(kShardId0).size());
    ASSERT_EQ(1, distStatus.getZoneInfoForShard(kShardId1).size());

    const auto& noZoneInfoS0 = distStatus.getZoneInfoForShard(kShardId0).at(ZoneInfo::kNoZoneName);
    ASSERT_EQ(1, noZoneInfoS0.numChunks);
    ASSERT_BSONOBJ_EQ(BSON("x" << 0), noZoneInfoS0.firstChunkMinKey);

    const auto& noZoneInfoS1 = distStatus.getZoneInfoForShard(kShardId1).at(ZoneInfo::kNoZoneName);
    ASSERT_EQ(1, noZoneInfoS1.numChunks);
    ASSERT_BSONOBJ_EQ(kSKeyPattern.globalMin(), noZoneInfoS1.firstChunkMinKey);

    checkChunksOnShardForTag(distStatus, kShardId0, "NotExistingZone", {});
    checkChunksOnShardForTag(distStatus, kShardId1, "NotExistingZone", {});
    checkChunksOnShardForTag(distStatus, kShardId0, ZoneInfo::kNoZoneName, {chunks[1]});
    checkChunksOnShardForTag(distStatus, kShardId1, ZoneInfo::kNoZoneName, {chunks[0]});
}

TEST(DistributionStatus, MultipleChunksTwoShardsOneZone) {
    const auto chunks = makeChunks({{kShardId0, {kSKeyPattern.globalMin(), BSON("x" << 0)}},
                                    {kShardId0, {BSON("x" << 0), kSKeyPattern.globalMax()}}});
    const auto cm = makeChunkManager(chunks);

    ZoneInfo zoneInfo;
    ASSERT_OK(zoneInfo.addRangeToZone(
        ZoneRange(kSKeyPattern.globalMin(), kSKeyPattern.globalMax(), "ZoneA")));
    const auto distStatus = makeDistStatus(cm, std::move(zoneInfo));

    ASSERT_EQ(kNamespace, distStatus.nss());

    ASSERT_EQ(0, distStatus.totalChunksWithTag(ZoneInfo::kNoZoneName));
    ASSERT_EQ(0, distStatus.totalChunksWithTag("NotExistingZone"));
    ASSERT_EQ(2, distStatus.totalChunksWithTag("ZoneA"));

    ASSERT_EQ(2, distStatus.numberOfChunksInShard(kShardId0));
    ASSERT_EQ(0, distStatus.numberOfChunksInShard(kShardId1));

    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId0, ZoneInfo::kNoZoneName));
    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId0, "NotExistingZone"));
    ASSERT_EQ(2, distStatus.numberOfChunksInShardWithTag(kShardId0, "ZoneA"));

    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId1, ZoneInfo::kNoZoneName));
    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId1, "NotExistingZone"));
    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId1, "ZoneA"));

    ASSERT_EQ(1, distStatus.getZoneInfoForShard(kShardId0).size());
    ASSERT_EQ(0, distStatus.getZoneInfoForShard(kShardId1).size());

    const auto& noZoneInfo = distStatus.getZoneInfoForShard(kShardId0).at("ZoneA");
    ASSERT_EQ(2, noZoneInfo.numChunks);
    ASSERT_BSONOBJ_EQ(kSKeyPattern.globalMin(), noZoneInfo.firstChunkMinKey);

    checkChunksOnShardForTag(distStatus, kShardId0, ZoneInfo::kNoZoneName, {});
    checkChunksOnShardForTag(distStatus, kShardId0, "NotExistingZone", {});
    checkChunksOnShardForTag(distStatus, kShardId0, "ZoneA", chunks);
    checkChunksOnShardForTag(distStatus, kShardId1, ZoneInfo::kNoZoneName, {});
    checkChunksOnShardForTag(distStatus, kShardId1, "NotExistingZone", {});
    checkChunksOnShardForTag(distStatus, kShardId1, "ZoneA", {});
}

TEST(DistributionStatus, MultipleChunksTwoContiguosZones) {
    const auto chunks = makeChunks({{kShardId0, {kSKeyPattern.globalMin(), BSON("x" << 0)}},
                                    {kShardId0, {BSON("x" << 0), kSKeyPattern.globalMax()}}});
    const auto cm = makeChunkManager(chunks);

    ZoneInfo zoneInfo;
    ASSERT_OK(
        zoneInfo.addRangeToZone(ZoneRange(kSKeyPattern.globalMin(), BSON("x" << 0), "ZoneA")));
    ASSERT_OK(
        zoneInfo.addRangeToZone(ZoneRange(BSON("x" << 0), kSKeyPattern.globalMax(), "ZoneB")));
    const auto distStatus = makeDistStatus(cm, std::move(zoneInfo));

    ASSERT_EQ(kNamespace, distStatus.nss());

    ASSERT_EQ(0, distStatus.totalChunksWithTag(ZoneInfo::kNoZoneName));
    ASSERT_EQ(0, distStatus.totalChunksWithTag("NotExistingZone"));
    ASSERT_EQ(1, distStatus.totalChunksWithTag("ZoneA"));
    ASSERT_EQ(1, distStatus.totalChunksWithTag("ZoneB"));

    ASSERT_EQ(2, distStatus.numberOfChunksInShard(kShardId0));
    ASSERT_EQ(0, distStatus.numberOfChunksInShard(kShardId1));

    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId0, ZoneInfo::kNoZoneName));
    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId0, "NotExistingZone"));
    ASSERT_EQ(1, distStatus.numberOfChunksInShardWithTag(kShardId0, "ZoneA"));
    ASSERT_EQ(1, distStatus.numberOfChunksInShardWithTag(kShardId0, "ZoneB"));

    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId1, ZoneInfo::kNoZoneName));
    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId1, "NotExistingZone"));
    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId1, "ZoneA"));
    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId1, "ZoneB"));

    ASSERT_EQ(2, distStatus.getZoneInfoForShard(kShardId0).size());
    ASSERT_EQ(0, distStatus.getZoneInfoForShard(kShardId1).size());

    auto& shardZoneAInfo = distStatus.getZoneInfoForShard(kShardId0).at("ZoneA");
    ASSERT_EQ(1, shardZoneAInfo.numChunks);
    ASSERT_BSONOBJ_EQ(kSKeyPattern.globalMin(), shardZoneAInfo.firstChunkMinKey);

    auto& shardZoneBInfo = distStatus.getZoneInfoForShard(kShardId0).at("ZoneB");
    ASSERT_EQ(1, shardZoneBInfo.numChunks);
    ASSERT_BSONOBJ_EQ(BSON("x" << 0), shardZoneBInfo.firstChunkMinKey);

    checkChunksOnShardForTag(distStatus, kShardId1, ZoneInfo::kNoZoneName, {});
    checkChunksOnShardForTag(distStatus, kShardId1, "NotExistingZone", {});
    checkChunksOnShardForTag(distStatus, kShardId1, "ZoneA", {});
    checkChunksOnShardForTag(distStatus, kShardId1, "ZoneB", {});
    checkChunksOnShardForTag(distStatus, kShardId0, ZoneInfo::kNoZoneName, {});
    checkChunksOnShardForTag(distStatus, kShardId0, "NotExistingZone", {});
    checkChunksOnShardForTag(distStatus, kShardId0, "ZoneA", {chunks[0]});
    checkChunksOnShardForTag(distStatus, kShardId0, "ZoneB", {chunks[1]});
}

TEST(DistributionStatus, MultipleChunksTwoZones) {
    const auto chunks = makeChunks({{kShardId0, {kSKeyPattern.globalMin(), BSON("x" << 0)}},
                                    {kShardId1, {BSON("x" << 0), BSON("x" << 10)}},
                                    {kShardId2, {BSON("x" << 10), BSON("x" << 20)}},
                                    {kShardId2, {BSON("x" << 20), BSON("x" << 30)}},
                                    {kShardId0, {BSON("x" << 30), BSON("x" << 40)}},
                                    {kShardId1, {BSON("x" << 40), BSON("x" << 50)}},
                                    {kShardId2, {BSON("x" << 50), kSKeyPattern.globalMax()}}});
    const auto cm = makeChunkManager(chunks);

    ZoneInfo zoneInfo;
    ASSERT_OK(
        zoneInfo.addRangeToZone(ZoneRange(kSKeyPattern.globalMin(), BSON("x" << 20), "ZoneA")));
    ASSERT_OK(
        zoneInfo.addRangeToZone(ZoneRange(BSON("x" << 30), kSKeyPattern.globalMax(), "ZoneB")));
    const auto distStatus = makeDistStatus(cm, std::move(zoneInfo));

    ASSERT_EQ(kNamespace, distStatus.nss());

    ASSERT_EQ(1, distStatus.totalChunksWithTag(ZoneInfo::kNoZoneName));
    ASSERT_EQ(3, distStatus.totalChunksWithTag("ZoneA"));
    ASSERT_EQ(3, distStatus.totalChunksWithTag("ZoneB"));
    ASSERT_EQ(0, distStatus.totalChunksWithTag("NotExistingZone"));

    ASSERT_EQ(2, distStatus.numberOfChunksInShard(kShardId0));
    ASSERT_EQ(2, distStatus.numberOfChunksInShard(kShardId1));
    ASSERT_EQ(3, distStatus.numberOfChunksInShard(kShardId2));

    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId0, ZoneInfo::kNoZoneName));
    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId0, "NotExistingZone"));
    ASSERT_EQ(1, distStatus.numberOfChunksInShardWithTag(kShardId0, "ZoneA"));
    ASSERT_EQ(1, distStatus.numberOfChunksInShardWithTag(kShardId0, "ZoneB"));

    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId1, ZoneInfo::kNoZoneName));
    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId1, "NotExistingZone"));
    ASSERT_EQ(1, distStatus.numberOfChunksInShardWithTag(kShardId1, "ZoneA"));
    ASSERT_EQ(1, distStatus.numberOfChunksInShardWithTag(kShardId1, "ZoneB"));

    ASSERT_EQ(1, distStatus.numberOfChunksInShardWithTag(kShardId2, ZoneInfo::kNoZoneName));
    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId2, "NotExistingZone"));
    ASSERT_EQ(1, distStatus.numberOfChunksInShardWithTag(kShardId2, "ZoneA"));
    ASSERT_EQ(1, distStatus.numberOfChunksInShardWithTag(kShardId2, "ZoneB"));

    ASSERT_EQ(2, distStatus.getZoneInfoForShard(kShardId0).size());
    ASSERT_EQ(2, distStatus.getZoneInfoForShard(kShardId1).size());
    ASSERT_EQ(3, distStatus.getZoneInfoForShard(kShardId2).size());

    auto& shard0ZoneAInfo = distStatus.getZoneInfoForShard(kShardId0).at("ZoneA");
    ASSERT_EQ(1, shard0ZoneAInfo.numChunks);
    ASSERT_BSONOBJ_EQ(kSKeyPattern.globalMin(), shard0ZoneAInfo.firstChunkMinKey);
    auto& shard0ZoneBInfo = distStatus.getZoneInfoForShard(kShardId0).at("ZoneB");
    ASSERT_EQ(1, shard0ZoneBInfo.numChunks);
    ASSERT_BSONOBJ_EQ(BSON("x" << 30), shard0ZoneBInfo.firstChunkMinKey);

    auto& shard1ZoneAInfo = distStatus.getZoneInfoForShard(kShardId1).at("ZoneA");
    ASSERT_EQ(1, shard1ZoneAInfo.numChunks);
    ASSERT_BSONOBJ_EQ(BSON("x" << 0), shard1ZoneAInfo.firstChunkMinKey);
    auto& shard1ZoneBInfo = distStatus.getZoneInfoForShard(kShardId1).at("ZoneB");
    ASSERT_EQ(1, shard1ZoneBInfo.numChunks);
    ASSERT_BSONOBJ_EQ(BSON("x" << 40), shard1ZoneBInfo.firstChunkMinKey);

    auto& shard2ZoneAInfo = distStatus.getZoneInfoForShard(kShardId2).at("ZoneA");
    ASSERT_EQ(1, shard2ZoneAInfo.numChunks);
    ASSERT_BSONOBJ_EQ(BSON("x" << 10), shard2ZoneAInfo.firstChunkMinKey);
    auto& shard2NoZoneInfo = distStatus.getZoneInfoForShard(kShardId2).at(ZoneInfo::kNoZoneName);
    ASSERT_EQ(1, shard2NoZoneInfo.numChunks);
    ASSERT_BSONOBJ_EQ(BSON("x" << 20), shard2NoZoneInfo.firstChunkMinKey);
    auto& shard2ZoneBInfo = distStatus.getZoneInfoForShard(kShardId2).at("ZoneB");
    ASSERT_EQ(1, shard2ZoneBInfo.numChunks);
    ASSERT_BSONOBJ_EQ(BSON("x" << 50), shard2ZoneBInfo.firstChunkMinKey);

    checkChunksOnShardForTag(distStatus, kShardId0, ZoneInfo::kNoZoneName, {});
    checkChunksOnShardForTag(distStatus, kShardId0, "NotExistingZone", {});
    checkChunksOnShardForTag(distStatus, kShardId0, "ZoneA", {chunks[0]});
    checkChunksOnShardForTag(distStatus, kShardId0, "ZoneB", {chunks[4]});

    checkChunksOnShardForTag(distStatus, kShardId1, ZoneInfo::kNoZoneName, {});
    checkChunksOnShardForTag(distStatus, kShardId1, "NotExistingZone", {});
    checkChunksOnShardForTag(distStatus, kShardId1, "ZoneA", {chunks[1]});
    checkChunksOnShardForTag(distStatus, kShardId1, "ZoneB", {chunks[5]});

    checkChunksOnShardForTag(distStatus, kShardId2, ZoneInfo::kNoZoneName, {chunks[3]});
    checkChunksOnShardForTag(distStatus, kShardId2, "NotExistingZone", {});
    checkChunksOnShardForTag(distStatus, kShardId2, "ZoneA", {chunks[2]});
    checkChunksOnShardForTag(distStatus, kShardId2, "ZoneB", {chunks[6]});
}

TEST(DistributionStatus, MultipleChunksMulitpleZoneRanges) {
    const auto chunks = makeChunks({{kShardId0, {kSKeyPattern.globalMin(), BSON("x" << 0)}},

                                    {kShardId1, {BSON("x" << 0), BSON("x" << 10)}},   // ZoneA
                                    {kShardId0, {BSON("x" << 10), BSON("x" << 20)}},  // ZoneA

                                    {kShardId1, {BSON("x" << 20), BSON("x" << 30)}},
                                    {kShardId0, {BSON("x" << 30), BSON("x" << 40)}},

                                    {kShardId1, {BSON("x" << 40), BSON("x" << 50)}},  // ZoneA
                                    {kShardId0, {BSON("x" << 50), BSON("x" << 60)}},  // ZoneA

                                    {kShardId1, {BSON("x" << 60), kSKeyPattern.globalMax()}}});
    const auto cm = makeChunkManager(chunks);

    ZoneInfo zoneInfo;
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(BSON("x" << 0), BSON("x" << 20), "ZoneA")));
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(BSON("x" << 40), BSON("x" << 60), "ZoneA")));
    const auto distStatus = makeDistStatus(cm, std::move(zoneInfo));

    ASSERT_EQ(kNamespace, distStatus.nss());

    ASSERT_EQ(4, distStatus.totalChunksWithTag(ZoneInfo::kNoZoneName));
    ASSERT_EQ(4, distStatus.totalChunksWithTag("ZoneA"));
    ASSERT_EQ(0, distStatus.totalChunksWithTag("NotExistingZone"));

    ASSERT_EQ(4, distStatus.numberOfChunksInShard(kShardId0));
    ASSERT_EQ(4, distStatus.numberOfChunksInShard(kShardId1));

    ASSERT_EQ(2, distStatus.numberOfChunksInShardWithTag(kShardId0, ZoneInfo::kNoZoneName));
    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId0, "NotExistingZone"));
    ASSERT_EQ(2, distStatus.numberOfChunksInShardWithTag(kShardId0, "ZoneA"));

    ASSERT_EQ(2, distStatus.numberOfChunksInShardWithTag(kShardId1, ZoneInfo::kNoZoneName));
    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId1, "NotExistingZone"));
    ASSERT_EQ(2, distStatus.numberOfChunksInShardWithTag(kShardId1, "ZoneA"));

    ASSERT_EQ(2, distStatus.getZoneInfoForShard(kShardId0).size());
    ASSERT_EQ(2, distStatus.getZoneInfoForShard(kShardId1).size());

    auto& shard0ZoneAInfo = distStatus.getZoneInfoForShard(kShardId0).at("ZoneA");
    ASSERT_EQ(2, shard0ZoneAInfo.numChunks);
    ASSERT_BSONOBJ_EQ(BSON("x" << 10), shard0ZoneAInfo.firstChunkMinKey);
    auto& shard0NoZoneInfo = distStatus.getZoneInfoForShard(kShardId0).at(ZoneInfo::kNoZoneName);
    ASSERT_EQ(2, shard0NoZoneInfo.numChunks);
    ASSERT_BSONOBJ_EQ(kSKeyPattern.globalMin(), shard0NoZoneInfo.firstChunkMinKey);

    auto& shard1ZoneAInfo = distStatus.getZoneInfoForShard(kShardId1).at("ZoneA");
    ASSERT_EQ(2, shard1ZoneAInfo.numChunks);
    ASSERT_BSONOBJ_EQ(BSON("x" << 0), shard1ZoneAInfo.firstChunkMinKey);
    auto& shard1NoZoneInfo = distStatus.getZoneInfoForShard(kShardId1).at(ZoneInfo::kNoZoneName);
    ASSERT_EQ(2, shard1NoZoneInfo.numChunks);
    ASSERT_BSONOBJ_EQ(BSON("x" << 20), shard1NoZoneInfo.firstChunkMinKey);

    checkChunksOnShardForTag(distStatus, kShardId0, ZoneInfo::kNoZoneName, {chunks[0], chunks[4]});
    checkChunksOnShardForTag(distStatus, kShardId0, "ZoneA", {chunks[2], chunks[6]});
    checkChunksOnShardForTag(distStatus, kShardId0, "NotExistingZone", {});

    checkChunksOnShardForTag(distStatus, kShardId1, ZoneInfo::kNoZoneName, {chunks[3], chunks[7]});
    checkChunksOnShardForTag(distStatus, kShardId1, "ZoneA", {chunks[1], chunks[5]});
    checkChunksOnShardForTag(distStatus, kShardId1, "NotExistingZone", {});
}

TEST(DistributionStatus, MultipleChunksMulitpleZoneRangesNotAligned) {
    const auto chunks =
        makeChunks({{kShardId0, {kSKeyPattern.globalMin(), BSON("x" << 0)}},     // Zone A
                    {kShardId0, {BSON("x" << 0), BSON("x" << 10)}},              // Zone A
                    {kShardId2, {BSON("x" << 10), BSON("x" << 20)}},             // No Zone
                    {kShardId2, {BSON("x" << 20), BSON("x" << 30)}},             // Zone A
                    {kShardId1, {BSON("x" << 30), BSON("x" << 40)}},             // No Zone
                    {kShardId1, {BSON("x" << 40), BSON("x" << 50)}},             // Zone B
                    {kShardId0, {BSON("x" << 50), BSON("x" << 60)}},             // No Zone
                    {kShardId0, {BSON("x" << 60), BSON("x" << 70)}},             // Zone B
                    {kShardId2, {BSON("x" << 70), kSKeyPattern.globalMax()}}});  // No zone
    const auto cm = makeChunkManager(chunks);

    ZoneInfo zoneInfo;
    ASSERT_OK(
        zoneInfo.addRangeToZone(ZoneRange(kSKeyPattern.globalMin(), BSON("x" << 15), "ZoneA")));
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(BSON("x" << 15), BSON("x" << 35), "ZoneA")));
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(BSON("x" << 40), BSON("x" << 55), "ZoneB")));
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(BSON("x" << 55), BSON("x" << 75), "ZoneB")));
    const auto distStatus = makeDistStatus(cm, std::move(zoneInfo));

    ASSERT_EQ(kNamespace, distStatus.nss());

    ASSERT_EQ(4, distStatus.totalChunksWithTag(ZoneInfo::kNoZoneName));
    ASSERT_EQ(3, distStatus.totalChunksWithTag("ZoneA"));
    ASSERT_EQ(2, distStatus.totalChunksWithTag("ZoneB"));
    ASSERT_EQ(0, distStatus.totalChunksWithTag("NotExistingZone"));

    ASSERT_EQ(4, distStatus.numberOfChunksInShard(kShardId0));
    ASSERT_EQ(2, distStatus.numberOfChunksInShard(kShardId1));
    ASSERT_EQ(3, distStatus.numberOfChunksInShard(kShardId2));

    ASSERT_EQ(1, distStatus.numberOfChunksInShardWithTag(kShardId0, ZoneInfo::kNoZoneName));
    ASSERT_EQ(2, distStatus.numberOfChunksInShardWithTag(kShardId0, "ZoneA"));
    ASSERT_EQ(1, distStatus.numberOfChunksInShardWithTag(kShardId0, "ZoneB"));
    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId0, "NotExistingZone"));

    ASSERT_EQ(1, distStatus.numberOfChunksInShardWithTag(kShardId1, ZoneInfo::kNoZoneName));
    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId1, "ZoneA"));
    ASSERT_EQ(1, distStatus.numberOfChunksInShardWithTag(kShardId1, "ZoneB"));
    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId1, "NotExistingZone"));

    ASSERT_EQ(2, distStatus.numberOfChunksInShardWithTag(kShardId2, ZoneInfo::kNoZoneName));
    ASSERT_EQ(1, distStatus.numberOfChunksInShardWithTag(kShardId2, "ZoneA"));
    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId2, "ZoneB"));
    ASSERT_EQ(0, distStatus.numberOfChunksInShardWithTag(kShardId2, "NotExistingZone"));

    ASSERT_EQ(3, distStatus.getZoneInfoForShard(kShardId0).size());
    ASSERT_EQ(2, distStatus.getZoneInfoForShard(kShardId1).size());
    ASSERT_EQ(2, distStatus.getZoneInfoForShard(kShardId2).size());

    checkChunksOnShardForTag(distStatus, kShardId0, ZoneInfo::kNoZoneName, {chunks[6]});
    checkChunksOnShardForTag(distStatus, kShardId0, "ZoneA", {chunks[0], chunks[1]});
    checkChunksOnShardForTag(distStatus, kShardId0, "ZoneB", {chunks[7]});
    checkChunksOnShardForTag(distStatus, kShardId0, "NotExistingZone", {});

    checkChunksOnShardForTag(distStatus, kShardId1, ZoneInfo::kNoZoneName, {chunks[4]});
    checkChunksOnShardForTag(distStatus, kShardId1, "ZoneA", {});
    checkChunksOnShardForTag(distStatus, kShardId1, "ZoneB", {chunks[5]});
    checkChunksOnShardForTag(distStatus, kShardId1, "NotExistingZone", {});

    checkChunksOnShardForTag(distStatus, kShardId2, ZoneInfo::kNoZoneName, {chunks[2], chunks[8]});
    checkChunksOnShardForTag(distStatus, kShardId2, "ZoneA", {chunks[3]});
    checkChunksOnShardForTag(distStatus, kShardId2, "ZoneB", {});
    checkChunksOnShardForTag(distStatus, kShardId2, "NotExistingZone", {});
}

TEST(DistributionStatus, forEachChunkOnShardInZoneExitCondition) {
    const auto chunks = makeChunks({{kShardId0, {kSKeyPattern.globalMin(), BSON("x" << 0)}},

                                    {kShardId1, {BSON("x" << 0), BSON("x" << 10)}},   // ZoneA
                                    {kShardId0, {BSON("x" << 10), BSON("x" << 20)}},  // ZoneA

                                    {kShardId1, {BSON("x" << 20), BSON("x" << 30)}},
                                    {kShardId0, {BSON("x" << 30), BSON("x" << 40)}},

                                    {kShardId1, {BSON("x" << 40), BSON("x" << 50)}},  // ZoneA
                                    {kShardId0, {BSON("x" << 50), BSON("x" << 60)}},  // ZoneA

                                    {kShardId1, {BSON("x" << 60), kSKeyPattern.globalMax()}}});
    const auto cm = makeChunkManager(chunks);

    ZoneInfo zoneInfo;
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(BSON("x" << 0), BSON("x" << 20), "ZoneA")));
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(BSON("x" << 40), BSON("x" << 60), "ZoneA")));
    const auto distStatus = makeDistStatus(cm, std::move(zoneInfo));

    const auto assertLoopStopAt =
        [&](const ShardId& shardId, const std::string& zoneName, const size_t stopCount) {
            size_t numChunkIterated{0};

            const auto completed =
                distStatus.forEachChunkOnShardInZone(shardId, zoneName, [&](const auto& chunk) {
                    if (++numChunkIterated == stopCount) {
                        return false;  // break
                    }
                    return true;  // continue
                });

            ASSERT(!completed) << "forEachChunkOnShardInZone loop did not stop";
            ASSERT_EQ(stopCount, numChunkIterated);
        };

    assertLoopStopAt(kShardId0, ZoneInfo::kNoZoneName, 1);
    assertLoopStopAt(kShardId0, ZoneInfo::kNoZoneName, 2);

    assertLoopStopAt(kShardId1, ZoneInfo::kNoZoneName, 1);
    assertLoopStopAt(kShardId1, ZoneInfo::kNoZoneName, 2);

    assertLoopStopAt(kShardId0, "ZoneA", 1);
    assertLoopStopAt(kShardId0, "ZoneA", 2);

    assertLoopStopAt(kShardId1, "ZoneA", 1);
    assertLoopStopAt(kShardId1, "ZoneA", 2);
}

TEST(ZoneInfo, AddTagRangeOverlap) {
    ZoneInfo zInfo;

    // Note that there is gap between 10 and 20 for which there is no tag
    ASSERT_OK(zInfo.addRangeToZone(ZoneRange(BSON("x" << 1), BSON("x" << 10), "a")));
    ASSERT_OK(zInfo.addRangeToZone(ZoneRange(BSON("x" << 20), BSON("x" << 30), "b")));

    ASSERT_EQ(ErrorCodes::RangeOverlapConflict,
              zInfo.addRangeToZone(ZoneRange(kSKeyPattern.globalMin(), BSON("x" << 2), "d")));
    ASSERT_EQ(ErrorCodes::RangeOverlapConflict,
              zInfo.addRangeToZone(ZoneRange(BSON("x" << -1), BSON("x" << 5), "d")));
    ASSERT_EQ(ErrorCodes::RangeOverlapConflict,
              zInfo.addRangeToZone(ZoneRange(BSON("x" << 5), BSON("x" << 9), "d")));
    ASSERT_EQ(ErrorCodes::RangeOverlapConflict,
              zInfo.addRangeToZone(ZoneRange(BSON("x" << 1), BSON("x" << 10), "d")));
    ASSERT_EQ(ErrorCodes::RangeOverlapConflict,
              zInfo.addRangeToZone(ZoneRange(BSON("x" << 5), BSON("x" << 25), "d")));
    ASSERT_EQ(ErrorCodes::RangeOverlapConflict,
              zInfo.addRangeToZone(ZoneRange(BSON("x" << -1), BSON("x" << 32), "d")));
    ASSERT_EQ(ErrorCodes::RangeOverlapConflict,
              zInfo.addRangeToZone(ZoneRange(BSON("x" << 25), kSKeyPattern.globalMax(), "d")));
}

TEST(ZoneInfo, ChunkTagsSelectorWithRegularKeys) {
    ZoneInfo zInfo;
    ASSERT_OK(zInfo.addRangeToZone(ZoneRange(BSON("x" << 1), BSON("x" << 10), "a")));
    ASSERT_OK(zInfo.addRangeToZone(ZoneRange(BSON("x" << 10), BSON("x" << 20), "b")));
    ASSERT_OK(zInfo.addRangeToZone(ZoneRange(BSON("x" << 20), BSON("x" << 30), "c")));

    ASSERT_EQUALS(ZoneInfo::kNoZoneName,
                  zInfo.getZoneForChunk({kSKeyPattern.globalMin(), BSON("x" << 1)}));
    ASSERT_EQUALS(ZoneInfo::kNoZoneName, zInfo.getZoneForChunk({BSON("x" << 0), BSON("x" << 1)}));
    ASSERT_EQUALS("a", zInfo.getZoneForChunk({BSON("x" << 1), BSON("x" << 5)}));
    ASSERT_EQUALS("b", zInfo.getZoneForChunk({BSON("x" << 10), BSON("x" << 20)}));
    ASSERT_EQUALS("b", zInfo.getZoneForChunk({BSON("x" << 15), BSON("x" << 20)}));
    ASSERT_EQUALS("c", zInfo.getZoneForChunk({BSON("x" << 25), BSON("x" << 30)}));
    ASSERT_EQUALS(ZoneInfo::kNoZoneName, zInfo.getZoneForChunk({BSON("x" << 35), BSON("x" << 40)}));
    ASSERT_EQUALS(ZoneInfo::kNoZoneName,
                  zInfo.getZoneForChunk({BSON("x" << 30), kSKeyPattern.globalMax()}));
    ASSERT_EQUALS(ZoneInfo::kNoZoneName,
                  zInfo.getZoneForChunk({BSON("x" << 40), kSKeyPattern.globalMax()}));
}

TEST(ZoneInfo, ChunkTagsSelectorWithMinMaxKeys) {

    ZoneInfo zInfo;
    ASSERT_OK(zInfo.addRangeToZone(ZoneRange(kSKeyPattern.globalMin(), BSON("x" << -100), "a")));
    ASSERT_OK(zInfo.addRangeToZone(ZoneRange(BSON("x" << -10), BSON("x" << 10), "b")));
    ASSERT_OK(zInfo.addRangeToZone(ZoneRange(BSON("x" << 100), kSKeyPattern.globalMax(), "c")));

    ASSERT_EQUALS("a", zInfo.getZoneForChunk({kSKeyPattern.globalMin(), BSON("x" << -100)}));
    ASSERT_EQUALS(ZoneInfo::kNoZoneName,
                  zInfo.getZoneForChunk({BSON("x" << -100), BSON("x" << -11)}));
    ASSERT_EQUALS("b", zInfo.getZoneForChunk({BSON("x" << -10), BSON("x" << 0)}));
    ASSERT_EQUALS("b", zInfo.getZoneForChunk({BSON("x" << 0), BSON("x" << 10)}));
    ASSERT_EQUALS(ZoneInfo::kNoZoneName, zInfo.getZoneForChunk({BSON("x" << 10), BSON("x" << 20)}));
    ASSERT_EQUALS(ZoneInfo::kNoZoneName,
                  zInfo.getZoneForChunk({BSON("x" << 10), BSON("x" << 100)}));
    ASSERT_EQUALS("c", zInfo.getZoneForChunk({BSON("x" << 200), kSKeyPattern.globalMax()}));
}

}  // namespace
}  // namespace mongo
