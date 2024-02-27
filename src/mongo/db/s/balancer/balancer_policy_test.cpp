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

#include <algorithm>
#include <cstdint>
#include <iosfwd>
#include <iterator>
#include <memory>
#include <type_traits>

#include <absl/container/node_hash_set.h>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/s/balancer/balancer_policy.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/chunks_test_util.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

using std::map;
using std::string;
using std::stringstream;
using std::vector;

using ShardStatistics = ClusterStatistics::ShardStatistics;
typedef std::map<ShardId, std::vector<ChunkType>> ShardToChunksMap;

auto& kDefaultMaxChunkSizeBytes = ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes;
PseudoRandom _random{SecureRandom().nextInt64()};

const auto emptyZoneSet = std::set<std::string>();
const NamespaceString kNamespace =
    NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
const KeyPattern kShardKeyPattern(BSON("x" << 1));
const Timestamp kCollTimestamp{1, 1};
const OID kCollEpoch;

struct ShardSpec {
    ShardSpec(size_t numChunks,
              size_t currSizeBytes,
              bool isDraining = false,
              const std::set<std::string>& shardZones = emptyZoneSet)
        : numChunks(numChunks),
          currSizeBytes(currSizeBytes),
          isDraining(isDraining),
          shardZones(shardZones) {}

    size_t numChunks;
    size_t currSizeBytes;
    bool isDraining;
    std::set<std::string> shardZones;
};

ShardId getShardId(size_t shardIdx) {
    return {std::string(str::stream() << "shard_" << shardIdx)};
}

const UUID& collUUID() {
    static const UUID kCollectionUUID{UUID::gen()};
    return kCollectionUUID;
}

std::vector<ChunkType> makeChunks(const std::vector<std::pair<ShardId, ChunkRange>>& specs) {
    ChunkVersion chunkVersion({kCollEpoch, kCollTimestamp}, {1, 0});
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
                                        kShardKeyPattern,
                                        false /* unsplittable */,
                                        nullptr /* defaultCollator */,
                                        false /* unique */,
                                        kCollEpoch,
                                        kCollTimestamp,
                                        boost::none /* timeseriesFields */,
                                        boost::none /* reshardingFields */,
                                        true /* allowMigrations */,
                                        chunks);
}

ChunkManager makeChunkManager(const std::vector<ChunkType>& chunks) {
    static const auto kConfigId = ShardId("config");
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
    const vector<ShardSpec>& shardsSpec) {

    const auto totalNumChunks = [&] {
        size_t total{0};
        for (const auto& shardSpec : shardsSpec) {
            total += shardSpec.numChunks;
        }
        return total;
    }();

    std::vector<ChunkType> chunks;
    ShardToChunksMap chunkMap;
    ShardStatisticsVector shardStats;

    size_t currentChunk{0};

    ChunkVersion chunkVersion({kCollEpoch, kCollTimestamp}, {1, 0});

    size_t currentShardIdx{0};

    for (const auto& shardSpec : shardsSpec) {
        const auto shardId = getShardId(currentShardIdx++);

        // Ensure that an entry is created
        chunkMap[shardId];

        for (size_t i = 0; i < shardSpec.numChunks; i++, currentChunk++) {
            ChunkType chunk;

            chunk.setCollectionUUID(collUUID());
            chunk.setMin(currentChunk == 0 ? kShardKeyPattern.globalMin()
                                           : BSON("x" << (long long)currentChunk));
            chunk.setMax(currentChunk == totalNumChunks - 1
                             ? kShardKeyPattern.globalMax()
                             : BSON("x" << (long long)currentChunk + 1));
            chunk.setShard(shardId);
            chunk.setVersion(chunkVersion);

            chunkVersion.incMajor();

            chunkMap[shardId].push_back(chunk);
            chunks.push_back(std::move(chunk));
        }

        shardStats.emplace_back(shardId,
                                shardSpec.currSizeBytes,
                                shardSpec.isDraining,
                                shardSpec.shardZones,
                                ShardStatistics::use_bytes_t());
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

CollectionDataSizeInfoForBalancing buildDataSizeInfoForBalancingFromShardStats(
    const ShardStatisticsVector& shardStats) {
    std::map<ShardId, int64_t> collSizePerShard;
    for (const auto& shard : shardStats) {
        collSizePerShard.try_emplace(shard.shardId, shard.currSizeBytes);
    }

    return CollectionDataSizeInfoForBalancing(std::move(collSizePerShard),
                                              kDefaultMaxChunkSizeBytes);
}

MigrateInfosWithReason balanceChunks(const ShardStatisticsVector& shardStats,
                                     const DistributionStatus& distribution,
                                     bool shouldAggressivelyBalance,
                                     bool forceJumbo) {
    auto availableShards = getAllShardIds(shardStats);

    return BalancerPolicy::balance(shardStats,
                                   distribution,
                                   buildDataSizeInfoForBalancingFromShardStats(shardStats),
                                   &availableShards,
                                   forceJumbo);
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
    auto [cluster, cm] = generateCluster({{4, 4 * kDefaultMaxChunkSizeBytes},
                                          {0, 0 * kDefaultMaxChunkSizeBytes},
                                          {3, 3 * kDefaultMaxChunkSizeBytes}});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, makeDistStatus(cm), false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(getShardId(0), migrations[0].from);
    ASSERT_EQ(getShardId(1), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(0)][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);
}

TEST(BalancerPolicy, BasicWithManyChunks) {
    auto [cluster, cm] = generateCluster({{10, 1 * kDefaultMaxChunkSizeBytes},
                                          {2, 3 * kDefaultMaxChunkSizeBytes},
                                          {20, 0 * kDefaultMaxChunkSizeBytes}});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, makeDistStatus(cm), false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(getShardId(1), migrations[0].from);
    ASSERT_EQ(getShardId(2), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(1)][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);
}

TEST(BalancerPolicy, SmallSingleChunkShouldNotMove) {
    auto [cluster, cm] =
        generateCluster({{1, 1 * kDefaultMaxChunkSizeBytes}, {0, 0 * kDefaultMaxChunkSizeBytes}});
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
    auto [cluster, cm] = generateCluster({{2, 2 * kDefaultMaxChunkSizeBytes},
                                          {2, 2 * kDefaultMaxChunkSizeBytes},
                                          {1, 1 * kDefaultMaxChunkSizeBytes},
                                          {1, 1 * kDefaultMaxChunkSizeBytes}});

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
    auto [cluster, cm] = generateCluster({{4, 4 * kDefaultMaxChunkSizeBytes},
                                          {4, 4 * kDefaultMaxChunkSizeBytes},
                                          {0, 0 * kDefaultMaxChunkSizeBytes},
                                          {0, 0 * kDefaultMaxChunkSizeBytes}});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, makeDistStatus(cm), false, false);
    ASSERT_EQ(2U, migrations.size());

    ASSERT_EQ(getShardId(0), migrations[0].from);
    ASSERT_EQ(getShardId(2), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(0)][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);

    ASSERT_EQ(getShardId(1), migrations[1].from);
    ASSERT_EQ(getShardId(3), migrations[1].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(1)][0].getMin(), migrations[1].minKey);
}

TEST(BalancerPolicy, ParallelBalancingDoesNotScheduleMigrationsOnShardsAboveIdealDataSize) {
    // TotalDataSize = (360 * ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes)
    // NumShards = 6
    // IdealDataSize = TotalDataSize / NumShards = 60
    // No migration must be scheduled for shards owning am amount greater or equal than 60
    auto [cluster, cm] = generateCluster({{100, 100 * kDefaultMaxChunkSizeBytes},
                                          {90, 90 * kDefaultMaxChunkSizeBytes},
                                          {90, 90 * kDefaultMaxChunkSizeBytes},
                                          {80, 80 * kDefaultMaxChunkSizeBytes},
                                          {0, 0 * kDefaultMaxChunkSizeBytes},
                                          {0, 0 * kDefaultMaxChunkSizeBytes}});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, makeDistStatus(cm), false, false);
    ASSERT_EQ(2U, migrations.size());

    ASSERT_EQ(getShardId(0), migrations[0].from);
    ASSERT_EQ(getShardId(4), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(0)][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);

    ASSERT_EQ(getShardId(1), migrations[1].from);
    ASSERT_EQ(getShardId(5), migrations[1].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(1)][0].getMin(), migrations[1].minKey);
}

TEST(BalancerPolicy, ParallelBalancingDoesNotMoveChunksFromShardsBelowOptimal) {
    auto [cluster, cm] = generateCluster({{100, 100 * kDefaultMaxChunkSizeBytes},
                                          {30, 30 * kDefaultMaxChunkSizeBytes},
                                          {5, 5 * kDefaultMaxChunkSizeBytes},
                                          {0, 0 * kDefaultMaxChunkSizeBytes}});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, makeDistStatus(cm), false, false);
    ASSERT_EQ(1U, migrations.size());

    ASSERT_EQ(getShardId(0), migrations[0].from);
    ASSERT_EQ(getShardId(3), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(0)][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);
}

TEST(BalancerPolicy, ParallelBalancingNotSchedulingOnInUseSourceShardsWithMoveNecessary) {
    auto [cluster, cm] = generateCluster({{8, 8 * kDefaultMaxChunkSizeBytes},
                                          {4, 4 * kDefaultMaxChunkSizeBytes},
                                          {0, 0 * kDefaultMaxChunkSizeBytes},
                                          {0, 0 * kDefaultMaxChunkSizeBytes}});

    const auto& shardStats = cluster.first;

    // Here getShardId(0) would have been selected as a donor
    auto availableShards = getAllShardIds(shardStats);
    availableShards.erase(getShardId(0));
    const auto [migrations, reason] =
        BalancerPolicy::balance(shardStats,
                                makeDistStatus(cm),
                                buildDataSizeInfoForBalancingFromShardStats(shardStats),
                                &availableShards,
                                false);

    ASSERT_EQ(1U, migrations.size());

    ASSERT_EQ(getShardId(1), migrations[0].from);
    ASSERT_EQ(getShardId(2), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(1)][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);
}

TEST(BalancerPolicy, ParallelBalancingNotSchedulingOnInUseSourceShardsWithMoveNotNecessary) {
    auto [cluster, cm] = generateCluster({{12, 12 * kDefaultMaxChunkSizeBytes},
                                          {4, 4 * kDefaultMaxChunkSizeBytes},
                                          {0, 0 * kDefaultMaxChunkSizeBytes},
                                          {0, 0 * kDefaultMaxChunkSizeBytes}});

    // Here kShardId0 would have been selected as a donor
    auto availableShards = getAllShardIds(cluster.first);
    availableShards.erase(getShardId(0));
    const auto [migrations, reason] =
        BalancerPolicy::balance(cluster.first,
                                makeDistStatus(cm),
                                buildDataSizeInfoForBalancingFromShardStats(cluster.first),
                                &availableShards,
                                false);
    ASSERT_EQ(0U, migrations.size());
}

TEST(BalancerPolicy, ParallelBalancingNotSchedulingOnInUseDestinationShards) {
    auto [cluster, cm] = generateCluster({{4, 4 * kDefaultMaxChunkSizeBytes},
                                          {4, 4 * kDefaultMaxChunkSizeBytes},
                                          {0, 0 * kDefaultMaxChunkSizeBytes},
                                          {1, 1 * kDefaultMaxChunkSizeBytes}});

    // Here kShardId2 would have been selected as a recipient
    auto availableShards = getAllShardIds(cluster.first);
    availableShards.erase(getShardId(2));
    const auto [migrations, reason] =
        BalancerPolicy::balance(cluster.first,
                                makeDistStatus(cm),
                                buildDataSizeInfoForBalancingFromShardStats(cluster.first),
                                &availableShards,
                                false);
    ASSERT_EQ(1U, migrations.size());

    ASSERT_EQ(getShardId(0), migrations[0].from);
    ASSERT_EQ(getShardId(3), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(0)][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);
}

TEST(BalancerPolicy, ParallelBalancingDoesNotMoveDataFromShardsBelowIdealZoneSize) {
    auto [cluster, cm] = generateCluster({{1, 100 * kDefaultMaxChunkSizeBytes},
                                          {1, 30 * kDefaultMaxChunkSizeBytes},
                                          {1, 5 * kDefaultMaxChunkSizeBytes},
                                          {1, 0 * kDefaultMaxChunkSizeBytes}});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, makeDistStatus(cm), false, false);
    ASSERT_EQ(1U, migrations.size());

    ASSERT_EQ(getShardId(0), migrations[0].from);
    ASSERT_EQ(getShardId(3), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(0)][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);
}

TEST(BalancerPolicy, JumboChunksNotMoved) {
    auto [cluster, cm] =
        generateCluster({{4, 4 * kDefaultMaxChunkSizeBytes}, {0, 0 * kDefaultMaxChunkSizeBytes}});

    // construct a new chunk map where all the chunks are jumbo except this one
    const auto& jumboChunk = cluster.second[getShardId(0)][1];

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
    ASSERT_EQ(getShardId(0), migrations[0].from);
    ASSERT_EQ(getShardId(1), migrations[0].to);
    ASSERT_BSONOBJ_EQ(jumboChunk.getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);
}

TEST(BalancerPolicy, JumboChunksNotMovedParallel) {
    auto [cluster, cm] = generateCluster({{1, 4 * kDefaultMaxChunkSizeBytes},
                                          {1, 0 * kDefaultMaxChunkSizeBytes},
                                          {1, 4 * kDefaultMaxChunkSizeBytes},
                                          {1, 0 * kDefaultMaxChunkSizeBytes}});

    // construct a new chunk map where all the following chunks are jumbo
    const auto& jumboChunk0 = cluster.second[getShardId(0)][0];
    const auto& jumboChunk1 = cluster.second[getShardId(2)][0];

    std::vector<ChunkType> chunks;
    cm.forEachChunk([&](const auto& chunk) {
        ChunkType ct{collUUID(), chunk.getRange(), chunk.getLastmod(), chunk.getShardId()};
        if (chunk.getLastmod() == jumboChunk0.getVersion() ||
            chunk.getLastmod() == jumboChunk1.getVersion())
            ct.setJumbo(true);
        else
            ct.setJumbo(false);
        chunks.emplace_back(std::move(ct));
        return true;
    });

    auto newCm = makeChunkManager(chunks);

    const auto [migrations, reason] =
        balanceChunks(cluster.first, makeDistStatus(newCm), false, false);
    ASSERT(migrations.empty());
}

TEST(BalancerPolicy, JumboChunksNotMovedWhileEnforcingZones) {
    auto [cluster, cm] = generateCluster({{3, 3 * kDefaultMaxChunkSizeBytes, false, emptyZoneSet},
                                          {3, 3 * kDefaultMaxChunkSizeBytes, false, {"a"}}});

    // construct a new chunk map where all the chunks are jumbo except this one
    const auto& jumboChunk = cluster.second[getShardId(0)][1];

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
        ZoneRange(kShardKeyPattern.globalMin(), kShardKeyPattern.globalMax(), "a")));
    const auto distribution = makeDistStatus(makeChunkManager(chunks), std::move(zoneInfo));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(getShardId(0), migrations[0].from);
    ASSERT_EQ(getShardId(1), migrations[0].to);
    ASSERT_BSONOBJ_EQ(jumboChunk.getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::zoneViolation, reason);
}

TEST(BalancerPolicy, JumboChunksNotMovedWhileEnforcingZonesRandom) {
    auto [cluster, cm] = generateCluster({{3, 3 * kDefaultMaxChunkSizeBytes, false, emptyZoneSet},
                                          {3, 3 * kDefaultMaxChunkSizeBytes, false, {"a"}}});

    // construct a new chunk map where all the chunks are jumbo except this one
    const auto jumboChunkIdx = _random.nextInt64(cluster.second[getShardId(0)].size());
    const auto& jumboChunk = cluster.second[getShardId(0)][jumboChunkIdx];

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
        ZoneRange(kShardKeyPattern.globalMin(), kShardKeyPattern.globalMax(), "a")));
    const auto distribution = makeDistStatus(makeChunkManager(chunks), std::move(zoneInfo));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(getShardId(0), migrations[0].from);
    ASSERT_EQ(getShardId(1), migrations[0].to);
    ASSERT_BSONOBJ_EQ(jumboChunk.getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::zoneViolation, reason);
}

TEST(BalancerPolicy, JumboChunksNotMovedRandom) {
    auto [cluster, cm] =
        generateCluster({{4, 3 * kDefaultMaxChunkSizeBytes}, {0, 0 * kDefaultMaxChunkSizeBytes}});

    // construct a new chunk map where all the chunks are jumbo except this one
    const auto jumboChunkIdx = _random.nextInt64(cluster.second[getShardId(0)].size());
    const auto& jumboChunk = cluster.second[getShardId(0)][jumboChunkIdx];

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
    ASSERT_EQ(getShardId(0), migrations[0].from);
    ASSERT_EQ(getShardId(1), migrations[0].to);
    ASSERT_BSONOBJ_EQ(jumboChunk.getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::chunksImbalance, reason);
}

TEST(BalancerPolicy, DrainingSingleChunkPerShardOnlyOneChunk) {
    // shard0 and shard2 are draining and chunks will go to shard1 and shard3 in parallel
    auto [cluster, cm] = generateCluster({{1, 2 * kDefaultMaxChunkSizeBytes, true},
                                          {1, 0 * kDefaultMaxChunkSizeBytes, false},
                                          {1, 2 * kDefaultMaxChunkSizeBytes, true},
                                          {1, 0 * kDefaultMaxChunkSizeBytes, false}});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, makeDistStatus(cm), false, false);
    ASSERT_EQ(2U, migrations.size());

    ASSERT_EQ(getShardId(0), migrations[0].from);
    ASSERT_EQ(getShardId(1), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(0)][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::drain, reason);

    ASSERT_EQ(getShardId(2), migrations[1].from);
    ASSERT_EQ(getShardId(3), migrations[1].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(2)][0].getMin(), migrations[1].minKey);
}

TEST(BalancerPolicy, DrainingSingleChunkPerShard) {
    // shard0 and shard2 are draining and chunks will go to shard1 and shard3 in parallel
    auto [cluster, cm] = generateCluster({{1, 2 * kDefaultMaxChunkSizeBytes, true},
                                          {5, 0 * kDefaultMaxChunkSizeBytes, false},
                                          {1, 2 * kDefaultMaxChunkSizeBytes, true},
                                          {5, 0 * kDefaultMaxChunkSizeBytes, false}});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, makeDistStatus(cm), false, false);
    ASSERT_EQ(2U, migrations.size());

    ASSERT_EQ(getShardId(0), migrations[0].from);
    ASSERT_EQ(getShardId(1), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(0)][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::drain, reason);

    ASSERT_EQ(getShardId(2), migrations[1].from);
    ASSERT_EQ(getShardId(3), migrations[1].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(2)][0].getMin(), migrations[1].minKey);
}


TEST(BalancerPolicy, DrainingFromShardWithFewData) {
    // shard1 is draining and chunks will go to shard0, even though it has a lot more data
    auto [cluster, cm] =
        generateCluster({{1, 20 * kDefaultMaxChunkSizeBytes, false /* isDraining */},
                         {1, 1 * kDefaultMaxChunkSizeBytes, true /* isDrainig */}});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, makeDistStatus(cm), false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(getShardId(1), migrations[0].from);
    ASSERT_EQ(getShardId(0), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(1)][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::drain, reason);
}

TEST(BalancerPolicy, DrainingSingleChunk) {
    // shard0 is draining and chunks will go to shard1, even though it has a lot more chunks
    auto [cluster, cm] =
        generateCluster({{1, 2 * kDefaultMaxChunkSizeBytes, true /* isDraining */},
                         {5, 0 * kDefaultMaxChunkSizeBytes, false /* isDraining */}});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, makeDistStatus(cm), false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(getShardId(0), migrations[0].from);
    ASSERT_EQ(getShardId(1), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(0)][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::drain, reason);
}

TEST(BalancerPolicy, DrainingMultipleShardsAtLeastOneSelected) {
    // shard1 and shard2 are both draining with very little data in them and chunks will go to
    // shard0, even though it has a lot more data that the other two
    auto [cluster, cm] = generateCluster({{1, 50 * kDefaultMaxChunkSizeBytes, false},
                                          {1, 5 * kDefaultMaxChunkSizeBytes, true},
                                          {1, 5 * kDefaultMaxChunkSizeBytes, true}});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, makeDistStatus(cm), false, false);
    ASSERT_EQ(1U, migrations.size());

    ASSERT_EQ(getShardId(1), migrations[0].from);
    ASSERT_EQ(getShardId(0), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(1)][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::drain, reason);
}

TEST(BalancerPolicy, DrainingMultipleShardsFirstOneSelected) {
    // shard0 and shard1 are both draining with very little data in them and chunks will go to
    // shard2, even though it has a lot more data that the other two
    auto [cluster, cm] = generateCluster({{1, 5 * kDefaultMaxChunkSizeBytes, true},
                                          {2, 5 * kDefaultMaxChunkSizeBytes, true},
                                          {16, 5 * kDefaultMaxChunkSizeBytes, false}});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, makeDistStatus(cm), false, false);

    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(getShardId(0), migrations[0].from);
    ASSERT_EQ(getShardId(2), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(0)][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::drain, reason);
}

TEST(BalancerPolicy, DrainingMultipleShardsWontAcceptMigrations) {
    // shard0 has many data, but can't move them to shard1 or shard2 because they are draining
    auto [cluster, cm] = generateCluster({{4, 2 * kDefaultMaxChunkSizeBytes, false},
                                          {0, 0 * kDefaultMaxChunkSizeBytes, true},
                                          {0, 0 * kDefaultMaxChunkSizeBytes, true}});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, makeDistStatus(cm), false, false);
    ASSERT(migrations.empty());
}

TEST(BalancerPolicy, DrainingWithTwoChunksFirstOneSelected) {
    // shard0 is draining and chunks will go to shard1, even though it has a lot more chunks
    auto [cluster, cm] = generateCluster(
        {{2, 2 * kDefaultMaxChunkSizeBytes, true}, {5, 0 * kDefaultMaxChunkSizeBytes, false}});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, makeDistStatus(cm), false, false);

    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(getShardId(0), migrations[0].from);
    ASSERT_EQ(getShardId(1), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(0)][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::drain, reason);
}

TEST(BalancerPolicy, DrainingSingleAppropriateShardFoundDueToZone) {
    auto [cluster, cm] = generateCluster({{4, 2 * kDefaultMaxChunkSizeBytes, false, {"NYC"}},
                                          {4, 2 * kDefaultMaxChunkSizeBytes, false, {"LAX"}},
                                          {1, 1 * kDefaultMaxChunkSizeBytes, true, {"LAX"}}});

    ZoneInfo zoneInfo;
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(cluster.second[getShardId(2)][0].getMin(),
                                                cluster.second[getShardId(2)][0].getMax(),
                                                "LAX")));
    const auto distribution = makeDistStatus(cm, std::move(zoneInfo));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(getShardId(2), migrations[0].from);
    ASSERT_EQ(getShardId(1), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(2)][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::drain, reason);
}

TEST(BalancerPolicy, DrainingNoAppropriateShardsFoundDueToZone) {
    auto [cluster, cm] = generateCluster({{4, 2 * kDefaultMaxChunkSizeBytes, false, {"NYC"}},
                                          {4, 2 * kDefaultMaxChunkSizeBytes, false, {"LAX"}},
                                          {1, 1 * kDefaultMaxChunkSizeBytes, true, {"SEA"}}});

    ZoneInfo zoneInfo;
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(cluster.second[getShardId(2)][0].getMin(),
                                                cluster.second[getShardId(2)][0].getMax(),
                                                "SEA")));
    const auto distribution = makeDistStatus(cm, std::move(zoneInfo));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT(migrations.empty());
}

TEST(BalancerPolicy, DrainingSingleAppropriateShardFoundMultipleTags) {
    auto [cluster, cm] =
        generateCluster({{4, 2 * kDefaultMaxChunkSizeBytes, true, emptyZoneSet},
                         {2, 2 * kDefaultMaxChunkSizeBytes, false, {"Zone1", "Zone2"}},
                         {2, 2 * kDefaultMaxChunkSizeBytes, false, {"Zone1", "Zone2"}},
                         {2, 2 * kDefaultMaxChunkSizeBytes, false, emptyZoneSet}});

    ZoneInfo zoneInfo;
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(cluster.second[getShardId(0)][0].getMin(),
                                                cluster.second[getShardId(0)][0].getMax(),
                                                "Zone1")));
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(cluster.second[getShardId(0)][1].getMin(),
                                                cluster.second[getShardId(0)][1].getMax(),
                                                "Zone2")));
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(cluster.second[getShardId(0)][2].getMin(),
                                                cluster.second[getShardId(0)][2].getMax(),
                                                "Zone1")));
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(cluster.second[getShardId(0)][3].getMin(),
                                                cluster.second[getShardId(0)][3].getMax(),
                                                "Zone2")));
    const auto distribution = makeDistStatus(cm, std::move(zoneInfo));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(getShardId(0), migrations[0].from);
    const auto& recipientShard = migrations[0].to;
    ASSERT(recipientShard == getShardId(1) || recipientShard == getShardId(2));
    ASSERT_EQ(MigrationReason::drain, reason);
}

TEST(BalancerPolicy, NoBalancingDueToAllNodesDraining) {
    auto [cluster, cm] = generateCluster({{1, 5 * kDefaultMaxChunkSizeBytes, true, emptyZoneSet},
                                          {1, 1 * kDefaultMaxChunkSizeBytes, true, emptyZoneSet}});

    const auto [migrations, reason] =
        balanceChunks(cluster.first, makeDistStatus(cm), false, false);
    ASSERT(migrations.empty());
}

TEST(BalancerPolicy, BalancerRespectsZonesWhenDraining) {
    // shard1 drains the proper chunk to shard0, even though it is more loaded than shard2
    auto [cluster, cm] = generateCluster({{6, 6 * kDefaultMaxChunkSizeBytes, false, {"a"}},
                                          {1, 1 * kDefaultMaxChunkSizeBytes, true, {"a", "b"}},
                                          {2, 2 * kDefaultMaxChunkSizeBytes, false, {"b"}}});

    ZoneInfo zoneInfo;
    ASSERT_OK(
        zoneInfo.addRangeToZone(ZoneRange(kShardKeyPattern.globalMin(), BSON("x" << 7), "a")));
    ASSERT_OK(
        zoneInfo.addRangeToZone(ZoneRange(BSON("x" << 8), kShardKeyPattern.globalMax(), "b")));

    const auto distribution = makeDistStatus(cm, std::move(zoneInfo));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(getShardId(1), migrations[0].from);
    ASSERT_EQ(getShardId(0), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(1)][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::drain, reason);
}

TEST(BalancerPolicy, BalancerRespectsZonePolicyBeforeImbalance) {
    // There is a large imbalance between shard0 and shard1, but the balancer must first fix the
    // chunks, which are on a wrong shard due to zone policy
    auto [cluster, cm] = generateCluster({{2, 5 * kDefaultMaxChunkSizeBytes, false, {"a"}},
                                          {6, 5 * kDefaultMaxChunkSizeBytes, false, {"a"}},
                                          {5, 2 * kDefaultMaxChunkSizeBytes, false, emptyZoneSet}});

    ZoneInfo zoneInfo;
    ASSERT_OK(
        zoneInfo.addRangeToZone(ZoneRange(kShardKeyPattern.globalMin(), BSON("x" << 100), "a")));
    const auto distribution = makeDistStatus(cm, std::move(zoneInfo));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(getShardId(2), migrations[0].from);
    ASSERT_EQ(getShardId(0), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(2)][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::zoneViolation, reason);
}

TEST(BalancerPolicy, OverloadedShardCannotDonateDueToTags) {
    // There is a large imbalance between shard0 and the other shard, but shard0 can't donate chunks
    // because it would violate tags.
    auto [cluster, cm] = generateCluster({{5, 5 * kDefaultMaxChunkSizeBytes, false, {"a"}},
                                          {0, 0 * kDefaultMaxChunkSizeBytes, false, emptyZoneSet},
                                          {0, 0 * kDefaultMaxChunkSizeBytes, false, emptyZoneSet}});

    ZoneInfo zoneInfo;
    ASSERT_OK(zoneInfo.addRangeToZone(
        ZoneRange(kShardKeyPattern.globalMin(), kShardKeyPattern.globalMax(), "a")));
    const auto distribution = makeDistStatus(cm, std::move(zoneInfo));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(0U, migrations.size());
}


TEST(BalancerPolicy, BalancerFixesIncorrectZonesWithCrossShardViolationOfZones) {
    // The zone policy dictates that the same shard must donate and also receive chunks. The
    // test validates that the same shard is not used as a donor and recipient as part of the
    // same round.
    auto [cluster, cm] = generateCluster({{3, 5 * kDefaultMaxChunkSizeBytes, false, {"a"}},
                                          {3, 5 * kDefaultMaxChunkSizeBytes, false, {"a"}},
                                          {3, 5 * kDefaultMaxChunkSizeBytes, false, {"b"}}});

    ZoneInfo zoneInfo;
    ASSERT_OK(
        zoneInfo.addRangeToZone(ZoneRange(kShardKeyPattern.globalMin(), BSON("x" << 1), "b")));
    ASSERT_OK(
        zoneInfo.addRangeToZone(ZoneRange(BSON("x" << 8), kShardKeyPattern.globalMax(), "a")));
    const auto distribution = makeDistStatus(cm, std::move(zoneInfo));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(getShardId(0), migrations[0].from);
    ASSERT_EQ(getShardId(2), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(0)][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::zoneViolation, reason);
}

TEST(BalancerPolicy, BalancerFixesIncorrectZonesInOtherwiseBalancedCluster) {
    // Chunks are balanced across shards, but there are wrong zones, which need to be fixed
    auto [cluster, cm] = generateCluster({{3, 5 * kDefaultMaxChunkSizeBytes, false, {"a"}},
                                          {3, 5 * kDefaultMaxChunkSizeBytes, false, {"a"}},
                                          {3, 5 * kDefaultMaxChunkSizeBytes, false, emptyZoneSet}});

    ZoneInfo zoneInfo;
    ASSERT_OK(
        zoneInfo.addRangeToZone(ZoneRange(kShardKeyPattern.globalMin(), BSON("x" << 10), "a")));
    const auto distribution = makeDistStatus(cm, std::move(zoneInfo));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(getShardId(2), migrations[0].from);
    ASSERT_EQ(getShardId(0), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(2)][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::zoneViolation, reason);
}

TEST(BalancerPolicy, BalancerFixesIncorrectTagsInOtherwiseBalancedCluster) {
    auto [cluster, cm] =
        generateCluster({{4, 2 * kDefaultMaxChunkSizeBytes, false, emptyZoneSet},
                         {2, 2 * kDefaultMaxChunkSizeBytes, false, {"Zone1", "Zone2"}},
                         {2, 2 * kDefaultMaxChunkSizeBytes, false, {"Zone1", "Zone2"}},
                         {2, 2 * kDefaultMaxChunkSizeBytes, false, emptyZoneSet}});

    ZoneInfo zoneInfo;
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(cluster.second[getShardId(0)][0].getMin(),
                                                cluster.second[getShardId(0)][0].getMax(),
                                                "Zone1")));
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(cluster.second[getShardId(0)][1].getMin(),
                                                cluster.second[getShardId(0)][1].getMax(),
                                                "Zone2")));
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(cluster.second[getShardId(0)][2].getMin(),
                                                cluster.second[getShardId(0)][2].getMax(),
                                                "Zone1")));
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(cluster.second[getShardId(0)][3].getMin(),
                                                cluster.second[getShardId(0)][3].getMax(),
                                                "Zone2")));
    const auto distribution = makeDistStatus(cm, std::move(zoneInfo));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(getShardId(0), migrations[0].from);
    const auto& recipientShard = migrations[0].to;
    ASSERT(recipientShard == getShardId(1) || recipientShard == getShardId(2));
    ASSERT_EQ(MigrationReason::zoneViolation, reason);
}

TEST(BalancerPolicy, BalancerZoneAlreadyBalanced) {
    // Chunks are balanced across shards for the zone.
    auto [cluster, cm] = generateCluster({{3, 2 * kDefaultMaxChunkSizeBytes, false, {"a"}},
                                          {2, 2 * kDefaultMaxChunkSizeBytes, false, {"a"}}});

    ZoneInfo zoneInfo;
    ASSERT_OK(zoneInfo.addRangeToZone(
        ZoneRange(kShardKeyPattern.globalMin(), kShardKeyPattern.globalMax(), "a")));

    const auto distribution = makeDistStatus(cm, std::move(zoneInfo));
    ASSERT(balanceChunks(cluster.first, distribution, false, false).first.empty());
}

TEST(BalancerPolicy, ScheduleMigrationForChunkViolatingZone) {
    // Zone violation: shard1 owns a chunk from zone "a"
    auto [cluster, cm] = generateCluster({{1, 2 * kDefaultMaxChunkSizeBytes, false, {"a"}},
                                          {1, 2 * kDefaultMaxChunkSizeBytes, false, {"b"}}});

    ZoneInfo zoneInfo;
    ASSERT_OK(zoneInfo.addRangeToZone(
        ZoneRange(kShardKeyPattern.globalMin(), kShardKeyPattern.globalMax(), "a")));
    const auto distribution = makeDistStatus(cm, std::move(zoneInfo));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(getShardId(1), migrations[0].from);
    ASSERT_EQ(getShardId(0), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(1)][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::zoneViolation, reason);
}

TEST(BalancerPolicy, ScheduleParallelMigrationsForZoneViolations) {
    // shard2 and shard3 own chunks from zone "a" that are violating the shards zone
    auto [cluster, cm] = generateCluster({{1, 2 * kDefaultMaxChunkSizeBytes, false, {"a"}},
                                          {1, 2 * kDefaultMaxChunkSizeBytes, false, {"a"}},
                                          {1, 2 * kDefaultMaxChunkSizeBytes, false, {"b"}},
                                          {1, 2 * kDefaultMaxChunkSizeBytes, false, {"b"}}});

    ZoneInfo zoneInfo;
    ASSERT_OK(zoneInfo.addRangeToZone(
        ZoneRange(kShardKeyPattern.globalMin(), kShardKeyPattern.globalMax(), "a")));

    const auto distribution = makeDistStatus(cm, std::move(zoneInfo));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(2U, migrations.size());
    ASSERT_EQ(getShardId(2), migrations[0].from);
    ASSERT_EQ(getShardId(0), migrations[0].to);

    ASSERT_EQ(getShardId(3), migrations[1].from);
    ASSERT_EQ(getShardId(1), migrations[1].to);

    ASSERT_EQ(MigrationReason::zoneViolation, reason);
}

TEST(BalancerPolicy, DrainingHasPrecedenceOverZoneViolation) {
    // shard1 owns a chunk from zone "a" that is violating the shards zone, however shard2 is in
    // draining mode so it has preference over shard1
    auto [cluster, cm] =
        generateCluster({{1, 2 * kDefaultMaxChunkSizeBytes, false, {"a"}},
                         {1, 2 * kDefaultMaxChunkSizeBytes, false, {"b"}},
                         {1, 2 * kDefaultMaxChunkSizeBytes, true /*draining*/, {"a"}}});

    ZoneInfo zoneInfo;
    ASSERT_OK(zoneInfo.addRangeToZone(
        ZoneRange(kShardKeyPattern.globalMin(), kShardKeyPattern.globalMax(), "a")));
    const auto distribution = makeDistStatus(cm, std::move(zoneInfo));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(getShardId(2), migrations[0].from);
    ASSERT_EQ(getShardId(0), migrations[0].to);
    ASSERT_EQ(MigrationReason::drain, reason);
}

TEST(BalancerPolicy, BalancerMostOverLoadShardHasMultipleZones) {
    // shard0 has chunks [MinKey, 1), [1, 2), [2, 3), [3, 4), [4, 5), so two chunks each
    // for zones "b" and "c". So [1, 2) is expected to be moved to shard1 in round 1.
    auto [cluster, cm] =
        generateCluster({{5, 5 * kDefaultMaxChunkSizeBytes, false, {"a", "b", "c"}},
                         {1, 1 * kDefaultMaxChunkSizeBytes, false, {"b"}},
                         {1, 1 * kDefaultMaxChunkSizeBytes, false, {"c"}}});

    ZoneInfo zoneInfo;
    ASSERT_OK(
        zoneInfo.addRangeToZone(ZoneRange(kShardKeyPattern.globalMin(), BSON("x" << 1), "a")));
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(BSON("x" << 1), BSON("x" << 3), "b")));
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(BSON("x" << 3), BSON("x" << 5), "c")));

    const auto distribution = makeDistStatus(cm, std::move(zoneInfo));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(1U, migrations.size());

    ASSERT_EQ(getShardId(0), migrations[0].from);
    ASSERT_EQ(getShardId(1), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(0)][1].getMin(), migrations[0].minKey);
}

TEST(BalancerPolicy, BalancerMostOverLoadShardHasMultipleZonesSkipZoneWithShardInUse) {
    // shard0 has chunks [MinKey, 1), [1, 2), [2, 3), [3, 4), [4, 5), so two chunks each
    // for zones "b" and "c". So [3, 4) is expected to be moved to shard2 because shard1 is
    // in use.
    auto [cluster, cm] =
        generateCluster({{5, 5 * kDefaultMaxChunkSizeBytes, false, {"a", "b", "c"}},
                         {1, 1 * kDefaultMaxChunkSizeBytes, false, {"b"}},
                         {1, 1 * kDefaultMaxChunkSizeBytes, false, {"c"}}});

    ZoneInfo zoneInfo;
    ASSERT_OK(
        zoneInfo.addRangeToZone(ZoneRange(kShardKeyPattern.globalMin(), BSON("x" << 1), "a")));
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(BSON("x" << 1), BSON("x" << 3), "b")));
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(BSON("x" << 3), BSON("x" << 5), "c")));
    const auto distribution = makeDistStatus(cm, std::move(zoneInfo));

    auto availableShards = getAllShardIds(cluster.first);
    availableShards.erase(getShardId(1));
    const auto [migrations, reason] =
        BalancerPolicy::balance(cluster.first,
                                distribution,
                                buildDataSizeInfoForBalancingFromShardStats(cluster.first),
                                &availableShards,
                                false);
    ASSERT_EQ(1U, migrations.size());

    ASSERT_EQ(getShardId(0), migrations[0].from);
    ASSERT_EQ(getShardId(2), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(0)][3].getMin(), migrations[0].minKey);
}

TEST(BalancerPolicy, BalancerFixesIncorrectZonesInOtherwiseBalancedClusterParallel) {
    // Chunks are balanced across shards, but there are wrong zones, which need to be fixed
    auto [cluster, cm] = generateCluster({{5, 3 * kDefaultMaxChunkSizeBytes, false, {"a"}},
                                          {5, 3 * kDefaultMaxChunkSizeBytes, false, {"a"}},
                                          {5, 3 * kDefaultMaxChunkSizeBytes, false, emptyZoneSet},
                                          {5, 3 * kDefaultMaxChunkSizeBytes, false, emptyZoneSet}});

    ZoneInfo zoneInfo;
    ASSERT_OK(
        zoneInfo.addRangeToZone(ZoneRange(kShardKeyPattern.globalMin(), BSON("x" << 20), "a")));
    const auto distribution = makeDistStatus(cm, std::move(zoneInfo));

    const auto [migrations, reason] = balanceChunks(cluster.first, distribution, false, false);
    ASSERT_EQ(2U, migrations.size());

    ASSERT_EQ(getShardId(2), migrations[0].from);
    ASSERT_EQ(getShardId(0), migrations[0].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(2)][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(MigrationReason::zoneViolation, reason);

    ASSERT_EQ(getShardId(3), migrations[1].from);
    ASSERT_EQ(getShardId(1), migrations[1].to);
    ASSERT_BSONOBJ_EQ(cluster.second[getShardId(3)][0].getMin(), migrations[1].minKey);
    ASSERT_EQ(MigrationReason::zoneViolation, reason);
}

TEST(BalancerPolicy, BalancerHandlesNoShardsWithZone) {
    auto [cluster, cm] =
        generateCluster({{2, 5 * kDefaultMaxChunkSizeBytes}, {2, 5 * kDefaultMaxChunkSizeBytes}});

    ZoneInfo zoneInfo;
    ASSERT_OK(zoneInfo.addRangeToZone(
        ZoneRange(kShardKeyPattern.globalMin(), BSON("x" << 7), "NonExistentZone")));

    const auto distribution = makeDistStatus(cm, std::move(zoneInfo));

    ASSERT(balanceChunks(cluster.first, distribution, false, false).first.empty());
}

TEST(DistributionStatus, OneChunkNoZone) {
    const auto chunks =
        makeChunks({{getShardId(0), {kShardKeyPattern.globalMin(), kShardKeyPattern.globalMax()}}});
    const auto cm = makeChunkManager(chunks);
    const auto distStatus = makeDistStatus(cm);

    ASSERT_EQ(kNamespace, distStatus.nss());

    ASSERT_EQ(1, distStatus.numberOfChunksInShard(getShardId(0)));
    ASSERT_EQ(0, distStatus.numberOfChunksInShard(getShardId(1)));

    ASSERT_EQ(1, distStatus.getZoneInfoForShard(getShardId(0)).size());
    ASSERT_EQ(0, distStatus.getZoneInfoForShard(getShardId(1)).size());

    const auto& noZoneInfo =
        distStatus.getZoneInfoForShard(getShardId(0)).at(ZoneInfo::kNoZoneName);
    ASSERT_EQ(1, noZoneInfo.numChunks);
    ASSERT_BSONOBJ_EQ(kShardKeyPattern.globalMin(), noZoneInfo.firstChunkMinKey);

    checkChunksOnShardForTag(distStatus, getShardId(0), "NotExistingZone", {});
    checkChunksOnShardForTag(distStatus, getShardId(1), "NotExistingZone", {});
    checkChunksOnShardForTag(distStatus, getShardId(1), ZoneInfo::kNoZoneName, {});
    checkChunksOnShardForTag(distStatus, getShardId(0), ZoneInfo::kNoZoneName, chunks);
}

TEST(DistributionStatus, OneChunkOneZone) {
    const auto chunks =
        makeChunks({{getShardId(0), {kShardKeyPattern.globalMin(), kShardKeyPattern.globalMax()}}});
    const auto cm = makeChunkManager(chunks);

    ZoneInfo zoneInfo;
    ASSERT_OK(zoneInfo.addRangeToZone(
        ZoneRange(kShardKeyPattern.globalMin(), kShardKeyPattern.globalMax(), "ZoneA")));
    const auto distStatus = makeDistStatus(cm, std::move(zoneInfo));

    ASSERT_EQ(kNamespace, distStatus.nss());

    ASSERT_EQ(1, distStatus.numberOfChunksInShard(getShardId(0)));
    ASSERT_EQ(0, distStatus.numberOfChunksInShard(getShardId(1)));

    ASSERT_EQ(1, distStatus.getZoneInfoForShard(getShardId(0)).size());
    ASSERT_EQ(0, distStatus.getZoneInfoForShard(getShardId(1)).size());

    const auto& shardZoneInfo = distStatus.getZoneInfoForShard(getShardId(0)).at("ZoneA");
    ASSERT_EQ(1, shardZoneInfo.numChunks);
    ASSERT_BSONOBJ_EQ(kShardKeyPattern.globalMin(), shardZoneInfo.firstChunkMinKey);

    checkChunksOnShardForTag(distStatus, getShardId(1), ZoneInfo::kNoZoneName, {});
    checkChunksOnShardForTag(distStatus, getShardId(1), "NotExistingZone", {});
    checkChunksOnShardForTag(distStatus, getShardId(1), "ZoneA", {});
    checkChunksOnShardForTag(distStatus, getShardId(0), ZoneInfo::kNoZoneName, {});
    checkChunksOnShardForTag(distStatus, getShardId(0), "NotExistingZone", {});
    checkChunksOnShardForTag(distStatus, getShardId(0), "ZoneA", chunks);
}

TEST(DistributionStatus, OneChunkMultipleContiguosZones) {
    const auto chunks =
        makeChunks({{getShardId(0), {kShardKeyPattern.globalMin(), kShardKeyPattern.globalMax()}}});
    const auto cm = makeChunkManager(chunks);

    ZoneInfo zoneInfo;
    ASSERT_OK(
        zoneInfo.addRangeToZone(ZoneRange(kShardKeyPattern.globalMin(), BSON("x" << 0), "ZoneA")));
    ASSERT_OK(
        zoneInfo.addRangeToZone(ZoneRange(BSON("x" << 0), kShardKeyPattern.globalMax(), "ZoneB")));
    const auto distStatus = makeDistStatus(cm, std::move(zoneInfo));

    ASSERT_EQ(kNamespace, distStatus.nss());

    ASSERT_EQ(1, distStatus.numberOfChunksInShard(getShardId(0)));
    ASSERT_EQ(0, distStatus.numberOfChunksInShard(getShardId(1)));

    ASSERT_EQ(1, distStatus.getZoneInfoForShard(getShardId(0)).size());
    ASSERT_EQ(0, distStatus.getZoneInfoForShard(getShardId(1)).size());

    const auto& shardZoneInfo =
        distStatus.getZoneInfoForShard(getShardId(0)).at(ZoneInfo::kNoZoneName);
    ASSERT_EQ(1, shardZoneInfo.numChunks);
    ASSERT_BSONOBJ_EQ(kShardKeyPattern.globalMin(), shardZoneInfo.firstChunkMinKey);

    checkChunksOnShardForTag(distStatus, getShardId(1), ZoneInfo::kNoZoneName, {});
    checkChunksOnShardForTag(distStatus, getShardId(1), "NotExistingZone", {});
    checkChunksOnShardForTag(distStatus, getShardId(1), "ZoneA", {});
    checkChunksOnShardForTag(distStatus, getShardId(0), ZoneInfo::kNoZoneName, chunks);
    checkChunksOnShardForTag(distStatus, getShardId(0), "NotExistingZone", {});
    checkChunksOnShardForTag(distStatus, getShardId(0), "ZoneA", {});
}

TEST(DistributionStatus, OneChunkMultipleSparseZones) {
    const auto chunks =
        makeChunks({{getShardId(0), {kShardKeyPattern.globalMin(), kShardKeyPattern.globalMax()}}});
    const auto cm = makeChunkManager(chunks);

    ZoneInfo zoneInfo;
    ASSERT_OK(
        zoneInfo.addRangeToZone(ZoneRange(kShardKeyPattern.globalMin(), BSON("x" << 0), "ZoneA")));
    ASSERT_OK(
        zoneInfo.addRangeToZone(ZoneRange(BSON("x" << 10), kShardKeyPattern.globalMax(), "ZoneB")));
    const auto distStatus = makeDistStatus(cm, std::move(zoneInfo));

    ASSERT_EQ(kNamespace, distStatus.nss());

    ASSERT_EQ(1, distStatus.numberOfChunksInShard(getShardId(0)));
    ASSERT_EQ(0, distStatus.numberOfChunksInShard(getShardId(1)));

    ASSERT_EQ(1, distStatus.getZoneInfoForShard(getShardId(0)).size());
    ASSERT_EQ(0, distStatus.getZoneInfoForShard(getShardId(1)).size());

    const auto& shardZoneInfo =
        distStatus.getZoneInfoForShard(getShardId(0)).at(ZoneInfo::kNoZoneName);
    ASSERT_EQ(1, shardZoneInfo.numChunks);
    ASSERT_BSONOBJ_EQ(kShardKeyPattern.globalMin(), shardZoneInfo.firstChunkMinKey);

    checkChunksOnShardForTag(distStatus, getShardId(1), ZoneInfo::kNoZoneName, {});
    checkChunksOnShardForTag(distStatus, getShardId(1), "NotExistingZone", {});
    checkChunksOnShardForTag(distStatus, getShardId(1), "ZoneA", {});
    checkChunksOnShardForTag(distStatus, getShardId(1), "ZoneB", {});
    checkChunksOnShardForTag(distStatus, getShardId(0), ZoneInfo::kNoZoneName, chunks);
    checkChunksOnShardForTag(distStatus, getShardId(0), "NotExistingZone", {});
    checkChunksOnShardForTag(distStatus, getShardId(0), "ZoneA", {});
    checkChunksOnShardForTag(distStatus, getShardId(0), "ZoneB", {});
}

TEST(DistributionStatus, MultipleChunksNoZone) {
    const auto chunks =
        makeChunks({{getShardId(0), {kShardKeyPattern.globalMin(), BSON("x" << 0)}},
                    {getShardId(0), {BSON("x" << 0), kShardKeyPattern.globalMax()}}});
    const auto cm = makeChunkManager(chunks);
    const auto distStatus = makeDistStatus(cm);

    ASSERT_EQ(kNamespace, distStatus.nss());

    ASSERT_EQ(2, distStatus.numberOfChunksInShard(getShardId(0)));
    ASSERT_EQ(0, distStatus.numberOfChunksInShard(getShardId(1)));

    ASSERT_EQ(1, distStatus.getZoneInfoForShard(getShardId(0)).size());
    ASSERT_EQ(0, distStatus.getZoneInfoForShard(getShardId(1)).size());

    const auto& noZoneInfo =
        distStatus.getZoneInfoForShard(getShardId(0)).at(ZoneInfo::kNoZoneName);
    ASSERT_EQ(2, noZoneInfo.numChunks);
    ASSERT_BSONOBJ_EQ(kShardKeyPattern.globalMin(), noZoneInfo.firstChunkMinKey);

    checkChunksOnShardForTag(distStatus, getShardId(0), "NotExistingZone", {});
    checkChunksOnShardForTag(distStatus, getShardId(1), "NotExistingZone", {});
    checkChunksOnShardForTag(distStatus, getShardId(1), ZoneInfo::kNoZoneName, {});
    checkChunksOnShardForTag(distStatus, getShardId(0), ZoneInfo::kNoZoneName, chunks);
}

TEST(DistributionStatus, MultipleChunksDistributedNoZone) {
    const auto chunks =
        makeChunks({{getShardId(1), {kShardKeyPattern.globalMin(), BSON("x" << 0)}},
                    {getShardId(0), {BSON("x" << 0), kShardKeyPattern.globalMax()}}});
    const auto cm = makeChunkManager(chunks);
    const auto distStatus = makeDistStatus(cm);

    ASSERT_EQ(kNamespace, distStatus.nss());

    ASSERT_EQ(1, distStatus.numberOfChunksInShard(getShardId(0)));
    ASSERT_EQ(1, distStatus.numberOfChunksInShard(getShardId(1)));

    ASSERT_EQ(1, distStatus.getZoneInfoForShard(getShardId(0)).size());
    ASSERT_EQ(1, distStatus.getZoneInfoForShard(getShardId(1)).size());

    const auto& noZoneInfoS0 =
        distStatus.getZoneInfoForShard(getShardId(0)).at(ZoneInfo::kNoZoneName);
    ASSERT_EQ(1, noZoneInfoS0.numChunks);
    ASSERT_BSONOBJ_EQ(BSON("x" << 0), noZoneInfoS0.firstChunkMinKey);

    const auto& noZoneInfoS1 =
        distStatus.getZoneInfoForShard(getShardId(1)).at(ZoneInfo::kNoZoneName);
    ASSERT_EQ(1, noZoneInfoS1.numChunks);
    ASSERT_BSONOBJ_EQ(kShardKeyPattern.globalMin(), noZoneInfoS1.firstChunkMinKey);

    checkChunksOnShardForTag(distStatus, getShardId(0), "NotExistingZone", {});
    checkChunksOnShardForTag(distStatus, getShardId(1), "NotExistingZone", {});
    checkChunksOnShardForTag(distStatus, getShardId(0), ZoneInfo::kNoZoneName, {chunks[1]});
    checkChunksOnShardForTag(distStatus, getShardId(1), ZoneInfo::kNoZoneName, {chunks[0]});
}

TEST(DistributionStatus, MultipleChunksTwoShardsOneZone) {
    const auto chunks =
        makeChunks({{getShardId(0), {kShardKeyPattern.globalMin(), BSON("x" << 0)}},
                    {getShardId(0), {BSON("x" << 0), kShardKeyPattern.globalMax()}}});
    const auto cm = makeChunkManager(chunks);

    ZoneInfo zoneInfo;
    ASSERT_OK(zoneInfo.addRangeToZone(
        ZoneRange(kShardKeyPattern.globalMin(), kShardKeyPattern.globalMax(), "ZoneA")));
    const auto distStatus = makeDistStatus(cm, std::move(zoneInfo));

    ASSERT_EQ(kNamespace, distStatus.nss());

    ASSERT_EQ(2, distStatus.numberOfChunksInShard(getShardId(0)));
    ASSERT_EQ(0, distStatus.numberOfChunksInShard(getShardId(1)));

    ASSERT_EQ(1, distStatus.getZoneInfoForShard(getShardId(0)).size());
    ASSERT_EQ(0, distStatus.getZoneInfoForShard(getShardId(1)).size());

    const auto& noZoneInfo = distStatus.getZoneInfoForShard(getShardId(0)).at("ZoneA");
    ASSERT_EQ(2, noZoneInfo.numChunks);
    ASSERT_BSONOBJ_EQ(kShardKeyPattern.globalMin(), noZoneInfo.firstChunkMinKey);

    checkChunksOnShardForTag(distStatus, getShardId(0), ZoneInfo::kNoZoneName, {});
    checkChunksOnShardForTag(distStatus, getShardId(0), "NotExistingZone", {});
    checkChunksOnShardForTag(distStatus, getShardId(0), "ZoneA", chunks);
    checkChunksOnShardForTag(distStatus, getShardId(1), ZoneInfo::kNoZoneName, {});
    checkChunksOnShardForTag(distStatus, getShardId(1), "NotExistingZone", {});
    checkChunksOnShardForTag(distStatus, getShardId(1), "ZoneA", {});
}

TEST(DistributionStatus, MultipleChunksTwoContiguosZones) {
    const auto chunks =
        makeChunks({{getShardId(0), {kShardKeyPattern.globalMin(), BSON("x" << 0)}},
                    {getShardId(0), {BSON("x" << 0), kShardKeyPattern.globalMax()}}});
    const auto cm = makeChunkManager(chunks);

    ZoneInfo zoneInfo;
    ASSERT_OK(
        zoneInfo.addRangeToZone(ZoneRange(kShardKeyPattern.globalMin(), BSON("x" << 0), "ZoneA")));
    ASSERT_OK(
        zoneInfo.addRangeToZone(ZoneRange(BSON("x" << 0), kShardKeyPattern.globalMax(), "ZoneB")));
    const auto distStatus = makeDistStatus(cm, std::move(zoneInfo));

    ASSERT_EQ(kNamespace, distStatus.nss());

    ASSERT_EQ(2, distStatus.numberOfChunksInShard(getShardId(0)));
    ASSERT_EQ(0, distStatus.numberOfChunksInShard(getShardId(1)));

    ASSERT_EQ(2, distStatus.getZoneInfoForShard(getShardId(0)).size());
    ASSERT_EQ(0, distStatus.getZoneInfoForShard(getShardId(1)).size());

    auto& shardZoneAInfo = distStatus.getZoneInfoForShard(getShardId(0)).at("ZoneA");
    ASSERT_EQ(1, shardZoneAInfo.numChunks);
    ASSERT_BSONOBJ_EQ(kShardKeyPattern.globalMin(), shardZoneAInfo.firstChunkMinKey);

    auto& shardZoneBInfo = distStatus.getZoneInfoForShard(getShardId(0)).at("ZoneB");
    ASSERT_EQ(1, shardZoneBInfo.numChunks);
    ASSERT_BSONOBJ_EQ(BSON("x" << 0), shardZoneBInfo.firstChunkMinKey);

    checkChunksOnShardForTag(distStatus, getShardId(1), ZoneInfo::kNoZoneName, {});
    checkChunksOnShardForTag(distStatus, getShardId(1), "NotExistingZone", {});
    checkChunksOnShardForTag(distStatus, getShardId(1), "ZoneA", {});
    checkChunksOnShardForTag(distStatus, getShardId(1), "ZoneB", {});
    checkChunksOnShardForTag(distStatus, getShardId(0), ZoneInfo::kNoZoneName, {});
    checkChunksOnShardForTag(distStatus, getShardId(0), "NotExistingZone", {});
    checkChunksOnShardForTag(distStatus, getShardId(0), "ZoneA", {chunks[0]});
    checkChunksOnShardForTag(distStatus, getShardId(0), "ZoneB", {chunks[1]});
}

TEST(DistributionStatus, MultipleChunksTwoZones) {
    const auto chunks =
        makeChunks({{getShardId(0), {kShardKeyPattern.globalMin(), BSON("x" << 0)}},
                    {getShardId(1), {BSON("x" << 0), BSON("x" << 10)}},
                    {getShardId(2), {BSON("x" << 10), BSON("x" << 20)}},
                    {getShardId(2), {BSON("x" << 20), BSON("x" << 30)}},
                    {getShardId(0), {BSON("x" << 30), BSON("x" << 40)}},
                    {getShardId(1), {BSON("x" << 40), BSON("x" << 50)}},
                    {getShardId(2), {BSON("x" << 50), kShardKeyPattern.globalMax()}}});
    const auto cm = makeChunkManager(chunks);

    ZoneInfo zoneInfo;
    ASSERT_OK(
        zoneInfo.addRangeToZone(ZoneRange(kShardKeyPattern.globalMin(), BSON("x" << 20), "ZoneA")));
    ASSERT_OK(
        zoneInfo.addRangeToZone(ZoneRange(BSON("x" << 30), kShardKeyPattern.globalMax(), "ZoneB")));
    const auto distStatus = makeDistStatus(cm, std::move(zoneInfo));

    ASSERT_EQ(kNamespace, distStatus.nss());

    ASSERT_EQ(2, distStatus.numberOfChunksInShard(getShardId(0)));
    ASSERT_EQ(2, distStatus.numberOfChunksInShard(getShardId(1)));
    ASSERT_EQ(3, distStatus.numberOfChunksInShard(getShardId(2)));

    ASSERT_EQ(2, distStatus.getZoneInfoForShard(getShardId(0)).size());
    ASSERT_EQ(2, distStatus.getZoneInfoForShard(getShardId(1)).size());
    ASSERT_EQ(3, distStatus.getZoneInfoForShard(getShardId(2)).size());

    auto& shard0ZoneAInfo = distStatus.getZoneInfoForShard(getShardId(0)).at("ZoneA");
    ASSERT_EQ(1, shard0ZoneAInfo.numChunks);
    ASSERT_BSONOBJ_EQ(kShardKeyPattern.globalMin(), shard0ZoneAInfo.firstChunkMinKey);
    auto& shard0ZoneBInfo = distStatus.getZoneInfoForShard(getShardId(0)).at("ZoneB");
    ASSERT_EQ(1, shard0ZoneBInfo.numChunks);
    ASSERT_BSONOBJ_EQ(BSON("x" << 30), shard0ZoneBInfo.firstChunkMinKey);

    auto& shard1ZoneAInfo = distStatus.getZoneInfoForShard(getShardId(1)).at("ZoneA");
    ASSERT_EQ(1, shard1ZoneAInfo.numChunks);
    ASSERT_BSONOBJ_EQ(BSON("x" << 0), shard1ZoneAInfo.firstChunkMinKey);
    auto& shard1ZoneBInfo = distStatus.getZoneInfoForShard(getShardId(1)).at("ZoneB");
    ASSERT_EQ(1, shard1ZoneBInfo.numChunks);
    ASSERT_BSONOBJ_EQ(BSON("x" << 40), shard1ZoneBInfo.firstChunkMinKey);

    auto& shard2ZoneAInfo = distStatus.getZoneInfoForShard(getShardId(2)).at("ZoneA");
    ASSERT_EQ(1, shard2ZoneAInfo.numChunks);
    ASSERT_BSONOBJ_EQ(BSON("x" << 10), shard2ZoneAInfo.firstChunkMinKey);
    auto& shard2NoZoneInfo =
        distStatus.getZoneInfoForShard(getShardId(2)).at(ZoneInfo::kNoZoneName);
    ASSERT_EQ(1, shard2NoZoneInfo.numChunks);
    ASSERT_BSONOBJ_EQ(BSON("x" << 20), shard2NoZoneInfo.firstChunkMinKey);
    auto& shard2ZoneBInfo = distStatus.getZoneInfoForShard(getShardId(2)).at("ZoneB");
    ASSERT_EQ(1, shard2ZoneBInfo.numChunks);
    ASSERT_BSONOBJ_EQ(BSON("x" << 50), shard2ZoneBInfo.firstChunkMinKey);

    checkChunksOnShardForTag(distStatus, getShardId(0), ZoneInfo::kNoZoneName, {});
    checkChunksOnShardForTag(distStatus, getShardId(0), "NotExistingZone", {});
    checkChunksOnShardForTag(distStatus, getShardId(0), "ZoneA", {chunks[0]});
    checkChunksOnShardForTag(distStatus, getShardId(0), "ZoneB", {chunks[4]});

    checkChunksOnShardForTag(distStatus, getShardId(1), ZoneInfo::kNoZoneName, {});
    checkChunksOnShardForTag(distStatus, getShardId(1), "NotExistingZone", {});
    checkChunksOnShardForTag(distStatus, getShardId(1), "ZoneA", {chunks[1]});
    checkChunksOnShardForTag(distStatus, getShardId(1), "ZoneB", {chunks[5]});

    checkChunksOnShardForTag(distStatus, getShardId(2), ZoneInfo::kNoZoneName, {chunks[3]});
    checkChunksOnShardForTag(distStatus, getShardId(2), "NotExistingZone", {});
    checkChunksOnShardForTag(distStatus, getShardId(2), "ZoneA", {chunks[2]});
    checkChunksOnShardForTag(distStatus, getShardId(2), "ZoneB", {chunks[6]});
}

TEST(DistributionStatus, MultipleChunksMulitpleZoneRanges) {
    const auto chunks =
        makeChunks({{getShardId(0), {kShardKeyPattern.globalMin(), BSON("x" << 0)}},

                    {getShardId(1), {BSON("x" << 0), BSON("x" << 10)}},   // ZoneA
                    {getShardId(0), {BSON("x" << 10), BSON("x" << 20)}},  // ZoneA

                    {getShardId(1), {BSON("x" << 20), BSON("x" << 30)}},
                    {getShardId(0), {BSON("x" << 30), BSON("x" << 40)}},

                    {getShardId(1), {BSON("x" << 40), BSON("x" << 50)}},  // ZoneA
                    {getShardId(0), {BSON("x" << 50), BSON("x" << 60)}},  // ZoneA

                    {getShardId(1), {BSON("x" << 60), kShardKeyPattern.globalMax()}}});
    const auto cm = makeChunkManager(chunks);

    ZoneInfo zoneInfo;
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(BSON("x" << 0), BSON("x" << 20), "ZoneA")));
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(BSON("x" << 40), BSON("x" << 60), "ZoneA")));
    const auto distStatus = makeDistStatus(cm, std::move(zoneInfo));

    ASSERT_EQ(kNamespace, distStatus.nss());

    ASSERT_EQ(4, distStatus.numberOfChunksInShard(getShardId(0)));
    ASSERT_EQ(4, distStatus.numberOfChunksInShard(getShardId(1)));

    ASSERT_EQ(2, distStatus.getZoneInfoForShard(getShardId(0)).size());
    ASSERT_EQ(2, distStatus.getZoneInfoForShard(getShardId(1)).size());

    auto& shard0ZoneAInfo = distStatus.getZoneInfoForShard(getShardId(0)).at("ZoneA");
    ASSERT_EQ(2, shard0ZoneAInfo.numChunks);
    ASSERT_BSONOBJ_EQ(BSON("x" << 10), shard0ZoneAInfo.firstChunkMinKey);
    auto& shard0NoZoneInfo =
        distStatus.getZoneInfoForShard(getShardId(0)).at(ZoneInfo::kNoZoneName);
    ASSERT_EQ(2, shard0NoZoneInfo.numChunks);
    ASSERT_BSONOBJ_EQ(kShardKeyPattern.globalMin(), shard0NoZoneInfo.firstChunkMinKey);

    auto& shard1ZoneAInfo = distStatus.getZoneInfoForShard(getShardId(1)).at("ZoneA");
    ASSERT_EQ(2, shard1ZoneAInfo.numChunks);
    ASSERT_BSONOBJ_EQ(BSON("x" << 0), shard1ZoneAInfo.firstChunkMinKey);
    auto& shard1NoZoneInfo =
        distStatus.getZoneInfoForShard(getShardId(1)).at(ZoneInfo::kNoZoneName);
    ASSERT_EQ(2, shard1NoZoneInfo.numChunks);
    ASSERT_BSONOBJ_EQ(BSON("x" << 20), shard1NoZoneInfo.firstChunkMinKey);

    checkChunksOnShardForTag(
        distStatus, getShardId(0), ZoneInfo::kNoZoneName, {chunks[0], chunks[4]});
    checkChunksOnShardForTag(distStatus, getShardId(0), "ZoneA", {chunks[2], chunks[6]});
    checkChunksOnShardForTag(distStatus, getShardId(0), "NotExistingZone", {});

    checkChunksOnShardForTag(
        distStatus, getShardId(1), ZoneInfo::kNoZoneName, {chunks[3], chunks[7]});
    checkChunksOnShardForTag(distStatus, getShardId(1), "ZoneA", {chunks[1], chunks[5]});
    checkChunksOnShardForTag(distStatus, getShardId(1), "NotExistingZone", {});
}

TEST(DistributionStatus, MultipleChunksMulitpleZoneRangesNotAligned) {
    const auto chunks =
        makeChunks({{getShardId(0), {kShardKeyPattern.globalMin(), BSON("x" << 0)}},     // Zone A
                    {getShardId(0), {BSON("x" << 0), BSON("x" << 10)}},                  // Zone A
                    {getShardId(2), {BSON("x" << 10), BSON("x" << 20)}},                 // No Zone
                    {getShardId(2), {BSON("x" << 20), BSON("x" << 30)}},                 // Zone A
                    {getShardId(1), {BSON("x" << 30), BSON("x" << 40)}},                 // No Zone
                    {getShardId(1), {BSON("x" << 40), BSON("x" << 50)}},                 // Zone B
                    {getShardId(0), {BSON("x" << 50), BSON("x" << 60)}},                 // No Zone
                    {getShardId(0), {BSON("x" << 60), BSON("x" << 70)}},                 // Zone B
                    {getShardId(2), {BSON("x" << 70), kShardKeyPattern.globalMax()}}});  // No zone
    const auto cm = makeChunkManager(chunks);

    ZoneInfo zoneInfo;
    ASSERT_OK(
        zoneInfo.addRangeToZone(ZoneRange(kShardKeyPattern.globalMin(), BSON("x" << 15), "ZoneA")));
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(BSON("x" << 15), BSON("x" << 35), "ZoneA")));
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(BSON("x" << 40), BSON("x" << 55), "ZoneB")));
    ASSERT_OK(zoneInfo.addRangeToZone(ZoneRange(BSON("x" << 55), BSON("x" << 75), "ZoneB")));
    const auto distStatus = makeDistStatus(cm, std::move(zoneInfo));

    ASSERT_EQ(kNamespace, distStatus.nss());

    ASSERT_EQ(4, distStatus.numberOfChunksInShard(getShardId(0)));
    ASSERT_EQ(2, distStatus.numberOfChunksInShard(getShardId(1)));
    ASSERT_EQ(3, distStatus.numberOfChunksInShard(getShardId(2)));

    ASSERT_EQ(3, distStatus.getZoneInfoForShard(getShardId(0)).size());
    ASSERT_EQ(2, distStatus.getZoneInfoForShard(getShardId(1)).size());
    ASSERT_EQ(2, distStatus.getZoneInfoForShard(getShardId(2)).size());

    checkChunksOnShardForTag(distStatus, getShardId(0), ZoneInfo::kNoZoneName, {chunks[6]});
    checkChunksOnShardForTag(distStatus, getShardId(0), "ZoneA", {chunks[0], chunks[1]});
    checkChunksOnShardForTag(distStatus, getShardId(0), "ZoneB", {chunks[7]});
    checkChunksOnShardForTag(distStatus, getShardId(0), "NotExistingZone", {});

    checkChunksOnShardForTag(distStatus, getShardId(1), ZoneInfo::kNoZoneName, {chunks[4]});
    checkChunksOnShardForTag(distStatus, getShardId(1), "ZoneA", {});
    checkChunksOnShardForTag(distStatus, getShardId(1), "ZoneB", {chunks[5]});
    checkChunksOnShardForTag(distStatus, getShardId(1), "NotExistingZone", {});

    checkChunksOnShardForTag(
        distStatus, getShardId(2), ZoneInfo::kNoZoneName, {chunks[2], chunks[8]});
    checkChunksOnShardForTag(distStatus, getShardId(2), "ZoneA", {chunks[3]});
    checkChunksOnShardForTag(distStatus, getShardId(2), "ZoneB", {});
    checkChunksOnShardForTag(distStatus, getShardId(2), "NotExistingZone", {});
}

TEST(DistributionStatus, forEachChunkOnShardInZoneExitCondition) {
    const auto chunks =
        makeChunks({{getShardId(0), {kShardKeyPattern.globalMin(), BSON("x" << 0)}},

                    {getShardId(1), {BSON("x" << 0), BSON("x" << 10)}},   // ZoneA
                    {getShardId(0), {BSON("x" << 10), BSON("x" << 20)}},  // ZoneA

                    {getShardId(1), {BSON("x" << 20), BSON("x" << 30)}},
                    {getShardId(0), {BSON("x" << 30), BSON("x" << 40)}},

                    {getShardId(1), {BSON("x" << 40), BSON("x" << 50)}},  // ZoneA
                    {getShardId(0), {BSON("x" << 50), BSON("x" << 60)}},  // ZoneA

                    {getShardId(1), {BSON("x" << 60), kShardKeyPattern.globalMax()}}});
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

    assertLoopStopAt(getShardId(0), ZoneInfo::kNoZoneName, 1);
    assertLoopStopAt(getShardId(0), ZoneInfo::kNoZoneName, 2);

    assertLoopStopAt(getShardId(1), ZoneInfo::kNoZoneName, 1);
    assertLoopStopAt(getShardId(1), ZoneInfo::kNoZoneName, 2);

    assertLoopStopAt(getShardId(0), "ZoneA", 1);
    assertLoopStopAt(getShardId(0), "ZoneA", 2);

    assertLoopStopAt(getShardId(1), "ZoneA", 1);
    assertLoopStopAt(getShardId(1), "ZoneA", 2);
}

TEST(ZoneInfo, AddZoneRangeOverlap) {
    ZoneInfo zInfo;

    // Note that there is gap between 10 and 20 for which there is no zone
    ASSERT_OK(zInfo.addRangeToZone(ZoneRange(BSON("x" << 1), BSON("x" << 10), "a")));
    ASSERT_OK(zInfo.addRangeToZone(ZoneRange(BSON("x" << 20), BSON("x" << 30), "b")));

    ASSERT_EQ(ErrorCodes::RangeOverlapConflict,
              zInfo.addRangeToZone(ZoneRange(kShardKeyPattern.globalMin(), BSON("x" << 2), "d")));
    ASSERT_EQ(ErrorCodes::RangeOverlapConflict,
              zInfo.addRangeToZone(ZoneRange(BSON("x" << -1), BSON("x" << 5), "d")));
    ASSERT_EQ(ErrorCodes::RangeOverlapConflict,
              zInfo.addRangeToZone(ZoneRange(BSON("x" << 5), BSON("x" << 9), "d")));
    ASSERT_EQ(ErrorCodes::RangeOverlapConflict,
              zInfo.addRangeToZone(ZoneRange(BSON("x" << 1), BSON("x" << 10), "d")));
    ASSERT_EQ(ErrorCodes::RangeOverlapConflict,
              zInfo.addRangeToZone(ZoneRange(BSON("x" << 5), BSON("x" << 12), "d")));
    ASSERT_EQ(ErrorCodes::RangeOverlapConflict,
              zInfo.addRangeToZone(ZoneRange(BSON("x" << 5), BSON("x" << 25), "d")));
    ASSERT_EQ(ErrorCodes::RangeOverlapConflict,
              zInfo.addRangeToZone(ZoneRange(BSON("x" << 19), BSON("x" << 21), "d")));
    ASSERT_EQ(ErrorCodes::RangeOverlapConflict,
              zInfo.addRangeToZone(ZoneRange(BSON("x" << -1), BSON("x" << 32), "d")));
    ASSERT_EQ(ErrorCodes::RangeOverlapConflict,
              zInfo.addRangeToZone(ZoneRange(BSON("x" << 25), kShardKeyPattern.globalMax(), "d")));
}

TEST(ZoneInfo, ChunkZonesSelectorWithRegularKeys) {
    ZoneInfo zInfo;

    ASSERT_OK(zInfo.addRangeToZone(ZoneRange(BSON("x" << 1), BSON("x" << 10), "a")));
    ASSERT_OK(zInfo.addRangeToZone(ZoneRange(BSON("x" << 10), BSON("x" << 20), "b")));
    ASSERT_OK(zInfo.addRangeToZone(ZoneRange(BSON("x" << 20), BSON("x" << 30), "c")));

    ASSERT_EQUALS(ZoneInfo::kNoZoneName,
                  zInfo.getZoneForRange({kShardKeyPattern.globalMin(), BSON("x" << 1)}));
    ASSERT_EQUALS(ZoneInfo::kNoZoneName, zInfo.getZoneForRange({BSON("x" << 0), BSON("x" << 1)}));
    ASSERT_EQUALS("a", zInfo.getZoneForRange({BSON("x" << 1), BSON("x" << 5)}));
    ASSERT_EQUALS("b", zInfo.getZoneForRange({BSON("x" << 10), BSON("x" << 20)}));
    ASSERT_EQUALS("b", zInfo.getZoneForRange({BSON("x" << 15), BSON("x" << 20)}));
    ASSERT_EQUALS("c", zInfo.getZoneForRange({BSON("x" << 25), BSON("x" << 30)}));
    ASSERT_EQUALS(ZoneInfo::kNoZoneName, zInfo.getZoneForRange({BSON("x" << 35), BSON("x" << 40)}));
    ASSERT_EQUALS(ZoneInfo::kNoZoneName,
                  zInfo.getZoneForRange({BSON("x" << 30), kShardKeyPattern.globalMax()}));
    ASSERT_EQUALS(ZoneInfo::kNoZoneName,
                  zInfo.getZoneForRange({BSON("x" << 40), kShardKeyPattern.globalMax()}));
}

TEST(ZoneInfo, ChunkZonesSelectorWithMinMaxKeys) {
    ZoneInfo zInfo;
    ASSERT_OK(
        zInfo.addRangeToZone(ZoneRange(kShardKeyPattern.globalMin(), BSON("x" << -100), "a")));
    ASSERT_OK(zInfo.addRangeToZone(ZoneRange(BSON("x" << -10), BSON("x" << 10), "b")));
    ASSERT_OK(zInfo.addRangeToZone(ZoneRange(BSON("x" << 100), kShardKeyPattern.globalMax(), "c")));

    ASSERT_EQUALS("a", zInfo.getZoneForRange({kShardKeyPattern.globalMin(), BSON("x" << -100)}));
    ASSERT_EQUALS(ZoneInfo::kNoZoneName,
                  zInfo.getZoneForRange({BSON("x" << -100), BSON("x" << -11)}));
    ASSERT_EQUALS("b", zInfo.getZoneForRange({BSON("x" << -10), BSON("x" << 0)}));
    ASSERT_EQUALS("b", zInfo.getZoneForRange({BSON("x" << 0), BSON("x" << 10)}));
    ASSERT_EQUALS(ZoneInfo::kNoZoneName, zInfo.getZoneForRange({BSON("x" << 10), BSON("x" << 20)}));
    ASSERT_EQUALS(ZoneInfo::kNoZoneName,
                  zInfo.getZoneForRange({BSON("x" << 10), BSON("x" << 100)}));
    ASSERT_EQUALS("c", zInfo.getZoneForRange({BSON("x" << 200), kShardKeyPattern.globalMax()}));
}

}  // namespace
}  // namespace mongo
