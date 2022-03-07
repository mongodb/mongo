/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/s/balancer/balancer_defragmentation_policy_impl.h"
#include "mongo/db/s/balancer/balancer_random.h"
#include "mongo/db/s/balancer/cluster_statistics_mock.h"
#include "mongo/db/s/config/config_server_test_fixture.h"

namespace mongo {
namespace {

using ShardStatistics = ClusterStatistics::ShardStatistics;

class BalancerDefragmentationPolicyTest : public ConfigServerTestFixture {
protected:
    const NamespaceString kNss{"testDb.testColl"};
    const UUID kUuid = UUID::gen();
    const ShardId kShardId0 = ShardId("shard0");
    const ShardId kShardId1 = ShardId("shard1");
    const ShardId kShardId2 = ShardId("shard2");
    const ShardId kShardId3 = ShardId("shard3");
    const ChunkVersion kCollectionVersion = ChunkVersion(1, 1, OID::gen(), Timestamp(10));
    const KeyPattern kShardKeyPattern = KeyPattern(BSON("x" << 1));
    const BSONObj kKeyAtMin = BSONObjBuilder().appendMinKey("x").obj();
    const BSONObj kKeyAtZero = BSON("x" << 0);
    const BSONObj kKeyAtTen = BSON("x" << 10);
    const BSONObj kKeyAtTwenty = BSON("x" << 20);
    const BSONObj kKeyAtThirty = BSON("x" << 30);
    const BSONObj kKeyAtForty = BSON("x" << 40);
    const BSONObj kKeyAtMax = BSONObjBuilder().appendMaxKey("x").obj();

    const long long kMaxChunkSizeBytes{2048};
    const HostAndPort kShardHost0 = HostAndPort("TestHost0", 12345);
    const HostAndPort kShardHost1 = HostAndPort("TestHost1", 12346);
    const HostAndPort kShardHost2 = HostAndPort("TestHost2", 12347);
    const HostAndPort kShardHost3 = HostAndPort("TestHost3", 12348);

    const int64_t kPhase3DefaultChunkSize =
        129 * 1024 * 1024;  // > 128MB should trigger AutoSplitVector

    const std::vector<ShardType> kShardList{
        ShardType(kShardId0.toString(), kShardHost0.toString()),
        ShardType(kShardId1.toString(), kShardHost1.toString()),
        ShardType(kShardId2.toString(), kShardHost2.toString()),
        ShardType(kShardId3.toString(), kShardHost3.toString())};

    BalancerDefragmentationPolicyTest()
        : _clusterStats(),
          _random(std::random_device{}()),
          _defragmentationPolicy(&_clusterStats, _random) {}

    CollectionType setupCollectionWithPhase(
        const std::vector<ChunkType>& chunkList,
        boost::optional<DefragmentationPhaseEnum> startingPhase = boost::none,
        boost::optional<int64_t> maxChunkSizeBytes = boost::none) {

        setupShards(kShardList);
        setupCollection(kNss, kShardKeyPattern, chunkList);

        const auto updateClause = [&] {
            BSONObjBuilder builder;
            BSONObjBuilder setObj(builder.subobjStart("$set"));
            setObj.append(CollectionType::kDefragmentCollectionFieldName, true);

            if (startingPhase) {
                setObj.append(CollectionType::kDefragmentationPhaseFieldName,
                              DefragmentationPhase_serializer(*startingPhase));
            }

            if (maxChunkSizeBytes) {
                setObj.append(CollectionType::kMaxChunkSizeBytesFieldName, *maxChunkSizeBytes);
            }
            setObj.done();
            return builder.obj();
        }();

        ASSERT_OK(updateToConfigCollection(operationContext(),
                                           CollectionType::ConfigNS,
                                           BSON(CollectionType::kUuidFieldName << kUuid),
                                           updateClause,
                                           false));
        return Grid::get(operationContext())
            ->catalogClient()
            ->getCollection(operationContext(), kUuid);
    }

    ChunkType makeConfigChunkEntry(const boost::optional<int64_t>& estimatedSize = boost::none) {
        ChunkType chunk(kUuid, ChunkRange(kKeyAtMin, kKeyAtMax), kCollectionVersion, kShardId0);
        chunk.setEstimatedSizeBytes(estimatedSize);
        return chunk;
    }

    std::vector<ChunkType> makeMergeableConfigChunkEntries() {
        return {ChunkType(kUuid, ChunkRange(kKeyAtMin, kKeyAtTen), kCollectionVersion, kShardId0),
                ChunkType(kUuid, ChunkRange(kKeyAtTen, kKeyAtMax), kCollectionVersion, kShardId0)};
    }

    BSONObj getConfigCollectionEntry() {
        DBDirectClient client(operationContext());
        FindCommandRequest findRequest{NamespaceStringOrUUID{CollectionType::ConfigNS}};
        findRequest.setFilter(BSON(CollectionType::kUuidFieldName << kUuid));
        auto cursor = client.find(std::move(findRequest));
        if (!cursor || !cursor->more())
            return BSONObj();
        else
            return cursor->next();
    }

    ClusterStatisticsMock _clusterStats;
    BalancerRandomSource _random;
    BalancerDefragmentationPolicyImpl _defragmentationPolicy;

    ShardStatistics buildShardStats(ShardId id,
                                    uint64_t currentSizeBytes,
                                    bool maxed = false,
                                    bool draining = false,
                                    std::set<std::string>&& zones = {}) {
        return ShardStatistics(
            id, maxed ? currentSizeBytes : 0, currentSizeBytes, draining, zones, "");
    }

    void setDefaultClusterStats() {
        uint64_t oneKB = 1024 * 1024;
        auto shardInstance = 0;
        std::vector<ShardStatistics> stats;
        std::map<NamespaceString, std::vector<ShardStatistics>> collStats;
        for (const auto& shard : kShardList) {
            ++shardInstance;
            stats.push_back(buildShardStats(shard.getName(), oneKB * 1024 * shardInstance));
            collStats[kNss].push_back(buildShardStats(shard.getName(), oneKB * shardInstance));
        }
        _clusterStats.setStats(std::move(stats), std::move(collStats));
    }

    void verifyExpectedDefragmentationPhaseOndisk(
        boost::optional<DefragmentationPhaseEnum> expectedPhase) {
        auto configDoc = findOneOnConfigCollection(operationContext(),
                                                   CollectionType::ConfigNS,
                                                   BSON(CollectionType::kUuidFieldName << kUuid))
                             .getValue();
        if (expectedPhase.has_value()) {
            auto storedDefragmentationPhase = DefragmentationPhase_parse(
                IDLParserErrorContext("BalancerDefragmentationPolicyTest"),
                configDoc.getStringField(CollectionType::kDefragmentationPhaseFieldName));
            ASSERT_TRUE(storedDefragmentationPhase == *expectedPhase);
        } else {
            ASSERT_FALSE(configDoc.hasField(CollectionType::kDefragmentationPhaseFieldName));
        }
    };
};

TEST_F(BalancerDefragmentationPolicyTest, TestGetNextActionIsNotReadyWhenNotDefragmenting) {
    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_FALSE(future.isReady());
}

TEST_F(BalancerDefragmentationPolicyTest, TestAddCollectionWhenCollectionRemovedFailsGracefully) {
    CollectionType coll(kNss, OID::gen(), Timestamp(1, 1), Date_t::now(), kUuid, kShardKeyPattern);
    coll.setDefragmentCollection(true);
    // Collection entry is not persisted (to simulate collection dropped), defragmentation should
    // not begin.
    ASSERT_FALSE(_defragmentationPolicy.isDefragmentingCollection(coll.getUuid()));
    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);
    ASSERT_FALSE(_defragmentationPolicy.isDefragmentingCollection(coll.getUuid()));
    auto configDoc = findOneOnConfigCollection(operationContext(),
                                               CollectionType::ConfigNS,
                                               BSON(CollectionType::kUuidFieldName << kUuid));
    ASSERT_EQ(configDoc.getStatus(), Status(ErrorCodes::NoMatchingDocument, "No document found"));
}

// Phase 1 tests.

TEST_F(BalancerDefragmentationPolicyTest, TestPhaseOneAddSingleChunkCollectionTriggersDataSize) {
    auto coll = setupCollectionWithPhase({makeConfigChunkEntry()});
    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_FALSE(future.isReady());
    ASSERT_FALSE(_defragmentationPolicy.isDefragmentingCollection(coll.getUuid()));

    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);

    // 1. The collection should be marked as undergoing through phase 1 of the algorithm...
    ASSERT_TRUE(_defragmentationPolicy.isDefragmentingCollection(coll.getUuid()));
    verifyExpectedDefragmentationPhaseOndisk(DefragmentationPhaseEnum::kMergeAndMeasureChunks);
    // 2. The action returned by the stream should be now an actionable DataSizeCommand...
    ASSERT_TRUE(future.isReady());
    DataSizeInfo dataSizeAction = stdx::get<DataSizeInfo>(future.get());
    // 3. with the expected content
    // TODO refactor chunk builder
    ASSERT_EQ(coll.getNss(), dataSizeAction.nss);
    ASSERT_BSONOBJ_EQ(kKeyAtMin, dataSizeAction.chunkRange.getMin());
    ASSERT_BSONOBJ_EQ(kKeyAtMax, dataSizeAction.chunkRange.getMax());
}

TEST_F(BalancerDefragmentationPolicyTest,
       AddSingleChunkCollectionWithKnownDataSizeCompletesDefragmentationWithNoOperationIssued) {
    auto coll = setupCollectionWithPhase({makeConfigChunkEntry(1024)});
    setDefaultClusterStats();

    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);

    ASSERT_TRUE(_defragmentationPolicy.isDefragmentingCollection(coll.getUuid()));

    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_FALSE(future.isReady());
    verifyExpectedDefragmentationPhaseOndisk(DefragmentationPhaseEnum::kMoveAndMergeChunks);

    // A single migration request should advance the defragmentation state to the end of the
    // algorithm
    stdx::unordered_set<ShardId> usedShards;

    auto pendingMigrations =
        _defragmentationPolicy.selectChunksToMove(operationContext(), &usedShards);

    ASSERT_TRUE(pendingMigrations.empty());
    ASSERT_FALSE(future.isReady());
    verifyExpectedDefragmentationPhaseOndisk(boost::none);
    ASSERT_FALSE(_defragmentationPolicy.isDefragmentingCollection(coll.getUuid()));
}

TEST_F(BalancerDefragmentationPolicyTest,
       TestPhaseOneAcknowledgeFinalDataSizeActionCompletesPhase) {
    auto coll = setupCollectionWithPhase({makeConfigChunkEntry()});
    setDefaultClusterStats();
    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);
    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    DataSizeInfo dataSizeAction = stdx::get<DataSizeInfo>(future.get());

    auto resp = StatusWith(DataSizeResponse(2000, 4));
    _defragmentationPolicy.acknowledgeDataSizeResult(operationContext(), dataSizeAction, resp);

    // 1. The outcome of the data size has been stored in the expected document...
    auto chunkQuery = BSON(ChunkType::collectionUUID()
                           << kUuid << ChunkType::min(kKeyAtMin) << ChunkType::max(kKeyAtMax));
    auto configChunkDoc =
        findOneOnConfigCollection(operationContext(), ChunkType::ConfigNS, chunkQuery).getValue();
    ASSERT_EQ(configChunkDoc.getIntField(ChunkType::estimatedSizeBytes.name()), 2000);

    // 2. and the algorithm transitioned to the next phase
    future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_FALSE(future.isReady());
    ASSERT_TRUE(_defragmentationPolicy.isDefragmentingCollection(coll.getUuid()));
    verifyExpectedDefragmentationPhaseOndisk(DefragmentationPhaseEnum::kMoveAndMergeChunks);
}

TEST_F(BalancerDefragmentationPolicyTest, TestRetriableFailedDataSizeActionGetsReissued) {
    auto coll = setupCollectionWithPhase({makeConfigChunkEntry()});
    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);
    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    DataSizeInfo failingDataSizeAction = stdx::get<DataSizeInfo>(future.get());

    _defragmentationPolicy.acknowledgeDataSizeResult(
        operationContext(),
        failingDataSizeAction,
        Status(ErrorCodes::NetworkTimeout, "Testing error response"));

    // Under the setup of this test, the stream should only contain one more action - which (version
    // aside) matches the failed one.
    future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    auto replayedDataSizeAction = stdx::get<DataSizeInfo>(future.get());
    ASSERT_BSONOBJ_EQ(failingDataSizeAction.chunkRange.getMin(),
                      replayedDataSizeAction.chunkRange.getMin());
    ASSERT_BSONOBJ_EQ(failingDataSizeAction.chunkRange.getMax(),
                      replayedDataSizeAction.chunkRange.getMax());
    ASSERT_EQ(failingDataSizeAction.uuid, replayedDataSizeAction.uuid);
    ASSERT_EQ(failingDataSizeAction.shardId, replayedDataSizeAction.shardId);
    ASSERT_EQ(failingDataSizeAction.nss, replayedDataSizeAction.nss);
    ASSERT_BSONOBJ_EQ(failingDataSizeAction.keyPattern.toBSON(),
                      replayedDataSizeAction.keyPattern.toBSON());
    ASSERT_EQ(failingDataSizeAction.estimatedValue, replayedDataSizeAction.estimatedValue);

    future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_FALSE(future.isReady());
}

TEST_F(BalancerDefragmentationPolicyTest, TestRemoveCollectionEndsDefragmentation) {
    auto coll = setupCollectionWithPhase({makeConfigChunkEntry()});
    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);
    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    DataSizeInfo dataSizeAction = stdx::get<DataSizeInfo>(future.get());

    auto resp = StatusWith(DataSizeResponse(2000, 4));
    _defragmentationPolicy.acknowledgeDataSizeResult(operationContext(), dataSizeAction, resp);

    // Remove collection entry from config.collections
    ASSERT_OK(deleteToConfigCollection(
        operationContext(), CollectionType::ConfigNS, coll.toBSON(), false));

    // getCollection should fail with NamespaceNotFound and end defragmentation on the collection.
    future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_FALSE(future.isReady());
    // Defragmentation should have stopped on the collection
    ASSERT_FALSE(_defragmentationPolicy.isDefragmentingCollection(coll.getUuid()));
}

TEST_F(BalancerDefragmentationPolicyTest, TestPhaseOneUserCancellationBeginsPhase3) {
    auto coll = setupCollectionWithPhase({makeConfigChunkEntry()});
    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);

    // Collection should be in phase 1
    verifyExpectedDefragmentationPhaseOndisk(DefragmentationPhaseEnum::kMergeAndMeasureChunks);

    // User cancellation of defragmentation
    _defragmentationPolicy.abortCollectionDefragmentation(operationContext(), kNss);

    // Defragmentation should transition to phase 3
    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    verifyExpectedDefragmentationPhaseOndisk(DefragmentationPhaseEnum::kSplitChunks);
    ASSERT_TRUE(future.isReady());
    auto splitVectorAction = stdx::get<AutoSplitVectorInfo>(future.get());
}

TEST_F(BalancerDefragmentationPolicyTest, TestNonRetriableErrorRebuildsCurrentPhase) {
    auto coll = setupCollectionWithPhase({makeConfigChunkEntry()});
    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);
    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    DataSizeInfo failingDataSizeAction = stdx::get<DataSizeInfo>(future.get());

    _defragmentationPolicy.acknowledgeDataSizeResult(
        operationContext(),
        failingDataSizeAction,
        Status(ErrorCodes::IllegalOperation, "Testing error response"));
    future = _defragmentationPolicy.getNextStreamingAction(operationContext());

    // 1. The collection should be marked as undergoing through phase 1 of the algorithm...
    ASSERT_TRUE(_defragmentationPolicy.isDefragmentingCollection(coll.getUuid()));
    verifyExpectedDefragmentationPhaseOndisk(DefragmentationPhaseEnum::kMergeAndMeasureChunks);
    // 2. The action returned by the stream should be now an actionable DataSizeCommand...
    ASSERT_TRUE(future.isReady());
    DataSizeInfo dataSizeAction = stdx::get<DataSizeInfo>(future.get());
    // 3. with the expected content
    // TODO refactor chunk builder
    ASSERT_EQ(coll.getNss(), dataSizeAction.nss);
    ASSERT_BSONOBJ_EQ(kKeyAtMin, dataSizeAction.chunkRange.getMin());
    ASSERT_BSONOBJ_EQ(kKeyAtMax, dataSizeAction.chunkRange.getMax());
}

TEST_F(BalancerDefragmentationPolicyTest,
       TestNonRetriableErrorWaitsForAllOutstandingActionsToComplete) {
    auto coll = setupCollectionWithPhase(
        {ChunkType{kUuid, ChunkRange(kKeyAtMin, kKeyAtTen), kCollectionVersion, kShardId0},
         ChunkType{kUuid, ChunkRange(BSON("x" << 11), kKeyAtMax), kCollectionVersion, kShardId0}});
    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);
    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    DataSizeInfo failingDataSizeAction = stdx::get<DataSizeInfo>(future.get());
    auto future2 = _defragmentationPolicy.getNextStreamingAction(operationContext());
    DataSizeInfo secondDataSizeAction = stdx::get<DataSizeInfo>(future2.get());

    _defragmentationPolicy.acknowledgeDataSizeResult(
        operationContext(),
        failingDataSizeAction,
        Status(ErrorCodes::NamespaceNotFound, "Testing error response"));

    // There should be no new actions.
    future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_FALSE(future.isReady());
    // Defragmentation should be waiting for second datasize action to complete
    ASSERT_TRUE(_defragmentationPolicy.isDefragmentingCollection(coll.getUuid()));
    // Defragmentation policy should ignore content of next acknowledge
    _defragmentationPolicy.acknowledgeDataSizeResult(
        operationContext(),
        secondDataSizeAction,
        Status(ErrorCodes::NetworkTimeout, "Testing error response"));
    // Phase 1 should restart.
    future2 = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_TRUE(future.isReady());
    ASSERT_TRUE(future2.isReady());
    DataSizeInfo dataSizeAction = stdx::get<DataSizeInfo>(future.get());
    DataSizeInfo dataSizeAction2 = stdx::get<DataSizeInfo>(future2.get());
}

TEST_F(BalancerDefragmentationPolicyTest,
       TestPhaseOneAcknowledgeMergeChunkActionsTriggersDataSizeOnResultingRange) {
    auto coll = setupCollectionWithPhase({makeMergeableConfigChunkEntries()});
    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);
    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    auto mergeChunksAction = stdx::get<MergeInfo>(future.get());

    _defragmentationPolicy.acknowledgeMergeResult(
        operationContext(), mergeChunksAction, Status::OK());

    // Under the setup of this test, the stream should only contain only a data size action over the
    // recently merged range.
    future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    auto dataSizeAction = stdx::get<DataSizeInfo>(future.get());
    ASSERT_BSONOBJ_EQ(dataSizeAction.chunkRange.getMin(), mergeChunksAction.chunkRange.getMin());
    ASSERT_BSONOBJ_EQ(dataSizeAction.chunkRange.getMax(), mergeChunksAction.chunkRange.getMax());
    ASSERT_EQ(dataSizeAction.uuid, mergeChunksAction.uuid);
    ASSERT_EQ(dataSizeAction.shardId, mergeChunksAction.shardId);
    ASSERT_EQ(dataSizeAction.nss, mergeChunksAction.nss);

    future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_FALSE(future.isReady());
}

TEST_F(BalancerDefragmentationPolicyTest, TestPhaseOneFailedMergeChunksActionGetsReissued) {
    auto coll = setupCollectionWithPhase(makeMergeableConfigChunkEntries());
    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);
    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    auto failingMergeChunksAction = stdx::get<MergeInfo>(future.get());

    _defragmentationPolicy.acknowledgeMergeResult(
        operationContext(),
        failingMergeChunksAction,
        Status(ErrorCodes::NetworkTimeout, "Testing error response"));
    // Under the setup of this test, the stream should only contain one more action - which (version
    // aside) matches the failed one.
    future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    auto replayedMergeChunksAction = stdx::get<MergeInfo>(future.get());
    ASSERT_EQ(failingMergeChunksAction.uuid, replayedMergeChunksAction.uuid);
    ASSERT_EQ(failingMergeChunksAction.shardId, replayedMergeChunksAction.shardId);
    ASSERT_EQ(failingMergeChunksAction.nss, replayedMergeChunksAction.nss);
    ASSERT_BSONOBJ_EQ(failingMergeChunksAction.chunkRange.getMin(),
                      replayedMergeChunksAction.chunkRange.getMin());
    ASSERT_BSONOBJ_EQ(failingMergeChunksAction.chunkRange.getMax(),
                      replayedMergeChunksAction.chunkRange.getMax());

    future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_FALSE(future.isReady());
}

TEST_F(BalancerDefragmentationPolicyTest, TestPhaseOneAcknowledgeSuccessfulMergeAction) {
    auto coll = setupCollectionWithPhase(makeMergeableConfigChunkEntries());
    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_FALSE(future.isReady());
    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);
    ASSERT_TRUE(future.isReady());
    MergeInfo mergeInfoAction = stdx::get<MergeInfo>(future.get());
    ASSERT_BSONOBJ_EQ(mergeInfoAction.chunkRange.getMin(), kKeyAtMin);
    ASSERT_BSONOBJ_EQ(mergeInfoAction.chunkRange.getMax(), kKeyAtMax);
    future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_FALSE(future.isReady());
    _defragmentationPolicy.acknowledgeMergeResult(
        operationContext(), mergeInfoAction, Status::OK());
    ASSERT_TRUE(future.isReady());
    DataSizeInfo dataSizeAction = stdx::get<DataSizeInfo>(future.get());
    ASSERT_EQ(mergeInfoAction.nss, dataSizeAction.nss);
    ASSERT_BSONOBJ_EQ(mergeInfoAction.chunkRange.getMin(), dataSizeAction.chunkRange.getMin());
    ASSERT_BSONOBJ_EQ(mergeInfoAction.chunkRange.getMax(), dataSizeAction.chunkRange.getMax());
}

TEST_F(BalancerDefragmentationPolicyTest, TestPhaseOneAllConsecutive) {
    // Set up collection with all mergeable chunks
    std::vector<ChunkType> chunkList;
    for (int i = 0; i < 5; i++) {
        const auto minKey = (i == 0) ? kKeyAtMin : BSON("x" << i);
        const auto maxKey = BSON("x" << i + 1);
        ChunkType chunk(
            kUuid,
            ChunkRange(minKey, maxKey),
            ChunkVersion(1, i, kCollectionVersion.epoch(), kCollectionVersion.getTimestamp()),
            kShardId0);
        chunkList.push_back(chunk);
    }
    for (int i = 5; i < 10; i++) {
        const auto minKey = BSON("x" << i);
        const auto maxKey = (i == 9) ? kKeyAtMax : BSON("x" << i + 1);
        ChunkType chunk(
            kUuid,
            ChunkRange(minKey, maxKey),
            ChunkVersion(1, i, kCollectionVersion.epoch(), kCollectionVersion.getTimestamp()),
            kShardId1);
        chunkList.push_back(chunk);
    }
    auto coll = setupCollectionWithPhase(chunkList, boost::none, boost::none);
    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);
    // Test
    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_TRUE(future.isReady());
    auto future2 = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_TRUE(future2.isReady());
    // Verify the content of the received merge actions
    // (Note: there is no guarantee on the order provided by the stream)
    MergeInfo mergeAction = stdx::get<MergeInfo>(future.get());
    MergeInfo mergeAction2 = stdx::get<MergeInfo>(future2.get());
    if (mergeAction.chunkRange.getMin().woCompare(kKeyAtMin) == 0) {
        ASSERT_BSONOBJ_EQ(mergeAction.chunkRange.getMin(), kKeyAtMin);
        ASSERT_BSONOBJ_EQ(mergeAction.chunkRange.getMax(), BSON("x" << 5));
        ASSERT_BSONOBJ_EQ(mergeAction2.chunkRange.getMin(), BSON("x" << 5));
        ASSERT_BSONOBJ_EQ(mergeAction2.chunkRange.getMax(), kKeyAtMax);
    } else {
        ASSERT_BSONOBJ_EQ(mergeAction2.chunkRange.getMin(), kKeyAtMin);
        ASSERT_BSONOBJ_EQ(mergeAction2.chunkRange.getMax(), BSON("x" << 5));
        ASSERT_BSONOBJ_EQ(mergeAction.chunkRange.getMin(), BSON("x" << 5));
        ASSERT_BSONOBJ_EQ(mergeAction.chunkRange.getMax(), kKeyAtMax);
    }
    auto future3 = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_FALSE(future3.isReady());
}

TEST_F(BalancerDefragmentationPolicyTest, PhaseOneNotConsecutive) {
    std::vector<ChunkType> chunkList;
    for (int i = 0; i < 10; i++) {
        const auto minKey = (i == 0) ? kKeyAtMin : BSON("x" << i);
        const auto maxKey = (i == 9) ? kKeyAtMax : BSON("x" << i + 1);
        ShardId chosenShard = (i == 5) ? kShardId1 : kShardId0;
        ChunkType chunk(
            kUuid,
            ChunkRange(minKey, maxKey),
            ChunkVersion(1, i, kCollectionVersion.epoch(), kCollectionVersion.getTimestamp()),
            chosenShard);
        chunkList.push_back(chunk);
    }
    auto coll = setupCollectionWithPhase(chunkList, boost::none, boost::none);
    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);
    // Three actions (in an unspecified order) should be immediately available.
    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_TRUE(future.isReady());
    auto future2 = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_TRUE(future2.isReady());
    auto future3 = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_TRUE(future3.isReady());
    // Verify their content of the received merge actions
    uint8_t timesLowerRangeMergeFound = 0;
    uint8_t timesUpperRangeMergeFound = 0;
    uint8_t timesMiddleRangeDataSizeFound = 0;
    auto inspectAction = [&](const DefragmentationAction& action) {
        stdx::visit(
            visit_helper::Overloaded{
                [&](const MergeInfo& mergeAction) {
                    if (mergeAction.chunkRange.getMin().woCompare(kKeyAtMin) == 0 &&
                        mergeAction.chunkRange.getMax().woCompare(BSON("x" << 5)) == 0) {
                        ++timesLowerRangeMergeFound;
                    }
                    if (mergeAction.chunkRange.getMin().woCompare(BSON("x" << 6)) == 0 &&
                        mergeAction.chunkRange.getMax().woCompare(kKeyAtMax) == 0) {
                        ++timesUpperRangeMergeFound;
                    }
                },
                [&](const DataSizeInfo& dataSizeAction) {
                    if (dataSizeAction.chunkRange.getMin().woCompare(BSON("x" << 5)) == 0 &&
                        dataSizeAction.chunkRange.getMax().woCompare(BSON("x" << 6)) == 0) {
                        ++timesMiddleRangeDataSizeFound;
                    }
                },
                [&](const AutoSplitVectorInfo& _) { FAIL("Unexpected action type"); },
                [&](const SplitInfoWithKeyPattern& _) { FAIL("Unexpected action type"); },
                [&](const MigrateInfo& _) { FAIL("Unexpected action type"); },
                [&](const EndOfActionStream& _) { FAIL("Unexpected action type"); }},
            action);
    };
    inspectAction(future.get());
    inspectAction(future2.get());
    inspectAction(future3.get());
    ASSERT_EQ(1, timesLowerRangeMergeFound);
    ASSERT_EQ(1, timesUpperRangeMergeFound);
    ASSERT_EQ(1, timesMiddleRangeDataSizeFound);

    auto future4 = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_FALSE(future4.isReady());
}

// Phase 2 tests.

TEST_F(BalancerDefragmentationPolicyTest, TestPhaseTwoMissingDataSizeRestartsPhase1) {
    auto coll = setupCollectionWithPhase({makeConfigChunkEntry()},
                                         DefragmentationPhaseEnum::kMoveAndMergeChunks);
    setDefaultClusterStats();
    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);

    // Should be in phase 1
    ASSERT_TRUE(_defragmentationPolicy.isDefragmentingCollection(coll.getUuid()));
    verifyExpectedDefragmentationPhaseOndisk(DefragmentationPhaseEnum::kMergeAndMeasureChunks);
    // There should be a datasize entry and no migrations
    stdx::unordered_set<ShardId> usedShards;
    auto pendingMigrations =
        _defragmentationPolicy.selectChunksToMove(operationContext(), &usedShards);
    ASSERT_EQ(0, pendingMigrations.size());
    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_TRUE(future.isReady());
    auto dataSizeAction = stdx::get<DataSizeInfo>(future.get());
}

TEST_F(BalancerDefragmentationPolicyTest, TestPhaseTwoChunkCanBeMovedAndMergedWithSibling) {
    ChunkType biggestChunk(
        kUuid,
        ChunkRange(kKeyAtMin, kKeyAtZero),
        ChunkVersion(1, 0, kCollectionVersion.epoch(), kCollectionVersion.getTimestamp()),
        kShardId0);
    biggestChunk.setEstimatedSizeBytes(2048);
    ChunkType smallestChunk(
        kUuid,
        ChunkRange(kKeyAtZero, kKeyAtMax),
        ChunkVersion(1, 1, kCollectionVersion.epoch(), kCollectionVersion.getTimestamp()),
        kShardId1);
    smallestChunk.setEstimatedSizeBytes(1024);

    auto coll = setupCollectionWithPhase({smallestChunk, biggestChunk},
                                         DefragmentationPhaseEnum::kMoveAndMergeChunks);
    std::vector<ShardStatistics> clusterStats{buildShardStats(kShardId0, 4),
                                              buildShardStats(kShardId1, 2)};
    std::map<NamespaceString, std::vector<ShardStatistics>> collectionStats{
        {kNss, {buildShardStats(kShardId0, 4), buildShardStats(kShardId1, 2)}}};
    _clusterStats.setStats(std::move(clusterStats), std::move(collectionStats));
    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);
    ASSERT_TRUE(_defragmentationPolicy.isDefragmentingCollection(coll.getUuid()));
    stdx::unordered_set<ShardId> usedShards;
    auto pendingMigrations =
        _defragmentationPolicy.selectChunksToMove(operationContext(), &usedShards);
    ASSERT_EQ(1, pendingMigrations.size());
    ASSERT_EQ(2, usedShards.size());
    auto moveAction = pendingMigrations.back();
    // The chunk belonging to the "fullest" shard is expected to be moved - even though it is bigger
    // than its sibling.
    ASSERT_EQ(biggestChunk.getShard(), moveAction.from);
    ASSERT_EQ(smallestChunk.getShard(), moveAction.to);
    ASSERT_BSONOBJ_EQ(biggestChunk.getMin(), moveAction.minKey);
    ASSERT_BSONOBJ_EQ(biggestChunk.getMax(), moveAction.maxKey);

    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_FALSE(future.isReady());

    _defragmentationPolicy.acknowledgeMoveResult(operationContext(), moveAction, Status::OK());
    ASSERT_TRUE(future.isReady());
    usedShards.clear();
    pendingMigrations = _defragmentationPolicy.selectChunksToMove(operationContext(), &usedShards);
    ASSERT_TRUE(pendingMigrations.empty());
    ASSERT_EQ(0, usedShards.size());

    auto mergeAction = stdx::get<MergeInfo>(future.get());
    ASSERT_EQ(smallestChunk.getShard(), mergeAction.shardId);
    ASSERT_TRUE(ChunkRange(biggestChunk.getMin(), smallestChunk.getMax()) ==
                mergeAction.chunkRange);

    _defragmentationPolicy.acknowledgeMergeResult(operationContext(), mergeAction, Status::OK());
    future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_FALSE(future.isReady());
    pendingMigrations = _defragmentationPolicy.selectChunksToMove(operationContext(), &usedShards);
    ASSERT_TRUE(pendingMigrations.empty());
}

TEST_F(BalancerDefragmentationPolicyTest,
       TestPhaseTwoMultipleCollectionChunkMigrationsMayBeIssuedConcurrently) {
    // Define a single collection, distributing 6 chunks across the 4 shards so that there cannot be
    // a merge without migrations
    ChunkType firstChunkOnShard0(
        kUuid,
        ChunkRange(kKeyAtMin, kKeyAtZero),
        ChunkVersion(1, 0, kCollectionVersion.epoch(), kCollectionVersion.getTimestamp()),
        kShardId0);
    firstChunkOnShard0.setEstimatedSizeBytes(1);

    ChunkType firstChunkOnShard1(
        kUuid,
        ChunkRange(kKeyAtZero, kKeyAtTen),
        ChunkVersion(1, 1, kCollectionVersion.epoch(), kCollectionVersion.getTimestamp()),
        kShardId1);
    firstChunkOnShard1.setEstimatedSizeBytes(1);

    ChunkType chunkOnShard2(
        kUuid,
        ChunkRange(kKeyAtTen, kKeyAtTwenty),
        ChunkVersion(1, 2, kCollectionVersion.epoch(), kCollectionVersion.getTimestamp()),
        kShardId2);
    chunkOnShard2.setEstimatedSizeBytes(1);

    ChunkType chunkOnShard3(
        kUuid,
        ChunkRange(kKeyAtTwenty, kKeyAtThirty),
        ChunkVersion(1, 3, kCollectionVersion.epoch(), kCollectionVersion.getTimestamp()),
        kShardId3);
    chunkOnShard3.setEstimatedSizeBytes(1);

    ChunkType secondChunkOnShard0(
        kUuid,
        ChunkRange(kKeyAtThirty, kKeyAtForty),
        ChunkVersion(1, 4, kCollectionVersion.epoch(), kCollectionVersion.getTimestamp()),
        kShardId0);
    secondChunkOnShard0.setEstimatedSizeBytes(1);

    ChunkType secondChunkOnShard1(
        kUuid,
        ChunkRange(kKeyAtForty, kKeyAtMax),
        ChunkVersion(1, 5, kCollectionVersion.epoch(), kCollectionVersion.getTimestamp()),
        kShardId1);
    secondChunkOnShard1.setEstimatedSizeBytes(1);

    auto coll = setupCollectionWithPhase({firstChunkOnShard0,
                                          firstChunkOnShard1,
                                          chunkOnShard2,
                                          chunkOnShard3,
                                          secondChunkOnShard0,
                                          secondChunkOnShard1},
                                         DefragmentationPhaseEnum::kMoveAndMergeChunks,
                                         boost::none);
    setDefaultClusterStats();
    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);

    // Two move operation should be returned within a single invocation, using all the possible
    // shards
    stdx::unordered_set<ShardId> usedShards;
    auto pendingMigrations =
        _defragmentationPolicy.selectChunksToMove(operationContext(), &usedShards);
    ASSERT_EQ(4, usedShards.size());
    ASSERT_EQ(2, pendingMigrations.size());
}

/** Phase 3 tests. By passing in DefragmentationPhaseEnum::kSplitChunks to
 * setupCollectionWithPhase, the persisted collection entry will have
 * kDefragmentationPhaseFieldName set to kSplitChunks and defragmentation will be started with
 * phase 3.
 */

TEST_F(BalancerDefragmentationPolicyTest, DefragmentationBeginsWithPhase3FromPersistedSetting) {
    auto coll = setupCollectionWithPhase({makeConfigChunkEntry(kPhase3DefaultChunkSize)},
                                         DefragmentationPhaseEnum::kSplitChunks);
    // Defragmentation does not start until refreshCollectionDefragmentationStatus is called
    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_FALSE(future.isReady());
    ASSERT_FALSE(_defragmentationPolicy.isDefragmentingCollection(coll.getUuid()));

    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);

    ASSERT_TRUE(_defragmentationPolicy.isDefragmentingCollection(coll.getUuid()));
    verifyExpectedDefragmentationPhaseOndisk(DefragmentationPhaseEnum::kSplitChunks);
}

TEST_F(BalancerDefragmentationPolicyTest, SingleLargeChunkCausesAutoSplitAndSplitActions) {
    auto coll = setupCollectionWithPhase({makeConfigChunkEntry(kPhase3DefaultChunkSize)},
                                         DefragmentationPhaseEnum::kSplitChunks);
    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());

    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);

    // The action returned by the stream should be now an actionable AutoSplitVector command...
    ASSERT_TRUE(future.isReady());
    AutoSplitVectorInfo splitVectorAction = stdx::get<AutoSplitVectorInfo>(future.get());
    // with the expected content
    ASSERT_EQ(coll.getNss(), splitVectorAction.nss);
    ASSERT_BSONOBJ_EQ(kKeyAtMin, splitVectorAction.minKey);
    ASSERT_BSONOBJ_EQ(kKeyAtMax, splitVectorAction.maxKey);
}

TEST_F(BalancerDefragmentationPolicyTest, CollectionMaxChunkSizeIsUsedForPhase3) {
    // One chunk > 1KB should trigger AutoSplitVector
    auto coll = setupCollectionWithPhase(
        {makeConfigChunkEntry(2 * 1024)}, DefragmentationPhaseEnum::kSplitChunks, 1024);

    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);

    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());

    // The action returned by the stream should be now an actionable AutoSplitVector command...
    ASSERT_TRUE(future.isReady());
    AutoSplitVectorInfo splitVectorAction = stdx::get<AutoSplitVectorInfo>(future.get());
    // with the expected content
    ASSERT_EQ(coll.getNss(), splitVectorAction.nss);
    ASSERT_BSONOBJ_EQ(kKeyAtMin, splitVectorAction.minKey);
    ASSERT_BSONOBJ_EQ(kKeyAtMax, splitVectorAction.maxKey);
}

TEST_F(BalancerDefragmentationPolicyTest, TestRetryableFailedAutoSplitActionGetsReissued) {
    auto coll = setupCollectionWithPhase({makeConfigChunkEntry(kPhase3DefaultChunkSize)},
                                         DefragmentationPhaseEnum::kSplitChunks);
    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);
    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    AutoSplitVectorInfo failingAutoSplitAction = stdx::get<AutoSplitVectorInfo>(future.get());

    _defragmentationPolicy.acknowledgeAutoSplitVectorResult(
        operationContext(),
        failingAutoSplitAction,
        Status(ErrorCodes::NetworkTimeout, "Testing error response"));

    // Under the setup of this test, the stream should only contain one more action - which (version
    // aside) matches the failed one.
    future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    auto replayedAutoSplitAction = stdx::get<AutoSplitVectorInfo>(future.get());
    ASSERT_BSONOBJ_EQ(failingAutoSplitAction.minKey, replayedAutoSplitAction.minKey);
    ASSERT_BSONOBJ_EQ(failingAutoSplitAction.maxKey, replayedAutoSplitAction.maxKey);
    ASSERT_EQ(failingAutoSplitAction.uuid, replayedAutoSplitAction.uuid);
    ASSERT_EQ(failingAutoSplitAction.shardId, replayedAutoSplitAction.shardId);
    ASSERT_EQ(failingAutoSplitAction.nss, replayedAutoSplitAction.nss);
    ASSERT_BSONOBJ_EQ(failingAutoSplitAction.keyPattern, replayedAutoSplitAction.keyPattern);
    ASSERT_EQ(failingAutoSplitAction.maxChunkSizeBytes, replayedAutoSplitAction.maxChunkSizeBytes);

    future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_FALSE(future.isReady());
}

TEST_F(BalancerDefragmentationPolicyTest,
       TestAcknowledgeAutoSplitActionTriggersSplitOnResultingRange) {
    auto coll = setupCollectionWithPhase({makeConfigChunkEntry(kPhase3DefaultChunkSize)},
                                         DefragmentationPhaseEnum::kSplitChunks);
    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);
    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    auto autoSplitAction = stdx::get<AutoSplitVectorInfo>(future.get());

    std::vector<BSONObj> splitPoints{BSON("x" << 5)};
    _defragmentationPolicy.acknowledgeAutoSplitVectorResult(
        operationContext(), autoSplitAction, StatusWith(splitPoints));

    // Under the setup of this test, the stream should only contain only a split action over the
    // recently AutoSplitVector-ed range.
    future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    auto splitAction = stdx::get<SplitInfoWithKeyPattern>(future.get());
    ASSERT_BSONOBJ_EQ(splitAction.info.minKey, autoSplitAction.minKey);
    ASSERT_BSONOBJ_EQ(splitAction.info.maxKey, autoSplitAction.maxKey);
    ASSERT_EQ(splitAction.uuid, autoSplitAction.uuid);
    ASSERT_EQ(splitAction.info.shardId, autoSplitAction.shardId);
    ASSERT_EQ(splitAction.info.nss, autoSplitAction.nss);
    ASSERT_EQ(splitAction.info.splitKeys.size(), 1);
    ASSERT_BSONOBJ_EQ(splitAction.info.splitKeys[0], splitPoints[0]);

    future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_FALSE(future.isReady());
}

TEST_F(BalancerDefragmentationPolicyTest, TestAutoSplitWithNoSplitPointsDoesNotTriggerSplit) {
    auto coll = setupCollectionWithPhase({makeConfigChunkEntry(kPhase3DefaultChunkSize)},
                                         DefragmentationPhaseEnum::kSplitChunks);
    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);
    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    auto autoSplitAction = stdx::get<AutoSplitVectorInfo>(future.get());

    std::vector<BSONObj> splitPoints;
    _defragmentationPolicy.acknowledgeAutoSplitVectorResult(
        operationContext(), autoSplitAction, StatusWith(splitPoints));

    // The stream should now be empty
    future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_FALSE(future.isReady());
}

TEST_F(BalancerDefragmentationPolicyTest, TestMoreThan16MBSplitPointsTriggersSplitAndAutoSplit) {
    auto coll = setupCollectionWithPhase({makeConfigChunkEntry(kPhase3DefaultChunkSize)},
                                         DefragmentationPhaseEnum::kSplitChunks);
    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);
    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    auto autoSplitAction = stdx::get<AutoSplitVectorInfo>(future.get());

    // TODO (SERVER-61678) use continuation flag instead of large vector
    std::vector<BSONObj> splitPoints = [] {
        std::vector<BSONObj> splitPoints;
        int splitPointSize = 0;
        std::string filler(1024 * 1024, 'x');
        int distinguisher = 0;
        while (splitPointSize < BSONObjMaxUserSize) {
            auto newBSON = BSON("id" << distinguisher++ << "filler" << filler);
            splitPointSize += newBSON.objsize();
            splitPoints.push_back(newBSON);
        }
        return splitPoints;
    }();
    _defragmentationPolicy.acknowledgeAutoSplitVectorResult(
        operationContext(), autoSplitAction, StatusWith(splitPoints));

    // The stream should now contain one Split action with the split points from above and one
    // AutoSplitVector action from the last split point to the end of the chunk
    future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    auto splitAction = stdx::get<SplitInfoWithKeyPattern>(future.get());
    ASSERT_BSONOBJ_EQ(splitAction.info.minKey, autoSplitAction.minKey);
    ASSERT_BSONOBJ_EQ(splitAction.info.maxKey, autoSplitAction.maxKey);
    ASSERT_EQ(splitAction.info.splitKeys.size(), splitPoints.size());
    ASSERT_BSONOBJ_EQ(splitAction.info.splitKeys[0], splitPoints[0]);
    ASSERT_BSONOBJ_EQ(splitAction.info.splitKeys.back(), splitPoints.back());
    future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    auto nextAutoSplitAction = stdx::get<AutoSplitVectorInfo>(future.get());
    ASSERT_BSONOBJ_EQ(nextAutoSplitAction.minKey, splitPoints.back());
    ASSERT_BSONOBJ_EQ(nextAutoSplitAction.maxKey, autoSplitAction.maxKey);

    future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_FALSE(future.isReady());
}

TEST_F(BalancerDefragmentationPolicyTest, TestFailedSplitChunkActionGetsReissued) {
    auto coll = setupCollectionWithPhase({makeConfigChunkEntry(kPhase3DefaultChunkSize)},
                                         DefragmentationPhaseEnum::kSplitChunks);
    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);
    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    auto autoSplitAction = stdx::get<AutoSplitVectorInfo>(future.get());

    std::vector<BSONObj> splitPoints{BSON("x" << 5)};
    _defragmentationPolicy.acknowledgeAutoSplitVectorResult(
        operationContext(), autoSplitAction, StatusWith(splitPoints));

    // The stream should now contain the split action for the recently AutoSplitVector-ed range.
    future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    auto failingSplitAction = stdx::get<SplitInfoWithKeyPattern>(future.get());
    _defragmentationPolicy.acknowledgeSplitResult(
        operationContext(),
        failingSplitAction,
        Status(ErrorCodes::NetworkTimeout, "Testing error response"));
    // Under the setup of this test, the stream should only contain one more action - which (version
    // aside) matches the failed one.
    future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    auto replayedSplitAction = stdx::get<SplitInfoWithKeyPattern>(future.get());
    ASSERT_EQ(failingSplitAction.uuid, replayedSplitAction.uuid);
    ASSERT_EQ(failingSplitAction.info.shardId, replayedSplitAction.info.shardId);
    ASSERT_EQ(failingSplitAction.info.nss, replayedSplitAction.info.nss);
    ASSERT_BSONOBJ_EQ(failingSplitAction.info.minKey, replayedSplitAction.info.minKey);
    ASSERT_BSONOBJ_EQ(failingSplitAction.info.maxKey, replayedSplitAction.info.maxKey);

    future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_FALSE(future.isReady());
}

TEST_F(BalancerDefragmentationPolicyTest,
       TestAcknowledgeLastSuccessfulSplitActionEndsDefragmentation) {
    auto coll = setupCollectionWithPhase({makeConfigChunkEntry(kPhase3DefaultChunkSize)},
                                         DefragmentationPhaseEnum::kSplitChunks);
    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);
    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    auto autoSplitAction = stdx::get<AutoSplitVectorInfo>(future.get());

    std::vector<BSONObj> splitPoints{BSON("x" << 5)};
    _defragmentationPolicy.acknowledgeAutoSplitVectorResult(
        operationContext(), autoSplitAction, StatusWith(splitPoints));

    // The stream should now contain the split action for the recently AutoSplitVector-ed range.
    future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    auto splitAction = stdx::get<SplitInfoWithKeyPattern>(future.get());
    _defragmentationPolicy.acknowledgeSplitResult(operationContext(), splitAction, Status::OK());

    // Successful split actions trigger no new actions
    future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_FALSE(future.isReady());

    // With phase 3 complete, defragmentation should be completed.
    ASSERT_FALSE(_defragmentationPolicy.isDefragmentingCollection(coll.getUuid()));
    verifyExpectedDefragmentationPhaseOndisk(boost::none);
}

}  // namespace
}  // namespace mongo
