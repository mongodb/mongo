/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/database_name.h"
#include "mongo/db/database_name_util.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/session/logical_session_cache.h"
#include "mongo/db/session/logical_session_cache_noop.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/sharding_environment/config_server_test_fixture.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/shard_ref.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/serialization_context.h"
#include "mongo/util/uuid.h"

#include <string>
#include <vector>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

/**
 * Coverage for ShardingCatalogManager::convertShardRefsInNamespaceMetadata(), which rewrites the
 * shard references persisted in the global catalog from legacy shard names into ShardRef(UUID)
 * values. It operates over two distinct namespace shapes:
 *   - a database-only namespace: the 'primary' field of the config.databases document, resolved
 *     through the ShardRegistry; and
 *   - a collection namespace: the 'shard' field and every 'history' entry of the matching
 *     config.chunks documents, resolved through the local catalog client's view of config.shards.
 */
class ConvertShardRefsInNamespaceMetadataTest : public ConfigServerTestFixture {
protected:
    const ShardId kShardA{"shardA"};
    const ShardId kShardB{"shardB"};
    // A shard that exists in config.shards but has not been assigned a UUID yet (e.g. a shard
    // registered by an older binary). The conversion is expected to tassert when it cannot obtain
    // UUID from the current content of config.shards.
    const ShardId kShardNoUuid{"shardNoUuid"};

    const UUID kShardAUuid = UUID::gen();
    const UUID kShardBUuid = UUID::gen();

    const DatabaseName kDbName = DatabaseName::createDatabaseName_forTest(boost::none, "testDB");
    const DatabaseName kOtherDbName =
        DatabaseName::createDatabaseName_forTest(boost::none, "otherDB");

    const NamespaceString kCollNss =
        NamespaceString::createNamespaceString_forTest(kDbName, "coll");
    const NamespaceString kOtherCollNss =
        NamespaceString::createNamespaceString_forTest(kDbName, "otherColl");

    // Single-field shard key shared by every collection set up in these tests.
    const KeyPattern kShardKey{BSON("x" << 1)};

    // Collection epoch/timestamp shared across the collections created here. Collections are
    // distinguished by their UUID, so reusing the same epoch/timestamp is harmless and keeps the
    // helpers simple.
    const OID kEpoch = OID::gen();
    const Timestamp kCollTimestamp{1, 1};

    void setUp() override {
        ConfigServerTestFixture::setUp();

        // convertShardRefsInNamespaceMetadata() performs plain (non-transactional) reads and writes
        // through a DBDirectClient. The session machinery below is not strictly required, but it
        // mirrors the other sharding_catalog_manager config-server tests and is harmless here.
        DBDirectClient client(operationContext());
        client.createCollection(NamespaceString::kSessionTransactionsTableNamespace);
        client.createIndexes(NamespaceString::kSessionTransactionsTableNamespace,
                             {MongoDSessionCatalog::getConfigTxnPartialIndexSpec()});
        LogicalSessionCache::set(getServiceContext(), std::make_unique<LogicalSessionCacheNoop>());
    }

    ShardingCatalogManager& shardingCatalogManager() {
        return *ShardingCatalogManager::get(operationContext());
    }

    /**
     * Helper function for the ShardingCatalogManager method covered in this file.
     */
    void convertShardReferences(const NamespaceString& nss) {
        shardingCatalogManager().convertShardRefsInNamespaceMetadata(operationContext(), nss);
    }

    // Builds a config.shards document. When 'uuid' is set, the persisted document carries the UUID,
    // which is precisely what lets the conversion map the shard name onto its ShardRef(UUID).
    static ShardType makeShard(const ShardId& shardId, boost::optional<UUID> uuid) {
        return ShardType(shardId.toString(), std::move(uuid), shardId.toString() + ":12345");
    }

    // Persists 'shards' into config.shards and reloads the shard registry so that the database path
    // (which resolves the primary shard via Grid::shardRegistry()) observes the UUIDs.
    void setupShardsAndReloadRegistry(const std::vector<ShardType>& shards) {
        setupShards(shards);
        shardRegistry()->reload(operationContext());
    }

    ChunkVersion chunkVersion() const {
        return ChunkVersion({kEpoch, kCollTimestamp}, {1, 0});
    }

    // Creates a full-range chunk for 'collUuid' owned by 'owner' with the given 'history'. The
    // newest history entry (front) must reference 'owner', consistent with how chunk history is
    // ordered and validated.
    ChunkType makeChunk(const UUID& collUuid,
                        const ShardId& owner,
                        std::vector<ChunkHistory> history) {
        ChunkType chunk(collUuid,
                        {kShardKey.globalMin(), kShardKey.globalMax()},
                        chunkVersion(),
                        ShardRef(owner));
        chunk.setName(OID::gen());
        chunk.setOnCurrentShardSince(history.front().getValidAfter());
        chunk.setHistory(std::move(history));
        return chunk;
    }

    // Creates a full-range chunk for 'collUuid' owned by 'owner' with no history field. This
    // mirrors never-migrated chunks, which do not serialize an empty history array.
    ChunkType makeChunkWithoutHistory(const UUID& collUuid, const ShardId& owner) {
        ChunkType chunk(collUuid,
                        {kShardKey.globalMin(), kShardKey.globalMax()},
                        chunkVersion(),
                        ShardRef(owner));
        chunk.setName(OID::gen());
        return chunk;
    }

    // Reads back the converted full-range chunk for 'collUuid'.
    ChunkType getChunk(const UUID& collUuid) {
        return uassertStatusOK(getChunkDoc(
            operationContext(), collUuid, kShardKey.globalMin(), kEpoch, kCollTimestamp));
    }

    // Creates a chunk for 'collUuid' with a custom range, owned by 'owner', with 'history'. Use
    // instead of makeChunk() when the test needs more than one chunk per collection.
    ChunkType makeChunkWithRange(const UUID& collUuid,
                                 const ShardRef& owner,
                                 const BSONObj& min,
                                 const BSONObj& max,
                                 std::vector<ChunkHistory> history) {
        ChunkType chunk(collUuid, {min, max}, chunkVersion(), owner);
        chunk.setName(OID::gen());
        chunk.setOnCurrentShardSince(history.front().getValidAfter());
        chunk.setHistory(std::move(history));
        return chunk;
    }

    // Reads back the chunk for 'collUuid' whose range starts at 'min'.
    ChunkType getChunkByMin(const UUID& collUuid, const BSONObj& min) {
        return uassertStatusOK(
            getChunkDoc(operationContext(), collUuid, min, kEpoch, kCollTimestamp));
    }

    // Reads back the 'primary' ShardRef persisted in config.databases for 'dbName', mirroring the
    // exact read/parse performed by the production code.
    ShardRef getPersistedPrimary(const DatabaseName& dbName) {
        const auto dbNameStr =
            DatabaseNameUtil::serialize(dbName, SerializationContext::stateDefault());
        const auto dbBSON = uassertStatusOK(
            findOneOnConfigCollection(operationContext(),
                                      NamespaceString::kConfigDatabasesNamespace,
                                      BSON(DatabaseType::kDbNameFieldName << dbNameStr)));
        return ShardRef::parse(dbBSON[DatabaseType::kPrimaryFieldName]);
    }

    static void assertIsUuid(const ShardRef& ref, const UUID& expected) {
        ASSERT_TRUE(ref.isUUID()) << "expected a UUID ShardRef but got " << ref;
        ASSERT_EQ(ref.getUUID(), expected);
    }

    static void assertIsName(const ShardRef& ref, const ShardId& expected) {
        ASSERT_TRUE(ref.isString()) << "expected a string ShardRef but got " << ref;
        ASSERT_EQ(ref.getString(), expected.toString());
    }
};

using ConvertShardRefsInNamespaceMetadataTestDeathTest = ConvertShardRefsInNamespaceMetadataTest;

//
// Database namespace path: converts the 'primary' field of the config.databases document.
//

TEST_F(ConvertShardRefsInNamespaceMetadataTest, ConvertsPrimaryShardNameToUuidInConfigDatabases) {
    setupShardsAndReloadRegistry({makeShard(kShardA, kShardAUuid)});
    setupDatabase(kDbName, kShardA);

    // Precondition: the primary is still persisted as a legacy shard name.
    assertIsName(getPersistedPrimary(kDbName), kShardA);

    convertShardReferences(NamespaceString::createNamespaceString_forTest(kDbName));

    assertIsUuid(getPersistedPrimary(kDbName), kShardAUuid);
}

TEST_F(ConvertShardRefsInNamespaceMetadataTest, OnlyConvertsTargetDatabaseLeavingOthersUnchanged) {
    setupShardsAndReloadRegistry(
        {makeShard(kShardA, kShardAUuid), makeShard(kShardB, kShardBUuid)});
    setupDatabase(kDbName, kShardA);
    setupDatabase(kOtherDbName, kShardB);

    convertShardReferences(NamespaceString::createNamespaceString_forTest(kDbName));

    // The targeted database is converted...
    assertIsUuid(getPersistedPrimary(kDbName), kShardAUuid);
    // ...while the untargeted database is left as a legacy shard name.
    assertIsName(getPersistedPrimary(kOtherDbName), kShardB);
}

TEST_F(ConvertShardRefsInNamespaceMetadataTest, ConvertingDatabaseWithUuidPrimaryIsNoOp) {
    setupShardsAndReloadRegistry({makeShard(kShardA, kShardAUuid)});
    setupDatabase(kDbName, kShardA);

    const auto dbNss = NamespaceString::createNamespaceString_forTest(kDbName);

    // The first conversion rewrites the legacy shard name into its UUID.
    convertShardReferences(dbNss);
    assertIsUuid(getPersistedPrimary(kDbName), kShardAUuid);

    // The document now already stores a UUID primary. A second conversion must resolve the shard by
    // its UUID (rather than by name) and leave the stored ShardRef unchanged.
    convertShardReferences(dbNss);
    assertIsUuid(getPersistedPrimary(kDbName), kShardAUuid);
}

TEST_F(ConvertShardRefsInNamespaceMetadataTest, DatabaseNotFoundInConfigDatabasesIsNoOp) {
    setupShardsAndReloadRegistry({makeShard(kShardA, kShardAUuid)});

    const auto dbNss = NamespaceString::createNamespaceString_forTest(kDbName);
    ASSERT_NO_THROW(convertShardReferences(dbNss));
}

//
// Collection namespace path: converts the 'shard' field and every 'history' entry of the matching
// config.chunks documents.
//

TEST_F(ConvertShardRefsInNamespaceMetadataTest, ConvertsShardAndHistoryToUuidForSingleShard) {
    setupShardsAndReloadRegistry({makeShard(kShardA, kShardAUuid)});

    const auto collUuid = UUID::gen();
    setupCollection(
        kCollNss,
        kShardKey,
        {makeChunk(collUuid, kShardA, {ChunkHistory(Timestamp(10, 0), ShardRef(kShardA))})});

    convertShardReferences(kCollNss);

    const auto chunk = getChunk(collUuid);
    assertIsUuid(chunk.getShard(), kShardAUuid);
    ASSERT_EQ(chunk.getHistory().size(), 1u);
    assertIsUuid(chunk.getHistory()[0].getShard(), kShardAUuid);
}

TEST_F(ConvertShardRefsInNamespaceMetadataTest, ConvertsHistoryEntriesAcrossMultipleShards) {
    setupShardsAndReloadRegistry(
        {makeShard(kShardA, kShardAUuid), makeShard(kShardB, kShardBUuid)});

    // A chunk currently owned by shardA whose history also references shardB. The conversion must
    // remap every history entry to the corresponding UUID, not just the entry for the current
    // owner.
    const auto collUuid = UUID::gen();
    setupCollection(kCollNss,
                    kShardKey,
                    {makeChunk(collUuid,
                               kShardA,
                               {ChunkHistory(Timestamp(20, 0), ShardRef(kShardA)),
                                ChunkHistory(Timestamp(10, 0), ShardRef(kShardB))})});

    convertShardReferences(kCollNss);

    const auto chunk = getChunk(collUuid);
    assertIsUuid(chunk.getShard(), kShardAUuid);
    ASSERT_EQ(chunk.getHistory().size(), 2u);
    assertIsUuid(chunk.getHistory()[0].getShard(), kShardAUuid);
    assertIsUuid(chunk.getHistory()[1].getShard(), kShardBUuid);
}

TEST_F(ConvertShardRefsInNamespaceMetadataTest, DoesNotModifyChunksOfOtherCollections) {
    setupShardsAndReloadRegistry({makeShard(kShardA, kShardAUuid)});

    const auto collUuid = UUID::gen();
    const auto otherCollUuid = UUID::gen();
    setupCollection(
        kCollNss,
        kShardKey,
        {makeChunk(collUuid, kShardA, {ChunkHistory(Timestamp(10, 0), ShardRef(kShardA))})});
    setupCollection(
        kOtherCollNss,
        kShardKey,
        {makeChunk(otherCollUuid, kShardA, {ChunkHistory(Timestamp(10, 0), ShardRef(kShardA))})});

    convertShardReferences(kCollNss);

    // The targeted collection's chunk is converted...
    assertIsUuid(getChunk(collUuid).getShard(), kShardAUuid);

    // ...while the other collection's chunk keeps its legacy shard name everywhere.
    const auto otherChunk = getChunk(otherCollUuid);
    assertIsName(otherChunk.getShard(), kShardA);
    ASSERT_EQ(otherChunk.getHistory().size(), 1u);
    assertIsName(otherChunk.getHistory()[0].getShard(), kShardA);
}

DEATH_TEST_REGEX_F(ConvertShardRefsInNamespaceMetadataTestDeathTest,
                   TassertsWhenHistoryEntryShardHasNoUuid,
                   "Tripwire assertion.*12888606") {
    // shardA has a UUID; kShardNoUuid intentionally does not. The conversion must tassert rather
    // than silently preserve the UUID-less shard's name.
    setupShardsAndReloadRegistry(
        {makeShard(kShardA, kShardAUuid), makeShard(kShardNoUuid, boost::none)});

    const auto collUuid = UUID::gen();
    setupCollection(kCollNss,
                    kShardKey,
                    {makeChunk(collUuid,
                               kShardA,
                               {ChunkHistory(Timestamp(20, 0), ShardRef(kShardA)),
                                ChunkHistory(Timestamp(10, 0), ShardRef(kShardNoUuid))})});

    convertShardReferences(kCollNss);
}

DEATH_TEST_REGEX_F(ConvertShardRefsInNamespaceMetadataTestDeathTest,
                   TassertsWhenChunkOwnerShardHasNoUuid,
                   "Tripwire assertion.*12888606") {
    // The only shard in the cluster has no UUID. The conversion must tassert rather than silently
    // skip it and produce an empty update.
    setupShardsAndReloadRegistry({makeShard(kShardNoUuid, boost::none)});

    const auto collUuid = UUID::gen();
    setupCollection(kCollNss,
                    kShardKey,
                    {makeChunk(collUuid,
                               kShardNoUuid,
                               {ChunkHistory(Timestamp(10, 0), ShardRef(kShardNoUuid))})});

    convertShardReferences(kCollNss);
}

TEST_F(ConvertShardRefsInNamespaceMetadataTest, ConvertingCollectionWithUuidShardsIsNoOp) {
    setupShardsAndReloadRegistry(
        {makeShard(kShardA, kShardAUuid), makeShard(kShardB, kShardBUuid)});

    const auto collUuid = UUID::gen();
    setupCollection(kCollNss,
                    kShardKey,
                    {makeChunk(collUuid,
                               kShardA,
                               {ChunkHistory(Timestamp(20, 0), ShardRef(kShardA)),
                                ChunkHistory(Timestamp(10, 0), ShardRef(kShardB))})});

    // Asserts that the chunk's 'shard' field and every history entry hold the expected UUIDs.
    auto assertChunkFullyConverted = [&] {
        const auto chunk = getChunk(collUuid);
        assertIsUuid(chunk.getShard(), kShardAUuid);
        ASSERT_EQ(chunk.getHistory().size(), 2u);
        assertIsUuid(chunk.getHistory()[0].getShard(), kShardAUuid);
        assertIsUuid(chunk.getHistory()[1].getShard(), kShardBUuid);
    };

    // The first conversion rewrites the shard name and every history entry into UUIDs.
    convertShardReferences(kCollNss);
    assertChunkFullyConverted();

    // The chunk's 'shard' field is now a UUID, so the by-name update no longer matches it; a second
    // conversion must leave the already-converted chunk untouched.
    convertShardReferences(kCollNss);
    assertChunkFullyConverted();
}

TEST_F(ConvertShardRefsInNamespaceMetadataTest, ConvertsPartiallyConvertedHistory) {
    setupShardsAndReloadRegistry(
        {makeShard(kShardA, kShardAUuid), makeShard(kShardB, kShardBUuid)});

    // A chunk owned by shardA (still a legacy name, so the by-name update matches it) whose history
    // is already partially converted: the middle entry holds shardB's UUID while the others still
    // hold legacy shard names.
    const auto collUuid = UUID::gen();
    setupCollection(kCollNss,
                    kShardKey,
                    {makeChunk(collUuid,
                               kShardA,
                               {ChunkHistory(Timestamp(30, 0), ShardRef(kShardA)),
                                ChunkHistory(Timestamp(20, 0), ShardRef(kShardBUuid)),
                                ChunkHistory(Timestamp(10, 0), ShardRef(kShardB))})});

    convertShardReferences(kCollNss);

    const auto chunk = getChunk(collUuid);
    assertIsUuid(chunk.getShard(), kShardAUuid);
    ASSERT_EQ(chunk.getHistory().size(), 3u);
    // The legacy name is converted...
    assertIsUuid(chunk.getHistory()[0].getShard(), kShardAUuid);
    // ...the already-converted UUID is preserved as-is (the $switch default branch)...
    assertIsUuid(chunk.getHistory()[1].getShard(), kShardBUuid);
    // ...and the remaining legacy name is converted.
    assertIsUuid(chunk.getHistory()[2].getShard(), kShardBUuid);
}

TEST_F(ConvertShardRefsInNamespaceMetadataTest, ConvertsShardOfChunkWithMissingHistory) {
    setupShardsAndReloadRegistry({makeShard(kShardA, kShardAUuid)});

    const auto collUuid = UUID::gen();
    setupCollection(kCollNss, kShardKey, {makeChunkWithoutHistory(collUuid, kShardA)});

    convertShardReferences(kCollNss);

    const auto chunk = getChunk(collUuid);
    assertIsUuid(chunk.getShard(), kShardAUuid);
    ASSERT_EQ(chunk.getHistory().size(), 0u);
}

TEST_F(ConvertShardRefsInNamespaceMetadataTest, ConvertsShardAndHistoryForMultipleChunks) {
    setupShardsAndReloadRegistry(
        {makeShard(kShardA, kShardAUuid), makeShard(kShardB, kShardBUuid)});

    // A collection with two chunks: the first owned by shardA and the second by shardB.
    const auto collUuid = UUID::gen();
    const auto kMidKey = BSON("x" << 0);
    setupCollection(kCollNss,
                    kShardKey,
                    {makeChunkWithRange(collUuid,
                                        ShardRef(kShardA),
                                        kShardKey.globalMin(),
                                        kMidKey,
                                        {ChunkHistory(Timestamp(10, 0), ShardRef(kShardA))}),
                     makeChunkWithRange(collUuid,
                                        ShardRef(kShardB),
                                        kMidKey,
                                        kShardKey.globalMax(),
                                        {ChunkHistory(Timestamp(10, 0), ShardRef(kShardB))})});

    convertShardReferences(kCollNss);

    const auto chunkA = getChunkByMin(collUuid, kShardKey.globalMin());
    assertIsUuid(chunkA.getShard(), kShardAUuid);
    ASSERT_EQ(chunkA.getHistory().size(), 1u);
    assertIsUuid(chunkA.getHistory()[0].getShard(), kShardAUuid);

    const auto chunkB = getChunkByMin(collUuid, kMidKey);
    assertIsUuid(chunkB.getShard(), kShardBUuid);
    ASSERT_EQ(chunkB.getHistory().size(), 1u);
    assertIsUuid(chunkB.getHistory()[0].getShard(), kShardBUuid);
}

TEST_F(ConvertShardRefsInNamespaceMetadataTest,
       ConvertsUnconvertedChunksLeavingUuidChunksUnchanged) {
    setupShardsAndReloadRegistry(
        {makeShard(kShardA, kShardAUuid), makeShard(kShardB, kShardBUuid)});

    // One chunk still holds legacy shard names; the other already holds UUID references (as if a
    // previous partial execution of this conversion had processed it but not the first chunk).
    const auto collUuid = UUID::gen();
    const auto kMidKey = BSON("x" << 0);
    setupCollection(kCollNss,
                    kShardKey,
                    {makeChunkWithRange(collUuid,
                                        ShardRef(kShardA),
                                        kShardKey.globalMin(),
                                        kMidKey,
                                        {ChunkHistory(Timestamp(10, 0), ShardRef(kShardA))}),
                     makeChunkWithRange(collUuid,
                                        ShardRef(kShardBUuid),
                                        kMidKey,
                                        kShardKey.globalMax(),
                                        {ChunkHistory(Timestamp(10, 0), ShardRef(kShardBUuid))})});

    convertShardReferences(kCollNss);

    // The legacy-name chunk is fully converted to UUIDs.
    const auto chunkA = getChunkByMin(collUuid, kShardKey.globalMin());
    assertIsUuid(chunkA.getShard(), kShardAUuid);
    ASSERT_EQ(chunkA.getHistory().size(), 1u);
    assertIsUuid(chunkA.getHistory()[0].getShard(), kShardAUuid);

    // The already-UUID chunk is preserved untouched (via the $switch default branch).
    const auto chunkB = getChunkByMin(collUuid, kMidKey);
    assertIsUuid(chunkB.getShard(), kShardBUuid);
    ASSERT_EQ(chunkB.getHistory().size(), 1u);
    assertIsUuid(chunkB.getHistory()[0].getShard(), kShardBUuid);
}

TEST_F(ConvertShardRefsInNamespaceMetadataTest,
       PreservesUnresolvableShardRefsWhileConvertingResolvableOnes) {
    // kRemovedShard is referenced in a chunk but does not appear in config.shards. The conversion
    // must convert only the resolvable shards, leaving unresolvable onesu ntouched.
    const ShardId kRemovedShard{"removedShard"};
    setupShardsAndReloadRegistry(
        {makeShard(kShardA, kShardAUuid), makeShard(kShardB, kShardBUuid)});

    const auto collUuid = UUID::gen();
    const auto kMidKey = BSON("x" << 0);
    setupCollection(
        kCollNss,
        kShardKey,
        // One chunk with a removeShard reference in its history
        {makeChunkWithRange(collUuid,
                            ShardRef(kShardA),
                            kShardKey.globalMin(),
                            kMidKey,
                            {ChunkHistory(Timestamp(10, 0), ShardRef(kShardA)),
                             ChunkHistory(Timestamp(9, 0), ShardRef(kRemovedShard)),
                             ChunkHistory(Timestamp(8, 0), ShardRef(kShardB))}),
         // One chunk assigned to a removed chunk (this case represents a catalog inconsistency)
         makeChunkWithRange(collUuid,
                            ShardRef(kRemovedShard),
                            kMidKey,
                            kShardKey.globalMax(),
                            {ChunkHistory(Timestamp(10, 0), ShardRef(kRemovedShard))})});

    convertShardReferences(kCollNss);

    const auto chunkA = getChunkByMin(collUuid, kShardKey.globalMin());
    assertIsUuid(chunkA.getShard(), kShardAUuid);
    ASSERT_EQ(chunkA.getHistory().size(), 3u);
    assertIsUuid(chunkA.getHistory()[0].getShard(), kShardAUuid);
    assertIsName(chunkA.getHistory()[1].getShard(), kRemovedShard);
    assertIsUuid(chunkA.getHistory()[2].getShard(), kShardBUuid);


    const auto chunkB = getChunkByMin(collUuid, kMidKey);
    assertIsName(chunkB.getShard(), kRemovedShard);
    ASSERT_EQ(chunkB.getHistory().size(), 1u);
    assertIsName(chunkB.getHistory()[0].getShard(), kRemovedShard);
}

TEST_F(ConvertShardRefsInNamespaceMetadataTest, CollectionNotFoundInConfigCollectionsIsNoOp) {
    setupShardsAndReloadRegistry({makeShard(kShardA, kShardAUuid)});

    ASSERT_NO_THROW(convertShardReferences(kCollNss));
}

}  // namespace
}  // namespace mongo
