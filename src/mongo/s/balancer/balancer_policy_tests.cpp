/**
 *    Copyright (C) 2012-2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/platform/random.h"
#include "mongo/s/balancer/balancer_policy.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

using std::map;
using std::string;
using std::stringstream;
using std::vector;

using ShardStatistics = ClusterStatistics::ShardStatistics;

const auto emptyTagSet = std::set<std::string>();
const std::string emptyShardVersion = "";
const auto kShardId0 = ShardId("shard0");
const auto kShardId1 = ShardId("shard1");
const auto kShardId2 = ShardId("shard2");
const NamespaceString kNamespace("TestDB", "TestColl");
const uint64_t kNoMaxSize = 0;

/**
 * Constructs a shard statistics vector and a consistent mapping of chunks to shards given the
 * specified input parameters. The generated chunks have an ever increasing min value. I.e, they
 * will be in the form:
 *
 * [MinKey, 1), [1, 2), [2, 3) ... [N - 1, MaxKey)
 */
std::pair<ShardStatisticsVector, ShardToChunksMap> generateCluster(
    const vector<std::pair<ShardStatistics, size_t>>& shardsAndNumChunks) {
    int64_t totalNumChunks = 0;
    for (const auto& entry : shardsAndNumChunks) {
        totalNumChunks += std::get<1>(entry);
    }

    ShardToChunksMap chunkMap;
    ShardStatisticsVector shardStats;

    int64_t currentChunk = 0;

    for (auto it = shardsAndNumChunks.begin(); it != shardsAndNumChunks.end(); it++) {
        ShardStatistics shard = std::move(it->first);
        const size_t numChunks = it->second;

        // Ensure that an entry is created
        chunkMap[shard.shardId];

        for (size_t i = 0; i < numChunks; i++, currentChunk++) {
            ChunkType chunk;
            chunk.setMin(currentChunk == 0 ? kMinBSONKey : BSON("x" << currentChunk));
            chunk.setMax(currentChunk == totalNumChunks - 1 ? kMaxBSONKey
                                                            : BSON("x" << currentChunk + 1));
            chunk.setShard(shard.shardId);

            chunkMap[shard.shardId].push_back(std::move(chunk));
        }

        shardStats.push_back(std::move(shard));
    }

    return std::make_pair(std::move(shardStats), std::move(chunkMap));
}

TEST(BalancerPolicy, Basic) {
    auto cluster = generateCluster(
        {{ShardStatistics(kShardId0, kNoMaxSize, 2, false, emptyTagSet, emptyShardVersion), 4},
         {ShardStatistics(kShardId1, kNoMaxSize, 0, false, emptyTagSet, emptyShardVersion), 0}});

    const auto migrations(BalancerPolicy::balance(
        cluster.first, DistributionStatus(kNamespace, cluster.second), false));
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(kShardId0, migrations[0].from);
    ASSERT_EQ(kShardId1, migrations[0].to);
    ASSERT_EQ(cluster.second[kShardId0][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(cluster.second[kShardId0][0].getMax(), migrations[0].maxKey);
}

TEST(BalancerPolicy, Jumbo) {
    auto cluster = generateCluster(
        {{ShardStatistics(kShardId0, kNoMaxSize, 2, false, emptyTagSet, emptyShardVersion), 4},
         {ShardStatistics(kShardId1, kNoMaxSize, 0, false, emptyTagSet, emptyShardVersion), 0}});

    cluster.second[kShardId0][0].setJumbo(true);
    cluster.second[kShardId0][1].setJumbo(false);  // Only chunk 1 is not jumbo
    cluster.second[kShardId0][2].setJumbo(true);
    cluster.second[kShardId0][3].setJumbo(true);

    const auto migrations(BalancerPolicy::balance(
        cluster.first, DistributionStatus(kNamespace, cluster.second), false));
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(kShardId0, migrations[0].from);
    ASSERT_EQ(kShardId1, migrations[0].to);
    ASSERT_EQ(cluster.second[kShardId0][1].getMin(), migrations[0].minKey);
    ASSERT_EQ(cluster.second[kShardId0][1].getMax(), migrations[0].maxKey);
}

TEST(BalancerPolicy, DrainingWithASingleChunk) {
    // shard0 is draining and chunks will go to shard1, even though it has a lot more chunks
    auto cluster = generateCluster(
        {{ShardStatistics(kShardId0, kNoMaxSize, 2, true, emptyTagSet, emptyShardVersion), 1},
         {ShardStatistics(kShardId1, kNoMaxSize, 0, false, emptyTagSet, emptyShardVersion), 5}});

    const auto migrations(BalancerPolicy::balance(
        cluster.first, DistributionStatus(kNamespace, cluster.second), false));
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(kShardId0, migrations[0].from);
    ASSERT_EQ(kShardId1, migrations[0].to);
    ASSERT_EQ(cluster.second[kShardId0][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(cluster.second[kShardId0][0].getMax(), migrations[0].maxKey);
}

TEST(BalancerPolicy, DrainingWithTwoChunksFirstOneSelected) {
    // shard0 is draining and chunks will go to shard1, even though it has a lot more chunks
    auto cluster = generateCluster(
        {{ShardStatistics(kShardId0, kNoMaxSize, 2, true, emptyTagSet, emptyShardVersion), 2},
         {ShardStatistics(kShardId1, kNoMaxSize, 0, false, emptyTagSet, emptyShardVersion), 5}});

    const auto migrations(BalancerPolicy::balance(
        cluster.first, DistributionStatus(kNamespace, cluster.second), false));
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(kShardId0, migrations[0].from);
    ASSERT_EQ(kShardId1, migrations[0].to);
    ASSERT_EQ(cluster.second[kShardId0][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(cluster.second[kShardId0][0].getMax(), migrations[0].maxKey);
}

TEST(BalancerPolicy, DrainingMultipleShardsFirstOneSelected) {
    // shard0 and shard1 are both draining with very little chunks in them and chunks will go to
    // shard2, even though it has a lot more chunks that the other two
    auto cluster = generateCluster(
        {{ShardStatistics(kShardId0, kNoMaxSize, 5, true, emptyTagSet, emptyShardVersion), 1},
         {ShardStatistics(kShardId1, kNoMaxSize, 5, true, emptyTagSet, emptyShardVersion), 2},
         {ShardStatistics(kShardId2, kNoMaxSize, 5, false, emptyTagSet, emptyShardVersion), 16}});

    const auto migrations(BalancerPolicy::balance(
        cluster.first, DistributionStatus(kNamespace, cluster.second), false));
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(kShardId0, migrations[0].from);
    ASSERT_EQ(kShardId2, migrations[0].to);
    ASSERT_EQ(cluster.second[kShardId0][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(cluster.second[kShardId0][0].getMax(), migrations[0].maxKey);
}

TEST(BalancerPolicy, DrainingMultipleShardsWontAcceptChunks) {
    // shard0 has many chunks, but can't move them to shard1 or shard2 because they are draining
    auto cluster = generateCluster(
        {{ShardStatistics(kShardId0, kNoMaxSize, 2, false, emptyTagSet, emptyShardVersion), 4},
         {ShardStatistics(kShardId1, kNoMaxSize, 0, true, emptyTagSet, emptyShardVersion), 0},
         {ShardStatistics(kShardId2, kNoMaxSize, 0, true, emptyTagSet, emptyShardVersion), 0}});

    const auto migrations(BalancerPolicy::balance(
        cluster.first, DistributionStatus(kNamespace, cluster.second), false));
    ASSERT(migrations.empty());
}

TEST(BalancerPolicy, DrainingSingleAppropriateShardFoundDueToTag) {
    auto cluster = generateCluster(
        {{ShardStatistics(kShardId0, kNoMaxSize, 2, false, {"NYC"}, emptyShardVersion), 4},
         {ShardStatistics(kShardId1, kNoMaxSize, 2, false, {"LAX"}, emptyShardVersion), 4},
         {ShardStatistics(kShardId2, kNoMaxSize, 1, true, {"LAX"}, emptyShardVersion), 1}});

    DistributionStatus distribution(kNamespace, cluster.second);
    distribution.addTagRange(TagRange(
        cluster.second[kShardId2][0].getMin(), cluster.second[kShardId2][0].getMax(), "LAX"));

    const auto migrations(BalancerPolicy::balance(cluster.first, distribution, false));
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(kShardId2, migrations[0].from);
    ASSERT_EQ(kShardId1, migrations[0].to);
    ASSERT_EQ(cluster.second[kShardId2][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(cluster.second[kShardId2][0].getMax(), migrations[0].maxKey);
}

TEST(BalancerPolicy, DrainingNoAppropriateShardsFoundDueToTag) {
    auto cluster = generateCluster(
        {{ShardStatistics(kShardId0, kNoMaxSize, 2, false, {"NYC"}, emptyShardVersion), 4},
         {ShardStatistics(kShardId1, kNoMaxSize, 2, false, {"LAX"}, emptyShardVersion), 4},
         {ShardStatistics(kShardId2, kNoMaxSize, 1, true, {"SEA"}, emptyShardVersion), 1}});

    DistributionStatus distribution(kNamespace, cluster.second);
    distribution.addTagRange(TagRange(
        cluster.second[kShardId2][0].getMin(), cluster.second[kShardId2][0].getMax(), "SEA"));

    const auto migrations(BalancerPolicy::balance(cluster.first, distribution, false));
    ASSERT(migrations.empty());
}

TEST(BalancerPolicy, NoBalancingDueToAllNodesEitherDrainingOrMaxedOut) {
    // shard0 and shard2 are draining, shard1 is maxed out
    auto cluster = generateCluster(
        {{ShardStatistics(kShardId0, kNoMaxSize, 2, true, emptyTagSet, emptyShardVersion), 1},
         {ShardStatistics(kShardId1, 1, 1, false, emptyTagSet, emptyShardVersion), 6},
         {ShardStatistics(kShardId2, kNoMaxSize, 1, true, emptyTagSet, emptyShardVersion), 1}});

    const auto migrations(BalancerPolicy::balance(
        cluster.first, DistributionStatus(kNamespace, cluster.second), false));
    ASSERT(migrations.empty());
}

TEST(BalancerPolicy, BalancerRespectsMaxShardSizeOnlyBalanceToNonMaxed) {
    // Note that maxSize of shard0 is 1, and it is therefore overloaded with currSize = 3. Other
    // shards have maxSize = 0 = unset. Even though the overloaded shard has the least number of
    // less chunks, we shouldn't move chunks to that shard.
    auto cluster = generateCluster(
        {{ShardStatistics(kShardId0, 1, 3, false, emptyTagSet, emptyShardVersion), 2},
         {ShardStatistics(kShardId1, kNoMaxSize, 4, false, emptyTagSet, emptyShardVersion), 4},
         {ShardStatistics(kShardId2, kNoMaxSize, 6, false, emptyTagSet, emptyShardVersion), 6}});

    const auto migrations(BalancerPolicy::balance(
        cluster.first, DistributionStatus(kNamespace, cluster.second), false));
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(kShardId2, migrations[0].from);
    ASSERT_EQ(kShardId1, migrations[0].to);
    ASSERT_EQ(cluster.second[kShardId2][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(cluster.second[kShardId2][0].getMax(), migrations[0].maxKey);
}

TEST(BalancerPolicy, BalancerRespectsMaxShardSizeWhenAllBalanced) {
    // Note that maxSize of shard0 is 1, and it is therefore overloaded with currSize = 4. Other
    // shards have maxSize = 0 = unset. We check that being over the maxSize is NOT equivalent to
    // draining, we don't want to empty shards for no other reason than they are over this limit.
    auto cluster = generateCluster(
        {{ShardStatistics(kShardId0, 1, 4, false, emptyTagSet, emptyShardVersion), 4},
         {ShardStatistics(kShardId1, kNoMaxSize, 4, false, emptyTagSet, emptyShardVersion), 4},
         {ShardStatistics(kShardId2, kNoMaxSize, 4, false, emptyTagSet, emptyShardVersion), 4}});

    const auto migrations(BalancerPolicy::balance(
        cluster.first, DistributionStatus(kNamespace, cluster.second), false));
    ASSERT(migrations.empty());
}

TEST(BalancerPolicy, BalancerFixesIncorrectTagsInOtherwiseBalancedCluster) {
    // Chunks are balanced across shards, but there are wrong tags, which need to be fixed
    auto cluster = generateCluster(
        {{ShardStatistics(kShardId0, kNoMaxSize, 5, false, {"a"}, emptyShardVersion), 3},
         {ShardStatistics(kShardId1, kNoMaxSize, 5, false, {"a"}, emptyShardVersion), 3},
         {ShardStatistics(kShardId2, kNoMaxSize, 5, false, emptyTagSet, emptyShardVersion), 3}});

    DistributionStatus distribution(kNamespace, cluster.second);
    distribution.addTagRange(TagRange(kMinBSONKey, BSON("x" << 10), "a"));

    const auto migrations(BalancerPolicy::balance(cluster.first, distribution, false));
    ASSERT_EQ(1U, migrations.size());
    ASSERT_EQ(kShardId2, migrations[0].from);
    ASSERT_EQ(kShardId0, migrations[0].to);
    ASSERT_EQ(cluster.second[kShardId2][0].getMin(), migrations[0].minKey);
    ASSERT_EQ(cluster.second[kShardId2][0].getMax(), migrations[0].maxKey);
}

TEST(DistributionStatus, TagsSelector) {
    DistributionStatus d(kNamespace, {});

    ASSERT(d.addTagRange(TagRange(BSON("x" << 1), BSON("x" << 10), "a")));
    ASSERT(d.addTagRange(TagRange(BSON("x" << 10), BSON("x" << 20), "b")));
    ASSERT(d.addTagRange(TagRange(BSON("x" << 20), BSON("x" << 30), "c")));

    ASSERT(!d.addTagRange(TagRange(BSON("x" << 20), BSON("x" << 30), "c")));
    ASSERT(!d.addTagRange(TagRange(BSON("x" << 22), BSON("x" << 28), "c")));
    ASSERT(!d.addTagRange(TagRange(BSON("x" << 28), BSON("x" << 33), "c")));

    {
        ChunkType chunk;
        chunk.setMin(BSON("x" << -4));
        ASSERT_EQUALS("", d.getTagForChunk(chunk));
    }

    {
        ChunkType chunk;
        chunk.setMin(BSON("x" << 0));
        ASSERT_EQUALS("", d.getTagForChunk(chunk));
    }

    {
        ChunkType chunk;
        chunk.setMin(BSON("x" << 1));
        ASSERT_EQUALS("a", d.getTagForChunk(chunk));
    }

    {
        ChunkType chunk;
        chunk.setMin(BSON("x" << 10));
        ASSERT_EQUALS("b", d.getTagForChunk(chunk));
    }

    {
        ChunkType chunk;
        chunk.setMin(BSON("x" << 15));
        ASSERT_EQUALS("b", d.getTagForChunk(chunk));
    }

    {
        ChunkType chunk;
        chunk.setMin(BSON("x" << 25));
        ASSERT_EQUALS("c", d.getTagForChunk(chunk));
    }

    {
        ChunkType chunk;
        chunk.setMin(BSON("x" << 35));
        ASSERT_EQUALS("", d.getTagForChunk(chunk));
    }
}

void addShard(ShardToChunksMap& shardToChunks, unsigned numChunks, bool last) {
    unsigned total = 0;
    for (const auto& chunk : shardToChunks) {
        total += chunk.second.size();
    }

    const string myName = str::stream() << "shard" << shardToChunks.size();

    vector<ChunkType> chunksList;

    for (unsigned i = 0; i < numChunks; i++) {
        ChunkType chunk;

        if (i == 0 && total == 0) {
            chunk.setMin(BSON("x" << BSON("$maxKey" << 1)));
        } else {
            chunk.setMin(BSON("x" << total + i));
        }

        if (last && i == (numChunks - 1)) {
            chunk.setMax(BSON("x" << BSON("$maxKey" << 1)));
        } else {
            chunk.setMax(BSON("x" << 1 + total + i));
        }

        chunk.setShard(myName);

        chunksList.push_back(chunk);
    }

    shardToChunks[myName] = chunksList;
}

ShardStatistics& findStat(std::vector<ShardStatistics>& stats, const ShardId& shardId) {
    for (auto& stat : stats) {
        if (stat.shardId == shardId)
            return stat;
    }

    MONGO_UNREACHABLE;
}

void moveChunk(ShardToChunksMap& shardToChunks, const MigrateInfo* m) {
    vector<ChunkType>& chunks = shardToChunks[m->from];

    for (vector<ChunkType>::iterator i = chunks.begin(); i != chunks.end(); ++i) {
        if (i->getMin() == m->minKey) {
            shardToChunks[m->to].push_back(*i);
            chunks.erase(i);
            return;
        }
    }

    MONGO_UNREACHABLE;
}

TEST(BalancerPolicySimulation, TagsDraining) {
    ShardToChunksMap chunks;
    addShard(chunks, 5, false);
    addShard(chunks, 5, false);
    addShard(chunks, 5, true);

    while (true) {
        DistributionStatus distributionStatus(NamespaceString("TestDB", "TestColl"), chunks);

        distributionStatus.addTagRange(TagRange(BSON("x" << -1), BSON("x" << 7), "a"));
        distributionStatus.addTagRange(TagRange(BSON("x" << 7), BSON("x" << 1000), "b"));

        const auto migrations(BalancerPolicy::balance(
            {ShardStatistics(kShardId0, kNoMaxSize, 5, false, {"a"}, emptyShardVersion),
             ShardStatistics(kShardId1, kNoMaxSize, 5, true, {"a", "b"}, emptyShardVersion),
             ShardStatistics(kShardId2, kNoMaxSize, 5, false, {"b"}, emptyShardVersion)},
            distributionStatus,
            false));
        if (migrations.empty()) {
            break;
        }

        if (migrations[0].minKey["x"].numberInt() < 7) {
            ASSERT_EQUALS(kShardId0, migrations[0].to);
        } else {
            ASSERT_EQUALS(kShardId2, migrations[0].to);
        }

        moveChunk(chunks, &migrations[0]);
    }

    ASSERT_EQUALS(7U, chunks[kShardId0].size());
    ASSERT_EQUALS(0U, chunks[kShardId1].size());
    ASSERT_EQUALS(8U, chunks[kShardId2].size());
}

TEST(BalancerPolicySimulation, TagsPolicyChange) {
    ShardToChunksMap chunks;
    addShard(chunks, 5, false);
    addShard(chunks, 5, false);
    addShard(chunks, 5, true);

    while (true) {
        DistributionStatus distributionStatus(NamespaceString("TestDB", "TestColl"), chunks);

        distributionStatus.addTagRange(TagRange(BSON("x" << -1), BSON("x" << 1000), "a"));

        const auto migrations(BalancerPolicy::balance(
            {ShardStatistics(kShardId0, kNoMaxSize, 5, false, {"a"}, emptyShardVersion),
             ShardStatistics(kShardId1, kNoMaxSize, 5, false, {"a"}, emptyShardVersion),
             ShardStatistics(kShardId2, kNoMaxSize, 5, false, emptyTagSet, emptyShardVersion)},
            distributionStatus,
            false));
        if (migrations.empty()) {
            break;
        }

        moveChunk(chunks, &migrations[0]);
    }

    const size_t shard0Size = chunks[kShardId0].size();
    const size_t shard1Size = chunks[kShardId1].size();

    ASSERT_EQ(15U, shard0Size + shard1Size);
    ASSERT(shard0Size == 7U || shard0Size == 8U);
    ASSERT_EQ(0U, chunks[kShardId2].size());
}

/**
 * Idea behind this test is that we set up several shards, the first two of which are draining and
 * the second two of which have a data size limit.  We also simulate a random number of chunks on
 * each shard.
 *
 * Once the shards are setup, we virtually migrate numChunks times, or until there are no more
 * migrations to run.  Each chunk is assumed to have a size of 1 unit, and we increment our currSize
 * for each shard as the chunks move.
 *
 * Finally, we ensure that the drained shards are drained, the data-limited shards aren't
 * overloaded, and that all shards (including the data limited shard if the baseline isn't over the
 * limit are balanced to within 1 unit of some baseline.
 *
 */
TEST(BalancerPolicySimulation, CompleteSimulation) {
    // Hardcode seed here, make test deterministic.
    const int64_t seed = 1337;
    PseudoRandom rng(seed);

    // Run test 10 times
    for (int test = 0; test < 10; test++) {
        // Setup our shards as draining, with maxSize, and normal
        int numShards = 7;
        int numChunks = 0;

        ShardToChunksMap chunks;
        vector<ShardStatistics> shards;

        map<ShardId, int> expected;

        for (int i = 0; i < numShards; i++) {
            int numShardChunks = rng.nextInt32(100);
            bool draining = i < 2;
            bool maxed = i >= 2 && i < 4;

            if (draining) {
                expected[ShardId(str::stream() << "shard" << i)] = 0;
            }

            if (maxed) {
                expected[ShardId(str::stream() << "shard" << i)] = numShardChunks + 1;
            }

            addShard(chunks, numShardChunks, false);
            numChunks += numShardChunks;

            shards.emplace_back(ShardId(str::stream() << "shard" << i),
                                maxed ? numShardChunks + 1 : 0,
                                numShardChunks,
                                draining,
                                emptyTagSet,
                                emptyShardVersion);
        }

        for (const auto& stat : shards) {
            log() << stat.shardId << " : " << stat.toBSON();
        }

        // Perform migrations and increment data size as chunks move
        for (int i = 0; i < numChunks; i++) {
            const auto migrations(BalancerPolicy::balance(
                shards, DistributionStatus(NamespaceString("TestDB", "TestColl"), chunks), i != 0));
            if (migrations.empty()) {
                log() << "Finished with test moves.";
                break;
            }

            moveChunk(chunks, &migrations[0]);

            findStat(shards, migrations[0].from).currSizeMB -= 1;
            findStat(shards, migrations[0].to).currSizeMB += 1;
        }

        // Make sure our balance is correct and our data size is low.

        // The balanced value is the count on the last shard, since it's not draining or
        // limited.
        const int64_t balancedSize = (--shards.end())->currSizeMB;

        for (const auto& stat : shards) {
            log() << stat.shardId << " : " << stat.toBSON();

            // Cast the size once and use it from here in order to avoid typecast errors
            const int shardCurrSizeMB = static_cast<int>(stat.currSizeMB);

            map<ShardId, int>::iterator expectedIt = expected.find(stat.shardId);

            if (expectedIt == expected.end()) {
                const bool isInRange =
                    shardCurrSizeMB >= balancedSize - 1 && shardCurrSizeMB <= balancedSize + 1;
                if (!isInRange) {
                    warning() << "non-limited and non-draining shard had " << shardCurrSizeMB
                              << " chunks, expected near " << balancedSize;
                }

                ASSERT(isInRange);
            } else {
                int expectedSize = expectedIt->second;
                bool isInRange = shardCurrSizeMB <= expectedSize;

                if (isInRange && expectedSize >= balancedSize) {
                    isInRange =
                        shardCurrSizeMB >= balancedSize - 1 && shardCurrSizeMB <= balancedSize + 1;
                }

                if (!isInRange) {
                    warning() << "limited or draining shard had " << shardCurrSizeMB
                              << " chunks, expected less than " << expectedSize
                              << " and (if less than expected) near " << balancedSize;
                }

                ASSERT(isInRange);
            }
        }
    }
}

}  // namespace
}  // namespace mongo
