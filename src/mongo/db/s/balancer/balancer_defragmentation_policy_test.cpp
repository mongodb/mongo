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
    const ShardId kShardId = ShardId("testShard");
    const ChunkVersion kCollectionVersion = ChunkVersion(1, 1, OID::gen(), Timestamp(10));
    const KeyPattern kShardKeyPattern = KeyPattern(BSON("x" << 1));
    const BSONObj kMinKey = BSON("x" << 0);
    const BSONObj kMaxKey = BSON("x" << 10);
    const long long kMaxChunkSizeBytes{2048};

    BalancerDefragmentationPolicyTest()
        : _random(std::random_device{}()),
          _clusterStats(std::make_unique<ClusterStatisticsImpl>(_random)),
          _defragmentationPolicy(_clusterStats.get()) {}

    void makeConfigCollectionEntry() {
        CollectionType shardedCollection(kNss, OID::gen(), Timestamp(1, 1), Date_t::now(), kUuid);
        shardedCollection.setKeyPattern(kShardKeyPattern);
        ASSERT_OK(insertToConfigCollection(
            operationContext(), CollectionType::ConfigNS, shardedCollection.toBSON()));
    }

    void makeConfigChunkEntry() {
        ChunkType chunk(kUuid, ChunkRange(kMinKey, kMaxKey), kCollectionVersion, kShardId);
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
    makeConfigCollectionEntry();
    _defragmentationPolicy.beginNewCollection(operationContext(), kUuid);
    // Test for persistence
    auto configDoc = findOneOnConfigCollection(operationContext(),
                                               CollectionType::ConfigNS,
                                               BSON(CollectionType::kUuidFieldName << kUuid))
                         .getValue();
    auto storedDefragmentationPhase = DefragmentationPhase_parse(
        IDLParserErrorContext("BalancerDefragmentationPolicyTest"),
        configDoc.getStringField(CollectionType::kDefragmentationPhaseFieldName));
    ASSERT_TRUE(storedDefragmentationPhase == DefragmentationPhaseEnum::kNotStarted);
}

TEST_F(BalancerDefragmentationPolicyTest, TestRemoveCollection) {
    makeConfigCollectionEntry();
    _defragmentationPolicy.beginNewCollection(operationContext(), kUuid);
    _defragmentationPolicy.removeCollection(operationContext(), kUuid);
    // Test for persistence removal
    auto configDoc = findOneOnConfigCollection(operationContext(),
                                               CollectionType::ConfigNS,
                                               BSON(CollectionType::kUuidFieldName << kUuid))
                         .getValue();
    ASSERT_FALSE(configDoc.hasField(CollectionType::kDefragmentationPhaseFieldName));
}

TEST_F(BalancerDefragmentationPolicyTest, TestIsDefragmentingCollection) {
    makeConfigCollectionEntry();
    _defragmentationPolicy.beginNewCollection(operationContext(), kUuid);
    ASSERT_TRUE(_defragmentationPolicy.isDefragmentingCollection(kUuid));
    ASSERT_FALSE(_defragmentationPolicy.isDefragmentingCollection(UUID::gen()));
}

TEST_F(BalancerDefragmentationPolicyTest, TestGetNextActionNoReadyActions) {
    auto future = _defragmentationPolicy.getNextStreamingAction();
    ASSERT_FALSE(future.isReady());
}

TEST_F(BalancerDefragmentationPolicyTest, TestAcknowledgeFailedMergeResult) {
    makeConfigCollectionEntry();
    _defragmentationPolicy.beginNewCollection(operationContext(), kUuid);
    auto future = _defragmentationPolicy.getNextStreamingAction();
    auto mergeInfo =
        MergeInfo(kShardId, kNss, kUuid, kCollectionVersion, ChunkRange(kMinKey, kMaxKey));
    _defragmentationPolicy.acknowledgeMergeResult(
        mergeInfo, Status(ErrorCodes::NetworkTimeout, "Testing error response"));
    ASSERT_TRUE(future.isReady());
    DefragmentationAction streamingAction = future.get();
    MergeInfo mergeAction = stdx::get<MergeInfo>(streamingAction);
    ASSERT_EQ(mergeAction.nss, mergeInfo.nss);
}

TEST_F(BalancerDefragmentationPolicyTest, TestAcknowledgeFailedSplitVectorResponse) {
    makeConfigCollectionEntry();
    _defragmentationPolicy.beginNewCollection(operationContext(), kUuid);
    auto future = _defragmentationPolicy.getNextStreamingAction();
    auto splitVectorInfo = AutoSplitVectorInfo(
        kShardId, kNss, kUuid, kCollectionVersion, BSONObj(), kMinKey, kMaxKey, 120);
    _defragmentationPolicy.acknowledgeAutoSplitVectorResult(
        splitVectorInfo, Status(ErrorCodes::NetworkTimeout, "Testing error response"));
    ASSERT_TRUE(future.isReady());
    AutoSplitVectorInfo splitVectorAction = stdx::get<AutoSplitVectorInfo>(future.get());
    ASSERT_EQ(splitVectorInfo.nss, splitVectorAction.nss);
}

TEST_F(BalancerDefragmentationPolicyTest, TestAcknowledgeFailedSplitAction) {
    makeConfigCollectionEntry();
    _defragmentationPolicy.beginNewCollection(operationContext(), kUuid);
    auto future = _defragmentationPolicy.getNextStreamingAction();
    auto splitInfo = SplitInfoWithKeyPattern(
        kShardId, kNss, kCollectionVersion, kMinKey, kMaxKey, {}, kUuid, kShardKeyPattern.toBSON());
    _defragmentationPolicy.acknowledgeSplitResult(
        splitInfo, Status(ErrorCodes::NetworkTimeout, "Testing error response"));
    ASSERT_TRUE(future.isReady());
    SplitInfoWithKeyPattern splitAction = stdx::get<SplitInfoWithKeyPattern>(future.get());
    ASSERT_EQ(splitInfo.info.nss, splitAction.info.nss);
}

TEST_F(BalancerDefragmentationPolicyTest, TestAcknowledgeFailedDataSizeAction) {
    makeConfigCollectionEntry();
    _defragmentationPolicy.beginNewCollection(operationContext(), kUuid);
    auto future = _defragmentationPolicy.getNextStreamingAction();
    auto dataSizeInfo = DataSizeInfo(kShardId,
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
    makeConfigCollectionEntry();
    _defragmentationPolicy.beginNewCollection(operationContext(), kUuid);
    auto future = _defragmentationPolicy.getNextStreamingAction();
    auto mergeInfo =
        MergeInfo(kShardId, kNss, kUuid, kCollectionVersion, ChunkRange(kMinKey, kMaxKey));
    _defragmentationPolicy.acknowledgeMergeResult(mergeInfo, Status::OK());
    ASSERT_TRUE(future.isReady());
    DataSizeInfo dataSizeAction = stdx::get<DataSizeInfo>(future.get());
    ASSERT_EQ(mergeInfo.nss, dataSizeAction.nss);
    ASSERT_BSONOBJ_EQ(mergeInfo.chunkRange.getMin(), dataSizeAction.chunkRange.getMin());
    ASSERT_BSONOBJ_EQ(mergeInfo.chunkRange.getMax(), dataSizeAction.chunkRange.getMax());
}

TEST_F(BalancerDefragmentationPolicyTest, TestAcknowledgeSuccessfulAutoSplitVectorAction) {
    std::vector<BSONObj> splitPoints = {BSON("x" << 4)};
    makeConfigCollectionEntry();
    _defragmentationPolicy.beginNewCollection(operationContext(), kUuid);
    auto future = _defragmentationPolicy.getNextStreamingAction();
    auto splitVectorInfo = AutoSplitVectorInfo(kShardId,
                                               kNss,
                                               kUuid,
                                               kCollectionVersion,
                                               kShardKeyPattern.toBSON(),
                                               kMinKey,
                                               kMaxKey,
                                               2048);
    _defragmentationPolicy.acknowledgeAutoSplitVectorResult(splitVectorInfo,
                                                            StatusWith(splitPoints));
    ASSERT_TRUE(future.isReady());
    SplitInfoWithKeyPattern splitAction = stdx::get<SplitInfoWithKeyPattern>(future.get());
    ASSERT_EQ(splitVectorInfo.nss, splitAction.info.nss);
    ASSERT_EQ(splitAction.info.splitKeys.size(), 1);
    ASSERT_BSONOBJ_EQ(splitAction.info.splitKeys.at(0), splitPoints.at(0));
}

TEST_F(BalancerDefragmentationPolicyTest, TestAcknowledgeSuccessfulSplitAction) {
    std::vector<BSONObj> splitPoints = {BSON("x" << 4)};
    makeConfigCollectionEntry();
    _defragmentationPolicy.beginNewCollection(operationContext(), kUuid);
    auto future = _defragmentationPolicy.getNextStreamingAction();
    auto splitInfo = SplitInfoWithKeyPattern(kShardId,
                                             kNss,
                                             kCollectionVersion,
                                             kMinKey,
                                             kMaxKey,
                                             splitPoints,
                                             kUuid,
                                             kShardKeyPattern.toBSON());
    _defragmentationPolicy.acknowledgeSplitResult(splitInfo, Status::OK());
    ASSERT_FALSE(future.isReady());
}

TEST_F(BalancerDefragmentationPolicyTest, TestAcknowledgeSuccessfulDataSizeAction) {
    makeConfigCollectionEntry();
    _defragmentationPolicy.beginNewCollection(operationContext(), kUuid);
    makeConfigChunkEntry();
    auto future = _defragmentationPolicy.getNextStreamingAction();
    auto dataSizeInfo = DataSizeInfo(kShardId,
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

}  // namespace
}  // namespace mongo
