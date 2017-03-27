/**
 *    Copyright (C) 2017 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include <set>

#include "mongo/db/query/query_request.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk_manager_test_fixture.h"

namespace mongo {
namespace {

using executor::RemoteCommandRequest;
using unittest::assertGet;

const NamespaceString kNss("TestDB", "TestColl");

class ChunkManagerLoadTest : public ChunkManagerTestFixture {
protected:
    void setUp() override {
        ChunkManagerTestFixture::setUp();

        setupShards([&]() {
            ShardType shard0;
            shard0.setName("0");
            shard0.setHost("Host0:12345");

            ShardType shard1;
            shard1.setName("1");
            shard1.setHost("Host1:12345");

            return std::vector<ShardType>{shard0, shard1};
        }());
    }

    void expectGetDatabase() {
        expectFindOnConfigSendBSONObjVector([&]() {
            DatabaseType db;
            db.setName(kNss.db().toString());
            db.setPrimary({"0"});
            db.setSharded(true);

            return std::vector<BSONObj>{db.toBSON()};
        }());
    }

    void expectGetCollection(OID epoch, const ShardKeyPattern& shardKeyPattern) {
        expectFindOnConfigSendBSONObjVector([&]() {
            CollectionType collType;
            collType.setNs(kNss);
            collType.setEpoch(epoch);
            collType.setKeyPattern(shardKeyPattern.toBSON());
            collType.setUnique(false);

            return std::vector<BSONObj>{collType.toBSON()};
        }());
    }
};

TEST_F(ChunkManagerLoadTest, FullLoad) {
    const OID epoch = OID::gen();
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

    auto future = scheduleRoutingInfoRefresh(kNss);

    expectGetDatabase();
    expectGetCollection(epoch, shardKeyPattern);

    expectGetCollection(epoch, shardKeyPattern);
    expectFindOnConfigSendBSONObjVector([&]() {
        ChunkVersion version(1, 0, epoch);

        ChunkType chunk1(kNss,
                         {shardKeyPattern.getKeyPattern().globalMin(), BSON("_id" << -100)},
                         version,
                         {"0"});
        version.incMinor();

        ChunkType chunk2(kNss, {BSON("_id" << -100), BSON("_id" << 0)}, version, {"1"});
        version.incMinor();

        ChunkType chunk3(kNss, {BSON("_id" << 0), BSON("_id" << 100)}, version, {"0"});
        version.incMinor();

        ChunkType chunk4(kNss,
                         {BSON("_id" << 100), shardKeyPattern.getKeyPattern().globalMax()},
                         version,
                         {"1"});
        version.incMinor();

        return std::vector<BSONObj>{chunk1.toConfigBSON(),
                                    chunk2.toConfigBSON(),
                                    chunk3.toConfigBSON(),
                                    chunk4.toConfigBSON()};
    }());

    auto routingInfo = future.timed_get(kFutureTimeout);
    ASSERT(routingInfo->cm());
    auto cm = routingInfo->cm();

    ASSERT_EQ(4, cm->numChunks());
}

TEST_F(ChunkManagerLoadTest, DatabaseNotFound) {
    auto future = scheduleRoutingInfoRefresh(kNss);

    // Return an empty database (need to return it twice because for missing databases, the
    // CatalogClient tries twice)
    expectFindOnConfigSendBSONObjVector({});
    expectFindOnConfigSendBSONObjVector({});

    try {
        auto routingInfo = future.timed_get(kFutureTimeout);
        auto cm = routingInfo->cm();
        auto primary = routingInfo->primary();

        FAIL(str::stream() << "Returning no database did not fail and returned "
                           << (cm ? cm->toString() : routingInfo->primaryId().toString()));
    } catch (const DBException& ex) {
        ASSERT_EQ(ErrorCodes::NamespaceNotFound, ex.getCode());
    }
}

TEST_F(ChunkManagerLoadTest, CollectionNotFound) {
    auto future = scheduleRoutingInfoRefresh(kNss);

    expectGetDatabase();

    // Return an empty collection
    expectFindOnConfigSendBSONObjVector({});

    auto routingInfo = future.timed_get(kFutureTimeout);
    ASSERT(!routingInfo->cm());
    ASSERT(routingInfo->primary());
    ASSERT_EQ(ShardId{"0"}, routingInfo->primaryId());
}

TEST_F(ChunkManagerLoadTest, NoChunksFoundForCollection) {
    const OID epoch = OID::gen();
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

    auto future = scheduleRoutingInfoRefresh(kNss);

    expectGetDatabase();
    expectGetCollection(epoch, shardKeyPattern);

    // Return no chunks three times, which is how frequently the catalog cache retries
    expectGetCollection(epoch, shardKeyPattern);
    expectFindOnConfigSendBSONObjVector({});

    expectGetCollection(epoch, shardKeyPattern);
    expectFindOnConfigSendBSONObjVector({});

    expectGetCollection(epoch, shardKeyPattern);
    expectFindOnConfigSendBSONObjVector({});

    try {
        auto routingInfo = future.timed_get(kFutureTimeout);
        auto cm = routingInfo->cm();
        auto primary = routingInfo->primary();

        FAIL(str::stream() << "Returning no chunks for collection did not fail and returned "
                           << (cm ? cm->toString() : routingInfo->primaryId().toString()));
    } catch (const DBException& ex) {
        ASSERT_EQ(ErrorCodes::ConflictingOperationInProgress, ex.getCode());
    }
}

TEST_F(ChunkManagerLoadTest, IncompleteChunksFoundForCollection) {
    const OID epoch = OID::gen();
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

    auto future = scheduleRoutingInfoRefresh(kNss);

    expectGetDatabase();
    expectGetCollection(epoch, shardKeyPattern);

    const auto incompleteChunks = [&]() {
        ChunkVersion version(1, 0, epoch);

        // Chunk from (MinKey, -100) is missing (as if someone is dropping the collection
        // concurrently)
        version.incMinor();

        ChunkType chunk2(kNss, {BSON("_id" << -100), BSON("_id" << 0)}, version, {"1"});
        version.incMinor();

        ChunkType chunk3(kNss, {BSON("_id" << 0), BSON("_id" << 100)}, version, {"0"});
        version.incMinor();

        ChunkType chunk4(kNss,
                         {BSON("_id" << 100), shardKeyPattern.getKeyPattern().globalMax()},
                         version,
                         {"1"});
        version.incMinor();

        return std::vector<BSONObj>{
            chunk2.toConfigBSON(), chunk3.toConfigBSON(), chunk4.toConfigBSON()};
    }();

    // Return incomplete set of chunks three times, which is how frequently the catalog cache
    // retries
    expectGetCollection(epoch, shardKeyPattern);
    expectFindOnConfigSendBSONObjVector(incompleteChunks);

    expectGetCollection(epoch, shardKeyPattern);
    expectFindOnConfigSendBSONObjVector(incompleteChunks);

    expectGetCollection(epoch, shardKeyPattern);
    expectFindOnConfigSendBSONObjVector(incompleteChunks);

    try {
        auto routingInfo = future.timed_get(kFutureTimeout);
        auto cm = routingInfo->cm();
        auto primary = routingInfo->primary();

        FAIL(
            str::stream() << "Returning incomplete chunks for collection did not fail and returned "
                          << (cm ? cm->toString() : routingInfo->primaryId().toString()));
    } catch (const DBException& ex) {
        ASSERT_EQ(ErrorCodes::ConflictingOperationInProgress, ex.getCode());
    }
}

TEST_F(ChunkManagerLoadTest, ChunkEpochChangeDuringIncrementalLoad) {
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

    auto initialRoutingInfo(makeChunkManager(kNss, shardKeyPattern, nullptr, true, {}));
    ASSERT_EQ(1, initialRoutingInfo->numChunks());

    auto future = scheduleRoutingInfoRefresh(kNss);

    ChunkVersion version = initialRoutingInfo->getVersion();

    const auto inconsistentChunks = [&]() {
        version.incMajor();
        ChunkType chunk1(
            kNss, {shardKeyPattern.getKeyPattern().globalMin(), BSON("_id" << 0)}, version, {"0"});

        ChunkType chunk2(kNss,
                         {BSON("_id" << 0), shardKeyPattern.getKeyPattern().globalMax()},
                         ChunkVersion(1, 0, OID::gen()),
                         {"0"});

        return std::vector<BSONObj>{chunk1.toConfigBSON(), chunk2.toConfigBSON()};
    }();

    // Return set of chunks, one of which has different epoch. Do it three times, which is how
    // frequently the catalog cache retries.
    expectGetCollection(initialRoutingInfo->getVersion().epoch(), shardKeyPattern);
    expectFindOnConfigSendBSONObjVector(inconsistentChunks);

    expectGetCollection(initialRoutingInfo->getVersion().epoch(), shardKeyPattern);
    expectFindOnConfigSendBSONObjVector(inconsistentChunks);

    expectGetCollection(initialRoutingInfo->getVersion().epoch(), shardKeyPattern);
    expectFindOnConfigSendBSONObjVector(inconsistentChunks);

    try {
        auto routingInfo = future.timed_get(kFutureTimeout);
        auto cm = routingInfo->cm();
        auto primary = routingInfo->primary();

        FAIL(str::stream()
             << "Returning chunks with different epoch for collection did not fail and returned "
             << (cm ? cm->toString() : routingInfo->primaryId().toString()));
    } catch (const DBException& ex) {
        ASSERT_EQ(ErrorCodes::ConflictingOperationInProgress, ex.getCode());
    }
}

TEST_F(ChunkManagerLoadTest, IncrementalLoadAfterSplit) {
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

    auto initialRoutingInfo(makeChunkManager(kNss, shardKeyPattern, nullptr, true, {}));
    ASSERT_EQ(1, initialRoutingInfo->numChunks());

    ChunkVersion version = initialRoutingInfo->getVersion();

    auto future = scheduleRoutingInfoRefresh(kNss);

    expectGetCollection(version.epoch(), shardKeyPattern);

    // Return set of chunks, which represent a split
    onFindCommand([&](const RemoteCommandRequest& request) {
        // Ensure it is a differential query
        const auto diffQuery =
            assertGet(QueryRequest::makeFromFindCommand(kNss, request.cmdObj, false));
        ASSERT_BSONOBJ_EQ(
            BSON("ns" << kNss.ns() << "lastmod"
                      << BSON("$gte" << Timestamp(version.majorVersion(), version.minorVersion()))),
            diffQuery->getFilter());

        version.incMajor();
        ChunkType chunk1(
            kNss, {shardKeyPattern.getKeyPattern().globalMin(), BSON("_id" << 0)}, version, {"0"});

        version.incMinor();
        ChunkType chunk2(
            kNss, {BSON("_id" << 0), shardKeyPattern.getKeyPattern().globalMax()}, version, {"0"});

        return std::vector<BSONObj>{chunk1.toConfigBSON(), chunk2.toConfigBSON()};
    });

    auto routingInfo = future.timed_get(kFutureTimeout);
    ASSERT(routingInfo->cm());
    auto cm = routingInfo->cm();

    ASSERT_EQ(2, cm->numChunks());
    ASSERT_EQ(version, cm->getVersion());
    ASSERT_EQ(version, cm->getVersion({"0"}));
    ASSERT_EQ(ChunkVersion(0, 0, version.epoch()), cm->getVersion({"1"}));
}

TEST_F(ChunkManagerLoadTest, IncrementalLoadAfterMove) {
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

    auto initialRoutingInfo(
        makeChunkManager(kNss, shardKeyPattern, nullptr, true, {BSON("_id" << 0)}));
    ASSERT_EQ(2, initialRoutingInfo->numChunks());

    ChunkVersion version = initialRoutingInfo->getVersion();

    auto future = scheduleRoutingInfoRefresh(kNss);

    ChunkVersion expectedDestShardVersion;

    expectGetCollection(version.epoch(), shardKeyPattern);

    // Return set of chunks, which represent a move
    expectFindOnConfigSendBSONObjVector([&]() {
        version.incMajor();
        expectedDestShardVersion = version;
        ChunkType chunk1(
            kNss, {shardKeyPattern.getKeyPattern().globalMin(), BSON("_id" << 0)}, version, {"1"});

        version.incMinor();
        ChunkType chunk2(
            kNss, {BSON("_id" << 0), shardKeyPattern.getKeyPattern().globalMax()}, version, {"0"});

        return std::vector<BSONObj>{chunk1.toConfigBSON(), chunk2.toConfigBSON()};
    }());

    auto routingInfo = future.timed_get(kFutureTimeout);
    ASSERT(routingInfo->cm());
    auto cm = routingInfo->cm();

    ASSERT_EQ(2, cm->numChunks());
    ASSERT_EQ(version, cm->getVersion());
    ASSERT_EQ(version, cm->getVersion({"0"}));
    ASSERT_EQ(expectedDestShardVersion, cm->getVersion({"1"}));
}

TEST_F(ChunkManagerLoadTest, IncrementalLoadAfterMoveLastChunk) {
    const ShardKeyPattern shardKeyPattern(BSON("_id" << 1));

    auto initialRoutingInfo(makeChunkManager(kNss, shardKeyPattern, nullptr, true, {}));
    ASSERT_EQ(1, initialRoutingInfo->numChunks());

    ChunkVersion version = initialRoutingInfo->getVersion();

    auto future = scheduleRoutingInfoRefresh(kNss);

    expectGetCollection(version.epoch(), shardKeyPattern);

    // Return set of chunks, which represent a move
    expectFindOnConfigSendBSONObjVector([&]() {
        version.incMajor();
        ChunkType chunk1(kNss,
                         {shardKeyPattern.getKeyPattern().globalMin(),
                          shardKeyPattern.getKeyPattern().globalMax()},
                         version,
                         {"1"});

        return std::vector<BSONObj>{chunk1.toConfigBSON()};
    }());

    expectFindOnConfigSendBSONObjVector([&]() {
        ShardType shard1;
        shard1.setName("0");
        shard1.setHost(str::stream() << "Host0:12345");

        ShardType shard2;
        shard2.setName("1");
        shard2.setHost(str::stream() << "Host1:12345");

        return std::vector<BSONObj>{shard1.toBSON(), shard2.toBSON()};
    }());

    auto routingInfo = future.timed_get(kFutureTimeout);
    ASSERT(routingInfo->cm());
    auto cm = routingInfo->cm();

    ASSERT_EQ(1, cm->numChunks());
    ASSERT_EQ(version, cm->getVersion());
    ASSERT_EQ(ChunkVersion(0, 0, version.epoch()), cm->getVersion({"0"}));
    ASSERT_EQ(version, cm->getVersion({"1"}));
}

}  // namespace
}  // namespace mongo
