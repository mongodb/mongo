/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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


#include "mongo/db/s/resharding/resharding_util.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/agg/pipeline_builder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/s/resharding/resharding_coordinator_service_util.h"
#include "mongo/db/s/resharding/resharding_noop_o2_field_gen.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/s/resharding/resharding_txn_cloner.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/session/session_txn_record_gen.h"
#include "mongo/db/sharding_environment/config_server_test_fixture.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <functional>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace resharding {
namespace {

class ReshardingUtilTest : public ConfigServerTestFixture {
protected:
    void setUp() override {
        ConfigServerTestFixture::setUp();
        ShardType shard1;
        shard1.setName("a");
        shard1.setHost("a:1234");
        ShardType shard2;
        shard2.setName("b");
        shard2.setHost("b:1234");
        setupShards({shard1, shard2});
    }
    void tearDown() override {
        ConfigServerTestFixture::tearDown();
    }

    std::string shardKey() {
        return _shardKey;
    }

    const KeyPattern& keyPattern() {
        return _shardKeyPattern.getKeyPattern();
    }

    NamespaceString nss() {
        return _nss;
    }

    ReshardingZoneType makeZone(const ChunkRange range, std::string zoneName) {
        return ReshardingZoneType(zoneName, range.getMin(), range.getMax());
    }

    std::string zoneName(std::string zoneNum) {
        return "_zoneName" + zoneNum;
    }

    void validateIndexes(std::vector<BSONObj>& sourceSpecs,
                         std::vector<BSONObj>& recipientSpecs,
                         ErrorCodes::Error code) {
        if (code == ErrorCodes::OK) {
            ASSERT_DOES_NOT_THROW(verifyIndexSpecsMatch(sourceSpecs.cbegin(),
                                                        sourceSpecs.cend(),
                                                        recipientSpecs.cbegin(),
                                                        recipientSpecs.cend()));
        } else {
            ASSERT_THROWS_CODE(verifyIndexSpecsMatch(sourceSpecs.cbegin(),
                                                     sourceSpecs.cend(),
                                                     recipientSpecs.cbegin(),
                                                     recipientSpecs.cend()),
                               DBException,
                               code);
        }
    }

    void validateIndexes(const BSONObj& sourceSpec,
                         const BSONObj& recipientSpec,
                         ErrorCodes::Error code) {
        std::vector<BSONObj> sourceIndexSpecs;
        std::vector<BSONObj> recipientSpecs;

        sourceIndexSpecs.push_back(sourceSpec);
        recipientSpecs.push_back(recipientSpec);
        validateIndexes(sourceIndexSpecs, recipientSpecs, code);
    }

private:
    const NamespaceString _nss = NamespaceString::createNamespaceString_forTest("test.foo");
    const std::string _shardKey = "x";
    const ShardKeyPattern _shardKeyPattern = ShardKeyPattern(BSON("x" << "hashed"));
};

// Confirm the highest minFetchTimestamp is properly computed.
TEST(SimpleReshardingUtilTest, HighestMinFetchTimestampSucceeds) {
    std::vector<DonorShardEntry> donorShards{
        makeDonorShard(ShardId("s0"), DonorStateEnum::kDonatingInitialData, Timestamp(10, 2)),
        makeDonorShard(ShardId("s1"), DonorStateEnum::kDonatingInitialData, Timestamp(10, 3)),
        makeDonorShard(ShardId("s2"), DonorStateEnum::kDonatingInitialData, Timestamp(10, 1))};
    auto highestMinFetchTimestamp = getHighestMinFetchTimestamp(donorShards);
    ASSERT_EQ(Timestamp(10, 3), highestMinFetchTimestamp);
}

TEST(SimpleReshardingUtilTest, HighestMinFetchTimestampThrowsWhenDonorMissingTimestamp) {
    std::vector<DonorShardEntry> donorShards{
        makeDonorShard(ShardId("s0"), DonorStateEnum::kDonatingInitialData, Timestamp(10, 3)),
        makeDonorShard(ShardId("s1"), DonorStateEnum::kDonatingInitialData),
        makeDonorShard(ShardId("s2"), DonorStateEnum::kDonatingInitialData, Timestamp(10, 2))};
    ASSERT_THROWS_CODE(getHighestMinFetchTimestamp(donorShards), DBException, 4957300);
}

TEST(SimpleReshardingUtilTest,
     HighestMinFetchTimestampSucceedsWithDonorStateGTkDonatingOplogEntries) {
    std::vector<DonorShardEntry> donorShards{
        makeDonorShard(ShardId("s0"), DonorStateEnum::kBlockingWrites, Timestamp(10, 2)),
        makeDonorShard(ShardId("s1"), DonorStateEnum::kDonatingOplogEntries, Timestamp(10, 3)),
        makeDonorShard(ShardId("s2"), DonorStateEnum::kDonatingOplogEntries, Timestamp(10, 1))};
    auto highestMinFetchTimestamp = getHighestMinFetchTimestamp(donorShards);
    ASSERT_EQ(Timestamp(10, 3), highestMinFetchTimestamp);
}

// Validate resharded chunks tests.

TEST_F(ReshardingUtilTest, SuccessfulValidateReshardedChunkCase) {
    std::vector<ReshardedChunk> chunks;
    chunks.emplace_back(ShardId("a"), keyPattern().globalMin(), BSON(shardKey() << 0));
    chunks.emplace_back(ShardId("b"), BSON(shardKey() << 0), keyPattern().globalMax());

    validateReshardedChunks(chunks, operationContext(), keyPattern());
}

TEST_F(ReshardingUtilTest, FailWhenHoleInChunkRange) {
    std::vector<ReshardedChunk> chunks;
    chunks.emplace_back(ShardId("a"), keyPattern().globalMin(), BSON(shardKey() << 0));
    chunks.emplace_back(ShardId("b"), BSON(shardKey() << 20), keyPattern().globalMax());

    ASSERT_THROWS_CODE(validateReshardedChunks(chunks, operationContext(), keyPattern()),
                       DBException,
                       ErrorCodes::BadValue);
}

TEST_F(ReshardingUtilTest, FailWhenOverlapInChunkRange) {
    std::vector<ReshardedChunk> chunks;
    chunks.emplace_back(ShardId("a"), keyPattern().globalMin(), BSON(shardKey() << 10));
    chunks.emplace_back(ShardId("b"), BSON(shardKey() << 5), keyPattern().globalMax());

    ASSERT_THROWS_CODE(validateReshardedChunks(chunks, operationContext(), keyPattern()),
                       DBException,
                       ErrorCodes::BadValue);
}

TEST_F(ReshardingUtilTest, FailWhenChunkRangeDoesNotStartAtGlobalMin) {
    std::vector<ReshardedChunk> chunks;

    chunks.emplace_back(ShardId("a"), BSON(shardKey() << 10), BSON(shardKey() << 20));
    chunks.emplace_back(ShardId("b"), BSON(shardKey() << 20), keyPattern().globalMax());

    ASSERT_THROWS_CODE(validateReshardedChunks(chunks, operationContext(), keyPattern()),
                       DBException,
                       ErrorCodes::BadValue);
}

TEST_F(ReshardingUtilTest, FailWhenChunkRangeDoesNotEndAtGlobalMax) {
    std::vector<ReshardedChunk> chunks;
    chunks.emplace_back(ShardId("a"), keyPattern().globalMin(), BSON(shardKey() << 0));
    chunks.emplace_back(ShardId("b"), BSON(shardKey() << 0), BSON(shardKey() << 10));

    ASSERT_THROWS_CODE(validateReshardedChunks(chunks, operationContext(), keyPattern()),
                       DBException,
                       ErrorCodes::BadValue);
}

// Validate zones tests.

TEST_F(ReshardingUtilTest, SuccessfulValidateZoneCase) {
    const std::vector<ChunkRange> zoneRanges = {
        ChunkRange(keyPattern().globalMin(), BSON(shardKey() << 0)),
        ChunkRange(BSON(shardKey() << 0), BSON(shardKey() << 10)),
    };
    std::vector<mongo::ReshardingZoneType> zones;
    zones.push_back(makeZone(zoneRanges[0], zoneName("1")));
    zones.push_back(makeZone(zoneRanges[1], zoneName("2")));

    checkForOverlappingZones(zones);
}


TEST_F(ReshardingUtilTest, FailWhenOverlappingZones) {
    const std::vector<ChunkRange> overlapZoneRanges = {
        ChunkRange(BSON(shardKey() << 0), BSON(shardKey() << 10)),
        ChunkRange(BSON(shardKey() << 8), keyPattern().globalMax()),
    };

    std::vector<ReshardingZoneType> zones;
    zones.push_back(makeZone(overlapZoneRanges[0], zoneName("0")));
    zones.push_back(makeZone(overlapZoneRanges[1], zoneName("1")));

    ASSERT_THROWS_CODE(checkForOverlappingZones(zones), DBException, ErrorCodes::BadValue);
}

TEST(SimpleReshardingUtilTest, AssertDonorOplogIdSerialization) {
    // It's a correctness requirement that `ReshardingDonorOplogId.toBSON` serializes as
    // `{clusterTime: <value>, ts: <value>}`, paying particular attention to the ordering of the
    // fields. The serialization order is defined as the ordering of the fields in the idl file.
    //
    // This is because a document with the same shape as a BSON serialized `ReshardingDonorOplogId`
    // is tacked on as the `_id` to documents in an aggregation pipeline. The pipeline then performs
    // a $gt on the `_id` value with an input `ReshardingDonorOplogId`. If the field ordering were
    // different, the comparison would silently evaluate to the wrong result.
    ReshardingDonorOplogId oplogId(Timestamp::min(), Timestamp::min());
    BSONObj oplogIdObj = oplogId.toBSON();
    BSONObjIterator it(oplogIdObj);
    ASSERT_EQ("clusterTime"_sd, it.next().fieldNameStringData()) << oplogIdObj;
    ASSERT_EQ("ts"_sd, it.next().fieldNameStringData()) << oplogIdObj;
    ASSERT_FALSE(it.more());
}

TEST_F(ReshardingUtilTest, ValidateIndexSpecsMatch) {
    // 1. Source has index, Recipient has none.
    validateIndexes(BSON("name" << "test"), BSONObj(), (ErrorCodes::Error)9365601);

    // 2. Collation subField difference.
    auto sourceSpec = BSON("key" << BSON("field" << 1) << "name"
                                 << "indexName"
                                 << "v" << 3 << "collation"
                                 << BSON("locale" << "en"
                                                  << "strength" << 2));

    auto recipientSpec = BSON("key" << BSON("field" << 1) << "name"
                                    << "indexName"
                                    << "v" << 3 << "collation"
                                    << BSON("locale" << "en"
                                                     << "strength" << 3));
    validateIndexes(sourceSpec, recipientSpec, (ErrorCodes::Error)9365602);

    // 3. Collation simple vs non-simple.
    sourceSpec = BSON("key" << BSON("field" << 1) << "name"
                            << "indexName"
                            << "v" << 3);

    recipientSpec = BSON("key" << BSON("field" << 1) << "name"
                               << "indexName"
                               << "v" << 3 << "collation"
                               << BSON("locale" << "en"
                                                << "strength" << 2));
    validateIndexes(sourceSpec, recipientSpec, (ErrorCodes::Error)9365602);

    // 4. Different field ordering.
    sourceSpec = BSON("key" << BSON("field" << 1) << "name"
                            << "indexName"
                            << "v" << 3);
    recipientSpec = BSON("key" << BSON("field" << 1) << "v" << 3 << "name"
                               << "indexName");
    validateIndexes(sourceSpec, recipientSpec, ErrorCodes::OK);

    // 5. Equal Indexes.
    std::vector<BSONObj> sourceSpecs{BSON("key" << BSON("field_2" << 1) << "name"
                                                << "indexName_2"
                                                << "v" << 3),
                                     BSON("key" << BSON("field" << 1) << "name"
                                                << "indexName"
                                                << "v" << 3 << "collation"
                                                << BSON("locale" << "en"
                                                                 << "strength" << 2))};
    std::vector<BSONObj> recipientSpecs{BSON("key" << BSON("field_2" << 1) << "name"
                                                   << "indexName_2"
                                                   << "v" << 3),
                                        BSON("key" << BSON("field" << 1) << "name"
                                                   << "indexName"
                                                   << "v" << 3 << "collation"
                                                   << BSON("locale" << "en"
                                                                    << "strength" << 2))};

    validateIndexes(sourceSpecs, recipientSpecs, ErrorCodes::OK);

    // 6. num(recipientSpecs) > num(recipientSpecs) works.
    std::vector<BSONObj> sourceSpecs2{BSON("key" << BSON("field2" << 1) << "name"
                                                 << "indexName_2"
                                                 << "v" << 3)};
    std::vector<BSONObj> recipientSpecs2{BSON("key" << BSON("field" << 1) << "name"
                                                    << "indexName"
                                                    << "v" << 3 << "collation"
                                                    << BSON("locale" << "en"
                                                                     << "strength" << 2)),
                                         BSON("key" << BSON("field2" << 1) << "name"
                                                    << "indexName_2"
                                                    << "v" << 3)};

    validateIndexes(sourceSpecs2, recipientSpecs2, ErrorCodes::OK);
}

TEST_F(ReshardingUtilTest, SetNumSamplesPerChunkThroughConfigsvrReshardCollectionRequest) {
    RAIIServerParameterControllerForTest featureFlagController(
        "featureFlagReshardingNumSamplesPerChunk", true);

    int numInitialChunks = 1;
    int numSamplesPerChunk = 10;

    const CollectionType collEntry(nss(),
                                   OID::gen(),
                                   Timestamp(static_cast<unsigned int>(std::time(nullptr)), 1),
                                   Date_t::now(),
                                   UUID::gen(),
                                   keyPattern());

    ConfigsvrReshardCollection configsvrReshardCollection(nss(), BSON(shardKey() << 1));
    configsvrReshardCollection.setDbName(nss().dbName());
    configsvrReshardCollection.setUnique(true);
    const auto collationObj = BSON("locale" << "en_US");
    configsvrReshardCollection.setCollation(collationObj);
    configsvrReshardCollection.setNumInitialChunks(numInitialChunks);

    boost::optional<ReshardingProvenanceEnum> provenance(
        ReshardingProvenanceEnum::kReshardCollection);
    configsvrReshardCollection.setProvenance(provenance);
    configsvrReshardCollection.setNumSamplesPerChunk(numSamplesPerChunk);


    ReshardingCoordinatorDocument coordinatorDoc = createReshardingCoordinatorDoc(
        operationContext(), configsvrReshardCollection, collEntry, nss(), true);
    auto numSamplesPerChunkOptional = coordinatorDoc.getNumSamplesPerChunk();
    ASSERT_TRUE(numSamplesPerChunkOptional.has_value());
    ASSERT_EQ(*numSamplesPerChunkOptional, numSamplesPerChunk);
}

TEST_F(ReshardingUtilTest, CreateCoordinatorDocPerformVerification) {
    for (auto performVerification : std::vector<boost::optional<bool>>{true, false, boost::none}) {
        for (bool enableVerification : {true, false}) {
            LOGV2(9849102,
                  "Running case",
                  "test"_attr = unittest::getTestName(),
                  "performVerification"_attr = performVerification,
                  "enableVerification"_attr = enableVerification);
            RAIIServerParameterControllerForTest featureFlagController(
                "featureFlagReshardingVerification", enableVerification);


            const CollectionType collEntry(
                nss(),
                OID::gen(),
                Timestamp(static_cast<unsigned int>(std::time(nullptr)), 1),
                Date_t::now(),
                UUID::gen(),
                keyPattern());

            ConfigsvrReshardCollection configsvrReshardCollection(nss(), BSON(shardKey() << 1));
            configsvrReshardCollection.setDbName(nss().dbName());
            configsvrReshardCollection.setPerformVerification(performVerification);

            ReshardingCoordinatorDocument coordinatorDoc = createReshardingCoordinatorDoc(
                operationContext(), configsvrReshardCollection, collEntry, nss(), true);

            auto actualPerformVerification = coordinatorDoc.getPerformVerification();
            if (performVerification.has_value()) {
                ASSERT(actualPerformVerification.has_value());
                ASSERT_EQ(actualPerformVerification, *performVerification);
            } else if (enableVerification) {
                ASSERT(actualPerformVerification.has_value());
                ASSERT_EQ(actualPerformVerification, true);
            } else {
                ASSERT_FALSE(actualPerformVerification.has_value());
            }
        }
    }
}

TEST_F(ReshardingUtilTest, GetMajorityReplicationLag_Basic) {
    auto replCoord = repl::ReplicationCoordinator::get(operationContext());

    auto expectedReplicationLag = Milliseconds(1500);
    auto lastCommittedOpTimeAndWallTime =
        repl::OpTimeAndWallTime{repl::OpTime(Timestamp(100, 1), 1), Date_t::now()};
    auto lastAppliedOpTimeAndWallTime =
        repl::OpTimeAndWallTime{repl::OpTime(Timestamp(120, 1), 2),
                                lastCommittedOpTimeAndWallTime.wallTime + expectedReplicationLag};
    replCoord->setMyLastAppliedOpTimeAndWallTimeForward(lastAppliedOpTimeAndWallTime);
    replCoord->advanceCommitPoint(lastCommittedOpTimeAndWallTime, false /* fromSyncSource */);

    auto actualReplicationLag = getMajorityReplicationLag(operationContext());
    ASSERT_EQ(actualReplicationLag, expectedReplicationLag);
}

TEST_F(ReshardingUtilTest, GetMajorityReplicationLag_LastAppliedEqualToLastCommitted) {
    auto replCoord = repl::ReplicationCoordinator::get(operationContext());

    auto lastCommittedOpTimeAndWallTime =
        repl::OpTimeAndWallTime{repl::OpTime(Timestamp(100, 1), 1), Date_t::now()};
    auto lastAppliedOpTimeAndWallTime = lastCommittedOpTimeAndWallTime;
    replCoord->setMyLastAppliedOpTimeAndWallTimeForward(lastAppliedOpTimeAndWallTime);
    replCoord->advanceCommitPoint(lastCommittedOpTimeAndWallTime, false /* fromSyncSource */);

    auto actualReplicationLag = getMajorityReplicationLag(operationContext());
    ASSERT_EQ(actualReplicationLag, Milliseconds(0));
}

TEST_F(ReshardingUtilTest, GetMajorityReplicationLag_LastAppliedLessThanLastCommitted) {
    auto replCoord = repl::ReplicationCoordinator::get(operationContext());

    auto lastCommittedOpTimeAndWallTime =
        repl::OpTimeAndWallTime{repl::OpTime(Timestamp(100, 1), 1), Date_t::now()};
    auto lastAppliedOpTimeAndWallTime =
        repl::OpTimeAndWallTime{repl::OpTime(Timestamp(80, 1), 1),
                                lastCommittedOpTimeAndWallTime.wallTime - Milliseconds(5)};
    replCoord->setMyLastAppliedOpTimeAndWallTimeForward(lastAppliedOpTimeAndWallTime);
    replCoord->advanceCommitPoint(lastCommittedOpTimeAndWallTime, false /* fromSyncSource */);

    auto actualReplicationLag = getMajorityReplicationLag(operationContext());
    ASSERT_EQ(actualReplicationLag, Milliseconds(0));
}

TEST_F(ReshardingUtilTest, SetDemoModeThroughConfigsvrReshardCollectionRequest) {
    const CollectionType collEntry(nss(),
                                   OID::gen(),
                                   Timestamp(static_cast<unsigned int>(std::time(nullptr)), 1),
                                   Date_t::now(),
                                   UUID::gen(),
                                   keyPattern());

    ConfigsvrReshardCollection configsvrReshardCollection(nss(), BSON(shardKey() << 1));
    configsvrReshardCollection.setDbName(nss().dbName());
    configsvrReshardCollection.setUnique(true);
    const auto collationObj = BSON("locale" << "en_US");
    configsvrReshardCollection.setCollation(collationObj);
    configsvrReshardCollection.setDemoMode(true);

    boost::optional<ReshardingProvenanceEnum> provenance(
        ReshardingProvenanceEnum::kReshardCollection);
    configsvrReshardCollection.setProvenance(provenance);

    ReshardingCoordinatorDocument coordinatorDoc = createReshardingCoordinatorDoc(
        operationContext(), configsvrReshardCollection, collEntry, nss(), true);
    auto demoModeOpt = coordinatorDoc.getDemoMode();
    ASSERT_TRUE(demoModeOpt.has_value() && demoModeOpt);

    // When demoMode is set to true, reshardingMinimumOperationDurationMillis value is overridden
    // to 0 for the reshardCollection operation.
    auto recipientFields = constructRecipientFields(coordinatorDoc);
    ASSERT_EQ(recipientFields.getMinimumOperationDurationMillis(), 0);
}

TEST_F(ReshardingUtilTest, EmptyDemoModeReshardCollectionRequest) {
    const CollectionType collEntry(nss(),
                                   OID::gen(),
                                   Timestamp(static_cast<unsigned int>(std::time(nullptr)), 1),
                                   Date_t::now(),
                                   UUID::gen(),
                                   keyPattern());

    ConfigsvrReshardCollection configsvrReshardCollection(nss(), BSON(shardKey() << 1));
    configsvrReshardCollection.setDbName(nss().dbName());
    configsvrReshardCollection.setUnique(true);
    const auto collationObj = BSON("locale" << "en_US");
    configsvrReshardCollection.setCollation(collationObj);

    boost::optional<ReshardingProvenanceEnum> provenance(
        ReshardingProvenanceEnum::kReshardCollection);
    configsvrReshardCollection.setProvenance(provenance);

    ReshardingCoordinatorDocument coordinatorDoc = createReshardingCoordinatorDoc(
        operationContext(), configsvrReshardCollection, collEntry, nss(), true);
    auto demoModeOpt = coordinatorDoc.getDemoMode();
    ASSERT_FALSE(demoModeOpt.has_value());

    // If demoMode is not specified or is set to False, then
    // reshardingMinimumOperationDurationMillis will default to the server's predefined parameter
    // value.
    auto recipientFields = constructRecipientFields(coordinatorDoc);
    ASSERT_EQ(recipientFields.getMinimumOperationDurationMillis(),
              gReshardingMinimumOperationDurationMillis.load());
}

TEST_F(ReshardingUtilTest, IsProgressMarkOplogCreatedAfterOplogApplicationStarted) {
    repl::MutableOplogEntry oplog;
    oplog.setNss(nss());
    oplog.setOpType(repl::OpTypeEnum::kNoop);
    oplog.setUuid(UUID::gen());
    oplog.set_id({});
    oplog.setObject({});
    oplog.setObject2(
        BSON(ReshardProgressMarkO2Field::kTypeFieldName
             << resharding::kReshardProgressMarkOpLogType
             << ReshardProgressMarkO2Field::kCreatedAfterOplogApplicationStartedFieldName << true));
    oplog.setOpTime(OplogSlot());
    oplog.setWallClockTime(getServiceContext()->getFastClockSource()->now());
    ASSERT(isProgressMarkOplogAfterOplogApplicationStarted({oplog.toBSON()}));
}

TEST_F(ReshardingUtilTest, IsNotProgressMarkOplogCreatedAfterOplogApplicationStarted_NotNoop) {
    repl::MutableOplogEntry oplog;
    oplog.setNss(nss());
    oplog.setOpType(repl::OpTypeEnum::kInsert);
    oplog.setUuid(UUID::gen());
    oplog.set_id({});
    oplog.setObject({});
    oplog.setObject2(
        BSON(ReshardProgressMarkO2Field::kTypeFieldName
             << resharding::kReshardProgressMarkOpLogType
             << ReshardProgressMarkO2Field::kCreatedAfterOplogApplicationStartedFieldName << true));
    oplog.setOpTime(OplogSlot());
    oplog.setWallClockTime(getServiceContext()->getFastClockSource()->now());
    ASSERT_FALSE(isProgressMarkOplogAfterOplogApplicationStarted({oplog.toBSON()}));
}

TEST_F(ReshardingUtilTest, IsNotProgressMarkOplogCreatedAfterOplogApplicationStarted_NoObject2) {
    repl::MutableOplogEntry oplog;
    oplog.setNss(nss());
    oplog.setOpType(repl::OpTypeEnum::kNoop);
    oplog.setUuid(UUID::gen());
    oplog.set_id({});
    oplog.setObject({});
    oplog.setOpTime(OplogSlot());
    oplog.setWallClockTime(getServiceContext()->getFastClockSource()->now());
    ASSERT_FALSE(isProgressMarkOplogAfterOplogApplicationStarted({oplog.toBSON()}));
}

TEST_F(ReshardingUtilTest,
       IsNotProgressMarkOplogCreatedAfterOplogApplicationStarted_NotProgressMarkType) {
    repl::MutableOplogEntry oplog;
    oplog.setNss(nss());
    oplog.setOpType(repl::OpTypeEnum::kNoop);
    oplog.setUuid(UUID::gen());
    oplog.set_id({});
    oplog.setObject({});
    oplog.setObject2(
        BSON(ReshardProgressMarkO2Field::kTypeFieldName
             << resharding::kReshardFinalOpLogType
             << ReshardProgressMarkO2Field::kCreatedAfterOplogApplicationStartedFieldName << true));
    oplog.setOpTime(OplogSlot());
    oplog.setWallClockTime(getServiceContext()->getFastClockSource()->now());
    ASSERT_FALSE(isProgressMarkOplogAfterOplogApplicationStarted({oplog.toBSON()}));
}

TEST_F(ReshardingUtilTest, IsNotProgressMarkOplogCreatedAfterOplogApplicationStarted_InvalidType) {
    repl::MutableOplogEntry oplog;
    oplog.setNss(nss());
    oplog.setOpType(repl::OpTypeEnum::kNoop);
    oplog.setUuid(UUID::gen());
    oplog.set_id({});
    oplog.setObject({});
    oplog.setObject2(BSON(
        ReshardProgressMarkO2Field::kTypeFieldName
        << 2 << ReshardProgressMarkO2Field::kCreatedAfterOplogApplicationStartedFieldName << true));
    oplog.setOpTime(OplogSlot());
    oplog.setWallClockTime(getServiceContext()->getFastClockSource()->now());
    ASSERT_FALSE(isProgressMarkOplogAfterOplogApplicationStarted({oplog.toBSON()}));
}

TEST_F(ReshardingUtilTest,
       IsNotProgressMarkOplogCreatedAfterOplogApplicationStarted_CreatedAfterNull) {
    repl::MutableOplogEntry oplog;
    oplog.setNss(nss());
    oplog.setOpType(repl::OpTypeEnum::kNoop);
    oplog.setUuid(UUID::gen());
    oplog.set_id({});
    oplog.setObject({});
    oplog.setObject2(BSON(ReshardProgressMarkO2Field::kTypeFieldName
                          << resharding::kReshardProgressMarkOpLogType));
    oplog.setOpTime(OplogSlot());
    oplog.setWallClockTime(getServiceContext()->getFastClockSource()->now());
    ASSERT_FALSE(isProgressMarkOplogAfterOplogApplicationStarted({oplog.toBSON()}));
}

TEST_F(ReshardingUtilTest,
       IsNotProgressMarkOplogCreatedAfterOplogApplicationStarted_CreatedAfterFalse) {
    repl::MutableOplogEntry oplog;
    oplog.setNss(nss());
    oplog.setOpType(repl::OpTypeEnum::kNoop);
    oplog.setUuid(UUID::gen());
    oplog.set_id({});
    oplog.setObject({});
    oplog.setObject2(BSON(
        ReshardProgressMarkO2Field::kTypeFieldName
        << resharding::kReshardProgressMarkOpLogType
        << ReshardProgressMarkO2Field::kCreatedAfterOplogApplicationStartedFieldName << false));
    oplog.setOpTime(OplogSlot());
    oplog.setWallClockTime(getServiceContext()->getFastClockSource()->now());
    ASSERT_FALSE(isProgressMarkOplogAfterOplogApplicationStarted({oplog.toBSON()}));
}

TEST_F(ReshardingUtilTest,
       IsNotProgressMarkOplogCreatedAfterOplogApplicationStarted_CreatedAfterNotBoolean) {
    repl::MutableOplogEntry oplog;
    oplog.setNss(nss());
    oplog.setOpType(repl::OpTypeEnum::kNoop);
    oplog.setUuid(UUID::gen());
    oplog.set_id({});
    oplog.setObject({});
    oplog.setObject2(
        BSON(ReshardProgressMarkO2Field::kTypeFieldName
             << resharding::kReshardProgressMarkOpLogType
             << ReshardProgressMarkO2Field::kCreatedAfterOplogApplicationStartedFieldName << 2));
    oplog.setOpTime(OplogSlot());
    oplog.setWallClockTime(getServiceContext()->getFastClockSource()->now());
    ASSERT_FALSE(isProgressMarkOplogAfterOplogApplicationStarted({oplog.toBSON()}));
}

TEST_F(ReshardingUtilTest, CalculateExponentialMovingAverageSmoothingFactorLessThanZero) {
    ASSERT_THROWS_CODE(
        calculateExponentialMovingAverage(0, 1, -0.1), DBException, ErrorCodes::InvalidOptions);
}

TEST_F(ReshardingUtilTest, CalculateExponentialMovingAverageSmoothingFactorEqualToZero) {
    ASSERT_THROWS_CODE(
        calculateExponentialMovingAverage(0, 1, 0), DBException, ErrorCodes::InvalidOptions);
}

TEST_F(ReshardingUtilTest, CalculateExponentialMovingAverageSmoothingFactorEqualToOne) {
    ASSERT_THROWS_CODE(
        calculateExponentialMovingAverage(0, 1, 1), DBException, ErrorCodes::InvalidOptions);
}

TEST_F(ReshardingUtilTest, CalculateExponentialMovingAverageSmoothingFactorEqualGreaterThanOne) {
    ASSERT_THROWS_CODE(
        calculateExponentialMovingAverage(0, 1, 1.1), DBException, ErrorCodes::InvalidOptions);
}

TEST_F(ReshardingUtilTest, CalculateExponentialMovingAverageBasic) {
    auto smoothFactor = 0.75;

    auto val0 = 1;
    auto avg0 = calculateExponentialMovingAverage(0, val0, smoothFactor);
    ASSERT_EQ(avg0, 0.75);

    auto val1 = 10.5;
    auto avg1 = calculateExponentialMovingAverage(avg0, val1, smoothFactor);
    ASSERT_EQ(avg1, (1 - smoothFactor) * avg0 + smoothFactor * val1);

    auto val2 = -0.2;
    auto avg2 = calculateExponentialMovingAverage(avg1, val2, smoothFactor);
    ASSERT_EQ(avg2, (1 - smoothFactor) * avg1 + smoothFactor * val2);

    auto val3 = 0;
    auto avg3 = calculateExponentialMovingAverage(avg2, val3, smoothFactor);
    ASSERT_EQ(avg3, (1 - smoothFactor) * avg2 + smoothFactor * val3);
}

class ReshardingTxnCloningPipelineTest : public AggregationContextFixture {

protected:
    std::pair<std::deque<DocumentSource::GetNextResult>, std::deque<SessionTxnRecord>>
    makeTransactions(size_t numRetryableWrites,
                     size_t numMultiDocTxns,
                     std::function<Timestamp(size_t)> getTimestamp) {
        std::deque<DocumentSource::GetNextResult> mockResults;
        std::deque<SessionTxnRecord>
            expectedTransactions;  // this will hold the expected result for this test
        for (size_t i = 0; i < numRetryableWrites; i++) {
            auto transaction = SessionTxnRecord(
                makeLogicalSessionIdForTest(), 0, repl::OpTime(getTimestamp(i), 0), Date_t());
            mockResults.emplace_back(Document(transaction.toBSON()));
            expectedTransactions.emplace_back(transaction);
        }
        for (size_t i = 0; i < numMultiDocTxns; i++) {
            auto transaction = SessionTxnRecord(makeLogicalSessionIdForTest(),
                                                0,
                                                repl::OpTime(getTimestamp(numMultiDocTxns), 0),
                                                Date_t());
            transaction.setState(DurableTxnStateEnum::kInProgress);
            mockResults.emplace_back(Document(transaction.toBSON()));
            expectedTransactions.emplace_back(transaction);
        }
        std::sort(expectedTransactions.begin(),
                  expectedTransactions.end(),
                  [](SessionTxnRecord a, SessionTxnRecord b) {
                      return a.getSessionId().toBSON().woCompare(b.getSessionId().toBSON()) < 0;
                  });
        return std::pair(mockResults, expectedTransactions);
    }

    std::unique_ptr<Pipeline> constructPipeline(
        std::deque<DocumentSource::GetNextResult> mockResults,
        Timestamp fetchTimestamp,
        boost::optional<LogicalSessionId> startAfter) {
        // create expression context
        static const NamespaceString _transactionsNss =
            NamespaceString::createNamespaceString_forTest("config.transactions");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(
            new ExpressionContextForTest(getOpCtx(), _transactionsNss));
        expCtx->setResolvedNamespace(_transactionsNss, {_transactionsNss, {}});

        auto pipeline =
            createConfigTxnCloningPipelineForResharding(expCtx, fetchTimestamp, startAfter);
        auto mockSource = DocumentSourceMock::createForTest(mockResults, expCtx);
        pipeline->addInitialSource(mockSource);
        return pipeline;
    }

    bool pipelineMatchesDeque(const std::unique_ptr<exec::agg::Pipeline>& execPipeline,
                              const std::deque<SessionTxnRecord>& transactions) {
        auto expected = transactions.begin();
        boost::optional<Document> next;
        for (size_t i = 0; i < transactions.size(); i++) {
            next = execPipeline->getNext();
            if (expected == transactions.end() || !next ||
                !expected->toBSON().binaryEqual(next->toBson())) {
                return false;
            }
            expected++;
        }
        return !execPipeline->getNext() && expected == transactions.end();
    }
};

TEST_F(ReshardingTxnCloningPipelineTest, TxnPipelineSorted) {
    auto [mockResults, expectedTransactions] =
        makeTransactions(10, 10, [](size_t) { return Timestamp::min(); });

    auto pipeline = constructPipeline(mockResults, Timestamp::max(), boost::none);
    auto execPipeline = exec::agg::buildPipeline(pipeline->freeze());

    ASSERT(pipelineMatchesDeque(execPipeline, expectedTransactions));
}

TEST_F(ReshardingTxnCloningPipelineTest, TxnPipelineAfterID) {
    size_t numTransactions = 10;
    auto [mockResults, expectedTransactions] = makeTransactions(
        numTransactions, numTransactions, [](size_t i) { return Timestamp(i + 1, 0); });
    auto middleTransaction = expectedTransactions.begin() + (numTransactions / 2);
    auto middleTransactionSessionId = middleTransaction->getSessionId();
    expectedTransactions.erase(expectedTransactions.begin(), middleTransaction + 1);

    auto pipeline = constructPipeline(mockResults, Timestamp::max(), middleTransactionSessionId);
    auto execPipeline = exec::agg::buildPipeline(pipeline->freeze());

    ASSERT(pipelineMatchesDeque(execPipeline, expectedTransactions));
}

}  // namespace

}  // namespace resharding

}  // namespace mongo
