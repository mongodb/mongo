/**
 *    Copyright (C) 2012 10gen Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/base/status.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/metadata_loader.h"
#include "mongo/db/server_options.h"
#include "mongo/s/catalog/dist_lock_catalog_mock.h"
#include "mongo/s/catalog/dist_lock_manager_mock.h"
#include "mongo/s/catalog/sharding_catalog_client_impl.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/shard_server_test_fixture.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

using std::string;
using std::unique_ptr;
using std::vector;
using unittest::assertGet;

const HostAndPort kConfigHostAndPort = HostAndPort("dummy", 123);
const NamespaceString kNss = NamespaceString("test.foo");
const NamespaceString kChunkMetadataNss = NamespaceString("config.chunks.test.foo");
const ShardId kShardId = ShardId("shard0");

class MetadataLoaderTest : public ShardServerTestFixture {
protected:
    /**
     * Goes throught the chunks in 'metadata' and uses a DBDirectClient to check that they are all
     * persisted. Also checks that 'totalNumChunksPersisted' is correct because 'metadata' is only
     * aware of a shard's chunks.
     */
    void checkCollectionMetadataChunksMatchPersistedChunks(
        const NamespaceString& nss,
        const CollectionMetadata& metadata,
        unsigned long long totalNumChunksPersisted) {
        try {
            DBDirectClient client(operationContext());
            ASSERT_EQUALS(client.count(nss.ns()), totalNumChunksPersisted);

            auto chunks = metadata.getChunks();
            for (auto& chunk : chunks) {
                Query query(BSON(ChunkType::minShardID() << chunk.first << ChunkType::max()
                                                         << chunk.second.getMaxKey()));
                query.readPref(ReadPreference::Nearest, BSONArray());

                std::unique_ptr<DBClientCursor> cursor = client.query(nss.ns().c_str(), query, 1);
                ASSERT(cursor);

                ASSERT(cursor->more());
                BSONObj queryResult = cursor->nextSafe();
                ChunkType foundChunk = assertGet(
                    ChunkType::fromShardBSON(queryResult, chunk.second.getVersion().epoch()));
                ASSERT_BSONOBJ_EQ(chunk.first, foundChunk.getMin());
                ASSERT_BSONOBJ_EQ(chunk.second.getMaxKey(), foundChunk.getMax());
            }
        } catch (const DBException& ex) {
            ASSERT(false);
        }
    }

    void expectFindOnConfigSendCollectionDefault() {
        CollectionType collType;
        collType.setNs(kNss);
        collType.setKeyPattern(BSON("a" << 1));
        collType.setUnique(false);
        collType.setUpdatedAt(Date_t::fromMillisSinceEpoch(1));
        collType.setEpoch(_maxCollVersion.epoch());
        ASSERT_OK(collType.validate());
        expectFindOnConfigSendBSONObjVector(std::vector<BSONObj>{collType.toBSON()});
    }

    void expectFindOnConfigSendChunksDefault() {
        BSONObj chunk = BSON(
            ChunkType::name("test.foo-a_MinKey")
            << ChunkType::ns(kNss.ns())
            << ChunkType::min(BSON("a" << MINKEY))
            << ChunkType::max(BSON("a" << MAXKEY))
            << ChunkType::DEPRECATED_lastmod(Date_t::fromMillisSinceEpoch(_maxCollVersion.toLong()))
            << ChunkType::DEPRECATED_epoch(_maxCollVersion.epoch())
            << ChunkType::shard(kShardId.toString()));
        expectFindOnConfigSendBSONObjVector(std::vector<BSONObj>{chunk});
    }

    /**
     * Helper to make a number of chunks that can then be manipulated in various ways in the tests.
     */
    std::vector<BSONObj> makeFourChunks() {
        std::vector<BSONObj> chunks;
        std::string names[] = {
            "test.foo-a_MinKey", "test.foo-a_10", "test.foo-a_50", "test.foo-a_100"};
        BSONObj mins[] = {BSON("a" << MINKEY), BSON("a" << 10), BSON("a" << 50), BSON("a" << 100)};
        BSONObj maxs[] = {BSON("a" << 10), BSON("a" << 50), BSON("a" << 100), BSON("a" << MAXKEY)};
        for (int i = 0; i < 4; ++i) {
            _maxCollVersion.incMajor();
            BSONObj chunk = BSON(ChunkType::name(names[i])
                                 << ChunkType::ns(kNss.ns())
                                 << ChunkType::min(mins[i])
                                 << ChunkType::max(maxs[i])
                                 << ChunkType::DEPRECATED_lastmod(
                                        Date_t::fromMillisSinceEpoch(_maxCollVersion.toLong()))
                                 << ChunkType::DEPRECATED_epoch(_maxCollVersion.epoch())
                                 << ChunkType::shard(kShardId.toString()));
            chunks.push_back(std::move(chunk));
        }
        return chunks;
    }

    ChunkVersion getMaxCollVersion() const {
        return _maxCollVersion;
    }

    ChunkVersion getMaxShardVersion() const {
        return _maxCollVersion;
    }

private:
    ChunkVersion _maxCollVersion{1, 0, OID::gen()};
};

// TODO: Test config server down
// TODO: Test read of chunks with new epoch
// TODO: Test that you can properly load config using format with deprecated fields?

TEST_F(MetadataLoaderTest, DroppedColl) {
    CollectionType collType;
    collType.setNs(kNss);
    collType.setKeyPattern(BSON("a" << 1));
    collType.setUpdatedAt(Date_t());
    collType.setEpoch(OID());
    collType.setDropped(true);
    ASSERT_OK(collType.validate());
    auto future = launchAsync([this] {
        CollectionMetadata metadata;
        auto status = MetadataLoader::makeCollectionMetadata(operationContext(),
                                                             catalogClient(),
                                                             kNss.ns(),
                                                             kShardId.toString(),
                                                             NULL, /* no old metadata */
                                                             &metadata);
        ASSERT_EQUALS(status.code(), ErrorCodes::NamespaceNotFound);
    });
    expectFindOnConfigSendBSONObjVector(std::vector<BSONObj>{collType.toBSON()});
    future.timed_get(kFutureTimeout);
}

TEST_F(MetadataLoaderTest, EmptyColl) {
    auto future = launchAsync([this] {
        CollectionMetadata metadata;
        auto status = MetadataLoader::makeCollectionMetadata(operationContext(),
                                                             catalogClient(),
                                                             kNss.ns(),
                                                             kShardId.toString(),
                                                             NULL, /* no old metadata */
                                                             &metadata);
        ASSERT_EQUALS(status.code(), ErrorCodes::NamespaceNotFound);
    });
    expectFindOnConfigSendErrorCode(ErrorCodes::NamespaceNotFound);
    future.timed_get(kFutureTimeout);
}

TEST_F(MetadataLoaderTest, BadColl) {
    BSONObj badCollToSend = BSON(CollectionType::fullNs(kNss.ns()));
    auto future = launchAsync([this] {
        CollectionMetadata metadata;
        auto status = MetadataLoader::makeCollectionMetadata(operationContext(),
                                                             catalogClient(),
                                                             kNss.ns(),
                                                             kShardId.toString(),
                                                             NULL, /* no old metadata */
                                                             &metadata);
        ASSERT_EQUALS(status.code(), ErrorCodes::NoSuchKey);
    });
    expectFindOnConfigSendBSONObjVector(std::vector<BSONObj>{badCollToSend});
    future.timed_get(kFutureTimeout);
}

TEST_F(MetadataLoaderTest, BadChunk) {
    ChunkType chunkInfo;
    chunkInfo.setNS(kNss.ns());
    chunkInfo.setVersion(ChunkVersion(1, 0, getMaxCollVersion().epoch()));
    ASSERT(!chunkInfo.validate().isOK());

    auto future = launchAsync([this] {
        CollectionMetadata metadata;
        auto status = MetadataLoader::makeCollectionMetadata(operationContext(),
                                                             catalogClient(),
                                                             kNss.ns(),
                                                             kShardId.toString(),
                                                             NULL, /* no old metadata */
                                                             &metadata);
        ASSERT_EQUALS(status.code(), ErrorCodes::NoSuchKey);
    });

    expectFindOnConfigSendCollectionDefault();
    expectFindOnConfigSendBSONObjVector(std::vector<BSONObj>{chunkInfo.toConfigBSON()});
    future.timed_get(kFutureTimeout);
}

TEST_F(MetadataLoaderTest, NoChunksIsDropped) {
    auto future = launchAsync([this] {
        ON_BLOCK_EXIT([&] { Client::destroy(); });
        Client::initThreadIfNotAlready("Test");
        auto txn = cc().makeOperationContext();

        CollectionMetadata metadata;
        auto status = MetadataLoader::makeCollectionMetadata(txn.get(),
                                                             catalogClient(),
                                                             kNss.ns(),
                                                             kShardId.toString(),
                                                             NULL, /* no old metadata */
                                                             &metadata);
        // This is interpreted as a dropped ns, since we drop the chunks first
        ASSERT_EQUALS(status.code(), ErrorCodes::NamespaceNotFound);

        checkCollectionMetadataChunksMatchPersistedChunks(kChunkMetadataNss, metadata, 0);
    });
    expectFindOnConfigSendCollectionDefault();
    expectFindOnConfigSendBSONObjVector(std::vector<BSONObj>{});

    future.timed_get(kFutureTimeout);
}

TEST_F(MetadataLoaderTest, CheckNumChunk) {
    // Need a chunk on another shard, otherwise the chunks are invalid in general and we
    // can't load metadata
    ChunkType chunkType;
    chunkType.setNS(kNss.ns());
    chunkType.setShard(ShardId("altshard"));
    chunkType.setMin(BSON("a" << MINKEY));
    chunkType.setMax(BSON("a" << MAXKEY));
    chunkType.setVersion(ChunkVersion(1, 0, getMaxCollVersion().epoch()));
    ASSERT(chunkType.validate().isOK());

    auto future = launchAsync([this] {
        ON_BLOCK_EXIT([&] { Client::destroy(); });
        Client::initThreadIfNotAlready("Test");
        auto txn = cc().makeOperationContext();

        CollectionMetadata metadata;
        auto status = MetadataLoader::makeCollectionMetadata(txn.get(),
                                                             catalogClient(),
                                                             kNss.ns(),
                                                             kShardId.toString(),
                                                             NULL, /* no old metadata */
                                                             &metadata);
        ASSERT_OK(status);
        ASSERT_EQUALS(0U, metadata.getNumChunks());
        ASSERT_EQUALS(1, metadata.getCollVersion().majorVersion());
        ASSERT_EQUALS(0, metadata.getShardVersion().majorVersion());

        checkCollectionMetadataChunksMatchPersistedChunks(kChunkMetadataNss, metadata, 1);
    });

    expectFindOnConfigSendCollectionDefault();
    expectFindOnConfigSendBSONObjVector(std::vector<BSONObj>{chunkType.toConfigBSON()});

    future.timed_get(kFutureTimeout);
}

TEST_F(MetadataLoaderTest, SingleChunkCheckNumChunk) {
    auto future = launchAsync([this] {
        ON_BLOCK_EXIT([&] { Client::destroy(); });
        Client::initThreadIfNotAlready("Test");
        auto txn = cc().makeOperationContext();

        CollectionMetadata metadata;
        auto status = MetadataLoader::makeCollectionMetadata(txn.get(),
                                                             catalogClient(),
                                                             kNss.ns(),
                                                             kShardId.toString(),
                                                             NULL, /* no old metadata */
                                                             &metadata);
        ASSERT_OK(status);
        ASSERT_EQUALS(1U, metadata.getNumChunks());
        ASSERT_EQUALS(getMaxCollVersion(), metadata.getCollVersion());
        ASSERT_EQUALS(getMaxCollVersion(), metadata.getShardVersion());

        checkCollectionMetadataChunksMatchPersistedChunks(kChunkMetadataNss, metadata, 1);
    });

    expectFindOnConfigSendCollectionDefault();
    expectFindOnConfigSendChunksDefault();

    future.timed_get(kFutureTimeout);
}

TEST_F(MetadataLoaderTest, SeveralChunksCheckNumChunks) {
    auto future = launchAsync([this] {
        ON_BLOCK_EXIT([&] { Client::destroy(); });
        Client::initThreadIfNotAlready("Test");
        auto txn = cc().makeOperationContext();

        CollectionMetadata metadata;
        auto status = MetadataLoader::makeCollectionMetadata(txn.get(),
                                                             catalogClient(),
                                                             kNss.ns(),
                                                             kShardId.toString(),
                                                             NULL, /* no old metadata */
                                                             &metadata);
        ASSERT_OK(status);
        ASSERT_EQUALS(4U, metadata.getNumChunks());
        ASSERT_EQUALS(getMaxCollVersion(), metadata.getCollVersion());
        ASSERT_EQUALS(getMaxCollVersion(), metadata.getShardVersion());

        checkCollectionMetadataChunksMatchPersistedChunks(kChunkMetadataNss, metadata, 4);
    });

    expectFindOnConfigSendCollectionDefault();
    expectFindOnConfigSendBSONObjVector(makeFourChunks());

    future.timed_get(kFutureTimeout);
}

TEST_F(MetadataLoaderTest, CollectionMetadataSetUp) {
    auto future = launchAsync([this] {
        ON_BLOCK_EXIT([&] { Client::destroy(); });
        Client::initThreadIfNotAlready("Test");
        auto txn = cc().makeOperationContext();

        CollectionMetadata metadata;
        auto status = MetadataLoader::makeCollectionMetadata(txn.get(),
                                                             catalogClient(),
                                                             kNss.ns(),
                                                             kShardId.toString(),
                                                             NULL, /* no old metadata */
                                                             &metadata);
        ASSERT_BSONOBJ_EQ(metadata.getKeyPattern(), BSON("a" << 1));
        ASSERT_TRUE(getMaxCollVersion().equals(metadata.getCollVersion()));
        ASSERT_TRUE(getMaxShardVersion().equals(metadata.getShardVersion()));

        checkCollectionMetadataChunksMatchPersistedChunks(kChunkMetadataNss, metadata, 1);
    });

    expectFindOnConfigSendCollectionDefault();
    expectFindOnConfigSendChunksDefault();

    future.timed_get(kFutureTimeout);
}

}  // namespace
}  // namespace mongo
