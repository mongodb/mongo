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

#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/owned_pointer_vector.h"
#include "mongo/client/connpool.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/metadata_loader.h"
#include "mongo/dbtests/mock/mock_conn_registry.h"
#include "mongo/dbtests/mock/mock_remote_db_server.h"
#include "mongo/s/catalog/legacy/catalog_manager_legacy.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {
namespace {

using std::unique_ptr;
using std::string;
using std::vector;

const std::string CONFIG_HOST_PORT = "$dummy_config:27017";

// TODO: Test config server down
// TODO: Test read of chunks with new epoch
// TODO: Test that you can properly load config using format with deprecated fields?

class MetadataLoaderFixture : public mongo::unittest::Test {
public:
    void setUp() {
        ConnectionString configLoc = ConnectionString(HostAndPort(CONFIG_HOST_PORT));
        ASSERT(configLoc.isValid());
        std::string lockProcessId = "testhost:123455:1234567890:9876543210";
        ASSERT_OK(_catalogManager.init(configLoc, lockProcessId));
    }

protected:
    CatalogManager* catalogManager() {
        return &_catalogManager;
    }

private:
    CatalogManagerLegacy _catalogManager;
};


TEST_F(MetadataLoaderFixture, DroppedColl) {
    OperationContextNoop txn;
    MockRemoteDBServer dummyConfig(CONFIG_HOST_PORT);
    mongo::ConnectionString::setConnectionHook(MockConnRegistry::get()->getConnStrHook());
    MockConnRegistry::get()->addServer(&dummyConfig);

    CollectionType collInfo;
    collInfo.setNs(NamespaceString{"test.foo"});
    collInfo.setKeyPattern(BSON("a" << 1));
    collInfo.setUpdatedAt(Date_t());
    collInfo.setEpoch(OID());
    collInfo.setDropped(true);
    ASSERT_OK(collInfo.validate());

    dummyConfig.insert(CollectionType::ConfigNS, collInfo.toBSON());

    MetadataLoader loader;

    string errmsg;
    CollectionMetadata metadata;
    Status status = loader.makeCollectionMetadata(&txn,
                                                  catalogManager(),
                                                  "test.foo",
                                                  "shard0000",
                                                  NULL, /* no old metadata */
                                                  &metadata);

    ASSERT_EQUALS(status.code(), ErrorCodes::NamespaceNotFound);

    MockConnRegistry::get()->clear();
    ScopedDbConnection::clearPool();
}

TEST_F(MetadataLoaderFixture, EmptyColl) {
    OperationContextNoop txn;
    MockRemoteDBServer dummyConfig(CONFIG_HOST_PORT);
    mongo::ConnectionString::setConnectionHook(MockConnRegistry::get()->getConnStrHook());
    MockConnRegistry::get()->addServer(&dummyConfig);

    MetadataLoader loader;

    string errmsg;
    CollectionMetadata metadata;
    Status status = loader.makeCollectionMetadata(&txn,
                                                  catalogManager(),
                                                  "test.foo",
                                                  "shard0000",
                                                  NULL, /* no old metadata */
                                                  &metadata);

    ASSERT_EQUALS(status.code(), ErrorCodes::NamespaceNotFound);

    MockConnRegistry::get()->clear();
    ScopedDbConnection::clearPool();
}

TEST_F(MetadataLoaderFixture, BadColl) {
    OperationContextNoop txn;
    MockRemoteDBServer dummyConfig(CONFIG_HOST_PORT);
    mongo::ConnectionString::setConnectionHook(MockConnRegistry::get()->getConnStrHook());
    MockConnRegistry::get()->addServer(&dummyConfig);

    dummyConfig.insert(CollectionType::ConfigNS, BSON(CollectionType::fullNs("test.foo")));

    MetadataLoader loader;

    string errmsg;
    CollectionMetadata metadata;
    Status status = loader.makeCollectionMetadata(&txn,
                                                  catalogManager(),
                                                  "test.foo",
                                                  "shard0000",
                                                  NULL, /* no old metadata */
                                                  &metadata);

    ASSERT_EQUALS(status.code(), ErrorCodes::NoSuchKey);

    MockConnRegistry::get()->clear();
    ScopedDbConnection::clearPool();
}

TEST_F(MetadataLoaderFixture, BadChunk) {
    OperationContextNoop txn;
    MockRemoteDBServer dummyConfig(CONFIG_HOST_PORT);
    mongo::ConnectionString::setConnectionHook(MockConnRegistry::get()->getConnStrHook());
    MockConnRegistry::get()->addServer(&dummyConfig);

    CollectionType collInfo;
    collInfo.setNs(NamespaceString{"test.foo"});
    collInfo.setUpdatedAt(Date_t::fromMillisSinceEpoch(1));
    collInfo.setKeyPattern(BSON("a" << 1));
    collInfo.setEpoch(OID::gen());
    ASSERT_OK(collInfo.validate());

    dummyConfig.insert(CollectionType::ConfigNS, collInfo.toBSON());

    ChunkType chunkInfo;
    chunkInfo.setNS(NamespaceString{"test.foo"}.ns());
    chunkInfo.setVersion(ChunkVersion(1, 0, collInfo.getEpoch()));
    ASSERT(!chunkInfo.validate().isOK());

    dummyConfig.insert(ChunkType::ConfigNS, chunkInfo.toBSON());

    MetadataLoader loader;

    string errmsg;
    CollectionMetadata metadata;
    Status status = loader.makeCollectionMetadata(&txn,
                                                  catalogManager(),
                                                  "test.foo",
                                                  "shard0000",
                                                  NULL, /* no old metadata */
                                                  &metadata);

    ASSERT_EQUALS(status.code(), ErrorCodes::FailedToParse);

    MockConnRegistry::get()->clear();
    ScopedDbConnection::clearPool();
}

class NoChunkFixture : public MetadataLoaderFixture {
protected:
    void setUp() {
        MetadataLoaderFixture::setUp();

        _dummyConfig.reset(new MockRemoteDBServer(CONFIG_HOST_PORT));
        mongo::ConnectionString::setConnectionHook(MockConnRegistry::get()->getConnStrHook());
        MockConnRegistry::get()->addServer(_dummyConfig.get());

        OID epoch = OID::gen();

        CollectionType collType;
        collType.setNs(NamespaceString{"test.foo"});
        collType.setKeyPattern(BSON("a" << 1));
        collType.setUnique(false);
        collType.setUpdatedAt(Date_t::fromMillisSinceEpoch(1));
        collType.setEpoch(epoch);
        _dummyConfig->insert(CollectionType::ConfigNS, collType.toBSON());
    }

    void tearDown() {
        MockConnRegistry::get()->clear();
        ScopedDbConnection::clearPool();
    }

private:
    unique_ptr<MockRemoteDBServer> _dummyConfig;
};

TEST_F(NoChunkFixture, NoChunksIsDropped) {
    OperationContextNoop txn;
    MetadataLoader loader;

    CollectionMetadata metadata;
    Status status = loader.makeCollectionMetadata(&txn,
                                                  catalogManager(),
                                                  "test.foo",
                                                  "shard0000",
                                                  NULL, /* no old metadata */
                                                  &metadata);

    // This is interpreted as a dropped ns, since we drop the chunks first
    ASSERT_EQUALS(status.code(), ErrorCodes::NamespaceNotFound);
}

class NoChunkHereFixture : public MetadataLoaderFixture {
protected:
    void setUp() {
        MetadataLoaderFixture::setUp();

        _dummyConfig.reset(new MockRemoteDBServer(CONFIG_HOST_PORT));
        mongo::ConnectionString::setConnectionHook(MockConnRegistry::get()->getConnStrHook());
        MockConnRegistry::get()->addServer(_dummyConfig.get());

        OID epoch = OID::gen();

        CollectionType collType;
        collType.setNs(NamespaceString{"test.foo"});
        collType.setKeyPattern(BSON("a" << 1));
        collType.setUnique(false);
        collType.setUpdatedAt(Date_t::fromMillisSinceEpoch(1));
        collType.setEpoch(epoch);
        ASSERT_OK(collType.validate());

        _dummyConfig->insert(CollectionType::ConfigNS, collType.toBSON());

        // Need a chunk on another shard, otherwise the chunks are invalid in general and we
        // can't load metadata
        ChunkType chunkType;
        chunkType.setNS("test.foo");
        chunkType.setShard("shard0001");
        chunkType.setMin(BSON("a" << MINKEY));
        chunkType.setMax(BSON("a" << MAXKEY));
        chunkType.setVersion(ChunkVersion(1, 0, epoch));
        chunkType.setName(OID::gen().toString());
        ASSERT(chunkType.validate().isOK());

        _dummyConfig->insert(ChunkType::ConfigNS, chunkType.toBSON());
    }

    MockRemoteDBServer* getDummyConfig() {
        return _dummyConfig.get();
    }

    void tearDown() {
        MockConnRegistry::get()->clear();
        ScopedDbConnection::clearPool();
    }

private:
    unique_ptr<MockRemoteDBServer> _dummyConfig;
};

TEST_F(NoChunkHereFixture, CheckNumChunk) {
    OperationContextNoop txn;
    MetadataLoader loader;

    CollectionMetadata metadata;
    Status status = loader.makeCollectionMetadata(&txn,
                                                  catalogManager(),
                                                  "test.foo",
                                                  "shard0000",
                                                  NULL, /* no old metadata */
                                                  &metadata);

    ASSERT(status.isOK());
    ASSERT_EQUALS(0U, metadata.getNumChunks());
    ASSERT_EQUALS(1, metadata.getCollVersion().majorVersion());
    ASSERT_EQUALS(0, metadata.getShardVersion().majorVersion());
    ASSERT_NOT_EQUALS(OID(), metadata.getCollVersion().epoch());
    ASSERT_NOT_EQUALS(OID(), metadata.getShardVersion().epoch());
}

class ConfigServerFixture : public MetadataLoaderFixture {
protected:
    void setUp() {
        MetadataLoaderFixture::setUp();

        _dummyConfig.reset(new MockRemoteDBServer(CONFIG_HOST_PORT));
        mongo::ConnectionString::setConnectionHook(MockConnRegistry::get()->getConnStrHook());
        MockConnRegistry::get()->addServer(_dummyConfig.get());

        OID epoch = OID::gen();
        _maxCollVersion = ChunkVersion(1, 0, epoch);

        CollectionType collType;
        collType.setNs(NamespaceString{"test.foo"});
        collType.setKeyPattern(BSON("a" << 1));
        collType.setUnique(false);
        collType.setUpdatedAt(Date_t::fromMillisSinceEpoch(1));
        collType.setEpoch(epoch);
        _dummyConfig->insert(CollectionType::ConfigNS, collType.toBSON());

        BSONObj fooSingle = BSON(
            ChunkType::name("test.foo-a_MinKey")
            << ChunkType::ns("test.foo") << ChunkType::min(BSON("a" << MINKEY))
            << ChunkType::max(BSON("a" << MAXKEY))
            << ChunkType::DEPRECATED_lastmod(Date_t::fromMillisSinceEpoch(_maxCollVersion.toLong()))
            << ChunkType::DEPRECATED_epoch(epoch) << ChunkType::shard("shard0000"));
        _dummyConfig->insert(ChunkType::ConfigNS, fooSingle);
    }

    void tearDown() {
        MockConnRegistry::get()->clear();
    }

    ChunkVersion getMaxCollVersion() const {
        return _maxCollVersion;
    }

    ChunkVersion getMaxShardVersion() const {
        return _maxCollVersion;
    }

    MockRemoteDBServer* getConfigServer() const {
        return _dummyConfig.get();
    }

private:
    unique_ptr<MockRemoteDBServer> _dummyConfig;
    ChunkVersion _maxCollVersion;
};

TEST_F(ConfigServerFixture, SingleChunkCheckNumChunk) {
    OperationContextNoop txn;
    MetadataLoader loader;
    CollectionMetadata metadata;
    Status status = loader.makeCollectionMetadata(&txn,
                                                  catalogManager(),
                                                  "test.foo",
                                                  "shard0000",
                                                  NULL, /* no old metadata */
                                                  &metadata);
    ASSERT(status.isOK());
    ASSERT_EQUALS(1U, metadata.getNumChunks());
}

TEST_F(ConfigServerFixture, SingleChunkGetNext) {
    OperationContextNoop txn;
    MetadataLoader loader;
    CollectionMetadata metadata;
    loader.makeCollectionMetadata(&txn,
                                  catalogManager(),
                                  "test.foo",
                                  "shard0000",
                                  NULL, /* no old metadata */
                                  &metadata);
    ChunkType chunkInfo;
    ASSERT_TRUE(metadata.getNextChunk(metadata.getMinKey(), &chunkInfo));
}

TEST_F(ConfigServerFixture, SingleChunkGetShardKey) {
    OperationContextNoop txn;
    MetadataLoader loader;
    CollectionMetadata metadata;
    loader.makeCollectionMetadata(&txn,
                                  catalogManager(),
                                  "test.foo",
                                  "shard0000",
                                  NULL, /* no old metadata */
                                  &metadata);
    ASSERT_TRUE(metadata.getKeyPattern().equal(BSON("a" << 1)));
}

TEST_F(ConfigServerFixture, SingleChunkGetMaxCollVersion) {
    OperationContextNoop txn;
    MetadataLoader loader;
    CollectionMetadata metadata;
    loader.makeCollectionMetadata(&txn,
                                  catalogManager(),
                                  "test.foo",
                                  "shard0000",
                                  NULL, /* no old metadata */
                                  &metadata);

    ASSERT_TRUE(getMaxCollVersion().equals(metadata.getCollVersion()));
}

TEST_F(ConfigServerFixture, SingleChunkGetMaxShardVersion) {
    OperationContextNoop txn;
    MetadataLoader loader;
    CollectionMetadata metadata;
    loader.makeCollectionMetadata(&txn,
                                  catalogManager(),
                                  "test.foo",
                                  "shard0000",
                                  NULL, /* no old metadata */
                                  &metadata);

    ASSERT_TRUE(getMaxShardVersion().equals(metadata.getShardVersion()));
}

TEST_F(ConfigServerFixture, NoChunks) {
    OperationContextNoop txn;
    getConfigServer()->remove(ChunkType::ConfigNS, BSONObj());

    MetadataLoader loader;
    CollectionMetadata metadata;
    Status status = loader.makeCollectionMetadata(&txn,
                                                  catalogManager(),
                                                  "test.foo",
                                                  "shard0000",
                                                  NULL, /* no old metadata */
                                                  &metadata);

    // NSNotFound because we're reloading with no old metadata
    ASSERT_EQUALS(status.code(), ErrorCodes::NamespaceNotFound);
}

class MultipleMetadataFixture : public MetadataLoaderFixture {
protected:
    void setUp() {
        MetadataLoaderFixture::setUp();

        _dummyConfig.reset(new MockRemoteDBServer(CONFIG_HOST_PORT));
        mongo::ConnectionString::setConnectionHook(MockConnRegistry::get()->getConnStrHook());
        MockConnRegistry::get()->addServer(_dummyConfig.get());

        ConnectionString confServerStr((HostAndPort(CONFIG_HOST_PORT)));
        ConnectionString configLoc(confServerStr);
        _loader.reset(new MetadataLoader);
    }

    MetadataLoader& loader() {
        return *_loader;
    }

    void getMetadataFor(OperationContext* txn,
                        const OwnedPointerVector<ChunkType>& chunks,
                        CollectionMetadata* metadata) {
        // Infer namespace, shard, epoch, keypattern from first chunk
        const ChunkType* firstChunk = *(chunks.vector().begin());

        const string ns = firstChunk->getNS();
        const string shardName = firstChunk->getShard();
        const OID epoch = firstChunk->getVersion().epoch();

        BSONObjBuilder keyPatternB;
        BSONObjIterator keyPatternIt(firstChunk->getMin());
        while (keyPatternIt.more())
            keyPatternB.append(keyPatternIt.next().fieldName(), 1);
        BSONObj keyPattern = keyPatternB.obj();

        _dummyConfig->remove(CollectionType::ConfigNS, BSONObj());
        _dummyConfig->remove(ChunkType::ConfigNS, BSONObj());

        CollectionType coll;
        coll.setNs(NamespaceString{ns});
        coll.setKeyPattern(BSON("a" << 1));
        coll.setUpdatedAt(Date_t::fromMillisSinceEpoch(1));
        coll.setEpoch(epoch);
        ASSERT_OK(coll.validate());

        _dummyConfig->insert(CollectionType::ConfigNS, coll.toBSON());

        ChunkVersion version(1, 0, epoch);
        for (const auto chunkVal : chunks.vector()) {
            ChunkType chunk(*chunkVal);

            chunk.setName(OID::gen().toString());
            if (!chunk.isVersionSet()) {
                chunk.setVersion(version);
                version.incMajor();
            }

            ASSERT(chunk.validate().isOK());

            _dummyConfig->insert(ChunkType::ConfigNS, chunk.toBSON());
        }

        Status status =
            loader().makeCollectionMetadata(txn, catalogManager(), ns, shardName, NULL, metadata);
        ASSERT(status.isOK());
    }

    void tearDown() {
        MockConnRegistry::get()->clear();
    }

private:
    unique_ptr<MockRemoteDBServer> _dummyConfig;
    unique_ptr<MetadataLoader> _loader;
};

TEST_F(MultipleMetadataFixture, PromotePendingNA) {
    OperationContextNoop txn;
    unique_ptr<ChunkType> chunk(new ChunkType());
    chunk->setNS("foo.bar");
    chunk->setShard("shard0000");
    chunk->setMin(BSON("x" << MINKEY));
    chunk->setMax(BSON("x" << 0));
    chunk->setVersion(ChunkVersion(1, 0, OID::gen()));

    OwnedPointerVector<ChunkType> chunks;
    chunks.mutableVector().push_back(chunk.release());

    CollectionMetadata afterMetadata;
    getMetadataFor(&txn, chunks, &afterMetadata);

    // Metadata of different epoch
    (*chunks.vector().begin())->setVersion(ChunkVersion(1, 0, OID::gen()));

    CollectionMetadata remoteMetadata;
    getMetadataFor(&txn, chunks, &remoteMetadata);

    Status status = loader().promotePendingChunks(&afterMetadata, &remoteMetadata);
    ASSERT(status.isOK());

    string errMsg;
    ChunkType pending;
    pending.setMin(BSON("x" << 0));
    pending.setMax(BSON("x" << 10));

    unique_ptr<CollectionMetadata> cloned(afterMetadata.clonePlusPending(pending, &errMsg));
    ASSERT(cloned != NULL);

    status = loader().promotePendingChunks(cloned.get(), &remoteMetadata);
    ASSERT(status.isOK());
    ASSERT_EQUALS(remoteMetadata.getNumPending(), 0u);
}

TEST_F(MultipleMetadataFixture, PromotePendingNAVersion) {
    OperationContextNoop txn;
    OID epoch = OID::gen();

    unique_ptr<ChunkType> chunk(new ChunkType());
    chunk->setNS("foo.bar");
    chunk->setShard("shard0000");
    chunk->setMin(BSON("x" << MINKEY));
    chunk->setMax(BSON("x" << 0));
    chunk->setVersion(ChunkVersion(1, 1, epoch));

    OwnedPointerVector<ChunkType> chunks;
    chunks.mutableVector().push_back(chunk.release());

    CollectionMetadata afterMetadata;
    getMetadataFor(&txn, chunks, &afterMetadata);

    // Metadata of same epoch, but lower version
    (*chunks.vector().begin())->setVersion(ChunkVersion(1, 0, epoch));

    CollectionMetadata remoteMetadata;
    getMetadataFor(&txn, chunks, &remoteMetadata);

    Status status = loader().promotePendingChunks(&afterMetadata, &remoteMetadata);
    ASSERT(status.isOK());

    string errMsg;
    ChunkType pending;
    pending.setMin(BSON("x" << 0));
    pending.setMax(BSON("x" << 10));

    unique_ptr<CollectionMetadata> cloned(afterMetadata.clonePlusPending(pending, &errMsg));
    ASSERT(cloned != NULL);

    status = loader().promotePendingChunks(cloned.get(), &remoteMetadata);
    ASSERT(status.isOK());
    ASSERT_EQUALS(remoteMetadata.getNumPending(), 0u);
}

TEST_F(MultipleMetadataFixture, PromotePendingGoodOverlap) {
    OperationContextNoop txn;
    OID epoch = OID::gen();

    //
    // Setup chunk range for remote metadata
    //

    OwnedPointerVector<ChunkType> chunks;

    unique_ptr<ChunkType> chunk(new ChunkType());
    chunk->setNS("foo.bar");
    chunk->setShard("shard0000");
    chunk->setMin(BSON("x" << MINKEY));
    chunk->setMax(BSON("x" << 0));
    chunk->setVersion(ChunkVersion(1, 0, epoch));
    chunks.mutableVector().push_back(chunk.release());

    chunk.reset(new ChunkType());
    chunk->setNS("foo.bar");
    chunk->setShard("shard0000");
    chunk->setMin(BSON("x" << 10));
    chunk->setMax(BSON("x" << 20));
    chunks.mutableVector().push_back(chunk.release());

    chunk.reset(new ChunkType());
    chunk->setNS("foo.bar");
    chunk->setShard("shard0000");
    chunk->setMin(BSON("x" << 30));
    chunk->setMax(BSON("x" << MAXKEY));
    chunks.mutableVector().push_back(chunk.release());

    CollectionMetadata remoteMetadata;
    getMetadataFor(&txn, chunks, &remoteMetadata);

    //
    // Setup chunk and pending range for afterMetadata
    //

    chunks.clear();

    chunk.reset(new ChunkType());
    chunk->setNS("foo.bar");
    chunk->setShard("shard0000");
    chunk->setMin(BSON("x" << 0));
    chunk->setMax(BSON("x" << 10));
    chunk->setVersion(ChunkVersion(1, 0, epoch));

    chunks.mutableVector().push_back(chunk.release());

    CollectionMetadata afterMetadata;
    getMetadataFor(&txn, chunks, &afterMetadata);

    string errMsg;
    ChunkType pending;
    pending.setMin(BSON("x" << MINKEY));
    pending.setMax(BSON("x" << 0));

    unique_ptr<CollectionMetadata> cloned(afterMetadata.clonePlusPending(pending, &errMsg));
    ASSERT(cloned != NULL);

    pending.setMin(BSON("x" << 10));
    pending.setMax(BSON("x" << 20));

    cloned.reset(cloned->clonePlusPending(pending, &errMsg));
    ASSERT(cloned != NULL);

    pending.setMin(BSON("x" << 20));
    pending.setMax(BSON("x" << 30));

    cloned.reset(cloned->clonePlusPending(pending, &errMsg));
    ASSERT(cloned != NULL);

    pending.setMin(BSON("x" << 30));
    pending.setMax(BSON("x" << MAXKEY));

    cloned.reset(cloned->clonePlusPending(pending, &errMsg));
    ASSERT(cloned != NULL);

    Status status = loader().promotePendingChunks(cloned.get(), &remoteMetadata);
    ASSERT(status.isOK());

    ASSERT_EQUALS(remoteMetadata.getNumPending(), 1u);
    ASSERT(remoteMetadata.keyIsPending(BSON("x" << 25)));
}

TEST_F(MultipleMetadataFixture, PromotePendingBadOverlap) {
    OperationContextNoop txn;
    OID epoch = OID::gen();

    //
    // Setup chunk range for remote metadata
    //

    OwnedPointerVector<ChunkType> chunks;

    unique_ptr<ChunkType> chunk(new ChunkType());
    chunk->setNS("foo.bar");
    chunk->setShard("shard0000");
    chunk->setMin(BSON("x" << MINKEY));
    chunk->setMax(BSON("x" << 0));
    chunk->setVersion(ChunkVersion(1, 0, epoch));

    chunks.mutableVector().push_back(chunk.release());

    CollectionMetadata remoteMetadata;
    getMetadataFor(&txn, chunks, &remoteMetadata);

    //
    // Setup chunk and pending range for afterMetadata
    //

    chunks.clear();

    chunk.reset(new ChunkType());
    chunk->setNS("foo.bar");
    chunk->setShard("shard0000");
    chunk->setMin(BSON("x" << 15));
    chunk->setMax(BSON("x" << MAXKEY));
    chunk->setVersion(ChunkVersion(1, 0, epoch));

    chunks.mutableVector().push_back(chunk.release());

    CollectionMetadata afterMetadata;
    getMetadataFor(&txn, chunks, &afterMetadata);

    string errMsg;
    ChunkType pending;
    pending.setMin(BSON("x" << MINKEY));
    pending.setMax(BSON("x" << 1));

    unique_ptr<CollectionMetadata> cloned(afterMetadata.clonePlusPending(pending, &errMsg));
    ASSERT(cloned != NULL);

    cloned.reset(cloned->clonePlusPending(pending, &errMsg));
    ASSERT(cloned != NULL);

    Status status = loader().promotePendingChunks(cloned.get(), &remoteMetadata);
    ASSERT_EQUALS(status.code(), ErrorCodes::RemoteChangeDetected);
}

#if 0
    // TODO: MockServer functionality does not support selective query - consider
    // inserting nothing at all to chunk/collections collection
    TEST_F(ConfigServerFixture, EmptyDataForNS) {
        MetadataLoader loader;
        CollectionMetadata metadata;
        Status status = loader.makeCollectionMetadata( "not.sharded",
                                                       "shard0000",
                                                       NULL, /* no old metadata */
                                                       &metadata );
        ASSERT( !status.isOK() );
    }
#endif

#if 0
    // TODO: d_chunk_manager_test has no tests for passing old ShardChunkManager
    class TwoChunkFixture : public mongo::unittest::Test {
    protected:
        void setUp() {
            _dummyConfig.reset( new MockRemoteDBServer( CONFIG_HOST_PORT ) );
            mongo::ConnectionString::setConnectionHook( MockConnRegistry::get()->getConnStrHook() );
            MockConnRegistry::get()->addServer( _dummyConfig.get() );

            OID epoch = OID::gen();
            _maxCollVersion = ChunkVersion( 1, 0, epoch );

            BSONObj collFoo = BSON(CollectionType::ns("test.foo") <<
                    CollectionType::keyPattern(BSON("a" << 1)) <<
                    CollectionType::unique(false) <<
                    CollectionType::updatedAt(1ULL) <<
                    CollectionType::epoch(epoch));
            _dummyConfig->insert( CollectionType::ConfigNS, collFoo );

            BSONObj fooSingle = BSON(ChunkType::name("test.foo-a_MinKey") <<
                    ChunkType::ns("test.foo") <<
                    ChunkType::min(BSON("a" << MINKEY)) <<
                    ChunkType::max(BSON("a" << MAXKEY)) <<
                    ChunkType::DEPRECATED_lastmod(_maxCollVersion.toLong()) <<
                    ChunkType::DEPRECATED_epoch(epoch) <<
                    ChunkType::shard("shard0000"));
            _dummyConfig->insert( ChunkType::ConfigNS, fooSingle );

            MetadataLoader loader;
            Status status = loader.makeCollectionMetadata( "not.sharded",
                                                           "shard0000",
                                                           NULL, /* no old metadata */
                                                           &_oldMetadata );
            ASSERT( status.isOK() );

            // Needs to delete the collection and rebuild because the mock server
            // not support updates.
            _dummyConfig->remove( CollectionType::ConfigNS, BSONObj() );
            _dummyConfig->remove( ChunkType::ConfigNS, BSONObj() );

            OID epoch2 = OID::gen();
            _maxCollVersion = ChunkVersion( 2, 0, epoch2 );

            BSONObj collFoo = BSON(CollectionType::ns("test.foo") <<
                    CollectionType::keyPattern(BSON("a" << 1)) <<
                    CollectionType::unique(false) <<
                    CollectionType::updatedAt(2ULL) <<
                    CollectionType::epoch(epoch2));
            _dummyConfig->insert( CollectionType::ConfigNS, collFoo );

            BSONObj chunk1 = BSON(ChunkType::name("test.foo-a_MinKey") <<
                    ChunkType::ns("test.foo") <<
                    ChunkType::min(BSON("a" << MINKEY)) <<
                    ChunkType::max(BSON("a" << 100)) <<
                    ChunkType::DEPRECATED_lastmod(_maxCollVersion.toLong()) <<
                    ChunkType::DEPRECATED_epoch(epoch2) <<
                    ChunkType::shard("shard0000"));
            _dummyConfig->insert( ChunkType::ConfigNS, chunk1 );

            BSONObj chunk2 = BSON(ChunkType::name("test.foo-a_100") <<
                    ChunkType::ns("test.foo") <<
                    ChunkType::min(BSON("a" << 100)) <<
                    ChunkType::max(BSON("a" << MAXKEY)) <<
                    ChunkType::DEPRECATED_lastmod(_maxCollVersion.toLong()) <<
                    ChunkType::DEPRECATED_epoch(epoch2) <<
                    ChunkType::shard("shard0000"));
            _dummyConfig->insert( ChunkType::ConfigNS, chunk2 );
        }

        void tearDown() {
            MockConnRegistry::get()->removeServer( _dummyConfig->getServerAddress() );
        }

        ChunkVersion getCollVersion() const {
            return _maxCollVersion;
        }

        const ChunkVersion& getShardVersion( size_t shard ) const {
            return _maxCollVersion;
        }

        const CollectionMetadata* getOldMetadata() const {
            return _oldMetadata;
        }

    private:
        unique_ptr<MockRemoteDBServer> _dummyConfig;
        CollectionMetadata _oldMetadata;

        ChunkVersion _maxCollVersion;
    };
#endif

#if 0

    // TODO: MockServer functionality does not support selective query
    class ThreeChunkTwoShardFixture : public mongo::unittest::Test {
    protected:
        void setUp() {
            _dummyConfig.reset( new MockRemoteDBServer( CONFIG_HOST_PORT ) );
            mongo::ConnectionString::setConnectionHook( MockConnRegistry::get()->getConnStrHook() );
            MockConnRegistry::get()->addServer( _dummyConfig.get() );

            OID epoch = OID::gen();
            _maxCollVersion = ChunkVersion( 1, 0, epoch );

            BSONObj collFoo = BSON(CollectionType::ns("test.foo") <<
                    CollectionType::keyPattern(BSON("a" << 1)) <<
                    CollectionType::unique(false) <<
                    CollectionType::updatedAt(1ULL) <<
                    CollectionType::epoch(epoch));
            _dummyConfig->insert( CollectionType::ConfigNS, collFoo );

            BSONObj fooSingle = BSON(ChunkType::name("test.foo-a_MinKey") <<
                    ChunkType::ns("test.foo") <<
                    ChunkType::min(BSON("a" << MINKEY)) <<
                    ChunkType::max(BSON("a" << MAXKEY)) <<
                    ChunkType::DEPRECATED_lastmod(_maxCollVersion.toLong()) <<
                    ChunkType::DEPRECATED_epoch(epoch) <<
                    ChunkType::shard("shard0000"));
            _dummyConfig->insert( ChunkType::ConfigNS, fooSingle );

            MetadataLoader loader;
            CollectionMetadata metadata;
            Status status = loader.makeCollectionMetadata( "not.sharded",
                                                           "shard0000",
                                                           NULL, /* no old metadata */
                                                           &metadata );
            ASSERT( status.isOK() );

            // Needs to delete the collection and rebuild because the mock server
            // not support updates.
            _dummyConfig->remove( CollectionType::ConfigNS, BSONObj() );
            _dummyConfig->remove( ChunkType::ConfigNS, BSONObj() );

            OID epoch2 = OID::gen();
            _maxCollVersion = ChunkVersion( 2, 0, epoch2 );
            _maxShardVersion.push_back( _maxCollVersion );

            BSONObj collFoo = BSON(CollectionType::ns("test.foo") <<
                    CollectionType::keyPattern(BSON("a" << 1)) <<
                    CollectionType::unique(false) <<
                    CollectionType::updatedAt(2ULL) <<
                    CollectionType::epoch(epoch2));
            _dummyConfig->insert( CollectionType::ConfigNS, collFoo );

            BSONObj chunk1 = BSON(ChunkType::name("test.foo-a_MinKey") <<
                    ChunkType::ns("test.foo") <<
                    ChunkType::min(BSON("a" << MINKEY)) <<
                    ChunkType::max(BSON("a" << 10)) <<
                    ChunkType::DEPRECATED_lastmod(_maxCollVersion.toLong()) <<
                    ChunkType::DEPRECATED_epoch(epoch2) <<
                    ChunkType::shard("shard0000"));
            _dummyConfig->insert( ChunkType::ConfigNS, chunk1 );

            OID epoch3 = OID::gen();
            _maxCollVersion = ChunkVersion( 2, 0, epoch3 );
            _maxShardVersion.push_back( _maxCollVersion );

            BSONObj chunk2 = BSON(ChunkType::name("test.foo-a_10") <<
                    ChunkType::ns("test.foo") <<
                    ChunkType::min(BSON("a" << 10)) <<
                    ChunkType::max(BSON("a" << 100)) <<
                    ChunkType::DEPRECATED_lastmod(_maxCollVersion.toLong()) <<
                    ChunkType::DEPRECATED_epoch(epoch3) <<
                    ChunkType::shard("shard0001"));
            _dummyConfig->insert( ChunkType::ConfigNS, chunk2 );

            BSONObj chunk3 = BSON(ChunkType::name("test.foo-a_100") <<
                    ChunkType::ns("test.foo") <<
                    ChunkType::min(BSON("a" << 100)) <<
                    ChunkType::max(BSON("a" << MAXKEY)) <<
                    ChunkType::DEPRECATED_lastmod(_maxCollVersion.toLong()) <<
                    ChunkType::DEPRECATED_epoch(epoch3) <<
                    ChunkType::shard("shard0001"));
            _dummyConfig->insert( ChunkType::ConfigNS, chunk3 );
        }

        void tearDown() {
            MockConnRegistry::get()->removeServer( _dummyConfig->getServerAddress() );
        }

        ChunkVersion getCollVersion() const {
            return _maxCollVersion;
        }

        const ChunkVersion& getShardVersion( size_t shard ) const {
            return _maxShardVersion[shard];
        }

    private:
        unique_ptr<MockRemoteDBServer> _dummyConfig;
        CollectionMetadata _oldMetadata;

        ChunkVersion _maxCollVersion;
        vector<ChunkVersion> _maxShardVersion;
    };
#endif

}  // namespace
}  // namespace mongo
