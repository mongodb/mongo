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

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/config_server_test_fixture.h"

namespace mongo {
namespace {

using unittest::assertGet;

using CommitChunkMigrate = ConfigServerTestFixture;

const NamespaceString kNamespace("TestDB.TestColl");
const KeyPattern kKeyPattern(BSON("a" << 1));

TEST_F(CommitChunkMigrate, ChunksUpdatedCorrectlyWithControlChunk) {
    const auto collUuid = UUID::gen();

    ShardType shard0;
    shard0.setName("shard0");
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    ChunkType migratedChunk, controlChunk;
    {
        ChunkVersion origVersion(12, 7, OID::gen());

        migratedChunk.setNS(kNamespace);
        migratedChunk.setVersion(origVersion);
        migratedChunk.setShard(shard0.getName());
        migratedChunk.setHistory({ChunkHistory(Timestamp(100, 0), shard0.getName())});
        migratedChunk.setMin(BSON("a" << 1));
        migratedChunk.setMax(BSON("a" << 10));

        origVersion.incMinor();

        controlChunk.setNS(kNamespace);
        controlChunk.setVersion(origVersion);
        controlChunk.setShard(shard0.getName());
        controlChunk.setHistory({ChunkHistory(Timestamp(50, 0), shard0.getName())});
        controlChunk.setMin(BSON("a" << 10));
        controlChunk.setMax(BSON("a" << 20));
        controlChunk.setJumbo(true);
    }

    setupCollection(migratedChunk.getNS(), collUuid, kKeyPattern, {migratedChunk, controlChunk});

    Timestamp validAfter{101, 0};
    BSONObj versions = assertGet(ShardingCatalogManager::get(operationContext())
                                     ->commitChunkMigration(operationContext(),
                                                            kNamespace,
                                                            migratedChunk,
                                                            migratedChunk.getVersion().epoch(),
                                                            ShardId(shard0.getName()),
                                                            ShardId(shard1.getName()),
                                                            validAfter));

    // Verify the versions returned match expected values.
    auto mver = assertGet(ChunkVersion::parseWithField(versions, "migratedChunkVersion"));
    ASSERT_EQ(ChunkVersion(migratedChunk.getVersion().majorVersion() + 1,
                           0,
                           migratedChunk.getVersion().epoch()),
              mver);

    auto cver = assertGet(ChunkVersion::parseWithField(versions, "controlChunkVersion"));
    ASSERT_EQ(ChunkVersion(migratedChunk.getVersion().majorVersion() + 1,
                           1,
                           migratedChunk.getVersion().epoch()),
              cver);

    // Verify the chunks ended up in the right shards, and versions match the values returned.
    auto chunkDoc0 = uassertStatusOK(getChunkDoc(operationContext(), migratedChunk.getMin()));
    ASSERT_EQ("shard1", chunkDoc0.getShard().toString());
    ASSERT_EQ(mver, chunkDoc0.getVersion());

    // The migrated chunk's history should be updated.
    ASSERT_EQ(2UL, chunkDoc0.getHistory().size());
    ASSERT_EQ(validAfter, chunkDoc0.getHistory().front().getValidAfter());

    auto chunkDoc1 = uassertStatusOK(getChunkDoc(operationContext(), controlChunk.getMin()));
    ASSERT_EQ("shard0", chunkDoc1.getShard().toString());
    ASSERT_EQ(cver, chunkDoc1.getVersion());

    // The control chunk's history and jumbo status should be unchanged.
    ASSERT_EQ(1UL, chunkDoc1.getHistory().size());
    ASSERT_EQ(controlChunk.getHistory().front().getValidAfter(),
              chunkDoc1.getHistory().front().getValidAfter());
    ASSERT_EQ(controlChunk.getHistory().front().getShard(),
              chunkDoc1.getHistory().front().getShard());
    ASSERT(chunkDoc1.getJumbo());
}

TEST_F(CommitChunkMigrate, ChunksUpdatedCorrectlyWithoutControlChunk) {
    const auto collUuid = UUID::gen();

    ShardType shard0;
    shard0.setName("shard0");
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    int origMajorVersion = 15;
    auto const origVersion = ChunkVersion(origMajorVersion, 4, OID::gen());

    ChunkType chunk0;
    chunk0.setNS(kNamespace);
    chunk0.setVersion(origVersion);
    chunk0.setShard(shard0.getName());
    chunk0.setHistory({ChunkHistory(Timestamp(100, 0), shard0.getName())});

    // apportion
    auto chunkMin = BSON("a" << 1);
    chunk0.setMin(chunkMin);
    auto chunkMax = BSON("a" << 10);
    chunk0.setMax(chunkMax);

    setupCollection(chunk0.getNS(), collUuid, kKeyPattern, {chunk0});

    Timestamp validAfter{101, 0};

    StatusWith<BSONObj> resultBSON = ShardingCatalogManager::get(operationContext())
                                         ->commitChunkMigration(operationContext(),
                                                                chunk0.getNS(),
                                                                chunk0,
                                                                origVersion.epoch(),
                                                                ShardId(shard0.getName()),
                                                                ShardId(shard1.getName()),
                                                                validAfter);

    ASSERT_OK(resultBSON.getStatus());

    // Verify the version returned matches expected value.
    BSONObj versions = resultBSON.getValue();
    auto mver = ChunkVersion::parseWithField(versions, "migratedChunkVersion");
    ASSERT_OK(mver.getStatus());
    ASSERT_EQ(ChunkVersion(origMajorVersion + 1, 0, origVersion.epoch()), mver.getValue());

    auto cver = ChunkVersion::parseWithField(versions, "controlChunkVersion");
    ASSERT_NOT_OK(cver.getStatus());

    // Verify the chunk ended up in the right shard, and version matches the value returned.
    auto chunkDoc0 = uassertStatusOK(getChunkDoc(operationContext(), chunkMin));
    ASSERT_EQ("shard1", chunkDoc0.getShard().toString());
    ASSERT_EQ(mver.getValue(), chunkDoc0.getVersion());
    // The history should be updated.
    ASSERT_EQ(2UL, chunkDoc0.getHistory().size());
    ASSERT_EQ(validAfter, chunkDoc0.getHistory().front().getValidAfter());
}

TEST_F(CommitChunkMigrate, CheckCorrectOpsCommandNoCtlTrimHistory) {
    const auto collUuid = UUID::gen();

    ShardType shard0;
    shard0.setName("shard0");
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    int origMajorVersion = 15;
    auto const origVersion = ChunkVersion(origMajorVersion, 4, OID::gen());

    ChunkType chunk0;
    chunk0.setNS(kNamespace);
    chunk0.setVersion(origVersion);
    chunk0.setShard(shard0.getName());
    chunk0.setHistory({ChunkHistory(Timestamp(100, 0), shard0.getName())});

    // apportion
    auto chunkMin = BSON("a" << 1);
    chunk0.setMin(chunkMin);
    auto chunkMax = BSON("a" << 10);
    chunk0.setMax(chunkMax);

    setupCollection(chunk0.getNS(), collUuid, kKeyPattern, {chunk0});

    // Make the time distance between the last history element large enough.
    Timestamp validAfter{200, 0};

    StatusWith<BSONObj> resultBSON = ShardingCatalogManager::get(operationContext())
                                         ->commitChunkMigration(operationContext(),
                                                                chunk0.getNS(),
                                                                chunk0,
                                                                origVersion.epoch(),
                                                                ShardId(shard0.getName()),
                                                                ShardId(shard1.getName()),
                                                                validAfter);

    ASSERT_OK(resultBSON.getStatus());

    // Verify the version returned matches expected value.
    BSONObj versions = resultBSON.getValue();
    auto mver = ChunkVersion::parseWithField(versions, "migratedChunkVersion");
    ASSERT_OK(mver.getStatus());
    ASSERT_EQ(ChunkVersion(origMajorVersion + 1, 0, origVersion.epoch()), mver.getValue());

    auto cver = ChunkVersion::parseWithField(versions, "controlChunkVersion");
    ASSERT_NOT_OK(cver.getStatus());

    // Verify the chunk ended up in the right shard, and version matches the value returned.
    auto chunkDoc0 = uassertStatusOK(getChunkDoc(operationContext(), chunkMin));
    ASSERT_EQ("shard1", chunkDoc0.getShard().toString());
    ASSERT_EQ(mver.getValue(), chunkDoc0.getVersion());
    // The history should be updated.
    ASSERT_EQ(1UL, chunkDoc0.getHistory().size());
    ASSERT_EQ(validAfter, chunkDoc0.getHistory().front().getValidAfter());
}

TEST_F(CommitChunkMigrate, RejectOutOfOrderHistory) {
    const auto collUuid = UUID::gen();

    ShardType shard0;
    shard0.setName("shard0");
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    int origMajorVersion = 15;
    auto const origVersion = ChunkVersion(origMajorVersion, 4, OID::gen());

    ChunkType chunk0;
    chunk0.setNS(kNamespace);
    chunk0.setVersion(origVersion);
    chunk0.setShard(shard0.getName());
    chunk0.setHistory({ChunkHistory(Timestamp(100, 0), shard0.getName())});

    // apportion
    auto chunkMin = BSON("a" << 1);
    chunk0.setMin(chunkMin);
    auto chunkMax = BSON("a" << 10);
    chunk0.setMax(chunkMax);

    setupCollection(chunk0.getNS(), collUuid, kKeyPattern, {chunk0});

    // Make the time before the last change to trigger the failure.
    Timestamp validAfter{99, 0};

    StatusWith<BSONObj> resultBSON = ShardingCatalogManager::get(operationContext())
                                         ->commitChunkMigration(operationContext(),
                                                                chunk0.getNS(),
                                                                chunk0,
                                                                origVersion.epoch(),
                                                                ShardId(shard0.getName()),
                                                                ShardId(shard1.getName()),
                                                                validAfter);

    ASSERT_EQ(ErrorCodes::IncompatibleShardingMetadata, resultBSON.getStatus());
}

TEST_F(CommitChunkMigrate, RejectWrongCollectionEpoch0) {
    const auto collUuid = UUID::gen();

    ShardType shard0;
    shard0.setName("shard0");
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    int origMajorVersion = 12;
    auto const origVersion = ChunkVersion(origMajorVersion, 7, OID::gen());

    ChunkType chunk0;
    chunk0.setNS(kNamespace);
    chunk0.setVersion(origVersion);
    chunk0.setShard(shard0.getName());

    // apportion
    auto chunkMin = BSON("a" << 1);
    chunk0.setMin(chunkMin);
    auto chunkMax = BSON("a" << 10);
    chunk0.setMax(chunkMax);

    ChunkType chunk1;
    chunk1.setNS(kNamespace);
    chunk1.setVersion(origVersion);
    chunk1.setShard(shard0.getName());

    chunk1.setMin(chunkMax);
    auto chunkMaxax = BSON("a" << 20);
    chunk1.setMax(chunkMaxax);

    setupCollection(chunk0.getNS(), collUuid, kKeyPattern, {chunk0, chunk1});

    Timestamp validAfter{1};

    StatusWith<BSONObj> resultBSON = ShardingCatalogManager::get(operationContext())
                                         ->commitChunkMigration(operationContext(),
                                                                chunk0.getNS(),
                                                                chunk0,
                                                                OID::gen(),
                                                                ShardId(shard0.getName()),
                                                                ShardId(shard1.getName()),
                                                                validAfter);

    ASSERT_EQ(ErrorCodes::StaleEpoch, resultBSON.getStatus());
}

TEST_F(CommitChunkMigrate, RejectWrongCollectionEpoch1) {
    const auto collUuid = UUID::gen();

    ShardType shard0;
    shard0.setName("shard0");
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    int origMajorVersion = 12;
    auto const origVersion = ChunkVersion(origMajorVersion, 7, OID::gen());
    auto const otherVersion = ChunkVersion(origMajorVersion, 7, OID::gen());

    ChunkType chunk0;
    chunk0.setNS(kNamespace);
    chunk0.setVersion(origVersion);
    chunk0.setShard(shard0.getName());

    // apportion
    auto chunkMin = BSON("a" << 1);
    chunk0.setMin(chunkMin);
    auto chunkMax = BSON("a" << 10);
    chunk0.setMax(chunkMax);

    ChunkType chunk1;
    chunk1.setNS(kNamespace);
    chunk1.setVersion(otherVersion);
    chunk1.setShard(shard0.getName());

    chunk1.setMin(chunkMax);
    auto chunkMaxax = BSON("a" << 20);
    chunk1.setMax(chunkMaxax);

    // get version from the control chunk this time
    setupCollection(chunk0.getNS(), collUuid, kKeyPattern, {chunk1, chunk0});

    Timestamp validAfter{1};

    StatusWith<BSONObj> resultBSON = ShardingCatalogManager::get(operationContext())
                                         ->commitChunkMigration(operationContext(),
                                                                chunk0.getNS(),
                                                                chunk0,
                                                                origVersion.epoch(),
                                                                ShardId(shard0.getName()),
                                                                ShardId(shard1.getName()),
                                                                validAfter);

    ASSERT_EQ(ErrorCodes::StaleEpoch, resultBSON.getStatus());
}

TEST_F(CommitChunkMigrate, RejectChunkMissing0) {
    const auto collUuid = UUID::gen();

    ShardType shard0;
    shard0.setName("shard0");
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    int origMajorVersion = 12;
    auto const origVersion = ChunkVersion(origMajorVersion, 7, OID::gen());

    ChunkType chunk0;
    chunk0.setNS(kNamespace);
    chunk0.setVersion(origVersion);
    chunk0.setShard(shard0.getName());

    // apportion
    auto chunkMin = BSON("a" << 1);
    chunk0.setMin(chunkMin);
    auto chunkMax = BSON("a" << 10);
    chunk0.setMax(chunkMax);

    ChunkType chunk1;
    chunk1.setNS(kNamespace);
    chunk1.setVersion(origVersion);
    chunk1.setShard(shard0.getName());

    chunk1.setMin(chunkMax);
    auto chunkMaxax = BSON("a" << 20);
    chunk1.setMax(chunkMaxax);

    setupCollection(chunk1.getNS(), collUuid, kKeyPattern, {chunk1});

    Timestamp validAfter{1};

    StatusWith<BSONObj> resultBSON = ShardingCatalogManager::get(operationContext())
                                         ->commitChunkMigration(operationContext(),
                                                                chunk0.getNS(),
                                                                chunk0,
                                                                origVersion.epoch(),
                                                                ShardId(shard0.getName()),
                                                                ShardId(shard1.getName()),
                                                                validAfter);

    ASSERT_EQ(40165, resultBSON.getStatus().code());
}

TEST_F(CommitChunkMigrate, CommitWithLastChunkOnShardShouldNotAffectOtherChunks) {
    const auto collUuid = UUID::gen();

    ShardType shard0;
    shard0.setName("shard0");
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    int origMajorVersion = 12;
    auto const origVersion = ChunkVersion(origMajorVersion, 7, OID::gen());

    ChunkType chunk0;
    chunk0.setNS(kNamespace);
    chunk0.setVersion(origVersion);
    chunk0.setShard(shard0.getName());
    chunk0.setHistory({ChunkHistory(Timestamp(100, 0), shard0.getName())});

    // apportion
    auto chunkMin = BSON("a" << 1);
    chunk0.setMin(chunkMin);
    auto chunkMax = BSON("a" << 10);
    chunk0.setMax(chunkMax);

    ChunkType chunk1;
    chunk1.setNS(kNamespace);
    chunk1.setVersion(origVersion);
    chunk1.setShard(shard1.getName());

    chunk1.setMin(chunkMax);
    auto chunkMaxax = BSON("a" << 20);
    chunk1.setMax(chunkMaxax);

    Timestamp ctrlChunkValidAfter = Timestamp(50, 0);
    chunk1.setHistory({ChunkHistory(ctrlChunkValidAfter, shard1.getName())});

    setupCollection(chunk0.getNS(), collUuid, kKeyPattern, {chunk0, chunk1});

    Timestamp validAfter{101, 0};
    StatusWith<BSONObj> resultBSON = ShardingCatalogManager::get(operationContext())
                                         ->commitChunkMigration(operationContext(),
                                                                chunk0.getNS(),
                                                                chunk0,
                                                                origVersion.epoch(),
                                                                ShardId(shard0.getName()),
                                                                ShardId(shard1.getName()),
                                                                validAfter);

    ASSERT_OK(resultBSON.getStatus());

    // Verify the versions returned match expected values.
    BSONObj versions = resultBSON.getValue();
    auto mver = ChunkVersion::parseWithField(versions, "migratedChunkVersion");
    ASSERT_OK(mver.getStatus());
    ASSERT_EQ(ChunkVersion(origMajorVersion + 1, 0, origVersion.epoch()), mver.getValue());

    ASSERT_TRUE(versions["controlChunkVersion"].eoo());

    // Verify the chunks ended up in the right shards, and versions match the values returned.
    auto chunkDoc0 = uassertStatusOK(getChunkDoc(operationContext(), chunkMin));
    ASSERT_EQ(shard1.getName(), chunkDoc0.getShard().toString());
    ASSERT_EQ(mver.getValue(), chunkDoc0.getVersion());

    // The migrated chunk's history should be updated.
    ASSERT_EQ(2UL, chunkDoc0.getHistory().size());
    ASSERT_EQ(validAfter, chunkDoc0.getHistory().front().getValidAfter());

    auto chunkDoc1 = uassertStatusOK(getChunkDoc(operationContext(), chunkMax));
    ASSERT_EQ(shard1.getName(), chunkDoc1.getShard().toString());
    ASSERT_EQ(chunk1.getVersion(), chunkDoc1.getVersion());

    // The control chunk's history should be unchanged.
    ASSERT_EQ(1UL, chunkDoc1.getHistory().size());
    ASSERT_EQ(ctrlChunkValidAfter, chunkDoc1.getHistory().front().getValidAfter());
}

}  // namespace
}  // namespace mongo
