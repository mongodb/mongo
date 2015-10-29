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

#include "mongo/db/operation_context_impl.h"
#include "mongo/dbtests/config_server_fixture.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/chunk_manager.h"

namespace mongo {

using std::unique_ptr;
using std::set;
using std::string;
using std::vector;

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

/**
 * Sets up a basic environment for loading chunks to/from the direct database connection. Redirects
 * connections to the direct database for the duration of the test.
 */
class ChunkManagerFixture : public ConfigServerFixture {
public:
    void setUp() override {
        ConfigServerFixture::setUp();

        _client.dropDatabase(nsGetDB(_collName));
        _client.insert(_collName,
                       BSON("hello"
                            << "world"));
        _client.dropCollection(_collName);

        // Add dummy shard to config DB
        _client.insert(ShardType::ConfigNS,
                       BSON(ShardType::name()
                            << _shardId << ShardType::host()
                            << ConnectionString(HostAndPort("$hostFooBar:27017")).toString()));
    }

protected:
    static const ShardId _shardId;
    static const string _collName;

    static const int numSplitPoints = 100;

    void genRandomSplitPoints(vector<int>* splitPoints) {
        for (int i = 0; i < numSplitPoints; i++) {
            splitPoints->push_back(rand(numSplitPoints * 10));
        }
    }

    void genRandomSplitKeys(const string& keyName, vector<BSONObj>* splitKeys) {
        vector<int> splitPoints;
        genRandomSplitPoints(&splitPoints);

        for (vector<int>::iterator it = splitPoints.begin(); it != splitPoints.end(); ++it) {
            splitKeys->push_back(BSON(keyName << *it));
        }
    }

    // Uses a chunk manager to create chunks
    void createChunks(const string& keyName) {
        vector<BSONObj> splitKeys;
        genRandomSplitKeys(keyName, &splitKeys);

        ShardKeyPattern shardKeyPattern(BSON(keyName << 1));
        ChunkManager manager(_collName, shardKeyPattern, false);

        manager.createFirstChunks(&_txn, _shardId, &splitKeys, NULL);
    }
};

const ShardId ChunkManagerFixture::_shardId{"shard0000"};
const string ChunkManagerFixture::_collName{"foo.bar"};

// Rename the fixture so that our tests have a useful name in the executable
typedef ChunkManagerFixture ChunkManagerTests;

/**
 * Tests creating a new chunk manager with random split points.  Creating chunks on multiple shards
 * is not tested here since there are unresolved race conditions there and probably should be
 * avoided if at all possible.
 */
TEST_F(ChunkManagerTests, FullTest) {
    string keyName = "_id";
    createChunks(keyName);

    unique_ptr<DBClientCursor> cursor =
        _client.query(ChunkType::ConfigNS, QUERY(ChunkType::ns(_collName)));

    set<int> minorVersions;
    OID epoch;

    // Check that all chunks were created with version 1|x with consistent epoch and unique
    // minor versions
    while (cursor->more()) {
        BSONObj chunk = cursor->next();

        ChunkVersion version = ChunkVersion::fromBSON(chunk, ChunkType::DEPRECATED_lastmod());

        ASSERT(version.majorVersion() == 1);
        ASSERT(version.epoch().isSet());

        if (!epoch.isSet()) {
            epoch = version.epoch();
        }

        ASSERT(version.epoch() == epoch);

        ASSERT(minorVersions.find(version.minorVersion()) == minorVersions.end());
        minorVersions.insert(version.minorVersion());

        ASSERT(chunk[ChunkType::shard()].String() == _shardId);
    }
}

/**
 * Tests that chunks are loaded correctly from the db with no a-priori info and also that they can
 * be reloaded on top of an old chunk manager with changes.
 */
TEST_F(ChunkManagerTests, Basic) {
    string keyName = "_id";
    createChunks(keyName);
    int numChunks =
        static_cast<int>(_client.count(ChunkType::ConfigNS, BSON(ChunkType::ns(_collName))));

    BSONObj firstChunk = _client.findOne(ChunkType::ConfigNS, BSONObj()).getOwned();

    ChunkVersion version = ChunkVersion::fromBSON(firstChunk, ChunkType::DEPRECATED_lastmod());

    // Make manager load existing chunks
    CollectionType collType;
    collType.setNs(NamespaceString{_collName});
    collType.setEpoch(version.epoch());
    collType.setUpdatedAt(jsTime());
    collType.setKeyPattern(BSON("_id" << 1));
    collType.setUnique(false);
    collType.setDropped(false);

    ChunkManager manager(collType);
    manager.loadExistingRanges(&_txn, nullptr);

    ASSERT(manager.getVersion().epoch() == version.epoch());
    ASSERT(manager.getVersion().minorVersion() == (numChunks - 1));
    ASSERT(static_cast<int>(manager.getChunkMap().size()) == numChunks);

    // Modify chunks collection
    BSONObjBuilder b;
    ChunkVersion laterVersion = ChunkVersion(2, 1, version.epoch());
    laterVersion.addToBSON(b, ChunkType::DEPRECATED_lastmod());

    _client.update(ChunkType::ConfigNS, BSONObj(), BSON("$set" << b.obj()));

    // Make new manager load chunk diff
    ChunkManager newManager(manager.getns(), manager.getShardKeyPattern(), manager.isUnique());
    newManager.loadExistingRanges(&_txn, &manager);

    ASSERT(newManager.getVersion().toLong() == laterVersion.toLong());
    ASSERT(newManager.getVersion().epoch() == laterVersion.epoch());
    ASSERT(static_cast<int>(newManager.getChunkMap().size()) == numChunks);
}

}  // namespace
}  // namespace mongo
