/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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
#include "mongo/db/s/balancer/cluster_chunks_resize_policy_impl.h"
#include "mongo/db/s/config/config_server_test_fixture.h"

namespace mongo {
namespace {

class ClusterChunksResizePolicyTest : public ConfigServerTestFixture {
protected:
    const NamespaceString kNss{"testDb.testColl"};
    const UUID kUuid = UUID::gen();
    const ChunkVersion kCollectionVersion = ChunkVersion({OID::gen(), Timestamp(10)}, {1, 1});

    const ShardId kShardId0 = ShardId("shard0");
    const ShardId kShardId1 = ShardId("shard1");
    const HostAndPort kShardHost0 = HostAndPort("TestHost0", 12345);
    const HostAndPort kShardHost1 = HostAndPort("TestHost1", 12346);
    const std::vector<ShardType> kShardList{
        ShardType(kShardId0.toString(), kShardHost0.toString()),
        ShardType(kShardId1.toString(), kShardHost1.toString())};

    const KeyPattern kShardKeyPattern = KeyPattern(BSON("x" << 1));
    const BSONObj kKeyAtMin = BSONObjBuilder().appendMinKey("x").obj();
    const BSONObj kKeyAtZero = BSON("x" << 0);
    const BSONObj kKeyAtTen = BSON("x" << 10);
    const BSONObj kKeyAtTwenty = BSON("x" << 20);
    const BSONObj kKeyAtThirty = BSON("x" << 30);
    const BSONObj kKeyAtForty = BSON("x" << 40);
    const BSONObj kKeyAtMax = BSONObjBuilder().appendMaxKey("x").obj();

    const int64_t kDefaultMaxChunksSizeBytes = 1024;

    OperationContext* _opCtx;

    ClusterChunksResizePolicyTest() : _clusterChunksResizePolicy([] {}) {}

    void setUp() override {
        ConfigServerTestFixture::setUp();
        _opCtx = operationContext();
        setupShards(kShardList);
    }

    ClusterChunksResizePolicyImpl _clusterChunksResizePolicy;

    CollectionType buildShardedCollection(
        const NamespaceString nss,
        const KeyPattern& shardKeyPattern,
        const std::vector<ChunkType>& chunkList,
        bool markAsAlreadyProcessed = false,
        boost::optional<int64_t> maxChunkSizeBytes = boost::none) {
        setupCollection(nss, shardKeyPattern, chunkList);
        if (markAsAlreadyProcessed || maxChunkSizeBytes.has_value()) {
            BSONObjBuilder updateQueryBuilder;
            BSONObjBuilder setObj(updateQueryBuilder.subobjStart("$set"));
            if (markAsAlreadyProcessed) {
                setObj.append(CollectionType::kChunksAlreadySplitForDowngradeFieldName, true);
            }
            if (maxChunkSizeBytes.has_value()) {
                setObj.append(CollectionType::kMaxChunkSizeBytesFieldName, *maxChunkSizeBytes);
            }
            setObj.done();
            ASSERT_OK(updateToConfigCollection(_opCtx,
                                               CollectionType::ConfigNS,
                                               BSON(CollectionType::kNssFieldName << nss.ns()),
                                               updateQueryBuilder.obj(),
                                               false));
        }

        return Grid::get(_opCtx)->catalogClient()->getCollection(_opCtx, nss);
    }
};

TEST_F(ClusterChunksResizePolicyTest, ThePolicyCanBeDestroyedWhileActive) {
    ASSERT_FALSE(_clusterChunksResizePolicy.isActive());
    auto _ = _clusterChunksResizePolicy.activate(_opCtx, kDefaultMaxChunksSizeBytes);
    ASSERT_TRUE(_clusterChunksResizePolicy.isActive());
    // as _clusterChunksResizePolicy goes out of scope, no exception is expected to be raised.
}

TEST_F(ClusterChunksResizePolicyTest, ThePolicyCanBeStopped) {
    auto completionFuture = _clusterChunksResizePolicy.activate(_opCtx, kDefaultMaxChunksSizeBytes);
    ASSERT_TRUE(_clusterChunksResizePolicy.isActive());
    ASSERT_FALSE(completionFuture.isReady());

    _clusterChunksResizePolicy.stop();

    ASSERT_FALSE(_clusterChunksResizePolicy.isActive());
    ASSERT_TRUE(completionFuture.isReady());
}

TEST_F(ClusterChunksResizePolicyTest, ResizeAClusterWithNoChunksEndsImmediately) {
    auto completionFuture = _clusterChunksResizePolicy.activate(_opCtx, kDefaultMaxChunksSizeBytes);
    ASSERT_TRUE(_clusterChunksResizePolicy.isActive());
    ASSERT_FALSE(completionFuture.isReady());

    // The policy requires at least one call to getNextStreamingAction() for its state to be
    // evaluated/updated.
    auto nextAction = _clusterChunksResizePolicy.getNextStreamingAction(_opCtx);

    ASSERT_FALSE(nextAction.has_value());
    ASSERT_TRUE(completionFuture.isReady());
    ASSERT_FALSE(_clusterChunksResizePolicy.isActive());
}

TEST_F(ClusterChunksResizePolicyTest,
       ThePolicyFirstGeneratesASplitVectorActionForAnUnprocessedChunk) {
    auto collectionChunk =
        ChunkType(kUuid, ChunkRange(kKeyAtMin, kKeyAtMax), kCollectionVersion, kShardId0);
    std::vector<ChunkType> collectionChunks{collectionChunk};
    auto coll = buildShardedCollection(kNss, kShardKeyPattern, collectionChunks);

    auto completionFuture = _clusterChunksResizePolicy.activate(_opCtx, kDefaultMaxChunksSizeBytes);
    ASSERT_TRUE(_clusterChunksResizePolicy.isActive());
    ASSERT_FALSE(completionFuture.isReady());

    auto nextAction = _clusterChunksResizePolicy.getNextStreamingAction(_opCtx);
    ASSERT_TRUE(_clusterChunksResizePolicy.isActive());
    ASSERT_FALSE(completionFuture.isReady());

    // the action holds the expected type and content
    auto autoSplitVectorAction = stdx::get<AutoSplitVectorInfo>(*nextAction);
    ASSERT_BSONOBJ_EQ(autoSplitVectorAction.keyPattern, kShardKeyPattern.toBSON());
    ASSERT_BSONOBJ_EQ(autoSplitVectorAction.minKey, collectionChunk.getMin());
    ASSERT_BSONOBJ_EQ(autoSplitVectorAction.maxKey, collectionChunk.getMax());
    ASSERT_EQ(autoSplitVectorAction.nss, kNss);
    ASSERT_EQ(autoSplitVectorAction.shardId, kShardId0);
    ASSERT_EQ(autoSplitVectorAction.uuid, kUuid);
    ASSERT_EQ(autoSplitVectorAction.collectionVersion, kCollectionVersion);
    ASSERT_EQ(autoSplitVectorAction.maxChunkSizeBytes, kDefaultMaxChunksSizeBytes);
}

TEST_F(ClusterChunksResizePolicyTest,
       ThePolicyGeneratesASplitChunkActionAfterReceivingACompleteSplitVector) {
    auto collectionChunk =
        ChunkType(kUuid, ChunkRange(kKeyAtMin, kKeyAtMax), kCollectionVersion, kShardId0);
    std::vector<ChunkType> collectionChunks{collectionChunk};
    auto coll = buildShardedCollection(kNss, kShardKeyPattern, collectionChunks);

    auto completionFuture = _clusterChunksResizePolicy.activate(_opCtx, kDefaultMaxChunksSizeBytes);
    auto nextAction = _clusterChunksResizePolicy.getNextStreamingAction(_opCtx);
    auto autoSplitVectorAction = stdx::get<AutoSplitVectorInfo>(*nextAction);

    std::vector<BSONObj> splitPoints{kKeyAtZero};
    DefragmentationActionResponse splitVectorResult = AutoSplitVectorResponse(splitPoints, false);

    _clusterChunksResizePolicy.applyActionResult(_opCtx, *nextAction, splitVectorResult);

    nextAction = _clusterChunksResizePolicy.getNextStreamingAction(_opCtx);

    // the action holds the expected type and content
    auto splitChunkAction = stdx::get<SplitInfoWithKeyPattern>(*nextAction);
    ASSERT_BSONOBJ_EQ(splitChunkAction.keyPattern, kShardKeyPattern.toBSON());
    ASSERT_EQ(splitChunkAction.uuid, kUuid);
    ASSERT_BSONOBJ_EQ(splitChunkAction.info.minKey, collectionChunk.getMin());
    ASSERT_BSONOBJ_EQ(splitChunkAction.info.maxKey, collectionChunk.getMax());
    ASSERT_EQ(splitChunkAction.info.nss, kNss);
    ASSERT_EQ(splitChunkAction.info.shardId, kShardId0);
    ASSERT_EQ(splitChunkAction.info.collectionVersion, kCollectionVersion);
    ASSERT_EQ(splitChunkAction.info.splitKeys.size(), 1);
    ASSERT_BSONOBJ_EQ(splitChunkAction.info.splitKeys.front(), kKeyAtZero);
}

TEST_F(ClusterChunksResizePolicyTest,
       ThePolicyGeneratesASplitVectorActionAfterReceivingAnIncompleteSplitVector) {
    auto collectionChunk =
        ChunkType(kUuid, ChunkRange(kKeyAtMin, kKeyAtMax), kCollectionVersion, kShardId0);
    std::vector<ChunkType> collectionChunks{collectionChunk};
    auto coll = buildShardedCollection(kNss, kShardKeyPattern, collectionChunks);

    auto completionFuture = _clusterChunksResizePolicy.activate(_opCtx, kDefaultMaxChunksSizeBytes);
    auto nextAction = _clusterChunksResizePolicy.getNextStreamingAction(_opCtx);
    auto autoSplitVectorAction = stdx::get<AutoSplitVectorInfo>(*nextAction);

    std::vector<BSONObj> splitPoints{kKeyAtZero};
    AutoSplitVectorResponse splitVectorResult(splitPoints, true);
    _clusterChunksResizePolicy.applyActionResult(_opCtx, *nextAction, splitVectorResult);

    nextAction = _clusterChunksResizePolicy.getNextStreamingAction(_opCtx);

    // the action holds the expected type and content
    autoSplitVectorAction = stdx::get<AutoSplitVectorInfo>(*nextAction);
    ASSERT_BSONOBJ_EQ(autoSplitVectorAction.keyPattern, kShardKeyPattern.toBSON());
    ASSERT_BSONOBJ_EQ(autoSplitVectorAction.minKey, splitPoints.front());
    ASSERT_BSONOBJ_EQ(autoSplitVectorAction.maxKey, collectionChunk.getMax());
    ASSERT_EQ(autoSplitVectorAction.nss, kNss);
    ASSERT_EQ(autoSplitVectorAction.shardId, kShardId0);
    ASSERT_EQ(autoSplitVectorAction.uuid, kUuid);
    ASSERT_EQ(autoSplitVectorAction.collectionVersion, kCollectionVersion);
    ASSERT_EQ(autoSplitVectorAction.maxChunkSizeBytes, kDefaultMaxChunksSizeBytes);
}

TEST_F(ClusterChunksResizePolicyTest, ThePolicyGeneratesNoActionAfterReceivingAnEmptySplitVector) {
    auto collectionChunk =
        ChunkType(kUuid, ChunkRange(kKeyAtMin, kKeyAtMax), kCollectionVersion, kShardId0);
    std::vector<ChunkType> collectionChunks{collectionChunk};
    auto coll = buildShardedCollection(kNss, kShardKeyPattern, collectionChunks);

    auto completionFuture = _clusterChunksResizePolicy.activate(_opCtx, kDefaultMaxChunksSizeBytes);
    auto nextAction = _clusterChunksResizePolicy.getNextStreamingAction(_opCtx);
    auto autoSplitVectorAction = stdx::get<AutoSplitVectorInfo>(*nextAction);

    std::vector<BSONObj> splitPoints{};
    AutoSplitVectorResponse splitVectorResult(splitPoints, false);
    _clusterChunksResizePolicy.applyActionResult(_opCtx, *nextAction, splitVectorResult);

    nextAction = _clusterChunksResizePolicy.getNextStreamingAction(_opCtx);

    ASSERT_FALSE(nextAction.has_value());
    // The process of the chunk is completed; being the only entry in config.chunks, the process of
    // the whole cluster should also be complete
    ASSERT_TRUE(completionFuture.isReady());
    ASSERT_FALSE(_clusterChunksResizePolicy.isActive());
}

TEST_F(ClusterChunksResizePolicyTest,
       ThePolicyReissuesSplitVectorWhenAcknowledgedWithARetriableError) {
    auto collectionChunk =
        ChunkType(kUuid, ChunkRange(kKeyAtMin, kKeyAtMax), kCollectionVersion, kShardId0);
    std::vector<ChunkType> collectionChunks{collectionChunk};
    auto coll = buildShardedCollection(kNss, kShardKeyPattern, collectionChunks);

    auto completionFuture = _clusterChunksResizePolicy.activate(_opCtx, kDefaultMaxChunksSizeBytes);
    auto nextAction = _clusterChunksResizePolicy.getNextStreamingAction(_opCtx);
    auto originalAction = stdx::get<AutoSplitVectorInfo>(*nextAction);
    _clusterChunksResizePolicy.applyActionResult(
        _opCtx,
        *nextAction,
        StatusWith<AutoSplitVectorResponse>(ErrorCodes::NetworkTimeout, "Testing error response"));

    nextAction = _clusterChunksResizePolicy.getNextStreamingAction(_opCtx);

    // The original and replayed actions are expected to match in type and content.
    auto reissuedAction = stdx::get<AutoSplitVectorInfo>(*nextAction);
    ASSERT_BSONOBJ_EQ(originalAction.keyPattern, reissuedAction.keyPattern);
    ASSERT_BSONOBJ_EQ(originalAction.minKey, reissuedAction.minKey);
    ASSERT_BSONOBJ_EQ(originalAction.maxKey, reissuedAction.maxKey);
    ASSERT_EQ(originalAction.nss, reissuedAction.nss);
    ASSERT_EQ(originalAction.shardId, reissuedAction.shardId);
    ASSERT_EQ(originalAction.uuid, reissuedAction.uuid);
    ASSERT_EQ(originalAction.collectionVersion, reissuedAction.collectionVersion);
    ASSERT_EQ(originalAction.maxChunkSizeBytes, reissuedAction.maxChunkSizeBytes);
}

TEST_F(ClusterChunksResizePolicyTest,
       ThePolicyReissuesSplitChunkWhenAcknowledgedWithARetriableError) {
    auto collectionChunk =
        ChunkType(kUuid, ChunkRange(kKeyAtMin, kKeyAtMax), kCollectionVersion, kShardId0);
    std::vector<ChunkType> collectionChunks{collectionChunk};
    auto coll = buildShardedCollection(kNss, kShardKeyPattern, collectionChunks);

    auto completionFuture = _clusterChunksResizePolicy.activate(_opCtx, kDefaultMaxChunksSizeBytes);
    auto nextAction = _clusterChunksResizePolicy.getNextStreamingAction(_opCtx);

    std::vector<BSONObj> splitPoints{kKeyAtZero};
    DefragmentationActionResponse splitVectorResult = AutoSplitVectorResponse(splitPoints, false);

    _clusterChunksResizePolicy.applyActionResult(_opCtx, *nextAction, splitVectorResult);

    nextAction = _clusterChunksResizePolicy.getNextStreamingAction(_opCtx);
    auto originalSplitAction = stdx::get<SplitInfoWithKeyPattern>(*nextAction);
    _clusterChunksResizePolicy.applyActionResult(
        _opCtx, *nextAction, Status(ErrorCodes::NetworkTimeout, "Testing error response"));

    nextAction = _clusterChunksResizePolicy.getNextStreamingAction(_opCtx);

    // The original and replayed actions are expected to match in type and content.
    auto reissuedSplitAction = stdx::get<SplitInfoWithKeyPattern>(*nextAction);
    ASSERT_BSONOBJ_EQ(originalSplitAction.keyPattern, reissuedSplitAction.keyPattern);
    ASSERT_EQ(originalSplitAction.uuid, reissuedSplitAction.uuid);
    ASSERT_BSONOBJ_EQ(originalSplitAction.info.minKey, reissuedSplitAction.info.minKey);
    ASSERT_BSONOBJ_EQ(originalSplitAction.info.maxKey, reissuedSplitAction.info.maxKey);
    ASSERT_EQ(originalSplitAction.info.nss, reissuedSplitAction.info.nss);
    ASSERT_EQ(originalSplitAction.info.shardId, reissuedSplitAction.info.shardId);
    ASSERT_EQ(originalSplitAction.info.collectionVersion,
              reissuedSplitAction.info.collectionVersion);
    ASSERT_EQ(originalSplitAction.info.splitKeys.size(), reissuedSplitAction.info.splitKeys.size());
    ASSERT_EQ(originalSplitAction.info.splitKeys.size(), 1);
    ASSERT_BSONOBJ_EQ(originalSplitAction.info.splitKeys.front(),
                      reissuedSplitAction.info.splitKeys.front());
}

TEST_F(ClusterChunksResizePolicyTest,
       ThePolicyRestartsTheCollectionProcessingWhenANonRetriableErrorIsReceived) {
    auto collectionChunk =
        ChunkType(kUuid, ChunkRange(kKeyAtMin, kKeyAtMax), kCollectionVersion, kShardId0);
    std::vector<ChunkType> collectionChunks{collectionChunk};
    auto coll = buildShardedCollection(kNss, kShardKeyPattern, collectionChunks);

    auto completionFuture = _clusterChunksResizePolicy.activate(_opCtx, kDefaultMaxChunksSizeBytes);
    auto nextAction = _clusterChunksResizePolicy.getNextStreamingAction(_opCtx);
    auto originalSplitVectorAction = stdx::get<AutoSplitVectorInfo>(*nextAction);

    std::vector<BSONObj> splitPoints{kKeyAtZero};
    DefragmentationActionResponse splitVectorResult = AutoSplitVectorResponse(splitPoints, false);

    _clusterChunksResizePolicy.applyActionResult(_opCtx, *nextAction, splitVectorResult);

    nextAction = _clusterChunksResizePolicy.getNextStreamingAction(_opCtx);
    _clusterChunksResizePolicy.applyActionResult(
        _opCtx, *nextAction, Status(ErrorCodes::OperationFailed, "Testing nonRetriable error"));

    nextAction = _clusterChunksResizePolicy.getNextStreamingAction(_opCtx);
    ASSERT_TRUE(nextAction.has_value());

    auto reissuedSplitVectorAction = stdx::get<AutoSplitVectorInfo>(*nextAction);
    ASSERT_BSONOBJ_EQ(originalSplitVectorAction.keyPattern, reissuedSplitVectorAction.keyPattern);
    ASSERT_BSONOBJ_EQ(originalSplitVectorAction.minKey, reissuedSplitVectorAction.minKey);
    ASSERT_BSONOBJ_EQ(originalSplitVectorAction.maxKey, reissuedSplitVectorAction.maxKey);
    ASSERT_EQ(originalSplitVectorAction.nss, reissuedSplitVectorAction.nss);
    ASSERT_EQ(originalSplitVectorAction.shardId, reissuedSplitVectorAction.shardId);
    ASSERT_EQ(originalSplitVectorAction.uuid, reissuedSplitVectorAction.uuid);
    ASSERT_EQ(originalSplitVectorAction.collectionVersion,
              reissuedSplitVectorAction.collectionVersion);
    ASSERT_EQ(originalSplitVectorAction.maxChunkSizeBytes,
              reissuedSplitVectorAction.maxChunkSizeBytes);
}

TEST_F(ClusterChunksResizePolicyTest, ThePolicyCompletesWhenAllActionsAreAcknowledged) {
    std::vector<ChunkType> collectionChunks{
        ChunkType(kUuid, ChunkRange(kKeyAtMin, kKeyAtTen), kCollectionVersion, kShardId0),
        ChunkType(kUuid, ChunkRange(kKeyAtTen, kKeyAtMax), kCollectionVersion, kShardId1)};
    auto coll = buildShardedCollection(kNss, kShardKeyPattern, collectionChunks);

    auto completionFuture = _clusterChunksResizePolicy.activate(_opCtx, kDefaultMaxChunksSizeBytes);
    ASSERT_TRUE(_clusterChunksResizePolicy.isActive());
    ASSERT_FALSE(completionFuture.isReady());

    // Multiple SplitVector actions may be extracted in parallel
    auto splitVectorForChunk1 = _clusterChunksResizePolicy.getNextStreamingAction(_opCtx);
    ASSERT_TRUE(stdx::holds_alternative<AutoSplitVectorInfo>(*splitVectorForChunk1));
    auto splitVectorForChunk2 = _clusterChunksResizePolicy.getNextStreamingAction(_opCtx);
    ASSERT_TRUE(stdx::holds_alternative<AutoSplitVectorInfo>(*splitVectorForChunk2));

    // As long as they are incomplete, no progress may be made
    auto noAction = _clusterChunksResizePolicy.getNextStreamingAction(_opCtx);
    ASSERT_TRUE(_clusterChunksResizePolicy.isActive());
    ASSERT_FALSE(completionFuture.isReady());
    ASSERT_FALSE(noAction.has_value());

    // As splitVectors are acknowledged, splitChunk Actions are generated
    StatusWith<AutoSplitVectorResponse> splitVectorResult1 =
        AutoSplitVectorResponse({kKeyAtZero}, false);
    _clusterChunksResizePolicy.applyActionResult(_opCtx, *splitVectorForChunk1, splitVectorResult1);
    auto splitChunkForChunk1 = _clusterChunksResizePolicy.getNextStreamingAction(_opCtx);
    ASSERT_TRUE(stdx::holds_alternative<SplitInfoWithKeyPattern>(*splitChunkForChunk1));

    StatusWith<AutoSplitVectorResponse> splitVectorResult2 =
        AutoSplitVectorResponse({kKeyAtForty}, false);
    _clusterChunksResizePolicy.applyActionResult(_opCtx, *splitVectorForChunk2, splitVectorResult2);
    auto splitChunkForChunk2 = _clusterChunksResizePolicy.getNextStreamingAction(_opCtx);
    ASSERT_TRUE(stdx::holds_alternative<SplitInfoWithKeyPattern>(*splitChunkForChunk2));

    // Acknowledging the remaining actions (and pulling one more) completes the process
    ASSERT_TRUE(_clusterChunksResizePolicy.isActive());
    ASSERT_FALSE(completionFuture.isReady());
    DBDirectClient dbClient(_opCtx);
    const auto fullyProcessesCollectionsQuery =
        BSON(CollectionType::kChunksAlreadySplitForDowngradeFieldName << true);
    auto numFullyProcessedCollections =
        dbClient.count(CollectionType::ConfigNS, fullyProcessesCollectionsQuery);
    ASSERT_EQ(0, numFullyProcessedCollections);

    // After the acknowledges, the collection should be marked as fully processed on disk
    _clusterChunksResizePolicy.applyActionResult(_opCtx, *splitChunkForChunk1, Status::OK());
    _clusterChunksResizePolicy.applyActionResult(_opCtx, *splitChunkForChunk2, Status::OK());
    numFullyProcessedCollections =
        dbClient.count(CollectionType::ConfigNS, fullyProcessesCollectionsQuery);
    ASSERT_EQ(1, numFullyProcessedCollections);

    auto nextAction = _clusterChunksResizePolicy.getNextStreamingAction(_opCtx);
    ASSERT_FALSE(nextAction.has_value());
    ASSERT_FALSE(_clusterChunksResizePolicy.isActive());
    ASSERT_TRUE(completionFuture.isReady());

    // After the completion of the whole process, the temporary mark should have been removed from
    // disk
    numFullyProcessedCollections =
        dbClient.count(CollectionType::ConfigNS, fullyProcessesCollectionsQuery);
    ASSERT_EQ(0, numFullyProcessedCollections);
}

TEST_F(ClusterChunksResizePolicyTest, CollectionsMarkedAsAlreadyProcessedGetIgnored) {
    std::vector<ChunkType> collectionChunks{
        ChunkType(kUuid, ChunkRange(kKeyAtMin, kKeyAtTen), kCollectionVersion, kShardId0),
        ChunkType(kUuid, ChunkRange(kKeyAtTen, kKeyAtMax), kCollectionVersion, kShardId1)};
    buildShardedCollection(kNss, kShardKeyPattern, collectionChunks, true);

    auto completionFuture = _clusterChunksResizePolicy.activate(_opCtx, kDefaultMaxChunksSizeBytes);
    ASSERT_TRUE(_clusterChunksResizePolicy.isActive());
    ASSERT_FALSE(completionFuture.isReady());
    auto nextAction = _clusterChunksResizePolicy.getNextStreamingAction(_opCtx);

    ASSERT_FALSE(nextAction.has_value());
    ASSERT_TRUE(completionFuture.isReady());
    ASSERT_FALSE(_clusterChunksResizePolicy.isActive());
}

}  // namespace
}  // namespace mongo
