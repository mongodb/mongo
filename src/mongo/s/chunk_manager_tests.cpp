/**
 *    Copyright (C) 2009 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/s/catalog/replset/sharding_catalog_test_fixture.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"

namespace mongo {

using std::unique_ptr;
using std::set;
using std::string;
using std::vector;

using executor::RemoteCommandResponse;
using executor::RemoteCommandRequest;

namespace {

static int rand(int max = -1) {
    static unsigned seed = 1337;

#if !defined(_WIN32)
    int r = rand_r(&seed);
#else
    int r = ::rand();  // seed not used in this case
#endif

    // Modding is bad, but don't really care in this case
    return max > 0 ? r % max : r;
}

class ChunkManagerFixture : public ShardingCatalogTestFixture {
public:
    void setUp() override {
        ShardingCatalogTestFixture::setUp();
        setRemote(HostAndPort("FakeRemoteClient:34567"));
        configTargeter()->setFindHostReturnValue(configHost);
    }

protected:
    const HostAndPort configHost{HostAndPort(CONFIG_HOST_PORT)};
    static const ShardId _shardId;
    static const string _collName;
    static const string _dbName;

    static const int numSplitPoints = 100;

    void genUniqueRandomSplitKeys(const string& keyName, vector<BSONObj>* splitKeys) {
        std::unordered_set<int> uniquePoints;
        while (static_cast<int>(uniquePoints.size()) < numSplitPoints) {
            uniquePoints.insert(rand(numSplitPoints * 10));
        }
        for (auto it = uniquePoints.begin(); it != uniquePoints.end(); ++it) {
            splitKeys->push_back(BSON(keyName << *it));
        }
    }

    void expectInsertOnConfigSaveChunkAndReturnOk(std::vector<BSONObj>& chunks) {
        onCommandWithMetadata([&](const RemoteCommandRequest& request) mutable {
            ASSERT_EQ(request.target, HostAndPort(CONFIG_HOST_PORT));
            ASSERT_EQ(request.dbname, "config");

            // Get "inserted" chunk doc from RemoteCommandRequest.
            BatchedCommandRequest batchedCommandRequest(BatchedCommandRequest::BatchType_Insert);
            string errmsg;
            batchedCommandRequest.parseBSON(_dbName, request.cmdObj, &errmsg);
            vector<BSONObj> docs = batchedCommandRequest.getInsertRequest()->getDocuments();
            BSONObj chunk = docs.front();

            // Save chunk (mimic "insertion").
            chunks.push_back(chunk);

            return RemoteCommandResponse(BSON("ok" << 1), BSONObj(), Milliseconds(1));
        });
    }

    void expectInsertOnConfigCheckMetadataAndReturnOk(set<int>& minorVersions, OID& epoch) {
        onCommandWithMetadata([&](const RemoteCommandRequest& request) mutable {
            ASSERT_EQ(request.target, HostAndPort(CONFIG_HOST_PORT));
            ASSERT_EQ(request.dbname, "config");

            // Get "inserted" chunk doc from RemoteCommandRequest.
            BatchedCommandRequest batchedCommandRequest(BatchedCommandRequest::BatchType_Insert);
            string errmsg;
            batchedCommandRequest.parseBSON(_dbName, request.cmdObj, &errmsg);
            vector<BSONObj> docs = batchedCommandRequest.getInsertRequest()->getDocuments();
            BSONObj chunk = docs.front();

            ChunkVersion version = ChunkVersion::fromBSON(chunk, ChunkType::DEPRECATED_lastmod());

            // Check chunk's major version.
            ASSERT(version.majorVersion() == 1);

            // Check chunk's minor version is unique.
            ASSERT(minorVersions.find(version.minorVersion()) == minorVersions.end());
            minorVersions.insert(version.minorVersion());

            // Check chunk's epoch is consistent.
            ASSERT(version.epoch().isSet());
            if (!epoch.isSet()) {
                epoch = version.epoch();
            }
            ASSERT(version.epoch() == epoch);

            // Check chunk's shard id.
            ASSERT(chunk[ChunkType::shard()].String() == _shardId.toString());

            return RemoteCommandResponse(BSON("ok" << 1), BSONObj(), Milliseconds(1));
        });
    }
};

const ShardId ChunkManagerFixture::_shardId{"shard0000"};
const string ChunkManagerFixture::_collName{"foo.bar"};
const string ChunkManagerFixture::_dbName{"foo"};

// Rename the fixture so that our tests have a useful name in the executable
typedef ChunkManagerFixture ChunkManagerTests;

/**
 * Tests loading chunks into a ChunkManager with or without an old ChunkManager.
 */
TEST_F(ChunkManagerTests, Basic) {
    string keyName = "_id";
    vector<BSONObj> splitKeys;
    genUniqueRandomSplitKeys(keyName, &splitKeys);
    ShardKeyPattern shardKeyPattern(BSON(keyName << 1));

    std::vector<BSONObj> shards{
        BSON(ShardType::name() << _shardId << ShardType::host()
                               << ConnectionString(HostAndPort("hostFooBar:27017")).toString())};

    // Generate and save a set of chunks with metadata using a temporary ChunkManager.

    std::vector<BSONObj> chunks;
    auto future = launchAsync([&] {
        ChunkManager manager(_collName, shardKeyPattern, false);
        auto status = manager.createFirstChunks(operationContext(), _shardId, &splitKeys, NULL);
        ASSERT_OK(status);
    });

    // Call the expect() one extra time since numChunks = numSplits + 1.
    for (int i = 0; i < static_cast<int>(splitKeys.size()) + 1; i++) {
        expectInsertOnConfigSaveChunkAndReturnOk(chunks);
    }

    future.timed_get(kFutureTimeout);

    // Test that a *new* ChunkManager correctly loads the chunks with *no prior info*.

    int numChunks = static_cast<int>(chunks.size());
    BSONObj firstChunk = chunks.back();
    ChunkVersion version = ChunkVersion::fromBSON(firstChunk, ChunkType::DEPRECATED_lastmod());

    CollectionType collType;
    collType.setNs(NamespaceString{_collName});
    collType.setEpoch(version.epoch());
    collType.setUpdatedAt(jsTime());
    collType.setKeyPattern(BSON(keyName << 1));
    collType.setUnique(false);
    collType.setDropped(false);

    ChunkManager manager(collType);
    future = launchAsync([&] {
        manager.loadExistingRanges(operationContext(), nullptr);

        ASSERT_EQ(version.epoch(), manager.getVersion().epoch());
        ASSERT_EQ(numChunks - 1, manager.getVersion().minorVersion());
        ASSERT_EQ(numChunks, static_cast<int>(manager.getChunkMap().size()));
    });
    expectFindOnConfigSendBSONObjVector(chunks);
    expectFindOnConfigSendBSONObjVector(shards);
    future.timed_get(kFutureTimeout);

    // Test that a *new* ChunkManager correctly loads modified chunks *given an old ChunkManager*.

    // Simulate modified chunks collection
    ChunkVersion laterVersion = ChunkVersion(2, 1, version.epoch());
    BSONObj oldChunk = chunks.front();
    BSONObjBuilder newChunk;
    newChunk.append("_id", oldChunk.getStringField("_id"));
    newChunk.append("ns", oldChunk.getStringField("ns"));
    newChunk.append("min", oldChunk.getObjectField("min"));
    newChunk.append("max", oldChunk.getObjectField("min"));
    newChunk.append("shard", oldChunk.getStringField("shard"));
    laterVersion.addToBSON(newChunk, ChunkType::DEPRECATED_lastmod());
    newChunk.append("lastmodEpoch", oldChunk.getField("lastmodEpoch").OID());

    // Make new manager load chunk diff
    future = launchAsync([&] {
        ChunkManager newManager(manager.getns(), manager.getShardKeyPattern(), manager.isUnique());
        newManager.loadExistingRanges(operationContext(), &manager);

        ASSERT_EQ(numChunks, static_cast<int>(manager.getChunkMap().size()));
        ASSERT_EQ(laterVersion.toString(), newManager.getVersion().toString());
    });
    expectFindOnConfigSendBSONObjVector(std::vector<BSONObj>{chunks.back(), newChunk.obj()});

    std::cout << "done";
    future.timed_get(kFutureTimeout);
    std::cout << "completely done";
}

/**
 * Tests that chunk metadata is created correctly when using ChunkManager to create chunks for the
 * first time. Creating chunks on multiple shards is not tested here since there are unresolved
 * race conditions there and probably should be avoided if at all possible.
 */
TEST_F(ChunkManagerTests, FullTest) {
    string keyName = "_id";
    vector<BSONObj> splitKeys;
    genUniqueRandomSplitKeys(keyName, &splitKeys);
    ShardKeyPattern shardKeyPattern(BSON(keyName << 1));

    auto future = launchAsync([&] {
        ChunkManager manager(_collName, shardKeyPattern, false);
        auto status = manager.createFirstChunks(operationContext(), _shardId, &splitKeys, NULL);
        ASSERT_OK(status);
    });

    // Check that config server receives chunks with the expected metadata.
    // Call expectInsertOnConfigCheckMetadataAndReturnOk one extra time since numChunks = numSplits
    // + 1
    set<int> minorVersions;
    OID epoch;
    for (auto it = splitKeys.begin(); it != splitKeys.end(); ++it) {
        expectInsertOnConfigCheckMetadataAndReturnOk(minorVersions, epoch);
    }
    expectInsertOnConfigCheckMetadataAndReturnOk(minorVersions, epoch);
    future.timed_get(kFutureTimeout);
}

}  // namespace
}  // namespace mongo
