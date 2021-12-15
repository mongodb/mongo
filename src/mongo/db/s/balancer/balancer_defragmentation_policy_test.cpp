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
    const BSONObj kKeyAtZero = BSON("x" << 0);
    const BSONObj kKeyAtTen = BSON("x" << 10);
    const BSONObj kKeyAtTwenty = BSON("x" << 20);
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

    void makeConfigChunkEntry(const boost::optional<long long>& estimatedSize = boost::none) {
        ChunkType chunk(kUuid, ChunkRange(kKeyAtZero, kKeyAtTen), kCollectionVersion, kShardId0);
        chunk.setEstimatedSizeBytes(estimatedSize);
        ASSERT_OK(insertToConfigCollection(
            operationContext(), ChunkType::ConfigNS, chunk.toConfigBSON()));
    }

    void makeMergeableConfigChunkEntries() {
        auto opCtx = operationContext();
        ChunkType chunk(kUuid, ChunkRange(kKeyAtZero, kKeyAtTen), kCollectionVersion, kShardId0);
        ChunkType chunk2(kUuid, ChunkRange(kKeyAtTen, kKeyAtTwenty), kCollectionVersion, kShardId0);
        ASSERT_OK(insertToConfigCollection(opCtx, ChunkType::ConfigNS, chunk.toConfigBSON()));
        ASSERT_OK(insertToConfigCollection(opCtx, ChunkType::ConfigNS, chunk2.toConfigBSON()));
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

TEST_F(BalancerDefragmentationPolicyTest, TestGetNextActionIsNotReadyWhenNotDefragmenting) {
    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_FALSE(future.isReady());
}

TEST_F(BalancerDefragmentationPolicyTest, TestAddEmptyCollectionDoesNotTriggerDefragmentation) {
    auto coll = makeConfigCollectionEntry();
    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);

    ASSERT_FALSE(_defragmentationPolicy.isDefragmentingCollection(coll.getUuid()));
    auto configDoc = findOneOnConfigCollection(operationContext(),
                                               CollectionType::ConfigNS,
                                               BSON(CollectionType::kUuidFieldName << kUuid))
                         .getValue();
    ASSERT_FALSE(configDoc.hasField(CollectionType::kDefragmentationPhaseFieldName));

    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_FALSE(future.isReady());
}

TEST_F(BalancerDefragmentationPolicyTest, TestAddSingleChunkCollectionTriggersDataSize) {
    auto coll = makeConfigCollectionEntry();
    makeConfigChunkEntry();
    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_FALSE(future.isReady());
    ASSERT_FALSE(_defragmentationPolicy.isDefragmentingCollection(coll.getUuid()));

    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);

    // 1. The collection should be marked as undergoing through phase 1 of the algorithm...
    ASSERT_TRUE(_defragmentationPolicy.isDefragmentingCollection(coll.getUuid()));
    auto configDoc = findOneOnConfigCollection(operationContext(),
                                               CollectionType::ConfigNS,
                                               BSON(CollectionType::kUuidFieldName << kUuid))
                         .getValue();
    auto storedDefragmentationPhase = DefragmentationPhase_parse(
        IDLParserErrorContext("BalancerDefragmentationPolicyTest"),
        configDoc.getStringField(CollectionType::kDefragmentationPhaseFieldName));
    ASSERT_TRUE(storedDefragmentationPhase == DefragmentationPhaseEnum::kMergeChunks);
    // 2. The action returned by the stream should be now an actionable DataSizeCommand...
    ASSERT_TRUE(future.isReady());
    DataSizeInfo dataSizeAction = stdx::get<DataSizeInfo>(future.get());
    // 3. with the expected content
    // TODO refactor chunk builder
    ASSERT_EQ(coll.getNss(), dataSizeAction.nss);
    ASSERT_BSONOBJ_EQ(kKeyAtZero, dataSizeAction.chunkRange.getMin());
    ASSERT_BSONOBJ_EQ(kKeyAtTen, dataSizeAction.chunkRange.getMax());
}

TEST_F(BalancerDefragmentationPolicyTest,
       TestAddSingleChunkCollectionAlreadyMeasuredDoesNotTriggerDefragmentation) {
    auto coll = makeConfigCollectionEntry();
    makeConfigChunkEntry(1024);

    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);

    ASSERT_FALSE(_defragmentationPolicy.isDefragmentingCollection(coll.getUuid()));
    auto configDoc = findOneOnConfigCollection(operationContext(),
                                               CollectionType::ConfigNS,
                                               BSON(CollectionType::kUuidFieldName << kUuid))
                         .getValue();
    ASSERT_FALSE(configDoc.hasField(CollectionType::kDefragmentationPhaseFieldName));


    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_FALSE(future.isReady());
}

// TODO switch to "TestAcknowledgeFinalDataSizeActionEndsPhase1"
TEST_F(BalancerDefragmentationPolicyTest, TestAcknowledgeFinalDataSizeActionEndsDefragmentation) {
    auto coll = makeConfigCollectionEntry();
    makeConfigChunkEntry();
    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);
    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    DataSizeInfo dataSizeAction = stdx::get<DataSizeInfo>(future.get());

    auto resp = StatusWith(DataSizeResponse(2000, 4));
    _defragmentationPolicy.acknowledgeDataSizeResult(operationContext(), dataSizeAction, resp);

    // 1. The outcome of the data size has been stored in the expected document...
    auto chunkQuery = BSON(ChunkType::collectionUUID()
                           << kUuid << ChunkType::min(kKeyAtZero) << ChunkType::max(kKeyAtTen));
    auto configChunkDoc =
        findOneOnConfigCollection(operationContext(), ChunkType::ConfigNS, chunkQuery).getValue();
    ASSERT_EQ(configChunkDoc.getIntField(ChunkType::estimatedSizeBytes.name()), 2000);

    // 2. and the algorithm is complete
    future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_FALSE(future.isReady());
    ASSERT_FALSE(_defragmentationPolicy.isDefragmentingCollection(coll.getUuid()));
    auto configCollDoc = findOneOnConfigCollection(operationContext(),
                                                   CollectionType::ConfigNS,
                                                   BSON(CollectionType::kUuidFieldName << kUuid))
                             .getValue();
    ASSERT_FALSE(configCollDoc.hasField(CollectionType::kDefragmentationPhaseFieldName));
}

// TODO (SERVER-61533) add tests to distinguish recoverable VS unrecoverable errors.
TEST_F(BalancerDefragmentationPolicyTest, TestFailedDataSizeActionGetsReissued) {
    auto coll = makeConfigCollectionEntry();
    makeConfigChunkEntry();
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


TEST_F(BalancerDefragmentationPolicyTest,
       TestAcknowledgeMergeChunkActionsTriggersDataSizeOnResultingRange) {
    auto coll = makeConfigCollectionEntry();
    makeMergeableConfigChunkEntries();
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

TEST_F(BalancerDefragmentationPolicyTest, TestFailedMergeChunksActionGetsReissued) {
    auto coll = makeConfigCollectionEntry();
    makeMergeableConfigChunkEntries();
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

TEST_F(BalancerDefragmentationPolicyTest, TestAcknowledgeSuccessfulMergeAction) {
    auto coll = makeConfigCollectionEntry();
    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_FALSE(future.isReady());
    makeMergeableConfigChunkEntries();
    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);
    ASSERT_TRUE(future.isReady());
    MergeInfo mergeInfoAction = stdx::get<MergeInfo>(future.get());
    ASSERT_BSONOBJ_EQ(mergeInfoAction.chunkRange.getMin(), kKeyAtZero);
    ASSERT_BSONOBJ_EQ(mergeInfoAction.chunkRange.getMax(), kKeyAtTwenty);
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
