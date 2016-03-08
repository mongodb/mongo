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

#include "mongo/base/status.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/db/commands.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/metadata_loader_fixture.h"
#include "mongo/s/write_ops/batched_command_response.h"

namespace mongo {

using std::string;
using std::unique_ptr;
using std::vector;

using executor::RemoteCommandRequest;

MetadataLoaderFixture::MetadataLoaderFixture() = default;
MetadataLoaderFixture::~MetadataLoaderFixture() = default;

void MetadataLoaderFixture::setUp() {
    CatalogManagerReplSetTestFixture::setUp();
    getMessagingPort()->setRemote(HostAndPort("FakeRemoteClient:34567"));
    configTargeter()->setFindHostReturnValue(configHost);
    _epoch = OID::gen();
    _maxCollVersion = ChunkVersion(1, 0, _epoch);
    _loader.reset(new MetadataLoader);
}

void MetadataLoaderFixture::expectFindOnConfigSendCollectionDefault() {
    CollectionType collType;
    collType.setNs(NamespaceString{"test.foo"});
    collType.setKeyPattern(BSON("a" << 1));
    collType.setUnique(false);
    collType.setUpdatedAt(Date_t::fromMillisSinceEpoch(1));
    collType.setEpoch(_epoch);
    ASSERT_OK(collType.validate());
    expectFindOnConfigSendBSONObjVector(std::vector<BSONObj>{collType.toBSON()});
}

void MetadataLoaderFixture::expectFindOnConfigSendChunksDefault() {
    ChunkVersion _maxCollVersion = ChunkVersion(1, 0, _epoch);
    BSONObj chunk = BSON(
        ChunkType::name("test.foo-a_MinKey")
        << ChunkType::ns("test.foo") << ChunkType::min(BSON("a" << MINKEY))
        << ChunkType::max(BSON("a" << MAXKEY))
        << ChunkType::DEPRECATED_lastmod(Date_t::fromMillisSinceEpoch(_maxCollVersion.toLong()))
        << ChunkType::DEPRECATED_epoch(_epoch) << ChunkType::shard("shard0000"));
    expectFindOnConfigSendBSONObjVector(std::vector<BSONObj>{chunk});
}

MetadataLoader& MetadataLoaderFixture::loader() const {
    return *_loader;
}

void MetadataLoaderFixture::getMetadataFor(const OwnedPointerVector<ChunkType>& chunks,
                                           CollectionMetadata* metadata) {
    // Infer namespace, shard, epoch, keypattern from first chunk
    const ChunkType* firstChunk = *(chunks.vector().begin());
    const string ns = firstChunk->getNS();
    const string shardName = firstChunk->getShard();
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

        chunk.setName(OID::gen().toString());
        if (!chunk.isVersionSet()) {
            chunk.setVersion(version);
            version.incMajor();
        }

        ASSERT(chunk.validate().isOK());
        chunksToSend.push_back(chunk.toBSON());
    }

    auto future = launchAsync([this, ns, shardName, metadata] {
        auto status = this->loader().makeCollectionMetadata(operationContext(),
                                                            catalogManager(),
                                                            ns,
                                                            shardName,
                                                            NULL, /* no old metadata */
                                                            metadata);
        ASSERT(status.isOK());
    });

    expectFindOnConfigSendBSONObjVector(collToSend);
    expectFindOnConfigSendBSONObjVector(chunksToSend);

    future.timed_get(kFutureTimeout);
}

ChunkVersion MetadataLoaderFixture::getMaxCollVersion() const {
    return _maxCollVersion;
}

ChunkVersion MetadataLoaderFixture::getMaxShardVersion() const {
    return _maxCollVersion;
}

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
                                                    catalogManager(),
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
                                                    catalogManager(),
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
                                                    catalogManager(),
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
                                                    catalogManager(),
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
                                                    catalogManager(),
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
    chunkType.setShard("shard0001");
    chunkType.setMin(BSON("a" << MINKEY));
    chunkType.setMax(BSON("a" << MAXKEY));
    chunkType.setVersion(ChunkVersion(1, 0, epoch));
    chunkType.setName(OID::gen().toString());
    ASSERT(chunkType.validate().isOK());

    auto future = launchAsync([this] {
        MetadataLoader loader;
        CollectionMetadata metadata;
        auto status = loader.makeCollectionMetadata(operationContext(),
                                                    catalogManager(),
                                                    "test.foo",
                                                    "shard0000",
                                                    NULL, /* no old metadata */
                                                    &metadata);
        std::cout << "status: " << status << std::endl;
        ASSERT(status.isOK());
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
                                                    catalogManager(),
                                                    "test.foo",
                                                    "shard0000",
                                                    NULL, /* no old metadata */
                                                    &metadata);
        ASSERT(status.isOK());
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
                                                    catalogManager(),
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
                                                    catalogManager(),
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
                                                    catalogManager(),
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
                                                    catalogManager(),
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
                                                    catalogManager(),
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
    chunk->setShard("shard0000");
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

TEST_F(MetadataLoaderFixture, PromotePendingNAVersion) {
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
    getMetadataFor(chunks, &afterMetadata);

    // Metadata of same epoch, but lower version
    (*chunks.vector().begin())->setVersion(ChunkVersion(1, 0, epoch));

    CollectionMetadata remoteMetadata;
    getMetadataFor(chunks, &remoteMetadata);

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

TEST_F(MetadataLoaderFixture, PromotePendingGoodOverlap) {
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
    getMetadataFor(chunks, &remoteMetadata);

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
    getMetadataFor(chunks, &afterMetadata);

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

TEST_F(MetadataLoaderFixture, PromotePendingBadOverlap) {
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
    getMetadataFor(chunks, &remoteMetadata);

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
    getMetadataFor(chunks, &afterMetadata);

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

}  // namespace mongo
