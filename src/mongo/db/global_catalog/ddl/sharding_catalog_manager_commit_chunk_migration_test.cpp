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

#include <boost/optional/optional.hpp>
#include <fmt/format.h>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/read_write_concern_defaults_cache_lookup_mock.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/db/server_options.h"
#include "mongo/db/session/logical_session_cache.h"
#include "mongo/db/session/logical_session_cache_noop.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/sharding_environment/config_server_test_fixture.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/shard_ref.h"
#include "mongo/db/topology/vector_clock/vector_clock.h"
#include "mongo/db/version_context.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/random.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/uuid.h"
#include "mongo/util/version/releases.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

using unittest::assertGet;

// Returns the comma-joined, sorted list of top-level field names of a BSON document, so two
// documents can be compared by structure (which fields are present) independently of their values.
std::string sortedFieldNames(const BSONObj& obj) {
    std::vector<std::string> names;
    for (const auto& elem : obj) {
        names.push_back(std::string{elem.fieldNameStringData()});
    }
    std::sort(names.begin(), names.end());
    std::string joined;
    for (const auto& name : names) {
        if (!joined.empty()) {
            joined += ", ";
        }
        joined += name;
    }
    return joined;
}

class CommitChunkMigrate : public ConfigServerTestFixture {
protected:
    void setUp() override {

        ConfigServerTestFixture::setUp();
        DBDirectClient client(operationContext());
        client.createCollection(NamespaceString::kSessionTransactionsTableNamespace);
        client.createIndexes(NamespaceString::kSessionTransactionsTableNamespace,
                             {MongoDSessionCatalog::getConfigTxnPartialIndexSpec()});

        ReadWriteConcernDefaults::create(getService(), _lookupMock.getFetchDefaultsFn());

        LogicalSessionCache::set(getServiceContext(), std::make_unique<LogicalSessionCacheNoop>());
        TransactionCoordinatorService::get(operationContext())
            ->initializeIfNeeded(operationContext(), /* term */ 1);

        WaitForMajorityService::get(getServiceContext()).startup(getServiceContext());
    }

    void tearDown() override {
        TransactionCoordinatorService::get(operationContext())->interruptForStepDown();
        WaitForMajorityService::get(getServiceContext()).shutDown();
        ConfigServerTestFixture::tearDown();
    }

    // Allows the usage of transactions.
    ReadWriteConcernDefaultsLookupMock _lookupMock;
};

const NamespaceString kNamespace =
    NamespaceString::createNamespaceString_forTest("TestDB.TestColl");
const KeyPattern kKeyPattern(BSON("x" << 1));

TEST_F(CommitChunkMigrate, ChunksUpdatedCorrectly) {
    const auto collUUID = UUID::gen();
    const auto collEpoch = OID::gen();
    const auto collTimestamp = Timestamp(42);

    ShardType shard0;
    shard0.setHandle(ShardHandle{ShardId("shard0"), boost::none});
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setHandle(ShardHandle{ShardId("shard1"), boost::none});
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    ChunkType migratedChunk, controlChunk;
    {
        ChunkVersion origVersion({collEpoch, collTimestamp}, {12, 7});

        migratedChunk.setName(OID::gen());
        migratedChunk.setCollectionUUID(collUUID);
        migratedChunk.setVersion(origVersion);
        migratedChunk.setShard(ShardRef{shard0.getName()});
        migratedChunk.setOnCurrentShardSince(Timestamp(100, 0));
        migratedChunk.setHistory(
            {ChunkHistory(*migratedChunk.getOnCurrentShardSince(), ShardRef{shard0.getName()})});
        migratedChunk.setRange({BSON("a" << 1), BSON("a" << 10)});

        origVersion.incMinor();

        controlChunk.setName(OID::gen());
        controlChunk.setCollectionUUID(collUUID);
        controlChunk.setVersion(origVersion);
        controlChunk.setShard(ShardRef{shard0.getName()});
        controlChunk.setOnCurrentShardSince(Timestamp(50, 0));
        controlChunk.setHistory(
            {ChunkHistory(*controlChunk.getOnCurrentShardSince(), ShardRef{shard0.getName()})});
        controlChunk.setRange({BSON("a" << 10), BSON("a" << 20)});
        controlChunk.setJumbo(true);
    }

    setupCollection(kNamespace, kKeyPattern, {migratedChunk, controlChunk});

    const auto currentTime = VectorClock::get(getServiceContext())->getTime();
    const auto expectedValidAfter = currentTime.clusterTime().asTimestamp();

    auto versions = assertGet(ShardingCatalogManager::get(operationContext())
                                  ->commitChunkMigration(operationContext(),
                                                         kNamespace,
                                                         migratedChunk,
                                                         migratedChunk.getVersion().epoch(),
                                                         collTimestamp,
                                                         ShardId(shard0.getName()),
                                                         ShardId(shard1.getName())));

    // Verify the versions returned match expected values.
    auto mver = versions.shardPlacementVersion;
    ASSERT_EQ(ChunkVersion(
                  {migratedChunk.getVersion().epoch(), migratedChunk.getVersion().getTimestamp()},
                  {migratedChunk.getVersion().majorVersion() + 1, 1}),
              mver);

    // Verify that a collection placement version is returned
    auto cver = versions.collectionPlacementVersion;
    auto compareResult = mver <=> cver;
    ASSERT_TRUE(compareResult == std::partial_ordering::less ||
                compareResult == std::partial_ordering::equivalent);

    // Verify the chunks ended up in the right shards.
    auto chunkDoc0 = uassertStatusOK(
        getChunkDoc(operationContext(), migratedChunk.getMin(), collEpoch, collTimestamp));
    ASSERT_EQ("shard1", chunkDoc0.getShard().toString());

    // The migrated chunk's history should be updated.
    ASSERT_EQ(2UL, chunkDoc0.getHistory().size());
    ASSERT_EQ(expectedValidAfter, chunkDoc0.getHistory().front().getValidAfter());
    ASSERT_EQ(expectedValidAfter, *chunkDoc0.getOnCurrentShardSince());

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

TEST_F(CommitChunkMigrate, RejectMigrationWhenChangedChunksExceedSizeLimit) {
    const auto collUUID = UUID::gen();
    const auto collEpoch = OID::gen();
    const auto collTimestamp = Timestamp(42);

    ShardType shard0;
    shard0.setHandle(ShardHandle{ShardId("shard0"), boost::none});
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setHandle(ShardHandle{ShardId("shard1"), boost::none});
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    // Use a multi-megabyte shard-key boundary shared by the two chunks. Each chunk document stays
    // under the 16MB BSON limit on its own, so setup and any single write are valid, but the
    // combined set of chunks changed by the migration (the migrated chunk plus the control chunk
    // the donor keeps) exceeds BSONObjMaxUserSize / 2.
    const std::string bigValue(9 * 1024 * 1024, 'x');

    ChunkType migratedChunk, controlChunk;
    {
        ChunkVersion origVersion({collEpoch, collTimestamp}, {12, 7});

        migratedChunk.setName(OID::gen());
        migratedChunk.setCollectionUUID(collUUID);
        migratedChunk.setVersion(origVersion);
        migratedChunk.setShard(ShardRef{shard0.getName()});
        migratedChunk.setOnCurrentShardSince(Timestamp(100, 0));
        migratedChunk.setHistory(
            {ChunkHistory(*migratedChunk.getOnCurrentShardSince(), ShardRef{shard0.getName()})});
        // A large string sorts after every number but before MaxKey in BSON order, so it is a valid
        // interior boundary between MinKey and MaxKey.
        migratedChunk.setRange({BSON("a" << MINKEY), BSON("a" << bigValue)});

        origVersion.incMinor();

        controlChunk.setName(OID::gen());
        controlChunk.setCollectionUUID(collUUID);
        controlChunk.setVersion(origVersion);
        controlChunk.setShard(ShardRef{shard0.getName()});
        controlChunk.setOnCurrentShardSince(Timestamp(50, 0));
        controlChunk.setHistory(
            {ChunkHistory(*controlChunk.getOnCurrentShardSince(), ShardRef{shard0.getName()})});
        controlChunk.setRange({BSON("a" << bigValue), BSON("a" << MAXKEY)});
    }

    setupCollection(kNamespace, kKeyPattern, {migratedChunk, controlChunk});

    ASSERT_THROWS_CODE(ShardingCatalogManager::get(operationContext())
                           ->commitChunkMigration(operationContext(),
                                                  kNamespace,
                                                  migratedChunk,
                                                  migratedChunk.getVersion().epoch(),
                                                  collTimestamp,
                                                  ShardId(shard0.getName()),
                                                  ShardId(shard1.getName())),
                       DBException,
                       ErrorCodes::BSONObjectTooLarge);

    // The commit must have been rejected before it changed the catalog: the migrated chunk still
    // belongs to the donor with its original version.
    auto chunkDoc = uassertStatusOK(
        getChunkDoc(operationContext(), migratedChunk.getMin(), collEpoch, collTimestamp));
    ASSERT_EQ("shard0", chunkDoc.getShard().toString());
    ASSERT_EQ(migratedChunk.getVersion(), chunkDoc.getVersion());
}

TEST_F(CommitChunkMigrate, RejectDuringFCVTransitionWithStableOperationFCV) {
    const auto originalFCV =
        serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion();
    ScopeGuard restoreFCV([&] { serverGlobalParams.mutableFCV.setVersion(originalFCV); });
    // (Generic FCV reference): This test intentionally verifies commitChunkMigration behavior
    // during an FCV transition, with a stable last LTS OFCV to ensure the server FCV being in
    // transitional state is what rejects the migration.
    serverGlobalParams.mutableFCV.setVersion(multiversion::GenericFCV::kLastLTS);
    VersionContext::FixedOperationFCVRegion fixedOperationFCV(operationContext());
    serverGlobalParams.mutableFCV.setVersion(
        multiversion::GenericFCV::kUpgradingFromLastLTSToLatest);

    const auto collUUID = UUID::gen();
    const auto collEpoch = OID::gen();
    const auto collTimestamp = Timestamp(42);

    ShardType shard0;
    shard0.setHandle(ShardHandle{ShardId("shard0"), boost::none});
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setHandle(ShardHandle{ShardId("shard1"), boost::none});
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    ChunkType migratedChunk;
    const auto version = ChunkVersion({collEpoch, collTimestamp}, {12, 7});
    migratedChunk.setName(OID::gen());
    migratedChunk.setCollectionUUID(collUUID);
    migratedChunk.setVersion(version);
    migratedChunk.setShard(ShardRef{shard0.getName()});
    migratedChunk.setOnCurrentShardSince(Timestamp(100, 0));
    migratedChunk.setHistory(
        {ChunkHistory(*migratedChunk.getOnCurrentShardSince(), ShardRef{shard0.getName()})});
    migratedChunk.setRange({BSON("a" << 1), BSON("a" << 10)});

    setupCollection(kNamespace, kKeyPattern, {migratedChunk});

    ASSERT_THROWS_CODE(ShardingCatalogManager::get(operationContext())
                           ->commitChunkMigration(operationContext(),
                                                  kNamespace,
                                                  migratedChunk,
                                                  migratedChunk.getVersion().epoch(),
                                                  collTimestamp,
                                                  ShardId(shard0.getName()),
                                                  ShardId(shard1.getName())),
                       DBException,
                       ErrorCodes::ConflictingOperationInProgress);
}

TEST_F(CommitChunkMigrate, RetryCommittedMigrationSucceedsDuringFCVTransition) {
    const auto collUUID = UUID::gen();
    const auto collEpoch = OID::gen();
    const auto collTimestamp = Timestamp(42);

    ShardType shard0;
    shard0.setHandle(ShardHandle{ShardId("shard0"), boost::none});
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setHandle(ShardHandle{ShardId("shard1"), boost::none});
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    ChunkType migratedChunk;
    const auto version = ChunkVersion({collEpoch, collTimestamp}, {12, 7});
    migratedChunk.setName(OID::gen());
    migratedChunk.setCollectionUUID(collUUID);
    migratedChunk.setVersion(version);
    migratedChunk.setShard(ShardRef{shard0.getName()});
    migratedChunk.setOnCurrentShardSince(Timestamp(100, 0));
    migratedChunk.setHistory(
        {ChunkHistory(*migratedChunk.getOnCurrentShardSince(), ShardRef{shard0.getName()})});
    migratedChunk.setRange({BSON("a" << 1), BSON("a" << 10)});

    setupCollection(kNamespace, kKeyPattern, {migratedChunk});

    const auto doMigration = [&] {
        return ShardingCatalogManager::get(operationContext())
            ->commitChunkMigration(operationContext(),
                                   kNamespace,
                                   migratedChunk,
                                   migratedChunk.getVersion().epoch(),
                                   collTimestamp,
                                   ShardId(shard0.getName()),
                                   ShardId(shard1.getName()));
    };

    ASSERT_OK(doMigration());

    const auto originalFCV =
        serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion();
    ScopeGuard restoreFCV([&] { serverGlobalParams.mutableFCV.setVersion(originalFCV); });
    // (Generic FCV reference): the retry carries a stable last LTS OFCV while server FCV
    // transitions.
    serverGlobalParams.mutableFCV.setVersion(multiversion::GenericFCV::kLastLTS);
    VersionContext::FixedOperationFCVRegion fixedOperationFCV(operationContext());
    serverGlobalParams.mutableFCV.setVersion(
        multiversion::GenericFCV::kUpgradingFromLastLTSToLatest);

    ASSERT_OK(doMigration());

    auto chunkDoc =
        uassertStatusOK(getChunkDoc(operationContext(), BSON("a" << 1), collEpoch, collTimestamp));
    ASSERT_EQ(shard1.getName(), chunkDoc.getShard());
}

TEST_F(CommitChunkMigrate, ChunksUpdatedCorrectlyWithoutControlChunk) {
    const auto collUUID = UUID::gen();
    const auto collEpoch = OID::gen();
    const auto collTimestamp = Timestamp(42);

    ShardType shard0;
    shard0.setHandle(ShardHandle{ShardId("shard0"), boost::none});
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setHandle(ShardHandle{ShardId("shard1"), boost::none});
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    uint32_t origMajorVersion = 15;
    auto const origVersion = ChunkVersion({collEpoch, collTimestamp}, {origMajorVersion, 4});

    ChunkType chunk0;
    chunk0.setName(OID::gen());
    chunk0.setCollectionUUID(collUUID);
    chunk0.setVersion(origVersion);
    chunk0.setShard(ShardRef{shard0.getName()});
    chunk0.setOnCurrentShardSince(Timestamp(100, 0));
    chunk0.setHistory({ChunkHistory(*chunk0.getOnCurrentShardSince(), ShardRef{shard0.getName()})});

    // apportion
    auto chunkMin = BSON("a" << 1);
    auto chunkMax = BSON("a" << 10);
    chunk0.setRange({chunkMin, chunkMax});

    setupCollection(kNamespace, kKeyPattern, {chunk0});
    const auto currentTime = VectorClock::get(getServiceContext())->getTime();
    const auto expectedValidAfter = currentTime.clusterTime().asTimestamp();

    StatusWith<ShardingCatalogManager::ShardAndCollectionPlacementVersions> result =
        ShardingCatalogManager::get(operationContext())
            ->commitChunkMigration(operationContext(),
                                   kNamespace,
                                   chunk0,
                                   origVersion.epoch(),
                                   collTimestamp,
                                   ShardId(shard0.getName()),
                                   ShardId(shard1.getName()));

    ASSERT_OK(result.getStatus());

    // Verify the version returned matches expected value.
    auto versions = result.getValue();
    auto mver = versions.shardPlacementVersion;
    ASSERT_EQ(ChunkVersion({origVersion.epoch(), origVersion.getTimestamp()}, {0, 0}), mver);

    // Verify that a collection placement version is returned
    auto cver = versions.collectionPlacementVersion;
    ASSERT_EQ(ChunkVersion({collEpoch, collTimestamp}, {origMajorVersion + 1, 0}), cver);

    // Verify the chunk ended up in the right shard.
    auto chunkDoc0 =
        uassertStatusOK(getChunkDoc(operationContext(), chunkMin, collEpoch, collTimestamp));
    ASSERT_EQ("shard1", chunkDoc0.getShard().toString());
    // The history should be updated.
    ASSERT_EQ(2UL, chunkDoc0.getHistory().size());
    ASSERT_EQ(expectedValidAfter, chunkDoc0.getHistory().front().getValidAfter());
    ASSERT_EQ(expectedValidAfter, *chunkDoc0.getOnCurrentShardSince());
}

TEST_F(CommitChunkMigrate, CheckCorrectOpsCommandNoCtlTrimHistory) {
    const auto collUUID = UUID::gen();
    const auto collEpoch = OID::gen();
    const auto collTimestamp = Timestamp(42);

    ShardType shard0;
    shard0.setHandle(ShardHandle{ShardId("shard0"), boost::none});
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setHandle(ShardHandle{ShardId("shard1"), boost::none});
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    uint32_t origMajorVersion = 15;
    auto const origVersion = ChunkVersion({collEpoch, collTimestamp}, {origMajorVersion, 4});

    ChunkType chunk0;
    chunk0.setName(OID::gen());
    chunk0.setCollectionUUID(collUUID);
    chunk0.setVersion(origVersion);
    chunk0.setShard(ShardRef{shard0.getName()});
    chunk0.setOnCurrentShardSince(Timestamp(100, 0));
    chunk0.setHistory({ChunkHistory(*chunk0.getOnCurrentShardSince(), ShardRef{shard0.getName()})});

    // apportion
    auto chunkMin = BSON("a" << 1);
    auto chunkMax = BSON("a" << 10);
    chunk0.setRange({chunkMin, chunkMax});

    setupCollection(kNamespace, kKeyPattern, {chunk0});

    // Make the time distance between the last history element large enough.
    const auto currentTime = VectorClock::get(getServiceContext())->getTime();
    const auto currentClusterTime = currentTime.clusterTime().asTimestamp();
    const auto updatedClusterTime = LogicalTime(currentClusterTime + Timestamp(200.0).asULL());
    VectorClock::get(getServiceContext())->advanceClusterTime_forTest(updatedClusterTime);

    StatusWith<ShardingCatalogManager::ShardAndCollectionPlacementVersions> result =
        ShardingCatalogManager::get(operationContext())
            ->commitChunkMigration(operationContext(),
                                   kNamespace,
                                   chunk0,
                                   origVersion.epoch(),
                                   collTimestamp,
                                   ShardId(shard0.getName()),
                                   ShardId(shard1.getName()));

    ASSERT_OK(result.getStatus());

    // Verify the version returned matches expected value.
    auto versions = result.getValue();
    auto mver = versions.shardPlacementVersion;
    ASSERT_EQ(ChunkVersion({origVersion.epoch(), origVersion.getTimestamp()}, {0, 0}), mver);

    // Verify the chunk ended up in the right shard.
    auto chunkDoc0 =
        uassertStatusOK(getChunkDoc(operationContext(), chunkMin, collEpoch, collTimestamp));
    ASSERT_EQ("shard1", chunkDoc0.getShard().toString());

    // The new history entry should be added, but the old one preserved.
    ASSERT_EQ(2UL, chunkDoc0.getHistory().size());
    ASSERT_EQ(updatedClusterTime.asTimestamp(), chunkDoc0.getHistory().front().getValidAfter());
    ASSERT_EQ(updatedClusterTime.asTimestamp(), *chunkDoc0.getOnCurrentShardSince());
}

TEST_F(CommitChunkMigrate, RejectOutOfOrderHistory) {
    const auto collUUID = UUID::gen();

    ShardType shard0;
    shard0.setHandle(ShardHandle{ShardId("shard0"), boost::none});
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setHandle(ShardHandle{ShardId("shard1"), boost::none});
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    uint32_t origMajorVersion = 15;
    auto const origVersion = ChunkVersion({OID::gen(), Timestamp(42)}, {origMajorVersion, 4});

    ChunkType chunk0;
    chunk0.setName(OID::gen());
    chunk0.setCollectionUUID(collUUID);
    chunk0.setVersion(origVersion);
    chunk0.setShard(ShardRef{shard0.getName()});
    chunk0.setOnCurrentShardSince(Timestamp(100, 1));
    chunk0.setHistory({ChunkHistory(*chunk0.getOnCurrentShardSince(), ShardRef{shard0.getName()})});

    // apportion
    auto chunkMin = BSON("a" << 1);
    auto chunkMax = BSON("a" << 10);
    chunk0.setRange({chunkMin, chunkMax});

    setupCollection(kNamespace, kKeyPattern, {chunk0});

    // Ensure that the current cluster time is earlier than the timestamp associated to the chunk
    // being migrated.
    VectorClock::get(getServiceContext())->resetVectorClock_forTest();

    StatusWith<ShardingCatalogManager::ShardAndCollectionPlacementVersions> result =
        ShardingCatalogManager::get(operationContext())
            ->commitChunkMigration(operationContext(),
                                   kNamespace,
                                   chunk0,
                                   origVersion.epoch(),
                                   origVersion.getTimestamp(),
                                   ShardId(shard0.getName()),
                                   ShardId(shard1.getName()));

    ASSERT_EQ(ErrorCodes::IncompatibleShardingMetadata, result.getStatus());
}

TEST_F(CommitChunkMigrate, RejectWrongCollectionEpoch0) {
    const auto collUUID = UUID::gen();

    ShardType shard0;
    shard0.setHandle(ShardHandle{ShardId("shard0"), boost::none});
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setHandle(ShardHandle{ShardId("shard1"), boost::none});
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    uint32_t origMajorVersion = 12;
    auto const origVersion = ChunkVersion({OID::gen(), Timestamp(42)}, {origMajorVersion, 7});

    ChunkType chunk0;
    chunk0.setName(OID::gen());
    chunk0.setCollectionUUID(collUUID);
    chunk0.setVersion(origVersion);
    chunk0.setShard(ShardRef{shard0.getName()});

    // apportion
    auto chunkMin = BSON("a" << 1);
    auto chunkMax = BSON("a" << 10);
    chunk0.setRange({chunkMin, chunkMax});

    ChunkType chunk1;
    chunk1.setName(OID::gen());
    chunk1.setCollectionUUID(collUUID);
    chunk1.setVersion(origVersion);
    chunk1.setShard(ShardRef{shard0.getName()});

    auto chunkMaxax = BSON("a" << 20);
    chunk1.setRange({chunkMax, chunkMaxax});

    setupCollection(kNamespace, kKeyPattern, {chunk0, chunk1});

    ASSERT_THROWS_CODE(ShardingCatalogManager::get(operationContext())
                           ->commitChunkMigration(operationContext(),
                                                  kNamespace,
                                                  chunk0,
                                                  OID::gen(),
                                                  Timestamp(52),
                                                  ShardId(shard0.getName()),
                                                  ShardId(shard1.getName())),
                       DBException,

                       ErrorCodes::StaleEpoch);
}

TEST_F(CommitChunkMigrate, RejectWrongCollectionEpoch1) {
    const auto collUUID = UUID::gen();

    ShardType shard0;
    shard0.setHandle(ShardHandle{ShardId("shard0"), boost::none});
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setHandle(ShardHandle{ShardId("shard1"), boost::none});
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    uint32_t origMajorVersion = 12;
    auto const origVersion = ChunkVersion({OID::gen(), Timestamp(42)}, {origMajorVersion, 7});
    auto const otherVersion = ChunkVersion({OID::gen(), Timestamp(42)}, {origMajorVersion, 7});

    ChunkType chunk0;
    chunk0.setName(OID::gen());
    chunk0.setCollectionUUID(collUUID);
    chunk0.setVersion(origVersion);
    chunk0.setShard(ShardRef{shard0.getName()});

    // apportion
    auto chunkMin = BSON("a" << 1);
    auto chunkMax = BSON("a" << 10);
    chunk0.setRange({chunkMin, chunkMax});

    ChunkType chunk1;
    chunk1.setName(OID::gen());
    chunk1.setCollectionUUID(collUUID);
    chunk1.setVersion(otherVersion);
    chunk1.setShard(ShardRef{shard0.getName()});

    auto chunkMaxax = BSON("a" << 20);
    chunk1.setRange({chunkMax, chunkMaxax});

    // get version from the control chunk this time
    setupCollection(kNamespace, kKeyPattern, {chunk1, chunk0});

    ASSERT_THROWS_CODE(ShardingCatalogManager::get(operationContext())
                           ->commitChunkMigration(operationContext(),
                                                  kNamespace,
                                                  chunk0,
                                                  origVersion.epoch(),
                                                  origVersion.getTimestamp(),
                                                  ShardId(shard0.getName()),
                                                  ShardId(shard1.getName())),
                       DBException,
                       ErrorCodes::StaleEpoch);
}

TEST_F(CommitChunkMigrate, CommitWithLastChunkOnShardShouldNotAffectOtherChunks) {
    const auto collUUID = UUID::gen();
    const auto collEpoch = OID::gen();
    const auto collTimestamp = Timestamp(42);

    ShardType shard0;
    shard0.setHandle(ShardHandle{ShardId("shard0"), boost::none});
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setHandle(ShardHandle{ShardId("shard1"), boost::none});
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    uint32_t origMajorVersion = 12;
    auto const origVersion = ChunkVersion({collEpoch, collTimestamp}, {origMajorVersion, 7});

    ChunkType chunk0;
    chunk0.setName(OID::gen());
    chunk0.setCollectionUUID(collUUID);
    chunk0.setVersion(origVersion);
    chunk0.setShard(ShardRef{shard0.getName()});
    chunk0.setOnCurrentShardSince(Timestamp(100, 0));
    chunk0.setHistory({ChunkHistory(*chunk0.getOnCurrentShardSince(), ShardRef{shard0.getName()})});

    // apportion
    auto chunkMin = BSON("a" << 1);
    auto chunkMax = BSON("a" << 10);
    chunk0.setRange({chunkMin, chunkMax});

    ChunkType chunk1;
    chunk1.setName(OID::gen());
    chunk1.setCollectionUUID(collUUID);
    chunk1.setVersion(origVersion);
    chunk1.setShard(ShardRef{shard1.getName()});

    auto chunkMaxax = BSON("a" << 20);
    chunk1.setRange({chunkMax, chunkMaxax});

    Timestamp ctrlChunkValidAfter = Timestamp(50, 0);
    chunk1.setOnCurrentShardSince(ctrlChunkValidAfter);
    chunk1.setHistory({ChunkHistory(*chunk1.getOnCurrentShardSince(), ShardRef{shard1.getName()})});

    setupCollection(kNamespace, kKeyPattern, {chunk0, chunk1});

    const auto currentTime = VectorClock::get(getServiceContext())->getTime();
    const auto expectedValidAfter = currentTime.clusterTime().asTimestamp();

    StatusWith<ShardingCatalogManager::ShardAndCollectionPlacementVersions> result =
        ShardingCatalogManager::get(operationContext())
            ->commitChunkMigration(operationContext(),
                                   kNamespace,
                                   chunk0,
                                   origVersion.epoch(),
                                   origVersion.getTimestamp(),
                                   ShardId(shard0.getName()),
                                   ShardId(shard1.getName()));

    ASSERT_OK(result.getStatus());

    // Verify the versions returned match expected values.
    auto versions = result.getValue();
    auto mver = versions.shardPlacementVersion;
    ASSERT_EQ(ChunkVersion({origVersion.epoch(), origVersion.getTimestamp()}, {0, 0}), mver);

    // Verify the chunks ended up in the right shards.
    auto chunkDoc0 =
        uassertStatusOK(getChunkDoc(operationContext(), chunkMin, collEpoch, collTimestamp));
    ASSERT_EQ(shard1.getName(), chunkDoc0.getShard().toString());

    // The migrated chunk's history should be updated.
    ASSERT_EQ(2UL, chunkDoc0.getHistory().size());
    ASSERT_EQ(chunkDoc0.getHistory().front().getValidAfter(), *chunkDoc0.getOnCurrentShardSince());
    ASSERT_EQ(expectedValidAfter, chunkDoc0.getHistory().front().getValidAfter());
    ASSERT_EQ(expectedValidAfter, *chunkDoc0.getOnCurrentShardSince());

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
    shard0.setHandle(ShardHandle{ShardId("shard0"), boost::none});
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setHandle(ShardHandle{ShardId("shard1"), boost::none});
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    ChunkVersion origVersion({OID::gen(), Timestamp(42)}, {12, 7});

    // Create migrate chunk with no chunk version set.
    ChunkType migratedChunk;
    migratedChunk.setName(OID::gen());
    migratedChunk.setCollectionUUID(collUUID);
    migratedChunk.setShard(ShardRef{shard0.getName()});
    migratedChunk.setOnCurrentShardSince(Timestamp(100, 0));
    migratedChunk.setHistory(
        {ChunkHistory(*migratedChunk.getOnCurrentShardSince(), ShardRef{shard0.getName()})});
    migratedChunk.setRange({BSON("a" << 1), BSON("a" << 10)});

    ChunkType currentChunk;
    currentChunk.setName(OID::gen());
    currentChunk.setCollectionUUID(collUUID);
    currentChunk.setVersion(origVersion);
    currentChunk.setShard(ShardRef{shard0.getName()});
    currentChunk.setOnCurrentShardSince(Timestamp(100, 0));
    currentChunk.setHistory(
        {ChunkHistory(*currentChunk.getOnCurrentShardSince(), ShardRef{shard0.getName()})});
    currentChunk.setRange({BSON("a" << 1), BSON("a" << 10)});

    setupCollection(kNamespace, kKeyPattern, {currentChunk});

    ASSERT_THROWS_CODE(ShardingCatalogManager::get(operationContext())
                           ->commitChunkMigration(operationContext(),
                                                  kNamespace,
                                                  migratedChunk,
                                                  origVersion.epoch(),
                                                  origVersion.getTimestamp(),
                                                  ShardId(shard0.getName()),
                                                  ShardId(shard1.getName())),
                       DBException,
                       4683300);
}

TEST_F(CommitChunkMigrate, RejectOlderChunkVersion) {
    const auto collUUID = UUID::gen();

    ShardType shard0;
    shard0.setHandle(ShardHandle{ShardId("shard0"), boost::none});
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setHandle(ShardHandle{ShardId("shard1"), boost::none});
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    auto epoch = OID::gen();
    ChunkVersion origVersion({epoch, Timestamp(42)}, {12, 7});

    ChunkType migratedChunk;
    migratedChunk.setName(OID::gen());
    migratedChunk.setCollectionUUID(collUUID);
    migratedChunk.setVersion(origVersion);
    migratedChunk.setShard(ShardRef{shard0.getName()});
    migratedChunk.setOnCurrentShardSince(Timestamp(100, 0));
    migratedChunk.setHistory(
        {ChunkHistory(*migratedChunk.getOnCurrentShardSince(), ShardRef{shard0.getName()})});
    migratedChunk.setRange({BSON("a" << 1), BSON("a" << 10)});

    ChunkVersion currentChunkVersion({epoch, Timestamp(42)}, {14, 7});

    ChunkType currentChunk;
    currentChunk.setName(OID::gen());
    currentChunk.setCollectionUUID(collUUID);
    currentChunk.setVersion(currentChunkVersion);
    currentChunk.setShard(ShardRef{shard0.getName()});
    currentChunk.setOnCurrentShardSince(Timestamp(100, 0));
    currentChunk.setHistory(
        {ChunkHistory(*currentChunk.getOnCurrentShardSince(), ShardRef{shard0.getName()})});
    currentChunk.setRange({BSON("a" << 1), BSON("a" << 10)});

    setupCollection(kNamespace, kKeyPattern, {currentChunk});

    auto result = ShardingCatalogManager::get(operationContext())
                      ->commitChunkMigration(operationContext(),
                                             kNamespace,
                                             migratedChunk,
                                             origVersion.epoch(),
                                             origVersion.getTimestamp(),
                                             ShardId(shard0.getName()),
                                             ShardId(shard1.getName()));

    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQ(result.getStatus(), ErrorCodes::ConflictingOperationInProgress);
}

TEST_F(CommitChunkMigrate, RejectMismatchedEpoch) {
    const auto collUUID = UUID::gen();

    ShardType shard0;
    shard0.setHandle(ShardHandle{ShardId("shard0"), boost::none});
    shard0.setHost("shard0:12");

    ShardType shard1;
    shard1.setHandle(ShardHandle{ShardId("shard1"), boost::none});
    shard1.setHost("shard1:12");

    setupShards({shard0, shard1});

    ChunkVersion origVersion({OID::gen(), Timestamp(42)}, {12, 7});

    ChunkType migratedChunk;
    migratedChunk.setName(OID::gen());
    migratedChunk.setCollectionUUID(collUUID);
    migratedChunk.setVersion(origVersion);
    migratedChunk.setShard(ShardRef{shard0.getName()});
    migratedChunk.setOnCurrentShardSince(Timestamp(100, 0));
    migratedChunk.setHistory(
        {ChunkHistory(*migratedChunk.getOnCurrentShardSince(), ShardRef{shard0.getName()})});
    migratedChunk.setRange({BSON("a" << 1), BSON("a" << 10)});

    ChunkVersion currentChunkVersion({OID::gen(), Timestamp(42)}, {12, 7});

    ChunkType currentChunk;
    currentChunk.setName(OID::gen());
    currentChunk.setCollectionUUID(collUUID);
    currentChunk.setVersion(currentChunkVersion);
    currentChunk.setShard(ShardRef{shard0.getName()});
    currentChunk.setOnCurrentShardSince(Timestamp(100, 0));
    currentChunk.setHistory(
        {ChunkHistory(*currentChunk.getOnCurrentShardSince(), ShardRef{shard0.getName()})});
    currentChunk.setRange({BSON("a" << 1), BSON("a" << 10)});

    setupCollection(kNamespace, kKeyPattern, {currentChunk});

    ASSERT_THROWS_CODE(ShardingCatalogManager::get(operationContext())
                           ->commitChunkMigration(operationContext(),
                                                  kNamespace,
                                                  migratedChunk,
                                                  origVersion.epoch(),
                                                  origVersion.getTimestamp(),
                                                  ShardId(shard0.getName()),
                                                  ShardId(shard1.getName())),
                       DBException,
                       ErrorCodes::StaleEpoch);
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
        chunk.setRange({min, max});

        return chunk;
    }

    /*
     * Setup the collection with `numberOfChunks` contiguous chunks covering all the shard key
     space
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
        const auto donor = migratedChunk.getShard();
        const auto recipient =
            migratedChunk.getShard() == _shardIds.at(0) ? _shardIds.at(1) : _shardIds.at(0);

        auto collPlacementVersionBefore = [&]() {
            const auto chunkDoc = uassertStatusOK(
                findOneOnConfigCollection(operationContext(),
                                          NamespaceString::kConfigsvrChunksNamespace,
                                          BSON(ChunkType::collectionUUID << _collUUID),
                                          BSON(ChunkType::lastmod << -1)));
            auto chunk = uassertStatusOK(
                ChunkType::parseFromConfigBSON(chunkDoc, _collEpoch, _collTimestamp));
            return chunk.getVersion();
        }();

        const auto currentTime = VectorClock::get(getServiceContext())->getTime();
        const auto expectedValidAfter = currentTime.clusterTime().asTimestamp();

        uassertStatusOK(ShardingCatalogManager::get(operationContext())
                            ->commitChunkMigration(operationContext(),
                                                   kNamespace,
                                                   migratedChunk,
                                                   migratedChunk.getVersion().epoch(),
                                                   migratedChunk.getVersion().getTimestamp(),
                                                   donor,
                                                   recipient));

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
            ASSERT_EQ(newChunk.getVersion().majorVersion(),
                      collPlacementVersionBefore.majorVersion() + 1);
            ASSERT_EQ(0, newChunk.getVersion().minorVersion());

            // The migrated chunk's history should have been updated with a new `validAfter` entry
            ASSERT_EQ(origChunk.getHistory().size() + 1, newChunk.getHistory().size());
            ASSERT_EQ(expectedValidAfter, newChunk.getHistory().front().getValidAfter());
            ASSERT_EQ(expectedValidAfter, *newChunk.getOnCurrentShardSince());

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
            ASSERT_EQ(collPlacementVersionBefore.majorVersion() + 1,
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
            ASSERT_EQ(collPlacementVersionBefore.majorVersion() + 1,
                      rightSplitChunk.getVersion().majorVersion());
            ASSERT_EQ(expectedMinVersion++, rightSplitChunk.getVersion().minorVersion());

            // The history of the right split chunk must be the same of the original chunk
            assertSameHistories(rightSplitChunk.getHistory(), origChunk.getHistory());
        }
    }

    // Builds a chunk owned by 'shardId' with a single-entry history (validAfter Timestamp(100, 0)).
    ChunkType createChunk(const BSONObj& min,
                          const BSONObj& max,
                          const ChunkVersion& version,
                          const ShardId& shardId) {
        return createChunk(
            _collUUID, min, max, version, shardId, {ChunkHistory(Timestamp(100, 0), shardId)});
    }

    // Returns the highest chunk version currently owned by 'shard', i.e. its shard placement
    // version.
    ChunkVersion getShardVersion(const ShardId& shard) {
        auto doc = uassertStatusOK(findOneOnConfigCollection(
            operationContext(),
            NamespaceString::kConfigsvrChunksNamespace,
            BSON(ChunkType::collectionUUID << _collUUID << ChunkType::shard(shard.toString())),
            BSON(ChunkType::lastmod << -1)));
        return uassertStatusOK(ChunkType::parseFromConfigBSON(doc, _collEpoch, _collTimestamp))
            .getVersion();
    }

    // Reads the collection's chunks back from the global catalog through the catalog client - the
    // same read participants use to load chunks.
    std::vector<ChunkType> getChunksFromCatalogClient() {
        return uassertStatusOK(
            catalogClient()->getChunks(operationContext(),
                                       BSON(ChunkType::collectionUUID() << _collUUID),
                                       BSON(ChunkType::min() << 1) /* sort */,
                                       boost::none /* limit */,
                                       nullptr /* opTime */,
                                       _collEpoch,
                                       _collTimestamp,
                                       repl::ReadConcernArgs::kLocal));
    }

    boost::optional<ChunkType> getChangedChunkByMin(const std::vector<ChunkType>& changedChunks,
                                                    const BSONObj& min) {
        for (const auto& chunk : changedChunks) {
            if (chunk.getMin().woCompare(min) == 0) {
                return chunk;
            }
        }
        return boost::none;
    }

    // Asserts two changed-chunks lists describe the same chunks. The order can differ between a
    // commit and its idempotent retry, so the comparison is order-independent (keyed by chunk min).
    void assertSameChangedChunks(std::vector<ChunkType> lhs, std::vector<ChunkType> rhs) {
        ASSERT_EQ(lhs.size(), rhs.size());
        const auto byMin = [](const ChunkType& l, const ChunkType& r) {
            return l.getMin().woCompare(r.getMin()) < 0;
        };
        std::sort(lhs.begin(), lhs.end(), byMin);
        std::sort(rhs.begin(), rhs.end(), byMin);
        for (size_t i = 0; i < lhs.size(); ++i) {
            ASSERT_BSONOBJ_EQ(lhs[i].getMin(), rhs[i].getMin());
            ASSERT_BSONOBJ_EQ(lhs[i].getMax(), rhs[i].getMax());
            ASSERT_EQ(lhs[i].getShard(), rhs[i].getShard());
            ASSERT_EQ(lhs[i].getVersion(), rhs[i].getVersion());
        }
    }

    std::vector<ChunkType> commit(const ChunkType& migratedChunk,
                                  const ChunkVersion& donorShardVersionPreMigration,
                                  const ShardId& donor,
                                  const ShardId& recipient) {
        return assertGet(ShardingCatalogManager::get(operationContext())
                             ->commitMoveRange(operationContext(),
                                               kNamespace,
                                               migratedChunk,
                                               donorShardVersionPreMigration,
                                               donor,
                                               recipient));
    }

    // Simulates a concurrent DDL disabling chunk operations on the collection by clearing the
    // allowChunkOperations flag on its config.collections document.
    void disallowChunkOperations() {
        DBDirectClient client(operationContext());
        client.update(
            NamespaceString::kConfigsvrCollectionsNamespace,
            BSON(CollectionType::kNssFieldName << NamespaceStringUtil::serialize(
                     kNamespace, SerializationContext::stateDefault())),
            BSON("$set" << BSON(CollectionType::kAllowChunkOperationsFieldName << false)));
    }

    std::vector<ShardId> _shardIds;
    std::vector<ChunkType> chunks;

private:
    void setUp() override {
        CommitChunkMigrate::setUp();

        ShardType shard0;
        shard0.setHandle(ShardHandle{ShardId("shard0"), boost::none});
        shard0.setHost("shard0:12");

        ShardType shard1;
        shard1.setHandle(ShardHandle{ShardId("shard1"), boost::none});
        shard1.setHost("shard1:12");

        setupShards({shard0, shard1});

        _shardIds = {shard0.getName(), shard1.getName()};
    }

    void tearDown() override {
        CommitChunkMigrate::tearDown();
        _shardIds = std::vector<ShardId>();
        chunks = std::vector<ChunkType>();
    }

protected:
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
    migratedChunk.setRange({BSON("x" << 10), migratedChunk.getMax()});

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
    migratedChunk.setRange({migratedChunk.getMin(), BSON("x" << 10)});

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
    migratedChunk.setRange({BSON("x" << 1), BSON("x" << 10)});

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
            migratedChunk.setRange({BSON("x" << newMin), migratedChunk.getMax()});
            return true;
        }
        return false;
    }();

    bool expectRightSplit = [&]() {
        if (origChunkIndex < nChunks - 1 && random.nextInt32(2)) {
            const auto newMax = origChunk.getMax().getIntField("x") - 2;
            migratedChunk.setRange({migratedChunk.getMin(), BSON("x" << newMax)});
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

// A moveChunk where the donor keeps another chunk: the response carries the migrated chunk (now on
// the recipient) and the donor's control chunk (whose version was bumped).
TEST_F(CommitMoveRangeTest, ReturnsMigratedAndControlChunk) {
    const auto chunkMin = BSON("x" << 0);
    const auto chunkMid = BSON("x" << 10);
    const auto chunkMax = BSON("x" << 20);

    auto migratedChunk = createChunk(
        chunkMin, chunkMid, ChunkVersion({_collEpoch, _collTimestamp}, {2, 0}), _shardIds[0]);
    auto controlChunk = createChunk(
        chunkMid, chunkMax, ChunkVersion({_collEpoch, _collTimestamp}, {2, 1}), _shardIds[0]);
    setupCollection(kNamespace, kKeyPattern, {migratedChunk, controlChunk});

    const auto donorPreVersion = getShardVersion(_shardIds[0]);
    auto changedChunks = commit(migratedChunk, donorPreVersion, _shardIds[0], _shardIds[1]);

    ASSERT_EQ(2U, changedChunks.size());

    auto migrated = getChangedChunkByMin(changedChunks, chunkMin);
    ASSERT(migrated.has_value());
    ASSERT_EQ(_shardIds[1], migrated->getShard());
    ASSERT_BSONOBJ_EQ(chunkMid, migrated->getMax());

    auto control = getChangedChunkByMin(changedChunks, chunkMid);
    ASSERT(control.has_value());
    ASSERT_EQ(_shardIds[0], control->getShard());
    ASSERT_BSONOBJ_EQ(chunkMax, control->getMax());

    // The migrated chunk's major version was bumped past the pre-migration collection version.
    ASSERT_EQ(donorPreVersion.majorVersion() + 1, migrated->getVersion().majorVersion());
}

// Migrating the donor's only chunk leaves no control chunk, so the response carries just the
// migrated chunk.
TEST_F(CommitMoveRangeTest, LastChunkOnDonorReturnsOnlyMigratedChunk) {
    auto chunk = createChunk(kKeyPattern.globalMin(),
                             kKeyPattern.globalMax(),
                             ChunkVersion({_collEpoch, _collTimestamp}, {1, 0}),
                             _shardIds[0]);
    setupCollection(kNamespace, kKeyPattern, {chunk});

    const auto donorPreVersion = getShardVersion(_shardIds[0]);
    auto changedChunks = commit(chunk, donorPreVersion, _shardIds[0], _shardIds[1]);

    ASSERT_EQ(1U, changedChunks.size());
    ASSERT_EQ(_shardIds[1], changedChunks[0].getShard());
    ASSERT_BSONOBJ_EQ(kKeyPattern.globalMin(), changedChunks[0].getMin());
    ASSERT_BSONOBJ_EQ(kKeyPattern.globalMax(), changedChunks[0].getMax());
}

// A moveRange that moves a sub-range of a chunk splits it first: the response carries the migrated
// sub-range (on the recipient) plus the left and right side chunks left on the donor.
TEST_F(CommitMoveRangeTest, MoveRangeSplitReturnsSideChunks) {
    auto chunk = createChunk(kKeyPattern.globalMin(),
                             kKeyPattern.globalMax(),
                             ChunkVersion({_collEpoch, _collTimestamp}, {1, 0}),
                             _shardIds[0]);
    setupCollection(kNamespace, kKeyPattern, {chunk});

    const auto subMin = BSON("x" << 1);
    const auto subMax = BSON("x" << 10);
    auto migratedChunk = chunk;
    migratedChunk.setRange({subMin, subMax});

    const auto donorPreVersion = getShardVersion(_shardIds[0]);
    auto changedChunks = commit(migratedChunk, donorPreVersion, _shardIds[0], _shardIds[1]);

    ASSERT_EQ(3U, changedChunks.size());

    auto migrated = getChangedChunkByMin(changedChunks, subMin);
    ASSERT(migrated.has_value());
    ASSERT_EQ(_shardIds[1], migrated->getShard());
    ASSERT_BSONOBJ_EQ(subMax, migrated->getMax());

    auto leftSplit = getChangedChunkByMin(changedChunks, kKeyPattern.globalMin());
    ASSERT(leftSplit.has_value());
    ASSERT_EQ(_shardIds[0], leftSplit->getShard());
    ASSERT_BSONOBJ_EQ(subMin, leftSplit->getMax());

    auto rightSplit = getChangedChunkByMin(changedChunks, subMax);
    ASSERT(rightSplit.has_value());
    ASSERT_EQ(_shardIds[0], rightSplit->getShard());
    ASSERT_BSONOBJ_EQ(kKeyPattern.globalMax(), rightSplit->getMax());
}

// Re-running the same commit must be idempotent: the migration is detected as already applied, the
// response is rebuilt from durable state, and no versions are bumped a second time.
TEST_F(CommitMoveRangeTest, IdempotentRetryReturnsSameChangedChunks) {
    const auto chunkMin = BSON("x" << 0);
    const auto chunkMid = BSON("x" << 10);
    const auto chunkMax = BSON("x" << 20);

    auto migratedChunk = createChunk(
        chunkMin, chunkMid, ChunkVersion({_collEpoch, _collTimestamp}, {2, 0}), _shardIds[0]);
    auto controlChunk = createChunk(
        chunkMid, chunkMax, ChunkVersion({_collEpoch, _collTimestamp}, {2, 1}), _shardIds[0]);
    setupCollection(kNamespace, kKeyPattern, {migratedChunk, controlChunk});

    const auto donorPreVersion = getShardVersion(_shardIds[0]);

    auto firstResult = commit(migratedChunk, donorPreVersion, _shardIds[0], _shardIds[1]);
    const auto collVersionAfterFirst = getShardVersion(_shardIds[1]);

    // Retry with the same arguments; the migration is already committed.
    auto secondResult = commit(migratedChunk, donorPreVersion, _shardIds[0], _shardIds[1]);

    assertSameChangedChunks(firstResult, secondResult);

    // The retry must not bump any version.
    ASSERT_EQ(collVersionAfterFirst, getShardVersion(_shardIds[1]));
}

// An idempotent retry of an already-applied commit must return OK even when chunk operations have
// since been disallowed on the collection (e.g. a concurrent DDL set allowChunkOperations=false).
// The allowChunkOperations check runs only on the non-retry path.
TEST_F(CommitMoveRangeTest, IdempotentRetrySucceedsWhenChunkOperationsDisallowed) {
    const auto chunkMin = BSON("x" << 0);
    const auto chunkMid = BSON("x" << 10);
    const auto chunkMax = BSON("x" << 20);

    auto migratedChunk = createChunk(
        chunkMin, chunkMid, ChunkVersion({_collEpoch, _collTimestamp}, {2, 0}), _shardIds[0]);
    auto controlChunk = createChunk(
        chunkMid, chunkMax, ChunkVersion({_collEpoch, _collTimestamp}, {2, 1}), _shardIds[0]);
    setupCollection(kNamespace, kKeyPattern, {migratedChunk, controlChunk});

    const auto donorPreVersion = getShardVersion(_shardIds[0]);

    auto firstResult = commit(migratedChunk, donorPreVersion, _shardIds[0], _shardIds[1]);
    const auto collVersionAfterFirst = getShardVersion(_shardIds[1]);

    disallowChunkOperations();

    // The retry is detected as already applied and must still return the same changed chunks rather
    // than failing with ConflictingOperationInProgress.
    auto secondResult = commit(migratedChunk, donorPreVersion, _shardIds[0], _shardIds[1]);

    assertSameChangedChunks(firstResult, secondResult);

    // The retry must not bump any version.
    ASSERT_EQ(collVersionAfterFirst, getShardVersion(_shardIds[1]));
}

// The commit returns chunks that participants store alongside chunks they load from the global
// catalog through the catalog client, so both must expose the same set of fields. This compares the
// field set (format), not the values, of each commit chunk against the catalog chunk for the same
// range. A moveRange that splits the source chunk is used so the response carries a recipient chunk
// plus the donor's two side chunks, covering generated _ids and inherited
// history/onCurrentShardSince.
TEST_F(CommitMoveRangeTest, ChangedChunksHaveSameFormatAsGetChunks) {
    auto chunk = createChunk(kKeyPattern.globalMin(),
                             kKeyPattern.globalMax(),
                             ChunkVersion({_collEpoch, _collTimestamp}, {1, 0}),
                             _shardIds[0]);
    setupCollection(kNamespace, kKeyPattern, {chunk});

    const auto subMin = BSON("x" << 1);
    const auto subMax = BSON("x" << 10);
    auto migratedChunk = chunk;
    migratedChunk.setRange({subMin, subMax});

    const auto donorPreVersion = getShardVersion(_shardIds[0]);
    auto changedChunks = commit(migratedChunk, donorPreVersion, _shardIds[0], _shardIds[1]);
    ASSERT_EQ(3U, changedChunks.size());

    const auto catalogChunks = getChunksFromCatalogClient();

    // Each chunk returned by the commit must have the same fields as the matching catalog chunk,
    // regardless of their values.
    for (const auto& changed : changedChunks) {
        auto fromCatalog = getChangedChunkByMin(catalogChunks, changed.getMin());
        ASSERT(fromCatalog.has_value());
        ASSERT_EQ(sortedFieldNames(changed.toConfigBSON()),
                  sortedFieldNames(fromCatalog->toConfigBSON()));
    }
}

}  // namespace
}  // namespace mongo
