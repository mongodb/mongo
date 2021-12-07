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
#include "mongo/db/s/balancer/cluster_statistics_impl.h"
#include "mongo/db/s/config/config_server_test_fixture.h"

namespace mongo {
namespace {

class BalancerDefragmentationPolicyTest : public ConfigServerTestFixture {
protected:
    const NamespaceString kNss{"testDb.testColl"};
    const UUID kUuid = UUID::gen();
    const ShardId kShardId0 = ShardId("shard0");
    const ShardId kShardId1 = ShardId("shard1");
    const ChunkVersion kCollectionVersion = ChunkVersion(1, 1, OID::gen(), Timestamp(10));
    const KeyPattern kShardKeyPattern = KeyPattern(BSON("x" << 1));
    const BSONObj kMinKey = BSON("x" << 0);
    const BSONObj kMaxKey = BSON("x" << 10);
    const long long kMaxChunkSizeBytes{2048};
    const HostAndPort kShardHost0 = HostAndPort("TestHost0", 12345);
    const HostAndPort kShardHost1 = HostAndPort("TestHost1", 12346);

    const std::vector<ShardType> kShardList{
        ShardType(kShardId0.toString(), kShardHost0.toString()),
        ShardType(kShardId1.toString(), kShardHost1.toString())};

    BalancerDefragmentationPolicyTest()
        : _random(std::random_device{}()),
          _clusterStats(std::make_unique<ClusterStatisticsImpl>(_random)),
          _defragmentationPolicy(_clusterStats.get()) {}

    CollectionType makeConfigCollectionEntry() {
        CollectionType shardedCollection(kNss, OID::gen(), Timestamp(1, 1), Date_t::now(), kUuid);
        shardedCollection.setKeyPattern(kShardKeyPattern);
        shardedCollection.setBalancerShouldMergeChunks(true);
        ASSERT_OK(insertToConfigCollection(
            operationContext(), CollectionType::ConfigNS, shardedCollection.toBSON()));
        return shardedCollection;
    }

    CollectionType setupCollectionForPhase1(std::vector<ChunkType>& chunkList) {
        setupShards(kShardList);
        setupCollection(kNss, kShardKeyPattern, chunkList);
        ASSERT_OK(updateToConfigCollection(
            operationContext(),
            CollectionType::ConfigNS,
            BSON(CollectionType::kUuidFieldName << kUuid),
            BSON("$set" << BSON(CollectionType::kBalancerShouldMergeChunksFieldName << true)),
            false));
        return Grid::get(operationContext())
            ->catalogClient()
            ->getCollection(operationContext(), kUuid);
    }

    void makeConfigChunkEntry() {
        ChunkType chunk(kUuid, ChunkRange(kMinKey, kMaxKey), kCollectionVersion, kShardId0);
        ASSERT_OK(insertToConfigCollection(
            operationContext(), ChunkType::ConfigNS, chunk.toConfigBSON()));
    }

    BSONObj getConfigCollectionEntry() {
        DBDirectClient client(operationContext());
        auto cursor = client.query(NamespaceStringOrUUID(CollectionType::ConfigNS),
                                   BSON(CollectionType::kUuidFieldName << kUuid),
                                   {});
        if (!cursor || !cursor->more())
            return BSONObj();
        else
            return cursor->next();
    }

    BalancerRandomSource _random;
    std::unique_ptr<ClusterStatistics> _clusterStats;
    BalancerDefragmentationPolicyImpl _defragmentationPolicy;
};

TEST_F(BalancerDefragmentationPolicyTest, TestAddCollection) {
    auto coll = makeConfigCollectionEntry();
    FailPointEnableBlock failpoint("skipPhaseTransition");
    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);
    // Test for persistence
    auto configDoc = findOneOnConfigCollection(operationContext(),
                                               CollectionType::ConfigNS,
                                               BSON(CollectionType::kUuidFieldName << kUuid))
                         .getValue();
    auto storedDefragmentationPhase = DefragmentationPhase_parse(
        IDLParserErrorContext("BalancerDefragmentationPolicyTest"),
        configDoc.getStringField(CollectionType::kDefragmentationPhaseFieldName));
    ASSERT_TRUE(storedDefragmentationPhase == DefragmentationPhaseEnum::kMergeChunks);
}

TEST_F(BalancerDefragmentationPolicyTest, TestAddCollectionNoActions) {
    auto coll = makeConfigCollectionEntry();
    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);
    auto configDoc = findOneOnConfigCollection(operationContext(),
                                               CollectionType::ConfigNS,
                                               BSON(CollectionType::kUuidFieldName << kUuid))
                         .getValue();
    ASSERT_FALSE(configDoc.hasField(CollectionType::kDefragmentationPhaseFieldName));
}

TEST_F(BalancerDefragmentationPolicyTest, TestIsDefragmentingCollection) {
    auto coll = makeConfigCollectionEntry();
    FailPointEnableBlock failpoint("skipPhaseTransition");
    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);
    ASSERT_TRUE(_defragmentationPolicy.isDefragmentingCollection(kUuid));
    ASSERT_FALSE(_defragmentationPolicy.isDefragmentingCollection(UUID::gen()));
}

TEST_F(BalancerDefragmentationPolicyTest, TestGetNextActionNoReadyActions) {
    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_FALSE(future.isReady());
}

TEST_F(BalancerDefragmentationPolicyTest, TestAcknowledgeFailedMergeResult) {
    auto coll = makeConfigCollectionEntry();
    FailPointEnableBlock failpoint("skipPhaseTransition");
    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);
    makeConfigChunkEntry();
    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    auto mergeInfo =
        MergeInfo(kShardId0, kNss, kUuid, kCollectionVersion, ChunkRange(kMinKey, kMaxKey));
    _defragmentationPolicy.acknowledgeMergeResult(
        operationContext(),
        mergeInfo,
        Status(ErrorCodes::NetworkTimeout, "Testing error response"));
    ASSERT_TRUE(future.isReady());
    DefragmentationAction streamingAction = future.get();
    MergeInfo mergeAction = stdx::get<MergeInfo>(streamingAction);
    ASSERT_EQ(mergeAction.nss, mergeInfo.nss);
}

TEST_F(BalancerDefragmentationPolicyTest, TestAcknowledgeFailedSplitVectorResponse) {
    auto coll = makeConfigCollectionEntry();
    FailPointEnableBlock failpoint("skipPhaseTransition");
    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);
    makeConfigChunkEntry();
    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    auto splitVectorInfo = AutoSplitVectorInfo(
        kShardId0, kNss, kUuid, kCollectionVersion, BSONObj(), kMinKey, kMaxKey, 120);
    _defragmentationPolicy.acknowledgeAutoSplitVectorResult(
        operationContext(),
        splitVectorInfo,
        Status(ErrorCodes::NetworkTimeout, "Testing error response"));
    ASSERT_TRUE(future.isReady());
    AutoSplitVectorInfo splitVectorAction = stdx::get<AutoSplitVectorInfo>(future.get());
    ASSERT_EQ(splitVectorInfo.nss, splitVectorAction.nss);
}

TEST_F(BalancerDefragmentationPolicyTest, TestAcknowledgeFailedSplitAction) {
    auto coll = makeConfigCollectionEntry();
    FailPointEnableBlock failpoint("skipPhaseTransition");
    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);
    makeConfigChunkEntry();
    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    auto splitInfo = SplitInfoWithKeyPattern(kShardId0,
                                             kNss,
                                             kCollectionVersion,
                                             kMinKey,
                                             kMaxKey,
                                             {},
                                             kUuid,
                                             kShardKeyPattern.toBSON());
    _defragmentationPolicy.acknowledgeSplitResult(
        operationContext(),
        splitInfo,
        Status(ErrorCodes::NetworkTimeout, "Testing error response"));
    ASSERT_TRUE(future.isReady());
    SplitInfoWithKeyPattern splitAction = stdx::get<SplitInfoWithKeyPattern>(future.get());
    ASSERT_EQ(splitInfo.info.nss, splitAction.info.nss);
}

TEST_F(BalancerDefragmentationPolicyTest, TestAcknowledgeFailedDataSizeAction) {
    auto coll = makeConfigCollectionEntry();
    FailPointEnableBlock failpoint("skipPhaseTransition");
    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);
    makeConfigChunkEntry();
    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    auto dataSizeInfo = DataSizeInfo(kShardId0,
                                     kNss,
                                     kUuid,
                                     ChunkRange(kMinKey, kMaxKey),
                                     kCollectionVersion,
                                     kShardKeyPattern,
                                     false);
    _defragmentationPolicy.acknowledgeDataSizeResult(
        operationContext(),
        dataSizeInfo,
        Status(ErrorCodes::NetworkTimeout, "Testing error response"));
    ASSERT_TRUE(future.isReady());
    DataSizeInfo dataSizeAction = stdx::get<DataSizeInfo>(future.get());
    ASSERT_EQ(dataSizeInfo.nss, dataSizeAction.nss);
}

TEST_F(BalancerDefragmentationPolicyTest, TestAcknowledgeSuccessfulMergeAction) {
    auto coll = makeConfigCollectionEntry();
    FailPointEnableBlock failpoint("skipPhaseTransition");
    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);
    makeConfigChunkEntry();
    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    auto mergeInfo =
        MergeInfo(kShardId0, kNss, kUuid, kCollectionVersion, ChunkRange(kMinKey, kMaxKey));
    _defragmentationPolicy.acknowledgeMergeResult(operationContext(), mergeInfo, Status::OK());
    ASSERT_TRUE(future.isReady());
    DataSizeInfo dataSizeAction = stdx::get<DataSizeInfo>(future.get());
    ASSERT_EQ(mergeInfo.nss, dataSizeAction.nss);
    ASSERT_BSONOBJ_EQ(mergeInfo.chunkRange.getMin(), dataSizeAction.chunkRange.getMin());
    ASSERT_BSONOBJ_EQ(mergeInfo.chunkRange.getMax(), dataSizeAction.chunkRange.getMax());
}

TEST_F(BalancerDefragmentationPolicyTest, TestAcknowledgeSuccessfulAutoSplitVectorAction) {
    std::vector<BSONObj> splitPoints = {BSON("x" << 4)};
    auto coll = makeConfigCollectionEntry();
    FailPointEnableBlock failpoint("skipPhaseTransition");
    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);
    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    auto splitVectorInfo = AutoSplitVectorInfo(kShardId0,
                                               kNss,
                                               kUuid,
                                               kCollectionVersion,
                                               kShardKeyPattern.toBSON(),
                                               kMinKey,
                                               kMaxKey,
                                               2048);

    _defragmentationPolicy.acknowledgeAutoSplitVectorResult(
        operationContext(), splitVectorInfo, StatusWith(splitPoints));
    ASSERT_TRUE(future.isReady());
    SplitInfoWithKeyPattern splitAction = stdx::get<SplitInfoWithKeyPattern>(future.get());
    ASSERT_EQ(splitVectorInfo.nss, splitAction.info.nss);
    ASSERT_EQ(splitAction.info.splitKeys.size(), 1);
    ASSERT_BSONOBJ_EQ(splitAction.info.splitKeys.at(0), splitPoints.at(0));
}

TEST_F(BalancerDefragmentationPolicyTest, TestAcknowledgeSuccessfulSplitAction) {
    std::vector<BSONObj> splitPoints = {BSON("x" << 4)};
    auto coll = makeConfigCollectionEntry();
    FailPointEnableBlock failpoint("skipPhaseTransition");
    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);
    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    auto splitInfo = SplitInfoWithKeyPattern(kShardId0,
                                             kNss,
                                             kCollectionVersion,
                                             kMinKey,
                                             kMaxKey,
                                             splitPoints,
                                             kUuid,
                                             kShardKeyPattern.toBSON());
    _defragmentationPolicy.acknowledgeSplitResult(operationContext(), splitInfo, Status::OK());
    ASSERT_FALSE(future.isReady());
}

TEST_F(BalancerDefragmentationPolicyTest, TestAcknowledgeSuccessfulDataSizeAction) {
    auto coll = makeConfigCollectionEntry();
    FailPointEnableBlock failpoint("skipPhaseTransition");
    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);
    makeConfigChunkEntry();
    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    auto dataSizeInfo = DataSizeInfo(kShardId0,
                                     kNss,
                                     kUuid,
                                     ChunkRange(kMinKey, kMaxKey),
                                     kCollectionVersion,
                                     kShardKeyPattern,
                                     false);
    auto resp = StatusWith(DataSizeResponse(2000, 4));
    _defragmentationPolicy.acknowledgeDataSizeResult(operationContext(), dataSizeInfo, resp);
    auto chunkQuery = BSON(ChunkType::collectionUUID()
                           << kUuid << ChunkType::min(kMinKey) << ChunkType::max(kMaxKey));
    auto configDoc =
        findOneOnConfigCollection(operationContext(), ChunkType::ConfigNS, chunkQuery).getValue();
    ASSERT_EQ(configDoc.getIntField(ChunkType::estimatedSizeBytes.name()), 2000);
}

TEST_F(BalancerDefragmentationPolicyTest, TestPhase1AllConsecutive) {
    // Set up collection with all mergeable chunks
    std::vector<ChunkType> chunkList;
    for (int i = 0; i < 5; i++) {
        ChunkType chunk(
            kUuid,
            ChunkRange(BSON("x" << i), BSON("x" << i + 1)),
            ChunkVersion(1, i, kCollectionVersion.epoch(), kCollectionVersion.getTimestamp()),
            kShardId0);
        chunkList.push_back(chunk);
    }
    for (int i = 5; i < 10; i++) {
        ChunkType chunk(
            kUuid,
            ChunkRange(BSON("x" << i), BSON("x" << i + 1)),
            ChunkVersion(1, i, kCollectionVersion.epoch(), kCollectionVersion.getTimestamp()),
            kShardId1);
        chunkList.push_back(chunk);
    }
    auto coll = setupCollectionForPhase1(chunkList);
    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);
    // Test
    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_TRUE(future.isReady());
    MergeInfo mergeAction = stdx::get<MergeInfo>(future.get());
    ASSERT_BSONOBJ_EQ(mergeAction.chunkRange.getMin(), BSON("x" << 0));
    ASSERT_BSONOBJ_EQ(mergeAction.chunkRange.getMax(), BSON("x" << 5));
    auto future2 = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_TRUE(future2.isReady());
    MergeInfo mergeAction2 = stdx::get<MergeInfo>(future2.get());
    ASSERT_BSONOBJ_EQ(mergeAction2.chunkRange.getMin(), BSON("x" << 5));
    ASSERT_BSONOBJ_EQ(mergeAction2.chunkRange.getMax(), BSON("x" << 10));
    auto future3 = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_FALSE(future3.isReady());
}

TEST_F(BalancerDefragmentationPolicyTest, Phase1NotConsecutive) {
    std::vector<ChunkType> chunkList;
    for (int i = 0; i < 10; i++) {
        ShardId chosenShard = (i == 5) ? kShardId1 : kShardId0;
        ChunkType chunk(
            kUuid,
            ChunkRange(BSON("x" << i), BSON("x" << i + 1)),
            ChunkVersion(1, i, kCollectionVersion.epoch(), kCollectionVersion.getTimestamp()),
            chosenShard);
        chunkList.push_back(chunk);
    }
    auto coll = setupCollectionForPhase1(chunkList);
    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);
    // Test
    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_TRUE(future.isReady());
    MergeInfo mergeAction = stdx::get<MergeInfo>(future.get());
    ASSERT_BSONOBJ_EQ(mergeAction.chunkRange.getMin(), BSON("x" << 0));
    ASSERT_BSONOBJ_EQ(mergeAction.chunkRange.getMax(), BSON("x" << 5));
    auto future2 = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_TRUE(future2.isReady());
    MergeInfo mergeAction2 = stdx::get<MergeInfo>(future2.get());
    ASSERT_BSONOBJ_EQ(mergeAction2.chunkRange.getMin(), BSON("x" << 6));
    ASSERT_BSONOBJ_EQ(mergeAction2.chunkRange.getMax(), BSON("x" << 10));
    auto future3 = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_TRUE(future3.isReady());
    DataSizeInfo dataSizeAction = stdx::get<DataSizeInfo>(future3.get());
    ASSERT_BSONOBJ_EQ(dataSizeAction.chunkRange.getMin(), BSON("x" << 5));
    ASSERT_BSONOBJ_EQ(dataSizeAction.chunkRange.getMax(), BSON("x" << 6));
    auto future4 = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_FALSE(future4.isReady());
}

}  // namespace
}  // namespace mongo
