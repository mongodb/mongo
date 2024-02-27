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

#include <absl/container/node_hash_set.h>
#include <algorithm>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <utility>
#include <variant>
#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bson_field.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/s/balancer/balancer_defragmentation_policy.h"
#include "mongo/db/s/balancer/cluster_statistics_mock.h"
#include "mongo/db/s/config/config_server_test_fixture.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/grid.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

using ShardStatistics = ClusterStatistics::ShardStatistics;

class BalancerDefragmentationPolicyTest : public ConfigServerTestFixture {
protected:
    inline static const NamespaceString kNss1 =
        NamespaceString::createNamespaceString_forTest("testDb.testColl1");
    inline static const UUID kUuid1 = UUID::gen();
    inline static const NamespaceString kNss2 =
        NamespaceString::createNamespaceString_forTest("testDb.testColl2");
    inline static const UUID kUuid2 = UUID::gen();
    inline static const NamespaceString kNss3 =
        NamespaceString::createNamespaceString_forTest("testDb.testColl3");
    inline static const UUID kUuid3 = UUID::gen();

    const ShardId kShardId0 = ShardId("shard0");
    const ShardId kShardId1 = ShardId("shard1");
    const ShardId kShardId2 = ShardId("shard2");
    const ShardId kShardId3 = ShardId("shard3");
    const ChunkVersion kCollectionPlacementVersion =
        ChunkVersion({OID::gen(), Timestamp(10)}, {1, 1});
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

    const std::vector<ShardType> kShardList{
        ShardType(kShardId0.toString(), kShardHost0.toString()),
        ShardType(kShardId1.toString(), kShardHost1.toString()),
        ShardType(kShardId2.toString(), kShardHost2.toString()),
        ShardType(kShardId3.toString(), kShardHost3.toString())};

    const std::function<void()> onDefragmentationStateUpdated = [] {
    };

    BalancerDefragmentationPolicyTest()
        : _clusterStats(), _defragmentationPolicy(&_clusterStats, onDefragmentationStateUpdated) {}

    CollectionType setupCollectionWithPhase(
        const NamespaceString& nss,
        const std::vector<ChunkType>& chunkList,
        boost::optional<DefragmentationPhaseEnum> startingPhase = boost::none,
        boost::optional<int64_t> maxChunkSizeBytes = boost::none) {

        setupCollection(nss, kShardKeyPattern, chunkList);

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

        const UUID& uuid = chunkList.at(0).getCollectionUUID();
        ASSERT_OK(updateToConfigCollection(operationContext(),
                                           CollectionType::ConfigNS,
                                           BSON(CollectionType::kUuidFieldName << uuid),
                                           updateClause,
                                           false));
        return Grid::get(operationContext())
            ->catalogClient()
            ->getCollection(operationContext(), uuid);
    }

    ChunkType makeConfigChunkEntry(const UUID& uuid,
                                   const boost::optional<int64_t>& estimatedSize = boost::none) {
        ChunkType chunk(
            uuid, ChunkRange(kKeyAtMin, kKeyAtMax), kCollectionPlacementVersion, kShardId0);
        chunk.setEstimatedSizeBytes(estimatedSize);
        return chunk;
    }

    std::vector<ChunkType> makeMergeableConfigChunkEntries(const UUID& uuid) {
        return {
            ChunkType(
                uuid, ChunkRange(kKeyAtMin, kKeyAtTen), kCollectionPlacementVersion, kShardId0),
            ChunkType(
                uuid, ChunkRange(kKeyAtTen, kKeyAtMax), kCollectionPlacementVersion, kShardId0)};
    }

    BSONObj getConfigCollectionEntry(const UUID& uuid) {
        DBDirectClient client(operationContext());
        FindCommandRequest findRequest{NamespaceStringOrUUID{CollectionType::ConfigNS}};
        findRequest.setFilter(BSON(CollectionType::kUuidFieldName << uuid));
        auto cursor = client.find(std::move(findRequest));
        if (!cursor || !cursor->more())
            return BSONObj();
        else
            return cursor->next();
    }

    ClusterStatisticsMock _clusterStats;
    BalancerDefragmentationPolicy _defragmentationPolicy;

    ShardStatistics buildShardStats(ShardId id,
                                    uint64_t currentSizeBytes,
                                    bool draining = false,
                                    std::set<std::string>&& zones = {}) {
        return ShardStatistics(id, currentSizeBytes, draining, zones);
    }

    void setDefaultClusterStats(const std::vector<NamespaceString>& nssList = {kNss1}) {
        uint64_t oneKB = 1024 * 1024;
        auto shardInstance = 0;
        std::vector<ShardStatistics> stats;
        std::map<NamespaceString, std::vector<ShardStatistics>> collStats;
        for (const auto& shard : kShardList) {
            ++shardInstance;
            stats.push_back(buildShardStats(shard.getName(), oneKB * 1024 * shardInstance));

            for (const auto& nss : nssList) {
                collStats[nss].push_back(buildShardStats(shard.getName(), oneKB * shardInstance));
            }
        }
        _clusterStats.setStats(std::move(stats), std::move(collStats));
    }

    void verifyExpectedDefragmentationStateOnDisk(
        const UUID& uuid, boost::optional<DefragmentationPhaseEnum> expectedPhase) {
        auto configDoc = findOneOnConfigCollection(operationContext(),
                                                   CollectionType::ConfigNS,
                                                   BSON(CollectionType::kUuidFieldName << uuid))
                             .getValue();
        if (expectedPhase.has_value()) {
            auto storedDefragmentationPhase = DefragmentationPhase_parse(
                IDLParserContext("BalancerDefragmentationPolicyTest"),
                configDoc.getStringField(CollectionType::kDefragmentationPhaseFieldName));
            ASSERT_TRUE(storedDefragmentationPhase == *expectedPhase);
            ASSERT_TRUE(configDoc[CollectionType::kDefragmentCollectionFieldName].Bool());
        } else {
            ASSERT_FALSE(configDoc.hasField(CollectionType::kDefragmentationPhaseFieldName));
            ASSERT_FALSE(configDoc.hasField(CollectionType::kDefragmentCollectionFieldName));
        }
    };

    stdx::unordered_set<ShardId> getAllShardIds(OperationContext* opCtx) {
        std::vector<ShardStatistics> shardStats = _clusterStats.getStats(opCtx).getValue();
        stdx::unordered_set<ShardId> shards;
        std::transform(shardStats.begin(),
                       shardStats.end(),
                       std::inserter(shards, shards.end()),
                       [](const ClusterStatistics::ShardStatistics& shardStatistics) -> ShardId {
                           return shardStatistics.shardId;
                       });

        return shards;
    }
};

TEST_F(BalancerDefragmentationPolicyTest, TestGetNextActionIsNotReadyWhenNotDefragmenting) {
    auto nextAction = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_TRUE(nextAction == boost::none);
}

TEST_F(BalancerDefragmentationPolicyTest, TestAddCollectionWhenCollectionRemovedFailsGracefully) {
    CollectionType coll(
        kNss1, OID::gen(), Timestamp(1, 1), Date_t::now(), kUuid1, kShardKeyPattern);
    coll.setDefragmentCollection(true);
    // Collection entry is not persisted (to simulate collection dropped), defragmentation should
    // not begin.
    ASSERT_FALSE(_defragmentationPolicy.isDefragmentingCollection(coll.getUuid()));
    _defragmentationPolicy.startCollectionDefragmentations(operationContext());
    ASSERT_FALSE(_defragmentationPolicy.isDefragmentingCollection(coll.getUuid()));
    auto configDoc = findOneOnConfigCollection(operationContext(),
                                               CollectionType::ConfigNS,
                                               BSON(CollectionType::kUuidFieldName << kUuid1));
    ASSERT_EQ(configDoc.getStatus(), Status(ErrorCodes::NoMatchingDocument, "No document found"));
}

// Phase 1 tests.

TEST_F(BalancerDefragmentationPolicyTest, TestPhaseOneAddSingleChunkCollectionTriggersDataSize) {
    setupShards(kShardList);
    auto coll = setupCollectionWithPhase(kNss1, {makeConfigChunkEntry(kUuid1)});
    auto nextAction = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_TRUE(nextAction == boost::none);
    ASSERT_FALSE(_defragmentationPolicy.isDefragmentingCollection(coll.getUuid()));

    _defragmentationPolicy.startCollectionDefragmentations(operationContext());

    // 1. The collection should be marked as undergoing through phase 1 of the algorithm...
    ASSERT_TRUE(_defragmentationPolicy.isDefragmentingCollection(coll.getUuid()));
    verifyExpectedDefragmentationStateOnDisk(coll.getUuid(),
                                             DefragmentationPhaseEnum::kMergeAndMeasureChunks);
    // 2. The action returned by the stream should be now an actionable DataSizeCommand...
    nextAction = _defragmentationPolicy.getNextStreamingAction(operationContext());
    DataSizeInfo dataSizeAction = get<DataSizeInfo>(*nextAction);
    // 3. with the expected content
    ASSERT_EQ(coll.getNss(), dataSizeAction.nss);
    ASSERT_BSONOBJ_EQ(kKeyAtMin, dataSizeAction.chunkRange.getMin());
    ASSERT_BSONOBJ_EQ(kKeyAtMax, dataSizeAction.chunkRange.getMax());
}

TEST_F(BalancerDefragmentationPolicyTest,
       AddSingleChunkCollectionWithKnownDataSizeCompletesDefragmentationWithNoOperationIssued) {
    setupShards(kShardList);
    auto coll = setupCollectionWithPhase(kNss1, {makeConfigChunkEntry(kUuid1, 1024)});
    setDefaultClusterStats();

    _defragmentationPolicy.startCollectionDefragmentations(operationContext());

    ASSERT_TRUE(_defragmentationPolicy.isDefragmentingCollection(coll.getUuid()));

    auto nextAction = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_TRUE(nextAction == boost::none);
    verifyExpectedDefragmentationStateOnDisk(coll.getUuid(),
                                             DefragmentationPhaseEnum::kMoveAndMergeChunks);

    // kMoveAndMergeChunks has no stream actions/migrations to offer, but the condition has to be
    // verified through a sequence of two action requests (the first being selectChunksToMove()) for
    // the phase to complete.
    auto availableShards = getAllShardIds(operationContext());
    auto pendingMigrations =
        _defragmentationPolicy.selectChunksToMove(operationContext(), &availableShards);
    ASSERT_TRUE(pendingMigrations.empty());
    verifyExpectedDefragmentationStateOnDisk(coll.getUuid(),
                                             DefragmentationPhaseEnum::kMoveAndMergeChunks);

    nextAction = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_TRUE(nextAction == boost::none);
    verifyExpectedDefragmentationStateOnDisk(coll.getUuid(), boost::none);
    ASSERT_FALSE(_defragmentationPolicy.isDefragmentingCollection(coll.getUuid()));
}

TEST_F(BalancerDefragmentationPolicyTest,
       TestPhaseOneAcknowledgeFinalDataSizeActionCompletesPhase) {
    setupShards(kShardList);
    auto coll = setupCollectionWithPhase(kNss1, {makeConfigChunkEntry(kUuid1)});
    setDefaultClusterStats();
    _defragmentationPolicy.startCollectionDefragmentations(operationContext());
    auto nextAction = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_TRUE(nextAction.has_value());
    DataSizeInfo dataSizeAction = get<DataSizeInfo>(*nextAction);

    auto resp = StatusWith(DataSizeResponse(2000, 4, false));
    _defragmentationPolicy.applyActionResult(operationContext(), dataSizeAction, resp);

    // 1. The outcome of the data size has been stored in the expected document...
    auto chunkQuery = BSON(ChunkType::collectionUUID()
                           << kUuid1 << ChunkType::min(kKeyAtMin) << ChunkType::max(kKeyAtMax));
    auto configChunkDoc =
        findOneOnConfigCollection(operationContext(), ChunkType::ConfigNS, chunkQuery).getValue();
    ASSERT_EQ(configChunkDoc.getIntField(ChunkType::estimatedSizeBytes.name()), 2000);

    // 2. and the algorithm transitioned to the next phase
    nextAction = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_TRUE(nextAction == boost::none);
    ASSERT_TRUE(_defragmentationPolicy.isDefragmentingCollection(coll.getUuid()));
    verifyExpectedDefragmentationStateOnDisk(coll.getUuid(),
                                             DefragmentationPhaseEnum::kMoveAndMergeChunks);
}

TEST_F(BalancerDefragmentationPolicyTest,
       TestPhaseOneDataSizeResponsesWithMaxSizeReachedCausesChunkToBeSkippedByPhaseTwo) {
    setupShards(kShardList);
    auto coll = setupCollectionWithPhase(kNss1, {makeConfigChunkEntry(kUuid1)});
    setDefaultClusterStats();
    _defragmentationPolicy.startCollectionDefragmentations(operationContext());
    auto nextAction = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_TRUE(nextAction.has_value());
    DataSizeInfo dataSizeAction = get<DataSizeInfo>(*nextAction);

    auto resp = StatusWith(DataSizeResponse(2000, 4, true));
    _defragmentationPolicy.applyActionResult(operationContext(), dataSizeAction, resp);

    // 1. The outcome of the data size has been stored in the expected document...
    auto chunkQuery = BSON(ChunkType::collectionUUID()
                           << kUuid1 << ChunkType::min(kKeyAtMin) << ChunkType::max(kKeyAtMax));
    auto configChunkDoc =
        findOneOnConfigCollection(operationContext(), ChunkType::ConfigNS, chunkQuery).getValue();
    ASSERT_EQ(configChunkDoc.getField("estimatedDataSizeBytes").safeNumberLong(),
              std::numeric_limits<int64_t>::max());

    // No new action is expected - and the algorithm should converge
    nextAction = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_TRUE(nextAction == boost::none);
    ASSERT_FALSE(_defragmentationPolicy.isDefragmentingCollection(coll.getUuid()));
    verifyExpectedDefragmentationStateOnDisk(coll.getUuid(), boost::none);
}

TEST_F(BalancerDefragmentationPolicyTest, TestRetriableFailedDataSizeActionGetsReissued) {
    setupShards(kShardList);
    auto coll = setupCollectionWithPhase(kNss1, {makeConfigChunkEntry(kUuid1)});
    _defragmentationPolicy.startCollectionDefragmentations(operationContext());
    auto nextAction = _defragmentationPolicy.getNextStreamingAction(operationContext());
    DataSizeInfo failingDataSizeAction = get<DataSizeInfo>(*nextAction);
    StatusWith<DataSizeResponse> response(
        Status(ErrorCodes::NetworkTimeout, "Testing error response"));
    _defragmentationPolicy.applyActionResult(operationContext(), failingDataSizeAction, response);

    // Under the setup of this test, the stream should only contain one more action - which (version
    // aside) matches the failed one.
    nextAction = _defragmentationPolicy.getNextStreamingAction(operationContext());
    auto replayedDataSizeAction = get<DataSizeInfo>(*nextAction);
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

    nextAction = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_TRUE(nextAction == boost::none);
}

TEST_F(BalancerDefragmentationPolicyTest, TestRemoveCollectionEndsDefragmentation) {
    setupShards(kShardList);
    auto coll = setupCollectionWithPhase(kNss1, {makeConfigChunkEntry(kUuid1)});
    _defragmentationPolicy.startCollectionDefragmentations(operationContext());
    auto nextAction = _defragmentationPolicy.getNextStreamingAction(operationContext());
    DataSizeInfo dataSizeAction = get<DataSizeInfo>(*nextAction);

    auto resp = StatusWith(DataSizeResponse(2000, 4, false));
    _defragmentationPolicy.applyActionResult(operationContext(), dataSizeAction, resp);

    // Remove collection entry from config.collections
    ASSERT_OK(deleteToConfigCollection(
        operationContext(), CollectionType::ConfigNS, coll.toBSON(), false));

    // getCollection should fail with NamespaceNotFound and end defragmentation on the collection.
    nextAction = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_TRUE(nextAction == boost::none);
    // Defragmentation should have stopped on the collection
    ASSERT_FALSE(_defragmentationPolicy.isDefragmentingCollection(coll.getUuid()));
}

TEST_F(BalancerDefragmentationPolicyTest, TestPhaseOneUserCancellationFinishesDefragmentation) {
    setupShards(kShardList);
    auto coll = setupCollectionWithPhase(kNss1, {makeConfigChunkEntry(kUuid1)});
    _defragmentationPolicy.startCollectionDefragmentations(operationContext());

    // Collection should be in phase 1
    verifyExpectedDefragmentationStateOnDisk(coll.getUuid(),
                                             DefragmentationPhaseEnum::kMergeAndMeasureChunks);

    // User cancellation of defragmentation
    _defragmentationPolicy.abortCollectionDefragmentation(operationContext(), kNss1);

    auto nextAction = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_TRUE(nextAction == boost::none);
    ASSERT_FALSE(_defragmentationPolicy.isDefragmentingCollection(coll.getUuid()));
    verifyExpectedDefragmentationStateOnDisk(coll.getUuid(), boost::none);
}

TEST_F(BalancerDefragmentationPolicyTest, TestNonRetriableErrorRebuildsCurrentPhase) {
    setupShards(kShardList);
    auto coll = setupCollectionWithPhase(kNss1, {makeConfigChunkEntry(kUuid1)});
    _defragmentationPolicy.startCollectionDefragmentations(operationContext());
    auto nextAction = _defragmentationPolicy.getNextStreamingAction(operationContext());
    DataSizeInfo failingDataSizeAction = get<DataSizeInfo>(*nextAction);
    StatusWith<DataSizeResponse> response(
        Status(ErrorCodes::IllegalOperation, "Testing error response"));

    _defragmentationPolicy.applyActionResult(operationContext(), failingDataSizeAction, response);
    nextAction = _defragmentationPolicy.getNextStreamingAction(operationContext());

    // 1. The collection should be marked as undergoing through phase 1 of the algorithm...
    ASSERT_TRUE(_defragmentationPolicy.isDefragmentingCollection(coll.getUuid()));
    verifyExpectedDefragmentationStateOnDisk(coll.getUuid(),
                                             DefragmentationPhaseEnum::kMergeAndMeasureChunks);
    // 2. The action returned by the stream should be now an actionable DataSizeCommand...
    ASSERT_TRUE(nextAction.has_value());
    DataSizeInfo dataSizeAction = get<DataSizeInfo>(*nextAction);
    // 3. with the expected content
    ASSERT_EQ(coll.getNss(), dataSizeAction.nss);
    ASSERT_BSONOBJ_EQ(kKeyAtMin, dataSizeAction.chunkRange.getMin());
    ASSERT_BSONOBJ_EQ(kKeyAtMax, dataSizeAction.chunkRange.getMax());
}

TEST_F(BalancerDefragmentationPolicyTest,
       TestNonRetriableErrorWaitsForAllOutstandingActionsToComplete) {
    setupShards(kShardList);
    auto coll = setupCollectionWithPhase(
        kNss1,
        {ChunkType{
             kUuid1, ChunkRange(kKeyAtMin, kKeyAtTen), kCollectionPlacementVersion, kShardId0},
         ChunkType{
             kUuid1, ChunkRange(kKeyAtTen, kKeyAtMax), kCollectionPlacementVersion, kShardId1}});
    _defragmentationPolicy.startCollectionDefragmentations(operationContext());
    auto nextAction = _defragmentationPolicy.getNextStreamingAction(operationContext());
    DataSizeInfo failingDataSizeAction = get<DataSizeInfo>(*nextAction);
    auto nextAction2 = _defragmentationPolicy.getNextStreamingAction(operationContext());
    DataSizeInfo secondDataSizeAction = get<DataSizeInfo>(*nextAction2);
    StatusWith<DataSizeResponse> response(
        Status(ErrorCodes::NamespaceNotFound, "Testing error response"));

    _defragmentationPolicy.applyActionResult(operationContext(), failingDataSizeAction, response);

    // There should be no new actions.
    nextAction = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_TRUE(nextAction == boost::none);
    // Defragmentation should be waiting for second datasize action to complete
    ASSERT_TRUE(_defragmentationPolicy.isDefragmentingCollection(coll.getUuid()));
    // Defragmentation policy should ignore content of next acknowledge
    _defragmentationPolicy.applyActionResult(
        operationContext(),
        secondDataSizeAction,
        Status(ErrorCodes::NetworkTimeout, "Testing error response"));
    // Phase 1 should restart.
    nextAction = _defragmentationPolicy.getNextStreamingAction(operationContext());
    nextAction2 = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_TRUE(nextAction.has_value());
    ASSERT_TRUE(nextAction2.has_value());
    DataSizeInfo dataSizeAction = get<DataSizeInfo>(*nextAction);
    DataSizeInfo dataSizeAction2 = get<DataSizeInfo>(*nextAction2);
}

TEST_F(BalancerDefragmentationPolicyTest,
       TestPhaseOneAcknowledgeMergeChunkActionsTriggersDataSizeOnResultingRange) {
    setupShards(kShardList);
    auto coll = setupCollectionWithPhase(kNss1, {makeMergeableConfigChunkEntries(kUuid1)});
    _defragmentationPolicy.startCollectionDefragmentations(operationContext());
    auto nextAction = _defragmentationPolicy.getNextStreamingAction(operationContext());
    auto mergeChunksAction = get<MergeInfo>(*nextAction);

    _defragmentationPolicy.applyActionResult(operationContext(), mergeChunksAction, Status::OK());

    // Under the setup of this test, the stream should only contain only a data size action over the
    // recently merged range.
    nextAction = _defragmentationPolicy.getNextStreamingAction(operationContext());
    auto dataSizeAction = get<DataSizeInfo>(*nextAction);
    ASSERT_BSONOBJ_EQ(dataSizeAction.chunkRange.getMin(), mergeChunksAction.chunkRange.getMin());
    ASSERT_BSONOBJ_EQ(dataSizeAction.chunkRange.getMax(), mergeChunksAction.chunkRange.getMax());
    ASSERT_EQ(dataSizeAction.uuid, mergeChunksAction.uuid);
    ASSERT_EQ(dataSizeAction.shardId, mergeChunksAction.shardId);
    ASSERT_EQ(dataSizeAction.nss, mergeChunksAction.nss);

    nextAction = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_TRUE(nextAction == boost::none);
}

TEST_F(BalancerDefragmentationPolicyTest, TestPhaseOneFailedMergeChunksActionGetsReissued) {
    setupShards(kShardList);
    auto coll = setupCollectionWithPhase(kNss1, {makeMergeableConfigChunkEntries(kUuid1)});
    _defragmentationPolicy.startCollectionDefragmentations(operationContext());
    auto nextAction = _defragmentationPolicy.getNextStreamingAction(operationContext());
    auto failingMergeChunksAction = get<MergeInfo>(*nextAction);

    _defragmentationPolicy.applyActionResult(
        operationContext(),
        failingMergeChunksAction,
        Status(ErrorCodes::NetworkTimeout, "Testing error response"));
    // Under the setup of this test, the stream should only contain one more action - which (version
    // aside) matches the failed one.
    nextAction = _defragmentationPolicy.getNextStreamingAction(operationContext());
    auto replayedMergeChunksAction = get<MergeInfo>(*nextAction);
    ASSERT_EQ(failingMergeChunksAction.uuid, replayedMergeChunksAction.uuid);
    ASSERT_EQ(failingMergeChunksAction.shardId, replayedMergeChunksAction.shardId);
    ASSERT_EQ(failingMergeChunksAction.nss, replayedMergeChunksAction.nss);
    ASSERT_BSONOBJ_EQ(failingMergeChunksAction.chunkRange.getMin(),
                      replayedMergeChunksAction.chunkRange.getMin());
    ASSERT_BSONOBJ_EQ(failingMergeChunksAction.chunkRange.getMax(),
                      replayedMergeChunksAction.chunkRange.getMax());

    nextAction = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_TRUE(nextAction == boost::none);
}

TEST_F(BalancerDefragmentationPolicyTest, TestPhaseOneAcknowledgeSuccessfulMergeAction) {
    setupShards(kShardList);
    auto coll = setupCollectionWithPhase(kNss1, {makeMergeableConfigChunkEntries(kUuid1)});
    auto nextAction = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_TRUE(nextAction == boost::none);
    _defragmentationPolicy.startCollectionDefragmentations(operationContext());
    nextAction = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_TRUE(nextAction.has_value());
    MergeInfo mergeInfoAction = get<MergeInfo>(*nextAction);
    ASSERT_BSONOBJ_EQ(mergeInfoAction.chunkRange.getMin(), kKeyAtMin);
    ASSERT_BSONOBJ_EQ(mergeInfoAction.chunkRange.getMax(), kKeyAtMax);
    nextAction = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_TRUE(nextAction == boost::none);
    _defragmentationPolicy.applyActionResult(operationContext(), mergeInfoAction, Status::OK());
    nextAction = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_TRUE(nextAction.has_value());
    DataSizeInfo dataSizeAction = get<DataSizeInfo>(*nextAction);
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
        ChunkType chunk(kUuid1,
                        ChunkRange(minKey, maxKey),
                        ChunkVersion({kCollectionPlacementVersion.epoch(),
                                      kCollectionPlacementVersion.getTimestamp()},
                                     {1, uint32_t(i)}),
                        kShardId0);
        chunkList.push_back(chunk);
    }
    for (int i = 5; i < 10; i++) {
        const auto minKey = BSON("x" << i);
        const auto maxKey = (i == 9) ? kKeyAtMax : BSON("x" << i + 1);
        ChunkType chunk(kUuid1,
                        ChunkRange(minKey, maxKey),
                        ChunkVersion({kCollectionPlacementVersion.epoch(),
                                      kCollectionPlacementVersion.getTimestamp()},
                                     {1, uint32_t(i)}),
                        kShardId1);
        chunkList.push_back(chunk);
    }
    setupShards(kShardList);
    auto coll = setupCollectionWithPhase(kNss1, chunkList, boost::none, boost::none);
    _defragmentationPolicy.startCollectionDefragmentations(operationContext());
    // Test
    auto nextAction = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_TRUE(nextAction.has_value());
    auto nextAction2 = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_TRUE(nextAction2.has_value());
    // Verify the content of the received merge actions
    // (Note: there is no guarantee on the order provided by the stream)
    MergeInfo mergeAction = get<MergeInfo>(*nextAction);
    MergeInfo mergeAction2 = get<MergeInfo>(*nextAction2);
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
    auto nextAction3 = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_FALSE(nextAction3.has_value());
}

TEST_F(BalancerDefragmentationPolicyTest, PhaseOneNotConsecutive) {
    std::vector<ChunkType> chunkList;
    for (int i = 0; i < 10; i++) {
        const auto minKey = (i == 0) ? kKeyAtMin : BSON("x" << i);
        const auto maxKey = (i == 9) ? kKeyAtMax : BSON("x" << i + 1);
        ShardId chosenShard = (i == 5) ? kShardId1 : kShardId0;
        ChunkType chunk(kUuid1,
                        ChunkRange(minKey, maxKey),
                        ChunkVersion({kCollectionPlacementVersion.epoch(),
                                      kCollectionPlacementVersion.getTimestamp()},
                                     {1, uint32_t(i)}),
                        chosenShard);
        chunkList.push_back(chunk);
    }
    setupShards(kShardList);
    auto coll = setupCollectionWithPhase(kNss1, chunkList, boost::none, boost::none);
    _defragmentationPolicy.startCollectionDefragmentations(operationContext());
    // Three actions (in an unspecified order) should be immediately available.
    auto nextAction = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_TRUE(nextAction.has_value());
    auto nextAction2 = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_TRUE(nextAction2.has_value());
    auto nextAction3 = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_TRUE(nextAction3.has_value());
    // Verify their content of the received merge actions
    uint8_t timesLowerRangeMergeFound = 0;
    uint8_t timesUpperRangeMergeFound = 0;
    uint8_t timesMiddleRangeDataSizeFound = 0;
    auto inspectAction = [&](const BalancerStreamAction& action) {
        visit(OverloadedVisitor{
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
                  [](const MigrateInfo& _) { FAIL("Unexpected action type"); },
                  [](const MergeAllChunksOnShardInfo& _) {
                      FAIL("Unexpected action type");
                  }},
              action);
    };
    inspectAction(*nextAction);
    inspectAction(*nextAction2);
    inspectAction(*nextAction3);
    ASSERT_EQ(1, timesLowerRangeMergeFound);
    ASSERT_EQ(1, timesUpperRangeMergeFound);
    ASSERT_EQ(1, timesMiddleRangeDataSizeFound);

    auto nextAction4 = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_FALSE(nextAction4.has_value());
}

// Phase 2 tests.

TEST_F(BalancerDefragmentationPolicyTest, TestPhaseTwoMissingDataSizeRestartsPhase1) {
    setupShards(kShardList);
    auto coll = setupCollectionWithPhase(
        kNss1, {makeConfigChunkEntry(kUuid1)}, DefragmentationPhaseEnum::kMoveAndMergeChunks);
    setDefaultClusterStats();
    _defragmentationPolicy.startCollectionDefragmentations(operationContext());

    // Should be in phase 1
    ASSERT_TRUE(_defragmentationPolicy.isDefragmentingCollection(coll.getUuid()));
    verifyExpectedDefragmentationStateOnDisk(kUuid1,
                                             DefragmentationPhaseEnum::kMergeAndMeasureChunks);
    // There should be a datasize entry and no migrations
    auto availableShards = getAllShardIds(operationContext());
    auto pendingMigrations =
        _defragmentationPolicy.selectChunksToMove(operationContext(), &availableShards);
    ASSERT_EQ(0, pendingMigrations.size());
    auto nextAction = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_TRUE(nextAction.has_value());
    auto dataSizeAction = get<DataSizeInfo>(*nextAction);
}

TEST_F(BalancerDefragmentationPolicyTest, TestPhaseTwoChunkCanBeMovedAndMergedWithSibling) {
    ChunkType biggestChunk(kUuid1,
                           ChunkRange(kKeyAtMin, kKeyAtZero),
                           ChunkVersion({kCollectionPlacementVersion.epoch(),
                                         kCollectionPlacementVersion.getTimestamp()},
                                        {1, 0}),
                           kShardId0);
    biggestChunk.setEstimatedSizeBytes(2048);
    ChunkType smallestChunk(kUuid1,
                            ChunkRange(kKeyAtZero, kKeyAtMax),
                            ChunkVersion({kCollectionPlacementVersion.epoch(),
                                          kCollectionPlacementVersion.getTimestamp()},
                                         {1, 1}),
                            kShardId1);
    smallestChunk.setEstimatedSizeBytes(1024);

    setupShards(kShardList);
    auto coll = setupCollectionWithPhase(
        kNss1, {smallestChunk, biggestChunk}, DefragmentationPhaseEnum::kMoveAndMergeChunks);
    std::vector<ShardStatistics> clusterStats{buildShardStats(kShardId0, 4),
                                              buildShardStats(kShardId1, 2)};
    std::map<NamespaceString, std::vector<ShardStatistics>> collectionStats{
        {kNss1, {buildShardStats(kShardId0, 4), buildShardStats(kShardId1, 2)}}};
    _clusterStats.setStats(std::move(clusterStats), std::move(collectionStats));
    _defragmentationPolicy.startCollectionDefragmentations(operationContext());
    ASSERT_TRUE(_defragmentationPolicy.isDefragmentingCollection(coll.getUuid()));


    auto availableShards = getAllShardIds(operationContext());
    auto numOfShards = availableShards.size();
    auto pendingMigrations =
        _defragmentationPolicy.selectChunksToMove(operationContext(), &availableShards);
    auto numOfUsedShards = numOfShards - availableShards.size();
    ASSERT_EQ(1, pendingMigrations.size());
    ASSERT_EQ(2, numOfUsedShards);

    auto moveAction = pendingMigrations.back();
    // The chunk belonging to the "fullest" shard is expected to be moved - even though it is bigger
    // than its sibling.
    ASSERT_EQ(biggestChunk.getShard(), moveAction.from);
    ASSERT_EQ(smallestChunk.getShard(), moveAction.to);
    ASSERT_BSONOBJ_EQ(biggestChunk.getMin(), moveAction.minKey);
    ASSERT_BSONOBJ_EQ(biggestChunk.getMax(), *moveAction.maxKey);

    auto nextAction = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_TRUE(nextAction == boost::none);

    _defragmentationPolicy.applyActionResult(operationContext(), moveAction, Status::OK());
    nextAction = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_TRUE(nextAction.has_value());

    availableShards = getAllShardIds(operationContext());
    numOfShards = availableShards.size();
    pendingMigrations =
        _defragmentationPolicy.selectChunksToMove(operationContext(), &availableShards);
    numOfUsedShards = numOfShards - availableShards.size();
    ASSERT_TRUE(pendingMigrations.empty());
    ASSERT_EQ(0, numOfUsedShards);

    auto mergeAction = get<MergeInfo>(*nextAction);
    ASSERT_EQ(smallestChunk.getShard(), mergeAction.shardId);
    ASSERT_TRUE(ChunkRange(biggestChunk.getMin(), smallestChunk.getMax()) ==
                mergeAction.chunkRange);

    _defragmentationPolicy.applyActionResult(operationContext(), mergeAction, Status::OK());
    nextAction = _defragmentationPolicy.getNextStreamingAction(operationContext());
    ASSERT_TRUE(nextAction == boost::none);
    pendingMigrations =
        _defragmentationPolicy.selectChunksToMove(operationContext(), &availableShards);
    ASSERT_TRUE(pendingMigrations.empty());
}

TEST_F(BalancerDefragmentationPolicyTest,
       TestPhaseTwoMultipleCollectionChunkMigrationsMayBeIssuedConcurrently) {
    // Define a single collection, distributing 6 chunks across the 4 shards so that there cannot be
    // a merge without migrations
    ChunkType firstChunkOnShard0(kUuid1,
                                 ChunkRange(kKeyAtMin, kKeyAtZero),
                                 ChunkVersion({kCollectionPlacementVersion.epoch(),
                                               kCollectionPlacementVersion.getTimestamp()},
                                              {1, 0}),
                                 kShardId0);
    firstChunkOnShard0.setEstimatedSizeBytes(1);

    ChunkType firstChunkOnShard1(kUuid1,
                                 ChunkRange(kKeyAtZero, kKeyAtTen),
                                 ChunkVersion({kCollectionPlacementVersion.epoch(),
                                               kCollectionPlacementVersion.getTimestamp()},
                                              {1, 1}),
                                 kShardId1);
    firstChunkOnShard1.setEstimatedSizeBytes(1);

    ChunkType chunkOnShard2(kUuid1,
                            ChunkRange(kKeyAtTen, kKeyAtTwenty),
                            ChunkVersion({kCollectionPlacementVersion.epoch(),
                                          kCollectionPlacementVersion.getTimestamp()},
                                         {1, 2}),
                            kShardId2);
    chunkOnShard2.setEstimatedSizeBytes(1);

    ChunkType chunkOnShard3(kUuid1,
                            ChunkRange(kKeyAtTwenty, kKeyAtThirty),
                            ChunkVersion({kCollectionPlacementVersion.epoch(),
                                          kCollectionPlacementVersion.getTimestamp()},
                                         {1, 3}),
                            kShardId3);
    chunkOnShard3.setEstimatedSizeBytes(1);

    ChunkType secondChunkOnShard0(kUuid1,
                                  ChunkRange(kKeyAtThirty, kKeyAtForty),
                                  ChunkVersion({kCollectionPlacementVersion.epoch(),
                                                kCollectionPlacementVersion.getTimestamp()},
                                               {1, 4}),
                                  kShardId0);
    secondChunkOnShard0.setEstimatedSizeBytes(1);

    ChunkType secondChunkOnShard1(kUuid1,
                                  ChunkRange(kKeyAtForty, kKeyAtMax),
                                  ChunkVersion({kCollectionPlacementVersion.epoch(),
                                                kCollectionPlacementVersion.getTimestamp()},
                                               {1, 5}),
                                  kShardId1);
    secondChunkOnShard1.setEstimatedSizeBytes(1);

    setupShards(kShardList);
    auto coll = setupCollectionWithPhase(kNss1,
                                         {firstChunkOnShard0,
                                          firstChunkOnShard1,
                                          chunkOnShard2,
                                          chunkOnShard3,
                                          secondChunkOnShard0,
                                          secondChunkOnShard1},
                                         DefragmentationPhaseEnum::kMoveAndMergeChunks,
                                         boost::none);
    setDefaultClusterStats();
    _defragmentationPolicy.startCollectionDefragmentations(operationContext());

    // Two move operation should be returned within a single invocation, using all the possible
    // shards
    auto availableShards = getAllShardIds(operationContext());
    auto numOfShards = availableShards.size();
    auto pendingMigrations =
        _defragmentationPolicy.selectChunksToMove(operationContext(), &availableShards);
    auto numOfUsedShards = numOfShards - availableShards.size();
    ASSERT_EQ(4, numOfUsedShards);
    ASSERT_EQ(2, pendingMigrations.size());
}

TEST_F(BalancerDefragmentationPolicyTest, DontStartDefragmentationOnAnyCollection) {

    // Init a collection with defragmentation flag unset
    setupShards(kShardList);
    setupCollection(kNss1, kShardKeyPattern, {makeConfigChunkEntry(kUuid1)});

    _defragmentationPolicy.startCollectionDefragmentations(operationContext());

    verifyExpectedDefragmentationStateOnDisk(kUuid1, boost::none);
    ASSERT_FALSE(_defragmentationPolicy.isDefragmentingCollection(kUuid1));
}

TEST_F(BalancerDefragmentationPolicyTest, StartDefragmentationOnMultipleCollections) {

    // Setup 3 collections:
    //    coll1 ->     DEFRAGMENTING
    //    coll2 -> NOT DEFRAGMENTING
    //    coll3 ->     DEFRAGMENTING

    setupShards(kShardList);
    auto coll1 = setupCollectionWithPhase(kNss1, {makeConfigChunkEntry(kUuid1)});

    setupCollection(kNss2, kShardKeyPattern, {makeConfigChunkEntry(kUuid2)});
    auto coll2 =
        Grid::get(operationContext())->catalogClient()->getCollection(operationContext(), kUuid2);

    auto coll3 = setupCollectionWithPhase(kNss3, {makeConfigChunkEntry(kUuid3)});

    _defragmentationPolicy.startCollectionDefragmentations(operationContext());

    ASSERT_TRUE(_defragmentationPolicy.isDefragmentingCollection(coll1.getUuid()));
    ASSERT_FALSE(_defragmentationPolicy.isDefragmentingCollection(coll2.getUuid()));
    ASSERT_TRUE(_defragmentationPolicy.isDefragmentingCollection(coll3.getUuid()));

    verifyExpectedDefragmentationStateOnDisk(kUuid1,
                                             DefragmentationPhaseEnum::kMergeAndMeasureChunks);
    verifyExpectedDefragmentationStateOnDisk(kUuid2, boost::none);
    verifyExpectedDefragmentationStateOnDisk(kUuid3,
                                             DefragmentationPhaseEnum::kMergeAndMeasureChunks);
}

}  // namespace
}  // namespace mongo
