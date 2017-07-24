/*    Copyright 2017 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include <boost/optional/optional_io.hpp>

#include "mongo/db/s/split_chunk.h"

#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/json.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/catalog/dist_lock_manager_mock.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_server_test_fixture.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;

class SplitChunkTest : public ShardServerTestFixture {
public:
    void setUp() override {

        ShardServerTestFixture::setUp();

        // Initialize the CatalogCache so that shard server metadata refreshes will work.
        catalogCache()->initializeReplicaSetRole(true);

        // Instantiate names.
        _epoch = OID::gen();

        _shardId = ShardId("shardId");
        _nss = NamespaceString(StringData("dbName"), StringData("collName"));

        // Set up the databases collection
        _db.setName("dbName");
        _db.setPrimary(_shardId.toString());
        _db.setSharded(true);
        ASSERT_OK(_db.validate());

        // Set up the collections collection
        _coll.setNs(_nss);
        _coll.setEpoch(_epoch);
        _coll.setUpdatedAt(Date_t::fromMillisSinceEpoch(ChunkVersion(1, 3, _epoch).toLong()));
        _coll.setKeyPattern(BSON("_id" << 1));
        _coll.setUnique(false);
        ASSERT_OK(_coll.validate());

        // Set up the shard
        _shard.setName(_shardId.toString());
        _shard.setHost("TestHost1");
        ASSERT_OK(_shard.validate());

        setUpChunkRanges();
        setUpChunkVersions();
    }

    /**
     * Tells the DistLockManagerMock instance to expect a lock, and logs the corresponding
     * information.
     */
    void expectLock();

    /**
     * Returns the mock response that correspond with particular requests. For example, dbResponse
     * returns a response for when a find-databases request occurs.
     *
     * commitChunkSplitResponse    : responds with { "ok" : 1 } or { "ok" : 0 }
     * dbResponse    : responds with vector containing _db.toBSON()
     * collResponse  : responds with vector containing _coll.toBSON()
     * shardResponse : responds with vector containing _shard.toBSON()
     * chunkResponse : responds with vector containing all every chunk.toConfigBSON()
     * emptyResponse : responds with empty vector
     */
    void commitChunkSplitResponse(bool isOk);
    void dbResponse();
    void collResponse();
    void shardResponse();
    void chunkResponse();
    void emptyResponse();

protected:
    /**
     * Helper functions to return vectors of basic chunk ranges, chunk versions to
     * be used by some of the tests.
     */
    void setUpChunkRanges();
    void setUpChunkVersions();

    OID _epoch;

    NamespaceString _nss;
    ShardId _shardId;

    DatabaseType _db;
    CollectionType _coll;
    ShardType _shard;

    std::vector<ChunkRange> _chunkRanges;
    std::vector<ChunkVersion> _chunkVersions;
};

void SplitChunkTest::setUpChunkRanges() {
    BSONObjBuilder minKeyBuilder;
    BSONObjBuilder maxKeyBuilder;
    minKeyBuilder.appendMinKey("foo");
    maxKeyBuilder.appendMaxKey("foo");

    const BSONObj key1 = minKeyBuilder.obj();
    const BSONObj key2 = BSON("foo" << 0);
    const BSONObj key3 = BSON("foo" << 1024);
    const BSONObj key4 = maxKeyBuilder.obj();

    _chunkRanges.push_back(ChunkRange(key1, key2));
    _chunkRanges.push_back(ChunkRange(key2, key3));
    _chunkRanges.push_back(ChunkRange(key3, key4));
}

void SplitChunkTest::setUpChunkVersions() {
    _chunkVersions = {
        ChunkVersion(1, 1, _epoch), ChunkVersion(1, 2, _epoch), ChunkVersion(1, 3, _epoch)};
}

void SplitChunkTest::expectLock() {
    dynamic_cast<DistLockManagerMock*>(distLock())
        ->expectLock(
            [this](StringData name, StringData whyMessage, Milliseconds) {
                LOG(0) << name;
                LOG(0) << whyMessage;
            },
            Status::OK());
}

void SplitChunkTest::commitChunkSplitResponse(bool isOk) {
    onCommand([&](const RemoteCommandRequest& request) {
        return isOk ? BSON("ok" << 1) : BSON("ok" << 0);
    });
}

void SplitChunkTest::dbResponse() {
    onFindCommand(
        [&](const RemoteCommandRequest& request) { return std::vector<BSONObj>{_db.toBSON()}; });
}

void SplitChunkTest::collResponse() {
    onFindCommand(
        [&](const RemoteCommandRequest& request) { return std::vector<BSONObj>{_coll.toBSON()}; });
}

void SplitChunkTest::shardResponse() {
    onFindCommand(
        [&](const RemoteCommandRequest& request) { return std::vector<BSONObj>{_shard.toBSON()}; });
}

void SplitChunkTest::chunkResponse() {
    onFindCommand([&](const RemoteCommandRequest& request) {
        std::vector<BSONObj> response;
        for (unsigned long i = 0; i < _chunkRanges.size(); ++i) {
            ChunkType chunk(_nss, _chunkRanges[i], _chunkVersions[i], _shardId);
            response.push_back(chunk.toConfigBSON());
        }
        return response;
    });
}

void SplitChunkTest::emptyResponse() {
    onFindCommand([&](const RemoteCommandRequest& request) { return std::vector<BSONObj>(); });
}

TEST_F(SplitChunkTest, HashedKeyPatternNumberLongSplitKeys) {

    BSONObj keyPatternObj = BSON("foo"
                                 << "hashed");
    _coll.setKeyPattern(BSON("_id"
                             << "hashed"));

    // Build a vector of valid split keys, which are values of NumberLong types.
    std::vector<BSONObj> validSplitKeys;
    for (long long i = 256; i <= 1024; i += 256) {
        validSplitKeys.push_back(BSON("foo" << i));
    }

    // Force-set the sharding state to enabled with the _shardId, for testing purposes.
    ShardingState::get(operationContext())->setEnabledForTest(_shardId.toString());

    expectLock();

    // Call the splitChunk function asynchronously on a different thread, so that we do not block,
    // and so we can construct the mock responses to requests made by splitChunk below.
    auto future = launchAsync([&] {
        auto statusWithOptionalChunkRange = splitChunk(operationContext(),
                                                       _nss,
                                                       keyPatternObj,
                                                       _chunkRanges[1],
                                                       validSplitKeys,
                                                       _shardId.toString(),
                                                       _epoch);
        ASSERT_OK(statusWithOptionalChunkRange.getStatus());
    });

    // Here, we mock responses to the requests made by the splitChunk operation. The requests first
    // do a find on the databases, then a find on all collections in the database we are looking
    // for. Next, filter by the specific collection, and find the relevant chunks and shards.
    dbResponse();
    collResponse();
    collResponse();
    chunkResponse();
    shardResponse();

    // Because we provided valid split points, the config server should respond with { "ok" : 1 }.
    commitChunkSplitResponse(true);

    // Finally, we find the original collection, and then find the relevant chunks.
    collResponse();
    chunkResponse();

    future.timed_get(kFutureTimeout);
}

TEST_F(SplitChunkTest, HashedKeyPatternIntegerSplitKeys) {

    BSONObj keyPatternObj = BSON("foo"
                                 << "hashed");
    _coll.setKeyPattern(BSON("_id"
                             << "hashed"));

    // Build a vector of valid split keys, which contains values that may not necessarily be able
    // to be converted to NumberLong types.
    std::vector<BSONObj> invalidSplitKeys{
        BSON("foo" << -1), BSON("foo" << 0), BSON("foo" << 1), BSON("foo" << 42)};

    // Force-set the sharding state to enabled with the _shardId, for testing purposes.
    ShardingState::get(operationContext())->setEnabledForTest(_shardId.toString());

    expectLock();

    // Call the splitChunk function asynchronously on a different thread, so that we do not block,
    // and so we can construct the mock responses to requests made by splitChunk below.
    auto future = launchAsync([&] {
        auto statusWithOptionalChunkRange = splitChunk(operationContext(),
                                                       _nss,
                                                       keyPatternObj,
                                                       _chunkRanges[1],
                                                       invalidSplitKeys,
                                                       _shardId.toString(),
                                                       _epoch);
        ASSERT_EQUALS(ErrorCodes::CannotSplit, statusWithOptionalChunkRange.getStatus());
    });

    // Here, we mock responses to the requests made by the splitChunk operation. The requests first
    // do a find on the databases, then a find on all collections in the database we are looking
    // for. Next, filter by the specific collection, and find the relevant chunks and shards.
    dbResponse();
    collResponse();
    collResponse();
    chunkResponse();
    shardResponse();

    future.timed_get(kFutureTimeout);
}

TEST_F(SplitChunkTest, HashedKeyPatternDoubleSplitKeys) {

    BSONObj keyPatternObj = BSON("foo"
                                 << "hashed");
    _coll.setKeyPattern(BSON("_id"
                             << "hashed"));

    // Build a vector of valid split keys, which contains values that may not necessarily be able
    // to be converted to NumberLong types.
    std::vector<BSONObj> invalidSplitKeys{
        BSON("foo" << 47.21230129), BSON("foo" << 1.0), BSON("foo" << 0.0), BSON("foo" << -0.001)};

    // Force-set the sharding state to enabled with the _shardId, for testing purposes.
    ShardingState::get(operationContext())->setEnabledForTest(_shardId.toString());

    expectLock();

    // Call the splitChunk function asynchronously on a different thread, so that we do not block,
    // and so we can construct the mock responses to requests made by splitChunk below.
    auto future = launchAsync([&] {
        auto statusWithOptionalChunkRange = splitChunk(operationContext(),
                                                       _nss,
                                                       keyPatternObj,
                                                       _chunkRanges[1],
                                                       invalidSplitKeys,
                                                       _shardId.toString(),
                                                       _epoch);
        ASSERT_EQUALS(ErrorCodes::CannotSplit, statusWithOptionalChunkRange.getStatus());
    });

    // Here, we mock responses to the requests made by the splitChunk operation. The requests first
    // do a find on the databases, then a find on all collections in the database we are looking
    // for. Next, filter by the specific collection, and find the relevant chunks and shards.
    dbResponse();
    collResponse();
    collResponse();
    chunkResponse();
    shardResponse();

    future.timed_get(kFutureTimeout);
}

TEST_F(SplitChunkTest, HashedKeyPatternStringSplitKeys) {

    BSONObj keyPatternObj = BSON("foo"
                                 << "hashed");
    _coll.setKeyPattern(BSON("_id"
                             << "hashed"));

    // Build a vector of valid split keys, which contains values that may not necessarily be able
    // to be converted to NumberLong types.
    std::vector<BSONObj> invalidSplitKeys{BSON("foo"
                                               << "@&(9@*88+_241(/.*@8uuDU@(9];a;s;]3"),
                                          BSON("foo"
                                               << "string"),
                                          BSON("foo"
                                               << "14.13289"),
                                          BSON("foo"
                                               << "")};

    // Force-set the sharding state to enabled with the _shardId, for testing purposes.
    ShardingState::get(operationContext())->setEnabledForTest(_shardId.toString());

    expectLock();

    // Call the splitChunk function asynchronously on a different thread, so that we do not block,
    // and so we can construct the mock responses to requests made by splitChunk below.
    auto future = launchAsync([&] {
        auto statusWithOptionalChunkRange = splitChunk(operationContext(),
                                                       _nss,
                                                       keyPatternObj,
                                                       _chunkRanges[1],
                                                       invalidSplitKeys,
                                                       _shardId.toString(),
                                                       _epoch);
        ASSERT_EQUALS(ErrorCodes::CannotSplit, statusWithOptionalChunkRange.getStatus());
    });

    // Here, we mock responses to the requests made by the splitChunk operation. The requests first
    // do a find on the databases, then a find on all collections in the database we are looking
    // for. Next, filter by the specific collection, and find the relevant chunks and shards.
    dbResponse();
    collResponse();
    collResponse();
    chunkResponse();
    shardResponse();

    future.timed_get(kFutureTimeout);
}

TEST_F(SplitChunkTest, ValidRangeKeyPatternSplitKeys) {

    BSONObj keyPatternObj = BSON("foo" << 1);

    // Build a vector of valid split keys, which contains values that may not necessarily be able
    // be converted to NumberLong types. However, this does not matter since we are not using a
    // hashed shard key pattern.
    std::vector<BSONObj> validSplitKeys{BSON("foo" << 20),
                                        BSON("foo" << 512),
                                        BSON("foo"
                                             << "hello"),
                                        BSON("foo"
                                             << ""),
                                        BSON("foo" << 3.1415926535)};

    // Force-set the sharding state to enabled with the _shardId, for testing purposes.
    ShardingState::get(operationContext())->setEnabledForTest(_shardId.toString());

    expectLock();

    // Call the splitChunk function asynchronously on a different thread, so that we do not block,
    // and so we can construct the mock responses to requests made by splitChunk below.
    auto future = launchAsync([&] {
        auto statusWithOptionalChunkRange = splitChunk(operationContext(),
                                                       _nss,
                                                       keyPatternObj,
                                                       _chunkRanges[1],
                                                       validSplitKeys,
                                                       _shardId.toString(),
                                                       _epoch);
        ASSERT_OK(statusWithOptionalChunkRange.getStatus());
    });

    // Here, we mock responses to the requests made by the splitChunk operation. The requests first
    // do a find on the databases, then a find on all collections in the database we are looking
    // for. Next, filter by the specific collection, and find the relevant chunks and shards.
    dbResponse();
    collResponse();
    collResponse();
    chunkResponse();
    shardResponse();

    // Because we provided valid split points, the config server should respond with { "ok" : 1 }.
    commitChunkSplitResponse(true);

    // Finally, we find the original collection, and then find the relevant chunks.
    collResponse();
    chunkResponse();

    future.timed_get(kFutureTimeout);
}

TEST_F(SplitChunkTest, SplitChunkWithNoErrors) {

    BSONObj keyPatternObj = BSON("foo" << 1);

    // Build a vector of split keys. Note that we start at {"foo" : 256} and end at {"foo" : 768},
    // neither of which are boundary points.
    std::vector<BSONObj> splitKeys;
    for (int i = 256; i < 1024; i += 256) {
        splitKeys.push_back(BSON("foo" << i));
    }

    // Force-set the sharding state to enabled with the _shardId, for testing purposes.
    ShardingState::get(operationContext())->setEnabledForTest(_shardId.toString());

    expectLock();

    // Call the splitChunk function asynchronously on a different thread, so that we do not block,
    // and so we can construct the mock responses to requests made by splitChunk below.
    auto future = launchAsync([&] {
        auto statusWithOptionalChunkRange = splitChunk(operationContext(),
                                                       _nss,
                                                       keyPatternObj,
                                                       _chunkRanges[1],
                                                       splitKeys,
                                                       _shardId.toString(),
                                                       _epoch);
        ASSERT_OK(statusWithOptionalChunkRange.getStatus());
    });

    // Here, we mock responses to the requests made by the splitChunk operation. The requests first
    // do a find on the databases, then a find on all collections in the database we are looking
    // for. Next, filter by the specific collection, and find the relevant chunks and shards.
    dbResponse();
    collResponse();
    collResponse();
    chunkResponse();
    shardResponse();

    // Mock an OK response to the request to the config server regarding the chunk split, but first
    // check the request parameters.
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(HostAndPort("dummy", 123), request.target);
        ASSERT_BSONOBJ_EQ(request.cmdObj["min"].Obj(), BSON("foo" << 0));
        ASSERT_BSONOBJ_EQ(request.cmdObj["max"].Obj(), BSON("foo" << 1024));

        // Check that the split points in the request are the same as the split keys that were
        // initially passed to the splitChunk function.
        std::vector<BSONElement> splitPoints = request.cmdObj["splitPoints"].Array();
        ASSERT_EQ(splitKeys.size(), splitPoints.size());
        int i = 0;
        for (auto e : splitPoints) {
            ASSERT(e.Obj().woCompare(splitKeys[i]) == 0);
            i++;
        }

        return BSON("ok" << 1);
    });

    // Finally, we find the original collection, and then find the relevant chunks.
    collResponse();
    chunkResponse();

    future.timed_get(kFutureTimeout);
}

TEST_F(SplitChunkTest, AttemptSplitWithConfigsvrError) {

    BSONObj keyPatternObj = BSON("foo" << 1);

    // Build a vector of split keys. Note that we start at {"foo" : 0} and end at {"foo" : 1024},
    // both of which are boundary points.
    std::vector<BSONObj> splitKeys;
    for (int i = 0; i <= 1024; i += 256) {
        splitKeys.push_back(BSON("foo" << i));
    }

    // Force-set the sharding state to enabled with the _shardId, for testing purposes.
    ShardingState::get(operationContext())->setEnabledForTest(_shardId.toString());

    expectLock();

    // Call the splitChunk function asynchronously on a different thread, so that we do not block,
    // and so we can construct the mock responses to requests made by splitChunk below.
    auto future = launchAsync([&] {
        auto statusWithOptionalChunkRange = splitChunk(operationContext(),
                                                       _nss,
                                                       keyPatternObj,
                                                       _chunkRanges[1],
                                                       splitKeys,
                                                       _shardId.toString(),
                                                       _epoch);
        ASSERT_NOT_OK(statusWithOptionalChunkRange.getStatus());
    });

    // Here, we mock responses to the requests made by the splitChunk operation. The requests first
    // do a find on the databases, then a find on all collections in the database we are looking
    // for. Next, filter by the specific collection, and find the relevant chunks and shards.
    dbResponse();
    collResponse();
    collResponse();
    chunkResponse();
    shardResponse();

    // Because we provided invalid split points, the config server should respond with { "ok" : 0 }.
    commitChunkSplitResponse(false);

    // Finally, we find the original collection, and then find the relevant chunks.
    collResponse();
    chunkResponse();

    future.timed_get(kFutureTimeout);
}

TEST_F(SplitChunkTest, AttemptSplitOnNoDatabases) {

    BSONObj keyPatternObj = BSON("foo" << 1);

    // Build a vector of split keys. Note that we start at {"foo" : 256} and end at {"foo" : 768},
    // neither of which are boundary points.
    std::vector<BSONObj> splitKeys;
    for (int i = 256; i < 1024; i += 256) {
        splitKeys.push_back(BSON("foo" << i));
    }

    // Force-set the sharding state to enabled with the _shardId, for testing purposes.
    ShardingState::get(operationContext())->setEnabledForTest(_shardId.toString());

    expectLock();

    // Call the splitChunk function asynchronously on a different thread, so that we do not block,
    // and so we can construct the mock responses to requests made by splitChunk below.
    auto future = launchAsync([&] {
        auto statusWithOptionalChunkRange = splitChunk(operationContext(),
                                                       _nss,
                                                       keyPatternObj,
                                                       _chunkRanges[1],
                                                       splitKeys,
                                                       _shardId.toString(),
                                                       _epoch);
        ASSERT_NOT_OK(statusWithOptionalChunkRange.getStatus());
    });

    // Here, we mock responses to the requests made by the splitChunk operation. We give an empty
    // response to the request that finds the database, along with the request that finds all
    // collections in the database.
    emptyResponse();
    emptyResponse();

    future.timed_get(kFutureTimeout);
}

TEST_F(SplitChunkTest, AttemptSplitOnNoCollections) {

    BSONObj keyPatternObj = BSON("foo" << 1);

    // Build a vector of split keys. Note that we start at {"foo" : 256} and end at {"foo" : 768},
    // neither of which are boundary points.
    std::vector<BSONObj> splitKeys;
    for (int i = 256; i < 1024; i += 256) {
        splitKeys.push_back(BSON("foo" << i));
    }

    // Force-set the sharding state to enabled with the _shardId, for testing purposes.
    ShardingState::get(operationContext())->setEnabledForTest(_shardId.toString());

    expectLock();

    // Call the splitChunk function asynchronously on a different thread, so that we do not block,
    // and so we can construct the mock responses to requests made by splitChunk below.
    auto future = launchAsync([&] {
        auto statusWithOptionalChunkRange = splitChunk(operationContext(),
                                                       _nss,
                                                       keyPatternObj,
                                                       _chunkRanges[1],
                                                       splitKeys,
                                                       _shardId.toString(),
                                                       _epoch);
        ASSERT_NOT_OK(statusWithOptionalChunkRange.getStatus());
    });

    // Here, we mock responses to the requests made by the splitChunk operation. We first respond
    // to the request finding the databases.
    dbResponse();

    // Next, we give an empty response to the request for finding collections in the database,
    // followed by a response to the request for relevant shards.
    emptyResponse();
    shardResponse();

    future.timed_get(kFutureTimeout);
}

TEST_F(SplitChunkTest, AttemptSplitOnNoChunks) {

    BSONObj keyPatternObj = BSON("foo" << 1);

    // Build a vector of split keys. Note that we start at {"foo" : 256} and end at {"foo" : 768},
    // neither of which are boundary points.
    std::vector<BSONObj> splitKeys;
    for (int i = 256; i < 1024; i += 256) {
        splitKeys.push_back(BSON("foo" << i));
    }

    // Force-set the sharding state to enabled with the _shardId, for testing purposes.
    ShardingState::get(operationContext())->setEnabledForTest(_shardId.toString());

    expectLock();

    // Call the splitChunk function asynchronously on a different thread, so that we do not block,
    // and so we can construct the mock responses to requests made by splitChunk below.
    auto future = launchAsync([&] {
        auto statusWithOptionalChunkRange = splitChunk(operationContext(),
                                                       _nss,
                                                       keyPatternObj,
                                                       _chunkRanges[1],
                                                       splitKeys,
                                                       _shardId.toString(),
                                                       _epoch);
        ASSERT_NOT_OK(statusWithOptionalChunkRange.getStatus());
    });

    // Here, we mock responses to the requests made by the splitChunk operation. The requests first
    // do a find on the databases, then a find on all collections in the database we are looking
    // for.
    dbResponse();
    collResponse();

    // We attempt to find the relevant chunks three times. For each of these times, we will respond
    // with the relevant collection, but no chunks.
    collResponse();
    emptyResponse();
    collResponse();
    emptyResponse();
    collResponse();
    emptyResponse();

    future.timed_get(kFutureTimeout);
}

TEST_F(SplitChunkTest, NoCollectionAfterSplit) {

    BSONObj keyPatternObj = BSON("foo" << 1);

    // Build a vector of split keys. Note that we start at {"foo" : 256} and end at {"foo" : 768},
    // neither of which are boundary points.
    std::vector<BSONObj> splitKeys;
    for (int i = 256; i < 1024; i += 256) {
        splitKeys.push_back(BSON("foo" << i));
    }

    // Force-set the sharding state to enabled with the _shardId, for testing purposes.
    ShardingState::get(operationContext())->setEnabledForTest(_shardId.toString());

    expectLock();

    // Call the splitChunk function asynchronously on a different thread, so that we do not block,
    // and so we can construct the mock responses to requests made by splitChunk below.
    auto future = launchAsync([&] {
        auto statusWithOptionalChunkRange = splitChunk(operationContext(),
                                                       _nss,
                                                       keyPatternObj,
                                                       _chunkRanges[1],
                                                       splitKeys,
                                                       _shardId.toString(),
                                                       _epoch);
        ASSERT_OK(statusWithOptionalChunkRange.getStatus());
    });

    // Here, we mock responses to the requests made by the splitChunk operation. The requests first
    // do a find on the databases, then a find on all collections in the database we are looking
    // for. Next, filter by the specific collection, and find the relevant chunks and shards.
    dbResponse();
    collResponse();
    collResponse();
    chunkResponse();
    shardResponse();

    // Here, we mock a successful response from the config server, which denotes that the split was
    // successful on the config server's end.
    commitChunkSplitResponse(true);

    // Finally, give an empty response to a request regarding a find on the original collection.
    emptyResponse();

    future.timed_get(kFutureTimeout);
}

TEST_F(SplitChunkTest, NoChunksAfterSplit) {

    BSONObj keyPatternObj = BSON("foo" << 1);

    // Build a vector of split keys. Note that we start at {"foo" : 256} and end at {"foo" : 768},
    // neither of which are boundary points.
    std::vector<BSONObj> splitKeys;
    for (int i = 256; i < 1024; i += 256) {
        splitKeys.push_back(BSON("foo" << i));
    }

    // Force-set the sharding state to enabled with the _shardId, for testing purposes.
    ShardingState::get(operationContext())->setEnabledForTest(_shardId.toString());

    expectLock();

    // Call the splitChunk function asynchronously on a different thread, so that we do not block,
    // and so we can construct the mock responses to requests made by splitChunk below.
    auto future = launchAsync([&] {
        auto statusWithOptionalChunkRange = splitChunk(operationContext(),
                                                       _nss,
                                                       keyPatternObj,
                                                       _chunkRanges[1],
                                                       splitKeys,
                                                       _shardId.toString(),
                                                       _epoch);
        ASSERT_NOT_OK(statusWithOptionalChunkRange.getStatus());
    });

    // Here, we mock responses to the requests made by the splitChunk operation. The requests first
    // do a find on the databases, then a find on all collections in the database we are looking
    // for. Next, filter by the specific collection, and find the relevant chunks and shards.
    dbResponse();
    collResponse();
    collResponse();
    chunkResponse();
    shardResponse();

    // Here, we mock a successful response from the config server, which denotes that the split was
    // successful on the config server's end.
    commitChunkSplitResponse(true);

    // We attempt to find the relevant chunks three times. For each of these times, we will respond
    // with the relevant collection, but no chunks.
    collResponse();
    emptyResponse();
    collResponse();
    emptyResponse();
    collResponse();
    emptyResponse();

    future.timed_get(kFutureTimeout);
}

}  // namespace
}  // namespace mongo
