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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/read_write_concern_defaults_cache_lookup_mock.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/config/config_server_test_fixture.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/db/session/logical_session_cache_noop.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

using unittest::assertGet;

class CommitChunkMigrate : public ConfigServerTestFixture {
protected:
    void setUp() override {

        ConfigServerTestFixture::setUp();
        DBDirectClient client(operationContext());
        client.createCollection(NamespaceString::kSessionTransactionsTableNamespace.ns());
        client.createIndexes(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                             {MongoDSessionCatalog::getConfigTxnPartialIndexSpec()});

        ReadWriteConcernDefaults::create(getServiceContext(), _lookupMock.getFetchDefaultsFn());

        LogicalSessionCache::set(getServiceContext(), std::make_unique<LogicalSessionCacheNoop>());
        TransactionCoordinatorService::get(operationContext())
            ->onShardingInitialization(operationContext(), true);

        WaitForMajorityService::get(getServiceContext()).startup(getServiceContext());
    }

    void tearDown() override {
        TransactionCoordinatorService::get(operationContext())->onStepDown();
        WaitForMajorityService::get(getServiceContext()).shutDown();
        ConfigServerTestFixture::tearDown();
    }

    // Allows the usage of transactions.
    ReadWriteConcernDefaultsLookupMock _lookupMock;
};

const NamespaceString kNamespace("TestDB.TestColl");
const KeyPattern kKeyPattern(BSON("x" << 1));

TEST_F(CommitChunkMigrate, ChunksUpdatedCorrectly) {
    const auto collUUID = UUID::gen();
    const auto collEpoch = OID::gen();
    const auto collTimestamp = Timestamp(42);

    ShardType shard0;
    shard0.setName("shard0");
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    ChunkType migratedChunk, controlChunk;
    {
        ChunkVersion origVersion({collEpoch, collTimestamp}, {12, 7});

        migratedChunk.setName(OID::gen());
        migratedChunk.setCollectionUUID(collUUID);
        migratedChunk.setVersion(origVersion);
        migratedChunk.setShard(shard0.getName());
        migratedChunk.setOnCurrentShardSince(Timestamp(100, 0));
        migratedChunk.setHistory(
            {ChunkHistory(*migratedChunk.getOnCurrentShardSince(), shard0.getName())});
        migratedChunk.setMin(BSON("a" << 1));
        migratedChunk.setMax(BSON("a" << 10));

        origVersion.incMinor();

        controlChunk.setName(OID::gen());
        controlChunk.setCollectionUUID(collUUID);
        controlChunk.setVersion(origVersion);
        controlChunk.setShard(shard0.getName());
        controlChunk.setOnCurrentShardSince(Timestamp(50, 0));
        controlChunk.setHistory(
            {ChunkHistory(*controlChunk.getOnCurrentShardSince(), shard0.getName())});
        controlChunk.setMin(BSON("a" << 10));
        controlChunk.setMax(BSON("a" << 20));
        controlChunk.setJumbo(true);
    }

    setupCollection(kNamespace, kKeyPattern, {migratedChunk, controlChunk});

    Timestamp validAfter{101, 0};
    auto versions = assertGet(ShardingCatalogManager::get(operationContext())
                                  ->commitChunkMigration(operationContext(),
                                                         kNamespace,
                                                         migratedChunk,
                                                         migratedChunk.getVersion().epoch(),
                                                         collTimestamp,
                                                         ShardId(shard0.getName()),
                                                         ShardId(shard1.getName()),
                                                         validAfter));

    // Verify the versions returned match expected values.
    auto mver = versions.shardVersion;
    ASSERT_EQ(ChunkVersion(
                  {migratedChunk.getVersion().epoch(), migratedChunk.getVersion().getTimestamp()},
                  {migratedChunk.getVersion().majorVersion() + 1, 1}),
              mver);

    // Verify that a collection version is returned
    auto cver = versions.collectionVersion;
    ASSERT_TRUE(mver.isOlderOrEqualThan(cver));

    // Verify the chunks ended up in the right shards.
    auto chunkDoc0 = uassertStatusOK(
        getChunkDoc(operationContext(), migratedChunk.getMin(), collEpoch, collTimestamp));
    ASSERT_EQ("shard1", chunkDoc0.getShard().toString());

    // The migrated chunk's history should be updated.
    ASSERT_EQ(2UL, chunkDoc0.getHistory().size());
    ASSERT_EQ(validAfter, chunkDoc0.getHistory().front().getValidAfter());
    ASSERT_EQ(validAfter, *chunkDoc0.getOnCurrentShardSince());

    auto chunkDoc1 = uassertStatusOK(
        getChunkDoc(operationContext(), controlChunk.getMin(), collEpoch, collTimestamp));
    ASSERT_EQ("shard0", chunkDoc1.getShard().toString());

    // The control chunk's history and jumbo status should be unchanged.
    ASSERT_EQ(1UL, chunkDoc1.getHistory().size());
    ASSERT_EQ(controlChunk.getHistory().front().getValidAfter(),
              chunkDoc1.getHistory().front().getValidAfter());
    ASSERT_EQ(controlChunk.getHistory().front().getShard(),
              chunkDoc1.getHistory().front().getShard());
    ASSERT(chunkDoc1.getOnCurrentShardSince().has_value());
    ASSERT_EQ(controlChunk.getOnCurrentShardSince(), chunkDoc1.getOnCurrentShardSince());
    ASSERT(chunkDoc1.getJumbo());
}

TEST_F(CommitChunkMigrate, ChunksUpdatedCorrectlyWithoutControlChunk) {
    const auto collUUID = UUID::gen();
    const auto collEpoch = OID::gen();
    const auto collTimestamp = Timestamp(42);

    ShardType shard0;
    shard0.setName("shard0");
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    uint32_t origMajorVersion = 15;
    auto const origVersion = ChunkVersion({collEpoch, collTimestamp}, {origMajorVersion, 4});

    ChunkType chunk0;
    chunk0.setName(OID::gen());
    chunk0.setCollectionUUID(collUUID);
    chunk0.setVersion(origVersion);
    chunk0.setShard(shard0.getName());
    chunk0.setOnCurrentShardSince(Timestamp(100, 0));
    chunk0.setHistory({ChunkHistory(*chunk0.getOnCurrentShardSince(), shard0.getName())});

    // apportion
    auto chunkMin = BSON("a" << 1);
    chunk0.setMin(chunkMin);
    auto chunkMax = BSON("a" << 10);
    chunk0.setMax(chunkMax);

    setupCollection(kNamespace, kKeyPattern, {chunk0});

    Timestamp validAfter{101, 0};

    StatusWith<ShardingCatalogManager::ShardAndCollectionVersion> result =
        ShardingCatalogManager::get(operationContext())
            ->commitChunkMigration(operationContext(),
                                   kNamespace,
                                   chunk0,
                                   origVersion.epoch(),
                                   collTimestamp,
                                   ShardId(shard0.getName()),
                                   ShardId(shard1.getName()),
                                   validAfter);

    ASSERT_OK(result.getStatus());

    // Verify the version returned matches expected value.
    auto versions = result.getValue();
    auto mver = versions.shardVersion;
    ASSERT_EQ(ChunkVersion({origVersion.epoch(), origVersion.getTimestamp()}, {0, 0}), mver);

    // Verify that a collection version is returned
    auto cver = versions.collectionVersion;
    ASSERT_EQ(ChunkVersion({collEpoch, collTimestamp}, {origMajorVersion + 1, 0}), cver);

    // Verify the chunk ended up in the right shard.
    auto chunkDoc0 =
        uassertStatusOK(getChunkDoc(operationContext(), chunkMin, collEpoch, collTimestamp));
    ASSERT_EQ("shard1", chunkDoc0.getShard().toString());
    // The history should be updated.
    ASSERT_EQ(2UL, chunkDoc0.getHistory().size());
    ASSERT_EQ(validAfter, chunkDoc0.getHistory().front().getValidAfter());
    ASSERT_EQ(validAfter, *chunkDoc0.getOnCurrentShardSince());
}

TEST_F(CommitChunkMigrate, CheckCorrectOpsCommandNoCtlTrimHistory) {
    const auto collUUID = UUID::gen();
    const auto collEpoch = OID::gen();
    const auto collTimestamp = Timestamp(42);

    ShardType shard0;
    shard0.setName("shard0");
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    uint32_t origMajorVersion = 15;
    auto const origVersion = ChunkVersion({collEpoch, collTimestamp}, {origMajorVersion, 4});

    ChunkType chunk0;
    chunk0.setName(OID::gen());
    chunk0.setCollectionUUID(collUUID);
    chunk0.setVersion(origVersion);
    chunk0.setShard(shard0.getName());
    chunk0.setOnCurrentShardSince(Timestamp(100, 0));
    chunk0.setHistory({ChunkHistory(*chunk0.getOnCurrentShardSince(), shard0.getName())});

    // apportion
    auto chunkMin = BSON("a" << 1);
    chunk0.setMin(chunkMin);
    auto chunkMax = BSON("a" << 10);
    chunk0.setMax(chunkMax);

    setupCollection(kNamespace, kKeyPattern, {chunk0});

    // Make the time distance between the last history element large enough.
    Timestamp validAfter{200, 0};

    StatusWith<ShardingCatalogManager::ShardAndCollectionVersion> result =
        ShardingCatalogManager::get(operationContext())
            ->commitChunkMigration(operationContext(),
                                   kNamespace,
                                   chunk0,
                                   origVersion.epoch(),
                                   collTimestamp,
                                   ShardId(shard0.getName()),
                                   ShardId(shard1.getName()),
                                   validAfter);

    ASSERT_OK(result.getStatus());

    // Verify the version returned matches expected value.
    auto versions = result.getValue();
    auto mver = versions.shardVersion;
    ASSERT_EQ(ChunkVersion({origVersion.epoch(), origVersion.getTimestamp()}, {0, 0}), mver);

    // Verify the chunk ended up in the right shard.
    auto chunkDoc0 =
        uassertStatusOK(getChunkDoc(operationContext(), chunkMin, collEpoch, collTimestamp));
    ASSERT_EQ("shard1", chunkDoc0.getShard().toString());

    // The new history entry should be added, but the old one preserved.
    ASSERT_EQ(2UL, chunkDoc0.getHistory().size());
    ASSERT_EQ(validAfter, chunkDoc0.getHistory().front().getValidAfter());
    ASSERT_EQ(validAfter, *chunkDoc0.getOnCurrentShardSince());
}

TEST_F(CommitChunkMigrate, RejectOutOfOrderHistory) {
    const auto collUUID = UUID::gen();

    ShardType shard0;
    shard0.setName("shard0");
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    uint32_t origMajorVersion = 15;
    auto const origVersion = ChunkVersion({OID::gen(), Timestamp(42)}, {origMajorVersion, 4});

    ChunkType chunk0;
    chunk0.setName(OID::gen());
    chunk0.setCollectionUUID(collUUID);
    chunk0.setVersion(origVersion);
    chunk0.setShard(shard0.getName());
    chunk0.setOnCurrentShardSince(Timestamp(100, 0));
    chunk0.setHistory({ChunkHistory(*chunk0.getOnCurrentShardSince(), shard0.getName())});

    // apportion
    auto chunkMin = BSON("a" << 1);
    chunk0.setMin(chunkMin);
    auto chunkMax = BSON("a" << 10);
    chunk0.setMax(chunkMax);

    setupCollection(kNamespace, kKeyPattern, {chunk0});

    // Make the time before the last change to trigger the failure.
    Timestamp validAfter{99, 0};

    StatusWith<ShardingCatalogManager::ShardAndCollectionVersion> result =
        ShardingCatalogManager::get(operationContext())
            ->commitChunkMigration(operationContext(),
                                   kNamespace,
                                   chunk0,
                                   origVersion.epoch(),
                                   origVersion.getTimestamp(),
                                   ShardId(shard0.getName()),
                                   ShardId(shard1.getName()),
                                   validAfter);

    ASSERT_EQ(ErrorCodes::IncompatibleShardingMetadata, result.getStatus());
}

TEST_F(CommitChunkMigrate, RejectWrongCollectionEpoch0) {
    const auto collUUID = UUID::gen();

    ShardType shard0;
    shard0.setName("shard0");
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    uint32_t origMajorVersion = 12;
    auto const origVersion = ChunkVersion({OID::gen(), Timestamp(42)}, {origMajorVersion, 7});

    ChunkType chunk0;
    chunk0.setName(OID::gen());
    chunk0.setCollectionUUID(collUUID);
    chunk0.setVersion(origVersion);
    chunk0.setShard(shard0.getName());

    // apportion
    auto chunkMin = BSON("a" << 1);
    chunk0.setMin(chunkMin);
    auto chunkMax = BSON("a" << 10);
    chunk0.setMax(chunkMax);

    ChunkType chunk1;
    chunk1.setName(OID::gen());
    chunk1.setCollectionUUID(collUUID);
    chunk1.setVersion(origVersion);
    chunk1.setShard(shard0.getName());

    chunk1.setMin(chunkMax);
    auto chunkMaxax = BSON("a" << 20);
    chunk1.setMax(chunkMaxax);

    setupCollection(kNamespace, kKeyPattern, {chunk0, chunk1});

    Timestamp validAfter{1};

    StatusWith<ShardingCatalogManager::ShardAndCollectionVersion> result =
        ShardingCatalogManager::get(operationContext())
            ->commitChunkMigration(operationContext(),
                                   kNamespace,
                                   chunk0,
                                   OID::gen(),
                                   Timestamp(52),
                                   ShardId(shard0.getName()),
                                   ShardId(shard1.getName()),
                                   validAfter);

    ASSERT_EQ(ErrorCodes::StaleEpoch, result.getStatus());
}

TEST_F(CommitChunkMigrate, RejectWrongCollectionEpoch1) {
    const auto collUUID = UUID::gen();

    ShardType shard0;
    shard0.setName("shard0");
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    uint32_t origMajorVersion = 12;
    auto const origVersion = ChunkVersion({OID::gen(), Timestamp(42)}, {origMajorVersion, 7});
    auto const otherVersion = ChunkVersion({OID::gen(), Timestamp(42)}, {origMajorVersion, 7});

    ChunkType chunk0;
    chunk0.setName(OID::gen());
    chunk0.setCollectionUUID(collUUID);
    chunk0.setVersion(origVersion);
    chunk0.setShard(shard0.getName());

    // apportion
    auto chunkMin = BSON("a" << 1);
    chunk0.setMin(chunkMin);
    auto chunkMax = BSON("a" << 10);
    chunk0.setMax(chunkMax);

    ChunkType chunk1;
    chunk1.setName(OID::gen());
    chunk1.setCollectionUUID(collUUID);
    chunk1.setVersion(otherVersion);
    chunk1.setShard(shard0.getName());

    chunk1.setMin(chunkMax);
    auto chunkMaxax = BSON("a" << 20);
    chunk1.setMax(chunkMaxax);

    // get version from the control chunk this time
    setupCollection(kNamespace, kKeyPattern, {chunk1, chunk0});

    Timestamp validAfter{1};

    StatusWith<ShardingCatalogManager::ShardAndCollectionVersion> result =
        ShardingCatalogManager::get(operationContext())
            ->commitChunkMigration(operationContext(),
                                   kNamespace,
                                   chunk0,
                                   origVersion.epoch(),
                                   origVersion.getTimestamp(),
                                   ShardId(shard0.getName()),
                                   ShardId(shard1.getName()),
                                   validAfter);

    ASSERT_EQ(ErrorCodes::StaleEpoch, result.getStatus());
}

TEST_F(CommitChunkMigrate, CommitWithLastChunkOnShardShouldNotAffectOtherChunks) {
    const auto collUUID = UUID::gen();
    const auto collEpoch = OID::gen();
    const auto collTimestamp = Timestamp(42);

    ShardType shard0;
    shard0.setName("shard0");
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    uint32_t origMajorVersion = 12;
    auto const origVersion = ChunkVersion({collEpoch, collTimestamp}, {origMajorVersion, 7});

    ChunkType chunk0;
    chunk0.setName(OID::gen());
    chunk0.setCollectionUUID(collUUID);
    chunk0.setVersion(origVersion);
    chunk0.setShard(shard0.getName());
    chunk0.setOnCurrentShardSince(Timestamp(100, 0));
    chunk0.setHistory({ChunkHistory(*chunk0.getOnCurrentShardSince(), shard0.getName())});

    // apportion
    auto chunkMin = BSON("a" << 1);
    chunk0.setMin(chunkMin);
    auto chunkMax = BSON("a" << 10);
    chunk0.setMax(chunkMax);

    ChunkType chunk1;
    chunk1.setName(OID::gen());
    chunk1.setCollectionUUID(collUUID);
    chunk1.setVersion(origVersion);
    chunk1.setShard(shard1.getName());

    chunk1.setMin(chunkMax);
    auto chunkMaxax = BSON("a" << 20);
    chunk1.setMax(chunkMaxax);

    Timestamp ctrlChunkValidAfter = Timestamp(50, 0);
    chunk1.setOnCurrentShardSince(ctrlChunkValidAfter);
    chunk1.setHistory({ChunkHistory(*chunk1.getOnCurrentShardSince(), shard1.getName())});

    setupCollection(kNamespace, kKeyPattern, {chunk0, chunk1});

    Timestamp validAfter{101, 0};
    StatusWith<ShardingCatalogManager::ShardAndCollectionVersion> result =
        ShardingCatalogManager::get(operationContext())
            ->commitChunkMigration(operationContext(),
                                   kNamespace,
                                   chunk0,
                                   origVersion.epoch(),
                                   origVersion.getTimestamp(),
                                   ShardId(shard0.getName()),
                                   ShardId(shard1.getName()),
                                   validAfter);

    ASSERT_OK(result.getStatus());

    // Verify the versions returned match expected values.
    auto versions = result.getValue();
    auto mver = versions.shardVersion;
    ASSERT_EQ(ChunkVersion({origVersion.epoch(), origVersion.getTimestamp()}, {0, 0}), mver);

    // Verify the chunks ended up in the right shards.
    auto chunkDoc0 =
        uassertStatusOK(getChunkDoc(operationContext(), chunkMin, collEpoch, collTimestamp));
    ASSERT_EQ(shard1.getName(), chunkDoc0.getShard().toString());

    // The migrated chunk's history should be updated.
    ASSERT_EQ(2UL, chunkDoc0.getHistory().size());
    ASSERT_EQ(validAfter, chunkDoc0.getHistory().front().getValidAfter());
    ASSERT_EQ(validAfter, *chunkDoc0.getOnCurrentShardSince());

    auto chunkDoc1 =
        uassertStatusOK(getChunkDoc(operationContext(), chunkMax, collEpoch, collTimestamp));
    ASSERT_EQ(shard1.getName(), chunkDoc1.getShard().toString());
    ASSERT_EQ(chunk1.getVersion(), chunkDoc1.getVersion());

    // The control chunk's history should be unchanged.
    ASSERT_EQ(1UL, chunkDoc1.getHistory().size());
    ASSERT_EQ(ctrlChunkValidAfter, chunkDoc1.getHistory().front().getValidAfter());
    ASSERT_EQ(ctrlChunkValidAfter, *chunkDoc1.getOnCurrentShardSince());
}

TEST_F(CommitChunkMigrate, RejectMissingChunkVersion) {
    const auto collUUID = UUID::gen();

    ShardType shard0;
    shard0.setName("shard0");
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    ChunkVersion origVersion({OID::gen(), Timestamp(42)}, {12, 7});

    // Create migrate chunk with no chunk version set.
    ChunkType migratedChunk;
    migratedChunk.setName(OID::gen());
    migratedChunk.setCollectionUUID(collUUID);
    migratedChunk.setShard(shard0.getName());
    migratedChunk.setOnCurrentShardSince(Timestamp(100, 0));
    migratedChunk.setHistory(
        {ChunkHistory(*migratedChunk.getOnCurrentShardSince(), shard0.getName())});
    migratedChunk.setMin(BSON("a" << 1));
    migratedChunk.setMax(BSON("a" << 10));

    ChunkType currentChunk;
    currentChunk.setName(OID::gen());
    currentChunk.setCollectionUUID(collUUID);
    currentChunk.setVersion(origVersion);
    currentChunk.setShard(shard0.getName());
    currentChunk.setOnCurrentShardSince(Timestamp(100, 0));
    currentChunk.setHistory(
        {ChunkHistory(*currentChunk.getOnCurrentShardSince(), shard0.getName())});
    currentChunk.setMin(BSON("a" << 1));
    currentChunk.setMax(BSON("a" << 10));

    setupCollection(kNamespace, kKeyPattern, {currentChunk});

    Timestamp validAfter{101, 0};
    ASSERT_THROWS_CODE(ShardingCatalogManager::get(operationContext())
                           ->commitChunkMigration(operationContext(),
                                                  kNamespace,
                                                  migratedChunk,
                                                  origVersion.epoch(),
                                                  origVersion.getTimestamp(),
                                                  ShardId(shard0.getName()),
                                                  ShardId(shard1.getName()),
                                                  validAfter),
                       DBException,
                       4683300);
}

TEST_F(CommitChunkMigrate, RejectOlderChunkVersion) {
    const auto collUUID = UUID::gen();

    ShardType shard0;
    shard0.setName("shard0");
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    auto epoch = OID::gen();
    ChunkVersion origVersion({epoch, Timestamp(42)}, {12, 7});

    ChunkType migratedChunk;
    migratedChunk.setName(OID::gen());
    migratedChunk.setCollectionUUID(collUUID);
    migratedChunk.setVersion(origVersion);
    migratedChunk.setShard(shard0.getName());
    migratedChunk.setOnCurrentShardSince(Timestamp(100, 0));
    migratedChunk.setHistory(
        {ChunkHistory(*migratedChunk.getOnCurrentShardSince(), shard0.getName())});
    migratedChunk.setMin(BSON("a" << 1));
    migratedChunk.setMax(BSON("a" << 10));

    ChunkVersion currentChunkVersion({epoch, Timestamp(42)}, {14, 7});

    ChunkType currentChunk;
    currentChunk.setName(OID::gen());
    currentChunk.setCollectionUUID(collUUID);
    currentChunk.setVersion(currentChunkVersion);
    currentChunk.setShard(shard0.getName());
    currentChunk.setOnCurrentShardSince(Timestamp(100, 0));
    currentChunk.setHistory(
        {ChunkHistory(*currentChunk.getOnCurrentShardSince(), shard0.getName())});
    currentChunk.setMin(BSON("a" << 1));
    currentChunk.setMax(BSON("a" << 10));

    setupCollection(kNamespace, kKeyPattern, {currentChunk});

    Timestamp validAfter{101, 0};
    auto result = ShardingCatalogManager::get(operationContext())
                      ->commitChunkMigration(operationContext(),
                                             kNamespace,
                                             migratedChunk,
                                             origVersion.epoch(),
                                             origVersion.getTimestamp(),
                                             ShardId(shard0.getName()),
                                             ShardId(shard1.getName()),
                                             validAfter);

    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQ(result.getStatus(), ErrorCodes::ConflictingOperationInProgress);
}

TEST_F(CommitChunkMigrate, RejectMismatchedEpoch) {
    const auto collUUID = UUID::gen();

    ShardType shard0;
    shard0.setName("shard0");
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setName("shard1");
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    ChunkVersion origVersion({OID::gen(), Timestamp(42)}, {12, 7});

    ChunkType migratedChunk;
    migratedChunk.setName(OID::gen());
    migratedChunk.setCollectionUUID(collUUID);
    migratedChunk.setVersion(origVersion);
    migratedChunk.setShard(shard0.getName());
    migratedChunk.setOnCurrentShardSince(Timestamp(100, 0));
    migratedChunk.setHistory(
        {ChunkHistory(*migratedChunk.getOnCurrentShardSince(), shard0.getName())});
    migratedChunk.setMin(BSON("a" << 1));
    migratedChunk.setMax(BSON("a" << 10));

    ChunkVersion currentChunkVersion({OID::gen(), Timestamp(42)}, {12, 7});

    ChunkType currentChunk;
    currentChunk.setName(OID::gen());
    currentChunk.setCollectionUUID(collUUID);
    currentChunk.setVersion(currentChunkVersion);
    currentChunk.setShard(shard0.getName());
    currentChunk.setOnCurrentShardSince(Timestamp(100, 0));
    currentChunk.setHistory(
        {ChunkHistory(*currentChunk.getOnCurrentShardSince(), shard0.getName())});
    currentChunk.setMin(BSON("a" << 1));
    currentChunk.setMax(BSON("a" << 10));

    setupCollection(kNamespace, kKeyPattern, {currentChunk});

    Timestamp validAfter{101, 0};
    auto result = ShardingCatalogManager::get(operationContext())
                      ->commitChunkMigration(operationContext(),
                                             kNamespace,
                                             migratedChunk,
                                             origVersion.epoch(),
                                             origVersion.getTimestamp(),
                                             ShardId(shard0.getName()),
                                             ShardId(shard1.getName()),
                                             validAfter);

    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQ(result.getStatus(), ErrorCodes::StaleEpoch);
}

class CommitMoveRangeTest : public CommitChunkMigrate {
public:
    /*
     * Creates a chunk with the given arguments
     */
    ChunkType createChunk(const UUID& collectionUUID,
                          const BSONObj& min,
                          const BSONObj& max,
                          const ChunkVersion& version,
                          const ShardId& shardID,
                          std::vector<ChunkHistory> history) {
        ChunkType chunk;
        chunk.setName(OID::gen());
        chunk.setCollectionUUID(collectionUUID);
        chunk.setVersion(version);
        chunk.setShard(shardID);
        chunk.setHistory(history);
        if (!history.empty())
            chunk.setOnCurrentShardSince(history.front().getValidAfter());
        chunk.setMin(min);
        chunk.setMax(max);

        return chunk;
    }

    /*
     * Setup the collection with `numberOfChunks` contiguous chunks covering all the shard key space
     */
    void setupCollectionWithNChunks(int numberOfChunks) {
        invariant(numberOfChunks > 0);

        uint32_t currentMajorVersion = 1;
        int historyTimestampSecond = 100;

        std::vector<BSONObj> chunksMin = {kKeyPattern.globalMin()};
        for (int i = 10; i < numberOfChunks * 10; i += 10) {
            chunksMin.push_back(BSON("x" << i));
        }
        chunksMin.push_back(kKeyPattern.globalMax());

        for (int i = 0; i < (int)chunksMin.size() - 1; i++) {
            const auto min = chunksMin.at(i);          // Min key of the chunk being created
            const auto max = chunksMin.at(i + 1);      // Max key of the chunk being created
            const auto shardId = _shardIds.at(i % 2);  // Shard owning the chunk
            ChunkVersion version =
                ChunkVersion({_collEpoch, _collTimestamp}, {currentMajorVersion++, 0});
            std::vector<ChunkHistory> history{
                ChunkHistory(Timestamp(historyTimestampSecond++, 0), shardId)};
            ChunkType chunk = createChunk(_collUUID, min, max, version, shardId, history);
            chunks.push_back(chunk);
        }

        setupCollection(kNamespace, kKeyPattern, chunks);
    }

    void assertSameHistories(std::vector<ChunkHistory> l, std::vector<ChunkHistory> r) {
        ASSERT(std::equal(
            r.begin(), r.end(), l.begin(), [](const ChunkHistory& l, const ChunkHistory& r) {
                return (l.toBSON().woCompare(r.toBSON()) == 0);
            }));
    }

    void runMoveRangeAndVerify(const ChunkType& origChunk,
                               const ChunkType& migratedChunk,
                               const bool expectLeftSplit,
                               const bool expectRightSplit) {
        Timestamp validAfter = [&]() {
            auto currValidAfter = migratedChunk.getHistory().at(0).getValidAfter();
            return Timestamp(currValidAfter.getSecs() + 100, 0);
        }();

        const auto donor = migratedChunk.getShard();
        const auto recipient =
            migratedChunk.getShard() == _shardIds.at(0) ? _shardIds.at(1) : _shardIds.at(0);

        auto collVersionBefore = [&]() {
            const auto chunkDoc = uassertStatusOK(
                findOneOnConfigCollection(operationContext(),
                                          ChunkType::ConfigNS,
                                          BSON(ChunkType::collectionUUID << _collUUID),
                                          BSON(ChunkType::lastmod << -1)));
            auto chunk = uassertStatusOK(
                ChunkType::parseFromConfigBSON(chunkDoc, _collEpoch, _collTimestamp));
            return chunk.getVersion();
        }();

        uassertStatusOK(ShardingCatalogManager::get(operationContext())
                            ->commitChunkMigration(operationContext(),
                                                   kNamespace,
                                                   migratedChunk,
                                                   migratedChunk.getVersion().epoch(),
                                                   migratedChunk.getVersion().getTimestamp(),
                                                   donor,
                                                   recipient,
                                                   validAfter));

        // Verify the new chunk is on the recipient shard
        {
            auto newChunk = uassertStatusOK(getChunkDoc(operationContext(),
                                                        migratedChunk.getMin(),
                                                        migratedChunk.getVersion().epoch(),
                                                        migratedChunk.getVersion().getTimestamp()));
            ASSERT_EQ(recipient, newChunk.getShard());
            ASSERT(migratedChunk.getMin().woCompare(newChunk.getMin()) == 0);
            ASSERT(migratedChunk.getMax().woCompare(newChunk.getMax()) == 0);

            // The migrated chunk's version must have been bumped
            ASSERT_EQ(newChunk.getVersion().majorVersion(), collVersionBefore.majorVersion() + 1);
            ASSERT_EQ(0, newChunk.getVersion().minorVersion());

            // The migrated chunk's history should have been updated with a new `validAfter` entry
            ASSERT_EQ(origChunk.getHistory().size() + 1, newChunk.getHistory().size());
            ASSERT_EQ(validAfter, newChunk.getHistory().front().getValidAfter());
            ASSERT_EQ(validAfter, *newChunk.getOnCurrentShardSince());

            // The migrated chunk's history must inherit the previous chunk's history
            assertSameHistories(std::vector<ChunkHistory>(newChunk.getHistory().begin() + 1,
                                                          newChunk.getHistory().end()),
                                origChunk.getHistory());
        }

        int expectedMinVersion = 1;
        if (expectLeftSplit) {
            // Verify the new left split chunk on the donor shard
            auto leftSplitChunk =
                uassertStatusOK(getChunkDoc(operationContext(),
                                            origChunk.getMin(),
                                            migratedChunk.getVersion().epoch(),
                                            migratedChunk.getVersion().getTimestamp()));
            ASSERT_EQ(donor, leftSplitChunk.getShard());
            ASSERT_EQ(origChunk.getOnCurrentShardSince(), leftSplitChunk.getOnCurrentShardSince());

            // The min of the split chunk must be the min of the original chunk
            ASSERT(leftSplitChunk.getMin().woCompare(origChunk.getMin()) == 0);

            // The max of the split chunk must fit the min of the new chunk
            ASSERT(leftSplitChunk.getMax().woCompare(migratedChunk.getMin()) == 0);

            // The major and minor versions of the left split chunk must have been bumped
            ASSERT_EQ(collVersionBefore.majorVersion() + 1,
                      leftSplitChunk.getVersion().majorVersion());
            ASSERT_EQ(expectedMinVersion++, leftSplitChunk.getVersion().minorVersion());

            // The history of the left split chunk must be the same of the original chunk
            assertSameHistories(leftSplitChunk.getHistory(), origChunk.getHistory());
        }

        if (expectRightSplit) {
            // Verify the new right split chunk on the donor shard
            auto rightSplitChunk =
                uassertStatusOK(getChunkDoc(operationContext(),
                                            migratedChunk.getMax(),
                                            migratedChunk.getVersion().epoch(),
                                            migratedChunk.getVersion().getTimestamp()));
            ASSERT_EQ(donor, rightSplitChunk.getShard());
            ASSERT_EQ(origChunk.getOnCurrentShardSince(), rightSplitChunk.getOnCurrentShardSince());

            // The min of the right split chunk must fit the max of the new chunk
            ASSERT(rightSplitChunk.getMin().woCompare(migratedChunk.getMax()) == 0);

            // The max of the right split chunk must fit the max of the original chunk
            ASSERT(rightSplitChunk.getMax().woCompare(origChunk.getMax()) == 0);

            // The major and minor versions of the right split chunk must have been bumped
            ASSERT_EQ(collVersionBefore.majorVersion() + 1,
                      rightSplitChunk.getVersion().majorVersion());
            ASSERT_EQ(expectedMinVersion++, rightSplitChunk.getVersion().minorVersion());

            // The history of the right split chunk must be the same of the original chunk
            assertSameHistories(rightSplitChunk.getHistory(), origChunk.getHistory());
        }
    }

    std::vector<ShardId> _shardIds;
    std::vector<ChunkType> chunks;

private:
    void setUp() override {
        CommitChunkMigrate::setUp();

        ShardType shard0;
        shard0.setName("shard0");
        shard0.setHost("shard0:12");

        ShardType shard1;
        shard1.setName("shard1");
        shard1.setHost("shard1:12");

        setupShards({shard0, shard1});

        _shardIds = {shard0.getName(), shard1.getName()};
    }

    void tearDown() override {
        CommitChunkMigrate::tearDown();
        _shardIds = std::vector<ShardId>();
        chunks = std::vector<ChunkType>();
    }

    const UUID _collUUID = UUID::gen();
    const OID _collEpoch = OID::gen();
    const Timestamp _collTimestamp = Timestamp(42);
};

// Test that moveRange behaves as moveChunk if moving on a whole chunk
TEST_F(CommitMoveRangeTest, MoveRangeOneWholeChunk) {
    setupCollectionWithNChunks(1);

    const ChunkType origChunk = chunks.at(0);
    ChunkType migratedChunk = chunks.at(0);

    runMoveRangeAndVerify(
        origChunk, migratedChunk, false /* expectLeftSplit */, false /* expectRightSplit */);
}

/* Test that moveRange(min:10) correctly split+move.
 * From:
 *  - Shard0: [minKey, maxKey)
 * To:
 *  - Shard0: [minKey, 10)
 *  - Shard1: [10, maxKey)
 */
TEST_F(CommitMoveRangeTest, MoveRangeSplitChunkLeftSide) {
    setupCollectionWithNChunks(1);

    const ChunkType origChunk = chunks.at(0);
    ChunkType migratedChunk = origChunk;
    migratedChunk.setMin(BSON("x" << 10));

    runMoveRangeAndVerify(
        origChunk, migratedChunk, true /* expectLeftSplit */, false /* expectRightSplit */);
}

/* Test that moveRange(min:minKey, max: 10) correctly split+move.
 *
 * From:
 *  - Shard0: [minKey, maxKey)
 * To:
 *  - Shard0: [10, maxKey)
 *  - Shard1: [minKey, 10)
 */
TEST_F(CommitMoveRangeTest, MoveRangeSplitChunkRightSide) {
    setupCollectionWithNChunks(1);

    const ChunkType origChunk = chunks.at(0);
    ChunkType migratedChunk = origChunk;
    migratedChunk.setMax(BSON("x" << 10));

    runMoveRangeAndVerify(
        origChunk, migratedChunk, false /* expectLeftSplit */, true /* expectRightSplit */);
}

/* Test that moveRange(min:1, max: 10) correctly split+move.
 *
 * From:
 *  - Shard0: [minKey, maxKey)
 * To:
 *  - Shard0: [minKey, 1), [10, maxKey)
 *  - Shard1: [1, 10)
 */
TEST_F(CommitMoveRangeTest, MoveRangeSplitChunkLeftRightSide) {
    setupCollectionWithNChunks(1);

    const ChunkType origChunk = chunks.at(0);
    ChunkType migratedChunk = origChunk;
    migratedChunk.setMin(BSON("x" << 1));
    migratedChunk.setMax(BSON("x" << 10));

    runMoveRangeAndVerify(
        origChunk, migratedChunk, true /* expectLeftSplit */, true /* expectRightSplit */);
}

/* Test a random moveRange happening on a collection with several chunks */
TEST_F(CommitMoveRangeTest, MoveRangeRandom) {
    const int32_t nChunks = 10;
    setupCollectionWithNChunks(nChunks);

    mongo::PseudoRandom random(SecureRandom().nextInt64());
    const auto origChunkIndex = random.nextInt32(nChunks);

    const ChunkType origChunk = chunks.at(origChunkIndex);
    ChunkType migratedChunk = origChunk;

    bool expectLeftSplit = [&]() {
        if (origChunkIndex > 0 && random.nextInt32(2)) {
            const auto newMin = origChunk.getMin().getIntField("x") + 2;
            migratedChunk.setMin(BSON("x" << newMin));
            return true;
        }
        return false;
    }();

    bool expectRightSplit = [&]() {
        if (origChunkIndex < nChunks - 1 && random.nextInt32(2)) {
            const auto newMax = origChunk.getMax().getIntField("x") - 2;
            migratedChunk.setMax(BSON("x" << newMax));
            return true;
        }
        return false;
    }();

    LOGV2(6414800,
          "Running random move range",
          "origChunk"_attr = origChunk,
          "migratedChunk"_attr = migratedChunk);

    runMoveRangeAndVerify(origChunk, migratedChunk, expectLeftSplit, expectRightSplit);
}

}  // namespace
}  // namespace mongo
