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
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/metadata_loader.h"
#include "mongo/s/catalog/replset/sharding_catalog_test_fixture.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"

namespace mongo {
namespace {

using std::string;
using std::unique_ptr;
using std::vector;

class MetadataLoaderFixture : public ShardingCatalogTestFixture {
public:
    MetadataLoaderFixture() = default;
    ~MetadataLoaderFixture() = default;

protected:
    void setUp() override {
        ShardingCatalogTestFixture::setUp();
        setRemote(HostAndPort("FakeRemoteClient:34567"));
        configTargeter()->setFindHostReturnValue(configHost);
        _maxCollVersion = ChunkVersion(1, 0, OID::gen());
        _loader.reset(new MetadataLoader);
    }

    void expectFindOnConfigSendCollectionDefault() {
        CollectionType collType;
        collType.setNs(NamespaceString{"test.foo"});
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
            << ChunkType::ns("test.foo")
            << ChunkType::min(BSON("a" << MINKEY))
            << ChunkType::max(BSON("a" << MAXKEY))
            << ChunkType::DEPRECATED_lastmod(Date_t::fromMillisSinceEpoch(_maxCollVersion.toLong()))
            << ChunkType::DEPRECATED_epoch(_maxCollVersion.epoch())
            << ChunkType::shard("shard0000"));
        expectFindOnConfigSendBSONObjVector(std::vector<BSONObj>{chunk});
    }

    MetadataLoader& loader() const {
        return *_loader;
    }

    void getMetadataFor(const OwnedPointerVector<ChunkType>& chunks, CollectionMetadata* metadata) {
        // Infer namespace, shard, epoch, keypattern from first chunk
        const ChunkType* firstChunk = *(chunks.vector().begin());
        const string ns = firstChunk->getNS();
        const string shardName = firstChunk->getShard().toString();
        const OID epoch = firstChunk->getVersion().epoch();

        CollectionType coll;
        coll.setNs(NamespaceString{ns});
        coll.setKeyPattern(BSON("a" << 1));
        coll.setUpdatedAt(Date_t::fromMillisSinceEpoch(1));
        coll.setEpoch(epoch);
        ASSERT_OK(coll.validate());
        std::vector<BSONObj> collToSend{coll.toBSON()};

        ChunkVersion version(1, 0, epoch);
        std::vector<BSONObj> chunksToSend;
        for (const auto chunkVal : chunks.vector()) {
            ChunkType chunk(*chunkVal);

            if (!chunk.isVersionSet()) {
                chunk.setVersion(version);
                version.incMajor();
            }

            ASSERT(chunk.validate().isOK());
            chunksToSend.push_back(chunk.toBSON());
        }

        auto future = launchAsync([this, ns, shardName, metadata] {
            auto status = loader().makeCollectionMetadata(operationContext(),
                                                          catalogClient(),
                                                          ns,
                                                          shardName,
                                                          NULL, /* no old metadata */
                                                          metadata);
            ASSERT_OK(status);
        });

        expectFindOnConfigSendBSONObjVector(collToSend);
        expectFindOnConfigSendBSONObjVector(chunksToSend);

        future.timed_get(kFutureTimeout);
    }

    ChunkVersion getMaxCollVersion() const {
        return _maxCollVersion;
    }

    ChunkVersion getMaxShardVersion() const {
        return _maxCollVersion;
    }

private:
    const HostAndPort configHost{HostAndPort(CONFIG_HOST_PORT)};

    unique_ptr<MetadataLoader> _loader;
    ChunkVersion _maxCollVersion;
};

// TODO: Test config server down
// TODO: Test read of chunks with new epoch
// TODO: Test that you can properly load config using format with deprecated fields?

TEST_F(MetadataLoaderFixture, DroppedColl) {
    CollectionType collType;
    collType.setNs(NamespaceString{"test.foo"});
    collType.setKeyPattern(BSON("a" << 1));
    collType.setUpdatedAt(Date_t());
    collType.setEpoch(OID());
    collType.setDropped(true);
    ASSERT_OK(collType.validate());
    auto future = launchAsync([this] {
        MetadataLoader loader;
        CollectionMetadata metadata;
        auto status = loader.makeCollectionMetadata(operationContext(),
                                                    catalogClient(),
                                                    "test.foo",
                                                    "shard0000",
                                                    NULL, /* no old metadata */
                                                    &metadata);
        ASSERT_EQUALS(status.code(), ErrorCodes::NamespaceNotFound);
    });
    expectFindOnConfigSendBSONObjVector(std::vector<BSONObj>{collType.toBSON()});
    future.timed_get(kFutureTimeout);
}

TEST_F(MetadataLoaderFixture, EmptyColl) {
    auto future = launchAsync([this] {
        MetadataLoader loader;
        CollectionMetadata metadata;
        auto status = loader.makeCollectionMetadata(operationContext(),
                                                    catalogClient(),
                                                    "test.foo",
                                                    "shard0000",
                                                    NULL, /* no old metadata */
                                                    &metadata);
        ASSERT_EQUALS(status.code(), ErrorCodes::NamespaceNotFound);
    });
    expectFindOnConfigSendErrorCode(ErrorCodes::NamespaceNotFound);
    future.timed_get(kFutureTimeout);
}

TEST_F(MetadataLoaderFixture, BadColl) {
    BSONObj badCollToSend = BSON(CollectionType::fullNs("test.foo"));
    auto future = launchAsync([this] {
        MetadataLoader loader;
        CollectionMetadata metadata;
        auto status = loader.makeCollectionMetadata(operationContext(),
                                                    catalogClient(),
                                                    "test.foo",
                                                    "shard0000",
                                                    NULL, /* no old metadata */
                                                    &metadata);
        ASSERT_EQUALS(status.code(), ErrorCodes::NoSuchKey);
    });
    expectFindOnConfigSendBSONObjVector(std::vector<BSONObj>{badCollToSend});
    future.timed_get(kFutureTimeout);
}

TEST_F(MetadataLoaderFixture, BadChunk) {
    CollectionType collType;
    collType.setNs(NamespaceString{"test.foo"});
    collType.setUpdatedAt(Date_t::fromMillisSinceEpoch(1));
    collType.setKeyPattern(BSON("a" << 1));
    collType.setEpoch(OID::gen());
    ASSERT_OK(collType.validate());

    ChunkType chunkInfo;
    chunkInfo.setNS(NamespaceString{"test.foo"}.ns());
    chunkInfo.setVersion(ChunkVersion(1, 0, collType.getEpoch()));
    ASSERT(!chunkInfo.validate().isOK());

    auto future = launchAsync([this] {
        MetadataLoader loader;
        CollectionMetadata metadata;
        auto status = loader.makeCollectionMetadata(operationContext(),
                                                    catalogClient(),
                                                    "test.foo",
                                                    "shard0000",
                                                    NULL, /* no old metadata */
                                                    &metadata);
        ASSERT_EQUALS(status.code(), ErrorCodes::FailedToParse);
    });

    expectFindOnConfigSendBSONObjVector(std::vector<BSONObj>{collType.toBSON()});
    expectFindOnConfigSendBSONObjVector(std::vector<BSONObj>{chunkInfo.toBSON()});
    future.timed_get(kFutureTimeout);
}

TEST_F(MetadataLoaderFixture, NoChunksIsDropped) {
    OID epoch = OID::gen();

    CollectionType collType;
    collType.setNs(NamespaceString{"test.foo"});
    collType.setKeyPattern(BSON("a" << 1));
    collType.setUnique(false);
    collType.setUpdatedAt(Date_t::fromMillisSinceEpoch(1));
    collType.setEpoch(epoch);

    auto future = launchAsync([this] {
        MetadataLoader loader;
        CollectionMetadata metadata;
        auto status = loader.makeCollectionMetadata(operationContext(),
                                                    catalogClient(),
                                                    "test.foo",
                                                    "shard0000",
                                                    NULL, /* no old metadata */
                                                    &metadata);
        // This is interpreted as a dropped ns, since we drop the chunks first
        ASSERT_EQUALS(status.code(), ErrorCodes::NamespaceNotFound);
    });
    expectFindOnConfigSendBSONObjVector(std::vector<BSONObj>{collType.toBSON()});
    expectFindOnConfigSendErrorCode(ErrorCodes::NamespaceNotFound);
    future.timed_get(kFutureTimeout);
}

TEST_F(MetadataLoaderFixture, CheckNumChunk) {
    OID epoch = OID::gen();

    CollectionType collType;
    collType.setNs(NamespaceString{"test.foo"});
    collType.setKeyPattern(BSON("a" << 1));
    collType.setUnique(false);
    collType.setUpdatedAt(Date_t::fromMillisSinceEpoch(1));
    collType.setEpoch(epoch);
    ASSERT_OK(collType.validate());

    // Need a chunk on another shard, otherwise the chunks are invalid in general and we
    // can't load metadata
    ChunkType chunkType;
    chunkType.setNS("test.foo");
    chunkType.setShard(ShardId("shard0001"));
    chunkType.setMin(BSON("a" << MINKEY));
    chunkType.setMax(BSON("a" << MAXKEY));
    chunkType.setVersion(ChunkVersion(1, 0, epoch));
    ASSERT(chunkType.validate().isOK());

    auto future = launchAsync([this] {
        MetadataLoader loader;
        CollectionMetadata metadata;
        auto status = loader.makeCollectionMetadata(operationContext(),
                                                    catalogClient(),
                                                    "test.foo",
                                                    "shard0000",
                                                    NULL, /* no old metadata */
                                                    &metadata);
        std::cout << "status: " << status << std::endl;
        ASSERT_OK(status);
        ASSERT_EQUALS(0U, metadata.getNumChunks());
        ASSERT_EQUALS(1, metadata.getCollVersion().majorVersion());
        ASSERT_EQUALS(0, metadata.getShardVersion().majorVersion());
        ASSERT_NOT_EQUALS(OID(), metadata.getCollVersion().epoch());
        ASSERT_NOT_EQUALS(OID(), metadata.getShardVersion().epoch());
    });

    expectFindOnConfigSendBSONObjVector(std::vector<BSONObj>{collType.toBSON()});
    expectFindOnConfigSendBSONObjVector(std::vector<BSONObj>{chunkType.toBSON()});

    future.timed_get(kFutureTimeout);
}

TEST_F(MetadataLoaderFixture, SingleChunkCheckNumChunk) {
    auto future = launchAsync([this] {
        MetadataLoader loader;
        CollectionMetadata metadata;
        auto status = loader.makeCollectionMetadata(operationContext(),
                                                    catalogClient(),
                                                    "test.foo",
                                                    "shard0000",
                                                    NULL, /* no old metadata */
                                                    &metadata);
        ASSERT_OK(status);
        ASSERT_EQUALS(1U, metadata.getNumChunks());
    });

    expectFindOnConfigSendCollectionDefault();
    expectFindOnConfigSendChunksDefault();

    future.timed_get(kFutureTimeout);
}

TEST_F(MetadataLoaderFixture, SingleChunkGetNext) {
    auto future = launchAsync([this] {
        MetadataLoader loader;
        CollectionMetadata metadata;
        auto status = loader.makeCollectionMetadata(operationContext(),
                                                    catalogClient(),
                                                    "test.foo",
                                                    "shard0000",
                                                    NULL, /* no old metadata */
                                                    &metadata);
        ChunkType chunkInfo;
        ASSERT_TRUE(metadata.getNextChunk(metadata.getMinKey(), &chunkInfo));
    });

    expectFindOnConfigSendCollectionDefault();
    expectFindOnConfigSendChunksDefault();

    future.timed_get(kFutureTimeout);
}

TEST_F(MetadataLoaderFixture, SingleChunkGetShardKey) {
    auto future = launchAsync([this] {
        MetadataLoader loader;
        CollectionMetadata metadata;
        auto status = loader.makeCollectionMetadata(operationContext(),
                                                    catalogClient(),
                                                    "test.foo",
                                                    "shard0000",
                                                    NULL, /* no old metadata */
                                                    &metadata);
        ChunkType chunkInfo;
        ASSERT_TRUE(metadata.getKeyPattern().equal(BSON("a" << 1)));
    });

    expectFindOnConfigSendCollectionDefault();
    expectFindOnConfigSendChunksDefault();

    future.timed_get(kFutureTimeout);
}

TEST_F(MetadataLoaderFixture, SingleChunkGetMaxCollVersion) {
    auto future = launchAsync([this] {
        MetadataLoader loader;
        CollectionMetadata metadata;
        auto status = loader.makeCollectionMetadata(operationContext(),
                                                    catalogClient(),
                                                    "test.foo",
                                                    "shard0000",
                                                    NULL, /* no old metadata */
                                                    &metadata);
        ASSERT_TRUE(getMaxCollVersion().equals(metadata.getCollVersion()));
    });

    expectFindOnConfigSendCollectionDefault();
    expectFindOnConfigSendChunksDefault();

    future.timed_get(kFutureTimeout);
}
TEST_F(MetadataLoaderFixture, SingleChunkGetMaxShardVersion) {
    auto future = launchAsync([this] {
        MetadataLoader loader;
        CollectionMetadata metadata;
        auto status = loader.makeCollectionMetadata(operationContext(),
                                                    catalogClient(),
                                                    "test.foo",
                                                    "shard0000",
                                                    NULL, /* no old metadata */
                                                    &metadata);
        ASSERT_TRUE(getMaxShardVersion().equals(metadata.getShardVersion()));
    });
    expectFindOnConfigSendCollectionDefault();
    expectFindOnConfigSendChunksDefault();

    future.timed_get(kFutureTimeout);
}

TEST_F(MetadataLoaderFixture, NoChunks) {
    auto future = launchAsync([this] {
        MetadataLoader loader;
        CollectionMetadata metadata;
        auto status = loader.makeCollectionMetadata(operationContext(),
                                                    catalogClient(),
                                                    "test.foo",
                                                    "shard0000",
                                                    NULL, /* no old metadata */
                                                    &metadata);
        // NSNotFound because we're reloading with no old metadata
        ASSERT_EQUALS(status.code(), ErrorCodes::NamespaceNotFound);
    });
    expectFindOnConfigSendCollectionDefault();
    expectFindOnConfigSendErrorCode(ErrorCodes::NamespaceNotFound);

    future.timed_get(kFutureTimeout);
}

TEST_F(MetadataLoaderFixture, PromotePendingNA) {
    unique_ptr<ChunkType> chunk(new ChunkType());
    chunk->setNS("foo.bar");
    chunk->setShard(ShardId("shard0000"));
    chunk->setMin(BSON("x" << MINKEY));
    chunk->setMax(BSON("x" << 0));
    chunk->setVersion(ChunkVersion(1, 0, OID::gen()));

    OwnedPointerVector<ChunkType> chunks;
    chunks.mutableVector().push_back(chunk.release());

    CollectionMetadata afterMetadata;
    getMetadataFor(chunks, &afterMetadata);

    // Metadata of different epoch
    (*chunks.vector().begin())->setVersion(ChunkVersion(1, 0, OID::gen()));

    CollectionMetadata remoteMetadata;
    getMetadataFor(chunks, &remoteMetadata);

    Status status = loader().promotePendingChunks(&afterMetadata, &remoteMetadata);
    ASSERT_OK(status);

    ChunkType pending;
    pending.setMin(BSON("x" << 0));
    pending.setMax(BSON("x" << 10));

    unique_ptr<CollectionMetadata> cloned(afterMetadata.clonePlusPending(pending));
    status = loader().promotePendingChunks(cloned.get(), &remoteMetadata);
    ASSERT_OK(status);
    ASSERT_EQUALS(remoteMetadata.getNumPending(), 0u);
}

TEST_F(MetadataLoaderFixture, PromotePendingNAVersion) {
    OID epoch = OID::gen();

    unique_ptr<ChunkType> chunk(new ChunkType());
    chunk->setNS("foo.bar");
    chunk->setShard(ShardId("shard0000"));
    chunk->setMin(BSON("x" << MINKEY));
    chunk->setMax(BSON("x" << 0));
    chunk->setVersion(ChunkVersion(1, 1, epoch));

    OwnedPointerVector<ChunkType> chunks;
    chunks.mutableVector().push_back(chunk.release());

    CollectionMetadata afterMetadata;
    getMetadataFor(chunks, &afterMetadata);

    // Metadata of same epoch, but lower version
    (*chunks.vector().begin())->setVersion(ChunkVersion(1, 0, epoch));

    CollectionMetadata remoteMetadata;
    getMetadataFor(chunks, &remoteMetadata);

    Status status = loader().promotePendingChunks(&afterMetadata, &remoteMetadata);
    ASSERT_OK(status);

    ChunkType pending;
    pending.setMin(BSON("x" << 0));
    pending.setMax(BSON("x" << 10));

    unique_ptr<CollectionMetadata> cloned(afterMetadata.clonePlusPending(pending));
    status = loader().promotePendingChunks(cloned.get(), &remoteMetadata);
    ASSERT_OK(status);
    ASSERT_EQUALS(remoteMetadata.getNumPending(), 0u);
}

TEST_F(MetadataLoaderFixture, PromotePendingGoodOverlap) {
    OID epoch = OID::gen();

    //
    // Setup chunk range for remote metadata
    //

    OwnedPointerVector<ChunkType> chunks;

    unique_ptr<ChunkType> chunk(new ChunkType());
    chunk->setNS("foo.bar");
    chunk->setShard(ShardId("shard0000"));
    chunk->setMin(BSON("x" << MINKEY));
    chunk->setMax(BSON("x" << 0));
    chunk->setVersion(ChunkVersion(1, 0, epoch));
    chunks.mutableVector().push_back(chunk.release());

    chunk.reset(new ChunkType());
    chunk->setNS("foo.bar");
    chunk->setShard(ShardId("shard0000"));
    chunk->setMin(BSON("x" << 10));
    chunk->setMax(BSON("x" << 20));
    chunks.mutableVector().push_back(chunk.release());

    chunk.reset(new ChunkType());
    chunk->setNS("foo.bar");
    chunk->setShard(ShardId("shard0000"));
    chunk->setMin(BSON("x" << 30));
    chunk->setMax(BSON("x" << MAXKEY));
    chunks.mutableVector().push_back(chunk.release());

    CollectionMetadata remoteMetadata;
    getMetadataFor(chunks, &remoteMetadata);

    //
    // Setup chunk and pending range for afterMetadata
    //

    chunks.clear();

    chunk.reset(new ChunkType());
    chunk->setNS("foo.bar");
    chunk->setShard(ShardId("shard0000"));
    chunk->setMin(BSON("x" << 0));
    chunk->setMax(BSON("x" << 10));
    chunk->setVersion(ChunkVersion(1, 0, epoch));

    chunks.mutableVector().push_back(chunk.release());

    CollectionMetadata afterMetadata;
    getMetadataFor(chunks, &afterMetadata);

    ChunkType pending;
    pending.setMin(BSON("x" << MINKEY));
    pending.setMax(BSON("x" << 0));

    unique_ptr<CollectionMetadata> cloned(afterMetadata.clonePlusPending(pending));

    pending.setMin(BSON("x" << 10));
    pending.setMax(BSON("x" << 20));

    cloned = cloned->clonePlusPending(pending);

    pending.setMin(BSON("x" << 20));
    pending.setMax(BSON("x" << 30));

    cloned = cloned->clonePlusPending(pending);

    pending.setMin(BSON("x" << 30));
    pending.setMax(BSON("x" << MAXKEY));

    cloned = cloned->clonePlusPending(pending);

    Status status = loader().promotePendingChunks(cloned.get(), &remoteMetadata);
    ASSERT_OK(status);

    ASSERT_EQUALS(remoteMetadata.getNumPending(), 1u);
    ASSERT(remoteMetadata.keyIsPending(BSON("x" << 25)));
}

TEST_F(MetadataLoaderFixture, PromotePendingBadOverlap) {
    OID epoch = OID::gen();

    //
    // Setup chunk range for remote metadata
    //

    OwnedPointerVector<ChunkType> chunks;

    unique_ptr<ChunkType> chunk(new ChunkType());
    chunk->setNS("foo.bar");
    chunk->setShard(ShardId("shard0000"));
    chunk->setMin(BSON("x" << MINKEY));
    chunk->setMax(BSON("x" << 0));
    chunk->setVersion(ChunkVersion(1, 0, epoch));

    chunks.mutableVector().push_back(chunk.release());

    CollectionMetadata remoteMetadata;
    getMetadataFor(chunks, &remoteMetadata);

    //
    // Setup chunk and pending range for afterMetadata
    //

    chunks.clear();

    chunk.reset(new ChunkType());
    chunk->setNS("foo.bar");
    chunk->setShard(ShardId("shard0000"));
    chunk->setMin(BSON("x" << 15));
    chunk->setMax(BSON("x" << MAXKEY));
    chunk->setVersion(ChunkVersion(1, 0, epoch));

    chunks.mutableVector().push_back(chunk.release());

    CollectionMetadata afterMetadata;
    getMetadataFor(chunks, &afterMetadata);

    ChunkType pending;
    pending.setMin(BSON("x" << MINKEY));
    pending.setMax(BSON("x" << 1));

    unique_ptr<CollectionMetadata> cloned(afterMetadata.clonePlusPending(pending));
    cloned = cloned->clonePlusPending(pending);

    Status status = loader().promotePendingChunks(cloned.get(), &remoteMetadata);
    ASSERT_EQUALS(status.code(), ErrorCodes::RemoteChangeDetected);
}

}  // namespace
}  // namespace mongo
