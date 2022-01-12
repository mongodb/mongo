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
    const int64_t kPhase3DefaultChunkSize =
        129 * 1024 * 1024;  // > 128MB should trigger AutoSplitVector

    const std::vector<ShardType> kShardList{
        ShardType(kShardId0.toString(), kShardHost0.toString()),
        ShardType(kShardId1.toString(), kShardHost1.toString())};

    BalancerDefragmentationPolicyTest()
        : _random(std::random_device{}()),
          _clusterStats(std::make_unique<ClusterStatisticsImpl>(_random)),
          _defragmentationPolicy(_clusterStats.get()) {}

    CollectionType makeConfigCollectionEntry(
        boost::optional<DefragmentationPhaseEnum> phase = boost::none,
        boost::optional<int64_t> maxChunkSizeBytes = boost::none) {
        CollectionType shardedCollection(kNss, OID::gen(), Timestamp(1, 1), Date_t::now(), kUuid);
        shardedCollection.setKeyPattern(kShardKeyPattern);
        shardedCollection.setBalancerShouldMergeChunks(true);
        shardedCollection.setDefragmentationPhase(phase);
        if (maxChunkSizeBytes) {
            shardedCollection.setMaxChunkSizeBytes(maxChunkSizeBytes.get());
        }
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
        FindCommandRequest findRequest{NamespaceStringOrUUID{CollectionType::ConfigNS}};
        findRequest.setFilter(BSON(CollectionType::kUuidFieldName << kUuid));
        auto cursor = client.find(std::move(findRequest));
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

TEST_F(BalancerDefragmentationPolicyTest, TestAddCollectionWhenCollectionRemovedFailsGracefully) {
    CollectionType coll(kNss, OID::gen(), Timestamp(1, 1), Date_t::now(), kUuid);
    coll.setKeyPattern(kShardKeyPattern);
    coll.setBalancerShouldMergeChunks(true);
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

TEST_F(BalancerDefragmentationPolicyTest, TestRetriableFailedDataSizeActionGetsReissued) {
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
       TestNonRetriableErrorEndsDefragmentationButLeavesPersistedFields) {
    auto coll = makeConfigCollectionEntry();
    makeConfigChunkEntry();
    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);
    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    DataSizeInfo failingDataSizeAction = stdx::get<DataSizeInfo>(future.get());

    _defragmentationPolicy.acknowledgeDataSizeResult(
        operationContext(),
        failingDataSizeAction,
        Status(ErrorCodes::NamespaceNotFound, "Testing error response"));

    // Defragmentation should have stopped on the collection
    ASSERT_FALSE(_defragmentationPolicy.isDefragmentingCollection(coll.getUuid()));
    // There should be no new actions.
    future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_FALSE(future.isReady());
    // The defragmentation flags should still be present
    auto configDoc = findOneOnConfigCollection(operationContext(),
                                               CollectionType::ConfigNS,
                                               BSON(CollectionType::kUuidFieldName << kUuid))
                         .getValue();
    ASSERT_TRUE(configDoc.getBoolField(CollectionType::kBalancerShouldMergeChunksFieldName));
    auto storedDefragmentationPhase = DefragmentationPhase_parse(
        IDLParserErrorContext("BalancerDefragmentationPolicyTest"),
        configDoc.getStringField(CollectionType::kDefragmentationPhaseFieldName));
    ASSERT_TRUE(storedDefragmentationPhase == DefragmentationPhaseEnum::kMergeChunks);
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

/** Phase 3 tests. By passing in DefragmentationPhaseEnum::kSplitChunks to
 * makeConfigCollectionEntry, the persisted collection entry will have
 * kDefragmentationPhaseFieldName set to kSplitChunks and defragmentation will be started with
 * phase 3.
 */

TEST_F(BalancerDefragmentationPolicyTest, DefragmentationBeginsWithPhase3FromPersistedSetting) {
    auto coll = makeConfigCollectionEntry(DefragmentationPhaseEnum::kSplitChunks);
    makeConfigChunkEntry(kPhase3DefaultChunkSize);
    // Defragmentation does not start until refreshCollectionDefragmentationStatus is called
    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_FALSE(future.isReady());
    ASSERT_FALSE(_defragmentationPolicy.isDefragmentingCollection(coll.getUuid()));

    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);

    ASSERT_TRUE(_defragmentationPolicy.isDefragmentingCollection(coll.getUuid()));
    auto configDoc = findOneOnConfigCollection(operationContext(),
                                               CollectionType::ConfigNS,
                                               BSON(CollectionType::kUuidFieldName << kUuid))
                         .getValue();
    auto storedDefragmentationPhase = DefragmentationPhase_parse(
        IDLParserErrorContext("BalancerDefragmentationPolicyTest"),
        configDoc.getStringField(CollectionType::kDefragmentationPhaseFieldName));
    ASSERT_TRUE(storedDefragmentationPhase == DefragmentationPhaseEnum::kSplitChunks);
}

TEST_F(BalancerDefragmentationPolicyTest, SingleLargeChunkCausesAutoSplitAndSplitActions) {
    auto coll = makeConfigCollectionEntry(DefragmentationPhaseEnum::kSplitChunks);
    makeConfigChunkEntry(kPhase3DefaultChunkSize);
    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());

    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);

    // The action returned by the stream should be now an actionable AutoSplitVector command...
    ASSERT_TRUE(future.isReady());
    AutoSplitVectorInfo splitVectorAction = stdx::get<AutoSplitVectorInfo>(future.get());
    // with the expected content
    ASSERT_EQ(coll.getNss(), splitVectorAction.nss);
    ASSERT_BSONOBJ_EQ(kKeyAtZero, splitVectorAction.minKey);
    ASSERT_BSONOBJ_EQ(kKeyAtTen, splitVectorAction.maxKey);
}

TEST_F(BalancerDefragmentationPolicyTest, CollectionMaxChunkSizeIsUsedForPhase3) {
    auto coll = makeConfigCollectionEntry(DefragmentationPhaseEnum::kSplitChunks, 1024);
    makeConfigChunkEntry(2 * 1024);  // > 1KB should trigger AutoSplitVector

    _defragmentationPolicy.refreshCollectionDefragmentationStatus(operationContext(), coll);

    auto future = _defragmentationPolicy.getNextStreamingAction(operationContext());

    // The action returned by the stream should be now an actionable AutoSplitVector command...
    ASSERT_TRUE(future.isReady());
    AutoSplitVectorInfo splitVectorAction = stdx::get<AutoSplitVectorInfo>(future.get());
    // with the expected content
    ASSERT_EQ(coll.getNss(), splitVectorAction.nss);
    ASSERT_BSONOBJ_EQ(kKeyAtZero, splitVectorAction.minKey);
    ASSERT_BSONOBJ_EQ(kKeyAtTen, splitVectorAction.maxKey);
}

TEST_F(BalancerDefragmentationPolicyTest, TestRetryableFailedAutoSplitActionGetsReissued) {
    auto coll = makeConfigCollectionEntry(DefragmentationPhaseEnum::kSplitChunks);
    makeConfigChunkEntry(kPhase3DefaultChunkSize);
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
    auto coll = makeConfigCollectionEntry(DefragmentationPhaseEnum::kSplitChunks);
    makeConfigChunkEntry(kPhase3DefaultChunkSize);
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
    auto coll = makeConfigCollectionEntry(DefragmentationPhaseEnum::kSplitChunks);
    makeConfigChunkEntry(kPhase3DefaultChunkSize);
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
    auto coll = makeConfigCollectionEntry(DefragmentationPhaseEnum::kSplitChunks);
    makeConfigChunkEntry(kPhase3DefaultChunkSize);
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
    auto coll = makeConfigCollectionEntry(DefragmentationPhaseEnum::kSplitChunks);
    makeConfigChunkEntry(kPhase3DefaultChunkSize);
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
    auto coll = makeConfigCollectionEntry(DefragmentationPhaseEnum::kSplitChunks);
    makeConfigChunkEntry(kPhase3DefaultChunkSize);
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
    auto configDoc = findOneOnConfigCollection(operationContext(),
                                               CollectionType::ConfigNS,
                                               BSON(CollectionType::kUuidFieldName << kUuid))
                         .getValue();
    ASSERT_FALSE(configDoc.hasField(CollectionType::kDefragmentationPhaseFieldName));
}

}  // namespace
}  // namespace mongo
