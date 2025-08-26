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

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/none_t.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache_test_fixture.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace {

using executor::RemoteCommandRequest;

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");

class CatalogCacheRefreshTest : public RouterCatalogCacheTestFixture {
protected:
    void setUp() override {
        RouterCatalogCacheTestFixture::setUp();

        setupNShards(2);
    }

    void expectGetDatabase() {
        expectFindSendBSONObjVector(kConfigHostAndPort, [&]() {
            DatabaseType db(kNss.dbName(), {"0"}, DatabaseVersion(UUID::gen(), Timestamp(1, 1)));
            return std::vector<BSONObj>{db.toBSON()};
        }());
    }

    void expectGetCollection(OID epoch,
                             Timestamp timestamp,
                             const ShardKeyPattern& shardKeyPattern) {
        expectFindSendBSONObjVector(kConfigHostAndPort, [&]() {
            return std::vector<BSONObj>{
                getDefaultCollectionType(epoch, timestamp, shardKeyPattern).toBSON()};
        }());
    }

    void expectCollectionAndChunksAggregationWithReshardingFields(
        OID epoch,
        Timestamp timestamp,
        const ShardKeyPattern& shardKeyPattern,
        UUID reshardingUUID,
        const std::vector<ChunkType>& chunks) {
        expectFindSendBSONObjVector(kConfigHostAndPort, [&]() {
            auto collType = getDefaultCollectionType(epoch, timestamp, shardKeyPattern);

            TypeCollectionReshardingFields reshardingFields;
            reshardingFields.setReshardingUUID(reshardingUUID);
            collType.setReshardingFields(std::move(reshardingFields));

            std::vector<BSONObj> aggResult{collType.toBSON()};
            std::transform(
                chunks.begin(), chunks.end(), std::back_inserter(aggResult), [](const auto& chunk) {
                    return BSON("chunks" << chunk.toConfigBSON());
                });
            return aggResult;
        }());
    }

    CollectionType getDefaultCollectionType(OID epoch,
                                            Timestamp timestamp,
                                            const ShardKeyPattern& shardKeyPattern) {
        return {kNss, epoch, timestamp, Date_t::now(), UUID::gen(), shardKeyPattern.toBSON()};
    }
};

TEST_F(CatalogCacheRefreshTest, FullLoad) {
    const OID epoch = OID::gen();
    const Timestamp timestamp(1);
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));
    const UUID reshardingUUID = UUID::gen();

    auto future = scheduleRoutingInfoUnforcedRefresh(kNss);

    expectGetDatabase();

    ChunkVersion version({epoch, timestamp}, {1, 0});

    ChunkType chunk1(reshardingUUID,
                     {shardKeyPattern.getKeyPattern().globalMin(), BSON("_id" << -100)},
                     version,
                     {"0"});
    chunk1.setName(OID::gen());
    version.incMajor();

    ChunkType chunk2(reshardingUUID, {BSON("_id" << -100), BSON("_id" << 0)}, version, {"1"});
    chunk2.setName(OID::gen());
    version.incMajor();

    ChunkType chunk3(reshardingUUID, {BSON("_id" << 0), BSON("_id" << 100)}, version, {"0"});
    chunk3.setName(OID::gen());
    version.incMajor();

    ChunkType chunk4(reshardingUUID,
                     {BSON("_id" << 100), shardKeyPattern.getKeyPattern().globalMax()},
                     version,
                     {"1"});
    chunk4.setName(OID::gen());
    version.incMajor();

    expectCollectionAndChunksAggregationWithReshardingFields(
        epoch, timestamp, shardKeyPattern, reshardingUUID, {chunk1, chunk2, chunk3, chunk4});

    auto cri = *future.default_timed_get();
    ASSERT(cri.isSharded());
    ASSERT(cri.getChunkManager().isSharded());
    ASSERT_EQ(4, cri.getChunkManager().numChunks());
    ASSERT_EQ(reshardingUUID, cri.getChunkManager().getReshardingFields()->getReshardingUUID());
}

TEST_F(CatalogCacheRefreshTest, NoLoadIfShardNotMarkedStaleInOperationContext) {
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));
    auto initialRoutingInfo(
        makeCollectionRoutingInfo(kNss, shardKeyPattern, nullptr, true, {BSON("_id" << 0)}, {}));
    ASSERT_EQ(2, initialRoutingInfo.getChunkManager().numChunks());

    auto futureNoRefresh = scheduleRoutingInfoUnforcedRefresh(kNss);
    auto cri = *futureNoRefresh.default_timed_get();
    ASSERT(cri.isSharded());
    ASSERT(cri.getChunkManager().isSharded());
    ASSERT_EQ(2, cri.getChunkManager().numChunks());
}

DEATH_TEST_REGEX_F(CatalogCacheRefreshTest,
                   ShouldFailToRefreshWhenLocksAreHeld,
                   "Tripwire assertion.*10271000") {
    Lock::GlobalLock globalLock(operationContext(), MODE_X);
    auto future = scheduleRoutingInfoUnforcedRefresh(kNss);
    ASSERT_THROWS_CODE(future.default_timed_get(), DBException, 10271000);
}

TEST_F(CatalogCacheRefreshTest, DatabaseNotFound) {
    auto future = scheduleRoutingInfoUnforcedRefresh(kNss);

    // Return an empty database (need to return it twice because for missing databases, the
    // CatalogClient tries twice)
    expectFindSendBSONObjVector(kConfigHostAndPort, {});
    expectFindSendBSONObjVector(kConfigHostAndPort, {});

    try {
        auto cri = *future.default_timed_get();
        FAIL(std::string(str::stream() << "Returning no database did not fail and returned "
                                       << cri.getChunkManager().toString()));
    } catch (const DBException& ex) {
        ASSERT_EQ(ErrorCodes::NamespaceNotFound, ex.code());
    }
}

TEST_F(CatalogCacheRefreshTest, DatabaseBSONCorrupted) {
    auto future = scheduleRoutingInfoUnforcedRefresh(kNss);

    // Return a corrupted database entry
    expectFindSendBSONObjVector(
        kConfigHostAndPort,
        {BSON("BadValue" << "This value should not be in a database config document")});

    try {
        auto cri = *future.default_timed_get();
        FAIL(std::string(str::stream()
                         << "Returning corrupted database entry did not fail and returned "
                         << cri.getChunkManager().toString()));
    } catch (const DBException& ex) {
        ASSERT_EQ(ErrorCodes::IDLFailedToParse, ex.code());
    }
}

TEST_F(CatalogCacheRefreshTest, CollectionNotFound) {
    auto future = scheduleRoutingInfoUnforcedRefresh(kNss);

    expectGetDatabase();

    // Return an empty collection
    expectFindSendBSONObjVector(kConfigHostAndPort, {});

    auto cri = *future.default_timed_get();
    ASSERT(!cri.isSharded());
    ASSERT(!cri.getChunkManager().isSharded());
    ASSERT_EQ(ShardId{"0"}, cri.getDbPrimaryShardId());
}

TEST_F(CatalogCacheRefreshTest, CollectionBSONCorrupted) {
    auto future = scheduleRoutingInfoUnforcedRefresh(kNss);

    expectGetDatabase();

    // Return a corrupted collection entry
    expectFindSendBSONObjVector(
        kConfigHostAndPort,
        {BSON("BadValue" << "This value should not be in a collection config document")});

    try {
        auto cri = *future.default_timed_get();
        FAIL(std::string(str::stream()
                         << "Returning corrupted collection entry did not fail and returned "
                         << cri.getChunkManager().toString()));
    } catch (const DBException& ex) {
        ASSERT_EQ(ErrorCodes::IDLFailedToParse, ex.code());
    }
}

TEST_F(CatalogCacheRefreshTest, FullLoadNoChunksFound) {
    const OID epoch = OID::gen();
    const Timestamp timestamp(1);
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

    auto future = scheduleRoutingInfoUnforcedRefresh(kNss);

    expectGetDatabase();

    // Return no chunks three times, which is how frequently the catalog cache retries
    expectFindSendBSONObjVector(kConfigHostAndPort, [&] {
        const auto coll = getDefaultCollectionType(epoch, timestamp, shardKeyPattern);
        return std::vector<BSONObj>{coll.toBSON()};
    }());

    expectFindSendBSONObjVector(kConfigHostAndPort, [&] {
        const auto coll = getDefaultCollectionType(epoch, timestamp, shardKeyPattern);
        return std::vector<BSONObj>{coll.toBSON()};
    }());

    expectFindSendBSONObjVector(kConfigHostAndPort, [&] {
        const auto coll = getDefaultCollectionType(epoch, timestamp, shardKeyPattern);
        return std::vector<BSONObj>{coll.toBSON()};
    }());

    try {
        auto cri = *future.default_timed_get();
        FAIL(std::string(str::stream()
                         << "Returning no chunks for collection did not fail and returned "
                         << cri.getChunkManager().toString()));
    } catch (const DBException& ex) {
        ASSERT_EQ(ErrorCodes::ConflictingOperationInProgress, ex.code());
    }
}

TEST_F(CatalogCacheRefreshTest, IncrementalLoadNoChunksFound) {
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

    auto initialRoutingInfo =
        makeCollectionRoutingInfo(kNss, shardKeyPattern, nullptr, true, {}, {});

    const auto& cm = initialRoutingInfo.getChunkManager();

    const OID epoch = cm.getVersion().epoch();
    const Timestamp timestamp = cm.getVersion().getTimestamp();

    ASSERT_EQ(1, cm.numChunks());

    auto future = scheduleRoutingInfoForcedRefresh(kNss);

    // Return no chunks three times, which is how frequently the catalog cache retries
    expectFindSendBSONObjVector(kConfigHostAndPort, [&] {
        const auto coll = getDefaultCollectionType(epoch, timestamp, shardKeyPattern);
        return std::vector<BSONObj>{coll.toBSON()};
    }());

    expectFindSendBSONObjVector(kConfigHostAndPort, [&] {
        const auto coll = getDefaultCollectionType(epoch, timestamp, shardKeyPattern);
        return std::vector<BSONObj>{coll.toBSON()};
    }());

    expectFindSendBSONObjVector(kConfigHostAndPort, [&] {
        const auto coll = getDefaultCollectionType(epoch, timestamp, shardKeyPattern);
        return std::vector<BSONObj>{coll.toBSON()};
    }());

    try {
        auto cri = *future.default_timed_get();
        FAIL(std::string(str::stream()
                         << "Returning no chunks for collection did not fail and returned "
                         << cri.getChunkManager().toString()));
    } catch (const DBException& ex) {
        ASSERT_EQ(ErrorCodes::ConflictingOperationInProgress, ex.code());
    }
}

TEST_F(CatalogCacheRefreshTest, ChunksBSONCorrupted) {
    const OID epoch = OID::gen();
    const Timestamp timestamp(1);
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

    auto future = scheduleRoutingInfoUnforcedRefresh(kNss);

    expectGetDatabase();

    // Return no chunks three times, which is how frequently the catalog cache retries
    expectFindSendBSONObjVector(kConfigHostAndPort, [&] {
        const auto coll = getDefaultCollectionType(epoch, timestamp, shardKeyPattern);
        const auto chunk1 =
            ChunkType(coll.getUuid(),
                      {shardKeyPattern.getKeyPattern().globalMin(), BSON("_id" << 0)},
                      ChunkVersion({epoch, Timestamp(1, 1)}, {1, 0}),
                      {"0"});
        return std::vector<BSONObj>{
            /* collection */
            coll.toBSON(),
            /* chunks */
            coll.toBSON().addFields(BSON("chunks" << chunk1.toConfigBSON())),
            BSON("BadValue" << "This value should not be in a chunk config document")};
    }());

    try {
        auto cri = *future.default_timed_get();
        FAIL(std::string(str::stream()
                         << "Returning no chunks for collection did not fail and returned "
                         << cri.getChunkManager().toString()));
    } catch (const DBException& ex) {
        ASSERT_EQ(ErrorCodes::NoSuchKey, ex.code());
    }
}

TEST_F(CatalogCacheRefreshTest, FullLoadMissingChunkWithLowestVersion) {
    const OID epoch = OID::gen();
    const UUID uuid = UUID::gen();
    const Timestamp timestamp(1, 1);
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

    auto future = scheduleRoutingInfoUnforcedRefresh(kNss);

    expectGetDatabase();

    const auto incompleteChunks = [&]() {
        ChunkVersion version({epoch, timestamp}, {1, 0});

        // Chunk from (MinKey, -100) is missing (as if someone is dropping the collection
        // concurrently) and has the lowest version.
        version.incMinor();

        ChunkType chunk2(uuid, {BSON("_id" << -100), BSON("_id" << 0)}, version, {"1"});
        chunk2.setName(OID::gen());
        version.incMinor();

        ChunkType chunk3(uuid, {BSON("_id" << 0), BSON("_id" << 100)}, version, {"0"});
        chunk3.setName(OID::gen());
        version.incMinor();

        ChunkType chunk4(uuid,
                         {BSON("_id" << 100), shardKeyPattern.getKeyPattern().globalMax()},
                         version,
                         {"1"});
        chunk4.setName(OID::gen());
        version.incMinor();

        return std::vector<ChunkType>{chunk2, chunk3, chunk4};
    }();

    expectCollectionAndChunksAggregation(
        kNss, epoch, timestamp, uuid, shardKeyPattern, incompleteChunks);

    ASSERT_THROWS_CODE(
        future.default_timed_get(), DBException, ErrorCodes::ChunkMetadataInconsistency);
}

TEST_F(CatalogCacheRefreshTest, FullLoadMissingChunkWithHighestVersion) {
    const OID epoch = OID::gen();
    const UUID uuid = UUID::gen();
    const Timestamp timestamp(1, 1);
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

    auto future = scheduleRoutingInfoUnforcedRefresh(kNss);

    expectGetDatabase();

    const auto incompleteChunks = [&]() {
        ChunkVersion version({epoch, timestamp}, {1, 0});

        // Chunk from (MinKey, -100) is missing (as if someone is dropping the collection
        // concurrently) and has the higest version.
        version.incMinor();

        ChunkType chunk2(uuid, {BSON("_id" << -100), BSON("_id" << 0)}, version, {"1"});
        chunk2.setName(OID::gen());
        version.incMinor();

        ChunkType chunk3(uuid, {BSON("_id" << 0), BSON("_id" << 100)}, version, {"0"});
        chunk3.setName(OID::gen());
        version.incMinor();

        ChunkType chunk4(uuid,
                         {BSON("_id" << 100), shardKeyPattern.getKeyPattern().globalMax()},
                         version,
                         {"1"});
        chunk4.setName(OID::gen());
        version.incMinor();

        return std::vector<ChunkType>{chunk2, chunk3, chunk4};
    }();

    expectCollectionAndChunksAggregation(
        kNss, epoch, timestamp, uuid, shardKeyPattern, incompleteChunks);

    try {
        auto cri = *future.default_timed_get();
        FAIL(std::string(str::stream()
                         << "Returning incomplete chunks for collection did not fail and returned "
                         << cri.getChunkManager().toString()));
    } catch (const DBException& ex) {
        ASSERT_EQ(ErrorCodes::ChunkMetadataInconsistency, ex.code());
    }
}

TEST_F(CatalogCacheRefreshTest, IncrementalLoadMissingChunkWithLowestVersion) {
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

    auto initialRoutingInfo(
        makeCollectionRoutingInfo(kNss, shardKeyPattern, nullptr, true, {}, {}));
    const auto& cm = initialRoutingInfo.getChunkManager();
    const OID epoch = cm.getVersion().epoch();
    const UUID uuid = cm.getUUID();
    const auto timestamp = cm.getVersion().getTimestamp();

    ASSERT_EQ(1, cm.numChunks());

    auto future = scheduleRoutingInfoIncrementalRefresh(kNss);

    const auto incompleteChunks = [&]() {
        ChunkVersion version({epoch, timestamp}, {1, 0});

        // Chunk from (MinKey, -100) is missing (as if someone is dropping the collection
        // concurrently) and has the lowest version.
        version.incMinor();

        ChunkType chunk2(uuid, {BSON("_id" << -100), BSON("_id" << 0)}, version, {"1"});
        chunk2.setName(OID::gen());
        version.incMinor();

        ChunkType chunk3(uuid, {BSON("_id" << 0), BSON("_id" << 100)}, version, {"0"});
        chunk3.setName(OID::gen());
        version.incMinor();

        ChunkType chunk4(uuid,
                         {BSON("_id" << 100), shardKeyPattern.getKeyPattern().globalMax()},
                         version,
                         {"1"});
        chunk4.setName(OID::gen());
        version.incMinor();

        return std::vector<ChunkType>{chunk2, chunk3, chunk4};
    }();

    expectCollectionAndChunksAggregation(
        kNss, epoch, timestamp, UUID::gen(), shardKeyPattern, incompleteChunks);

    try {
        auto cri = *future.default_timed_get();
        FAIL(std::string(str::stream()
                         << "Returning incomplete chunks for collection did not fail and returned "
                         << cri.getChunkManager().toString()));
    } catch (const DBException& ex) {
        ASSERT_EQ(ErrorCodes::ChunkMetadataInconsistency, ex.code());
    }
}

TEST_F(CatalogCacheRefreshTest, IncrementalLoadMissingChunkWithHighestVersion) {
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

    auto initialRoutingInfo(
        makeCollectionRoutingInfo(kNss, shardKeyPattern, nullptr, true, {}, {}));
    const auto& cm = initialRoutingInfo.getChunkManager();

    const OID epoch = cm.getVersion().epoch();
    const UUID uuid = cm.getUUID();
    const auto timestamp = cm.getVersion().getTimestamp();

    ASSERT_EQ(1, cm.numChunks());

    auto future = scheduleRoutingInfoIncrementalRefresh(kNss);

    const auto incompleteChunks = [&]() {
        ChunkVersion version({epoch, timestamp}, {1, 0});

        // Chunk from (MinKey, -100) is missing (as if someone is dropping the collection
        // concurrently) and has the higest version.

        ChunkType chunk2(uuid, {BSON("_id" << -100), BSON("_id" << 0)}, version, {"1"});
        chunk2.setName(OID::gen());
        version.incMinor();

        ChunkType chunk3(uuid, {BSON("_id" << 0), BSON("_id" << 100)}, version, {"0"});
        chunk3.setName(OID::gen());
        version.incMinor();

        ChunkType chunk4(uuid,
                         {BSON("_id" << 100), shardKeyPattern.getKeyPattern().globalMax()},
                         version,
                         {"1"});
        chunk4.setName(OID::gen());
        version.incMinor();

        return std::vector<ChunkType>{chunk2, chunk3, chunk4};
    }();

    expectCollectionAndChunksAggregation(
        kNss, epoch, timestamp, UUID::gen(), shardKeyPattern, incompleteChunks);

    try {
        auto cri = *future.default_timed_get();
        FAIL(std::string(str::stream()
                         << "Returning incomplete chunks for collection did not fail and returned "
                         << cri.getChunkManager().toString()));
    } catch (const DBException& ex) {
        ASSERT_EQ(ErrorCodes::ChunkMetadataInconsistency, ex.code());
    }
}

TEST_F(CatalogCacheRefreshTest, IncrementalLoadAfterCollectionEpochChange) {
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

    auto initialRoutingInfo(
        makeCollectionRoutingInfo(kNss, shardKeyPattern, nullptr, true, {}, {}));
    const auto& initialCm = initialRoutingInfo.getChunkManager();
    ASSERT_EQ(1, initialCm.numChunks());

    setupNShards(2);

    auto future = scheduleRoutingInfoIncrementalRefresh(kNss);

    ChunkVersion oldVersion = initialCm.getVersion();
    ChunkVersion newVersion({OID::gen(), Timestamp(2)}, {1, 0});
    const UUID uuid = initialCm.getUUID();

    // Return collection with a different epoch and a set of chunks, which represent a split
    onFindCommand([&](const RemoteCommandRequest& request) {
        const auto opMsg = static_cast<OpMsgRequest>(request);
        const auto aggRequest =
            unittest::assertGet(aggregation_request_helper::parseFromBSONForTests(opMsg.body));
        const auto& pipeline = aggRequest.getPipeline();

        ASSERT_BSONOBJ_EQ(
            pipeline[1]["$unionWith"]["pipeline"].Array()[1]["$match"]["lastmodEpoch"].Obj(),
            BSON("$eq" << oldVersion.epoch()));
        ASSERT_BSONOBJ_EQ(
            pipeline[2]["$unionWith"]["pipeline"].Array()[1]["$match"]["lastmodEpoch"].Obj(),
            BSON("$ne" << oldVersion.epoch()));

        const auto collBSON =
            getDefaultCollectionType(newVersion.epoch(), newVersion.getTimestamp(), shardKeyPattern)
                .toBSON();

        ChunkType chunk1(uuid,
                         {shardKeyPattern.getKeyPattern().globalMin(), BSON("_id" << 0)},
                         newVersion,
                         {"0"});
        chunk1.setName(OID::gen());
        newVersion.incMinor();

        ChunkType chunk2(uuid,
                         {BSON("_id" << 0), shardKeyPattern.getKeyPattern().globalMax()},
                         newVersion,
                         {"1"});
        chunk2.setName(OID::gen());

        const auto chunk1BSON = BSON("chunks" << chunk1.toConfigBSON());
        const auto chunk2BSON = BSON("chunks" << chunk2.toConfigBSON());
        return std::vector<BSONObj>{collBSON, chunk1BSON, chunk2BSON};
    });

    auto cri = *future.default_timed_get();
    const auto& cm = cri.getChunkManager();
    ASSERT(cm.isSharded());
    ASSERT_EQ(2, cm.numChunks());
    ASSERT_EQ(newVersion, cm.getVersion());
    ASSERT_EQ(ChunkVersion({newVersion.epoch(), newVersion.getTimestamp()}, {1, 0}),
              cm.getVersion({"0"}));
    ASSERT_EQ(ChunkVersion({newVersion.epoch(), newVersion.getTimestamp()}, {1, 1}),
              cm.getVersion({"1"}));
}

TEST_F(CatalogCacheRefreshTest, IncrementalLoadAfterSplit) {
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

    auto initialRoutingInfo(
        makeCollectionRoutingInfo(kNss, shardKeyPattern, nullptr, true, {}, {}));
    const auto& initialCm = initialRoutingInfo.getChunkManager();
    ASSERT_EQ(1, initialCm.numChunks());

    ChunkVersion version = initialCm.getVersion();

    auto future = scheduleRoutingInfoIncrementalRefresh(kNss);

    // Return set of chunks, which represent a split
    onFindCommand([&](const RemoteCommandRequest& request) {
        const auto opMsg = static_cast<OpMsgRequest>(request);
        const auto aggRequest =
            unittest::assertGet(aggregation_request_helper::parseFromBSONForTests(opMsg.body));
        const auto& pipeline = aggRequest.getPipeline();

        ASSERT_BSONOBJ_EQ(
            pipeline[1]["$unionWith"]["pipeline"]
                .Array()[2]["$lookup"]["pipeline"]
                .Array()[1]["$match"]["lastmod"]
                .Obj(),
            BSON("$gte" << Timestamp(version.majorVersion(), version.minorVersion())));

        ASSERT_BSONOBJ_EQ(
            pipeline[1]["$unionWith"]["pipeline"].Array()[1]["$match"]["lastmodEpoch"].Obj(),
            BSON("$eq" << version.epoch()));

        const auto coll =
            getDefaultCollectionType(version.epoch(), version.getTimestamp(), shardKeyPattern);
        const auto collBSON = coll.toBSON();

        version.incMajor();
        ChunkType chunk1(coll.getUuid(),
                         {shardKeyPattern.getKeyPattern().globalMin(), BSON("_id" << 0)},
                         version,
                         {"0"});
        chunk1.setName(OID::gen());

        version.incMinor();
        ChunkType chunk2(coll.getUuid(),
                         {BSON("_id" << 0), shardKeyPattern.getKeyPattern().globalMax()},
                         version,
                         {"0"});
        chunk2.setName(OID::gen());

        const auto chunk1BSON = BSON("chunks" << chunk1.toConfigBSON());
        const auto chunk2BSON = BSON("chunks" << chunk2.toConfigBSON());
        return std::vector<BSONObj>{collBSON, chunk1BSON, chunk2BSON};
    });

    auto cri = *future.default_timed_get();
    const auto& cm = cri.getChunkManager();
    ASSERT(cm.isSharded());
    ASSERT_EQ(2, cm.numChunks());
    ASSERT_EQ(version, cm.getVersion());
    ASSERT_EQ(version, cm.getVersion({"0"}));
    ASSERT_EQ(ChunkVersion({version.epoch(), version.getTimestamp()}, {0, 0}),
              cm.getVersion({"1"}));
}

TEST_F(CatalogCacheRefreshTest, IncrementalLoadAfterMoveWithReshardingFieldsAdded) {
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));
    const UUID reshardingUUID = UUID::gen();

    auto initialRoutingInfo(
        makeCollectionRoutingInfo(kNss, shardKeyPattern, nullptr, true, {BSON("_id" << 0)}, {}));
    const auto& initialCm = initialRoutingInfo.getChunkManager();
    ASSERT_EQ(2, initialCm.numChunks());
    ASSERT(boost::none == initialCm.getReshardingFields());

    ChunkVersion version = initialCm.getVersion();
    const UUID uuid = initialCm.getUUID();

    auto future = scheduleRoutingInfoIncrementalRefresh(kNss);

    ChunkVersion expectedDestPlacementVersion;

    // Return set of chunks, which represent a move
    version.incMajor();
    expectedDestPlacementVersion = version;
    ChunkType chunk1(
        uuid, {shardKeyPattern.getKeyPattern().globalMin(), BSON("_id" << 0)}, version, {"1"});
    chunk1.setName(OID::gen());

    version.incMinor();
    ChunkType chunk2(
        uuid, {BSON("_id" << 0), shardKeyPattern.getKeyPattern().globalMax()}, version, {"0"});
    chunk2.setName(OID::gen());

    expectCollectionAndChunksAggregationWithReshardingFields(
        version.epoch(), version.getTimestamp(), shardKeyPattern, reshardingUUID, {chunk1, chunk2});

    auto cri = *future.default_timed_get();
    const auto& cm = cri.getChunkManager();
    ASSERT(cm.isSharded());
    ASSERT_EQ(2, cm.numChunks());
    ASSERT_EQ(reshardingUUID, cm.getReshardingFields()->getReshardingUUID());
    ASSERT_EQ(version, cm.getVersion());
    ASSERT_EQ(version, cm.getVersion({"0"}));
    ASSERT_EQ(expectedDestPlacementVersion, cm.getVersion({"1"}));
}

TEST_F(CatalogCacheRefreshTest, IncrementalLoadAfterMoveLastChunkWithReshardingFieldsRemoved) {
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));
    const UUID reshardingUUID = UUID::gen();

    TypeCollectionReshardingFields reshardingFields;
    reshardingFields.setReshardingUUID(reshardingUUID);

    auto initialRoutingInfo(
        makeCollectionRoutingInfo(kNss, shardKeyPattern, nullptr, true, {}, reshardingFields));
    const auto& initialCm = initialRoutingInfo.getChunkManager();
    ASSERT_EQ(1, initialCm.numChunks());
    ASSERT_EQ(reshardingUUID, initialCm.getReshardingFields()->getReshardingUUID());

    setupNShards(2);

    ChunkVersion version = initialCm.getVersion();

    auto future = scheduleRoutingInfoIncrementalRefresh(kNss);

    // The collection type won't have resharding fields this time.
    // Return set of chunks, which represent a move
    version.incMajor();
    ChunkType chunk1(
        initialCm.getUUID(),
        {shardKeyPattern.getKeyPattern().globalMin(), shardKeyPattern.getKeyPattern().globalMax()},
        version,
        {"1"});
    chunk1.setName(OID::gen());

    expectCollectionAndChunksAggregation(
        kNss, version.epoch(), version.getTimestamp(), UUID::gen(), shardKeyPattern, {chunk1});

    auto cri = *future.default_timed_get();
    const auto& cm = cri.getChunkManager();
    ASSERT(cm.isSharded());
    ASSERT_EQ(1, cm.numChunks());
    ASSERT_EQ(version, cm.getVersion());
    ASSERT_EQ(ChunkVersion({version.epoch(), version.getTimestamp()}, {0, 0}),
              cm.getVersion({"0"}));
    ASSERT_EQ(version, cm.getVersion({"1"}));
    ASSERT(boost::none == cm.getReshardingFields());
}

}  // namespace
}  // namespace mongo
