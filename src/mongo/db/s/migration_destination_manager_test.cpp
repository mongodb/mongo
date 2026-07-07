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

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "cxxabi.h"
#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/index/index_constants.h"
#include "mongo/db/router_role/routing_cache/catalog_cache_test_fixture.h"
#include "mongo/db/s/migration_destination_manager.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/str.h"

#include <system_error>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

const ShardId kRecipientShard("recipientShard");
const ShardId kOtherShard("otherShard");

class MigrationDestinationManagerTest : public ShardServerTestFixture {
protected:
    /**
     * Instantiates a BSON object in which both "_id" and "X" are set to value.
     */
    static BSONObj createDocument(int value) {
        return BSON("_id" << value << "X" << value);
    }

    /**
     * Inserts a document into config.shard.catalog.chunks (ChunkType::toConfigBSON layout) for
     * 'collUuid' over the range [min, max), currently owned by 'currentShard', with the given
     * 'history' entries expressed as {validAfter, owningShard} pairs listed newest-first.
     *
     * Shard identity is written as a bare string, exactly as the shard catalog stores a shard name.
     */
    void insertShardCatalogChunk(const UUID& collUuid,
                                 const BSONObj& min,
                                 const BSONObj& max,
                                 const ShardId& currentShard,
                                 const std::vector<std::pair<Timestamp, ShardId>>& history) {
        BSONArrayBuilder historyBuilder;
        for (const auto& [validAfter, shard] : history) {
            BSONObjBuilder entry;
            entry.append(ChunkHistoryBase::kValidAfterFieldName, validAfter);
            entry.append(ChunkHistoryBase::kShardFieldName, shard.toString());
            historyBuilder.append(entry.obj());
        }

        const auto onCurrentShardSince = history.empty() ? Timestamp(1, 1) : history.front().first;
        BSONObjBuilder builder;
        builder.append(ChunkType::name.name(), OID::gen());
        collUuid.appendToBuilder(&builder, ChunkType::collectionUUID.name());
        builder.append(ChunkType::min.name(), min);
        builder.append(ChunkType::max.name(), max);
        builder.append(ChunkType::shard.name(), currentShard.toString());
        builder.append("lastmod", Timestamp(1, 1));
        builder.append(ChunkType::onCurrentShardSince.name(), onCurrentShardSince);
        builder.append(ChunkType::history.name(), historyBuilder.arr());

        DBDirectClient client(operationContext());
        client.insert(NamespaceString::kConfigShardCatalogChunksNamespace, builder.obj());
    }

    /**
     * Forces the storage engine's oldest timestamp, which is the lower bound of point-in-time
     * (PIT) reachability that the check under test compares chunk history against. Self-validates
     * that the value took effect in this fixture.
     */
    void setOldestTimestamp(Timestamp ts) {
        auto* storageEngine = getServiceContext()->getStorageEngine();
        storageEngine->setOldestTimestamp(ts, /*force=*/true);
        ASSERT_EQ(storageEngine->getOldestTimestamp(), ts);
    }

    bool hasConflict(const UUID& collUuid,
                     const ChunkRange& span,
                     const ShardId& recipient = kRecipientShard) {
        return MigrationDestinationManager::migrationWouldDropPITHistory(
            operationContext(), collUuid, recipient, span);
    }

    /**
     * Creates a list of documents to clone.
     */
    static std::vector<BSONObj> createDocumentsToClone() {
        return {createDocument(1), createDocument(2), createDocument(3)};
    }

    /**
     * Creates a list of documents to clone and converts it to a BSONArray.
     */
    static BSONArray createDocumentsToCloneArray() {
        BSONArrayBuilder arrayBuilder;
        for (auto& doc : createDocumentsToClone()) {
            arrayBuilder.append(doc);
        }
        return arrayBuilder.arr();
    }
};

// Tests that documents will ferry from the fetch logic to the insert logic successfully.
TEST_F(MigrationDestinationManagerTest, CloneDocumentsFromDonorWorksCorrectly) {
    bool ranOnce = false;

    auto fetchBatchFn = [&](OperationContext* opCtx, BSONObj* nextBatch) {
        BSONObjBuilder fetchBatchResultBuilder;

        if (ranOnce) {
            fetchBatchResultBuilder.append("objects", BSONObj());
        } else {
            ranOnce = true;
            fetchBatchResultBuilder.append("objects", createDocumentsToCloneArray());
        }

        *nextBatch = fetchBatchResultBuilder.obj();
        return nextBatch->getField("objects").Obj().isEmpty();
    };

    std::vector<BSONObj> resultDocs;

    auto insertBatchFn = [&](OperationContext* opCtx, BSONObj docs) {
        auto arr = docs["objects"].Obj();
        if (arr.isEmpty())
            return false;
        for (auto&& docToClone : arr) {
            resultDocs.push_back(docToClone.Obj().getOwned());
        }
        return true;
    };

    MigrationDestinationManager::fetchAndApplyBatch(
        operationContext(), insertBatchFn, fetchBatchFn);

    std::vector<BSONObj> originalDocs = createDocumentsToClone();

    ASSERT_EQ(originalDocs.size(), resultDocs.size());

    for (auto originalDocsIt = originalDocs.begin(), resultDocsIt = resultDocs.begin();
         originalDocsIt != originalDocs.end() && resultDocsIt != resultDocs.end();
         ++originalDocsIt, ++resultDocsIt) {
        ASSERT_BSONOBJ_EQ(*originalDocsIt, *resultDocsIt);
    }
}

// Tests that an exception in the fetch logic will successfully throw an exception on the main
// thread.
TEST_F(MigrationDestinationManagerTest, CloneDocumentsThrowsFetchErrors) {
    bool ranOnce = false;

    auto fetchBatchFn = [&](OperationContext* opCtx, BSONObj* nextBatch) {
        BSONObjBuilder fetchBatchResultBuilder;

        if (ranOnce) {
            uasserted(ErrorCodes::NetworkTimeout, "network error");
        }

        ranOnce = true;
        fetchBatchResultBuilder.append("objects", createDocumentsToCloneArray());

        *nextBatch = fetchBatchResultBuilder.obj();
        return nextBatch->getField("objects").Obj().isEmpty();
    };

    auto insertBatchFn = [&](OperationContext* opCtx, BSONObj docs) {
        return true;
    };

    ASSERT_THROWS_CODE_AND_WHAT(MigrationDestinationManager::fetchAndApplyBatch(
                                    operationContext(), insertBatchFn, fetchBatchFn),
                                DBException,
                                ErrorCodes::NetworkTimeout,
                                "network error");
}

// Tests that an exception in the insertion logic will successfully throw an exception on the
// main thread.
TEST_F(MigrationDestinationManagerTest, CloneDocumentsCatchesInsertErrors) {
    auto fetchBatchFn = [&](OperationContext* opCtx, BSONObj* nextBatch) {
        BSONObjBuilder fetchBatchResultBuilder;
        fetchBatchResultBuilder.append("objects", createDocumentsToCloneArray());
        *nextBatch = fetchBatchResultBuilder.obj();
        return nextBatch->getField("objects").Obj().isEmpty();
    };

    auto insertBatchFn = [&](OperationContext* opCtx, BSONObj docs) {
        uasserted(ErrorCodes::FailedToParse, "insertion error");
        return false;
    };

    // Since the error is thrown on another thread, the message becomes "operation was interrupted"
    // on the main thread.

    ASSERT_THROWS_CODE_AND_WHAT(MigrationDestinationManager::fetchAndApplyBatch(
                                    operationContext(), insertBatchFn, fetchBatchFn),
                                DBException,
                                51008,
                                "operation was interrupted");

    ASSERT_EQ(operationContext()->getKillStatus(), 51008);
}

using MigrationDestinationManagerNetworkTest = RouterCatalogCacheTestFixture;

// Verifies MigrationDestinationManager::getCollectionOptions() and
// MigrationDestinationManager::getCollectionIndexes() won't use shard/db versioning without a chunk
// manager and won't include a read concern without afterClusterTime.
TEST_F(MigrationDestinationManagerNetworkTest,
       MigrationDestinationManagerGetIndexesAndCollectionsNoVersionsOrReadConcern) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("db.foo");

    // Shard nss by _id with chunks [minKey, 0), [0, maxKey] on shards "0" and "1" respectively.
    // ShardId("1") is the primary shard for the database.
    auto shards = setupNShards(2);
    auto cm = loadRoutingTableWithTwoChunksAndTwoShardsImpl(
        nss, BSON("_id" << 1), boost::optional<std::string>("1"));

    auto future = launchAsync([&] {
        onCommand([&](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(request.cmdObj.firstElementFieldName(), "listCollections"sv);
            ASSERT_EQUALS(request.target, HostAndPort("Host0:12345"));
            ASSERT_FALSE(request.cmdObj.hasField("readConcern"));
            ASSERT_FALSE(request.cmdObj.hasField("databaseVersion"));
            ASSERT_BSONOBJ_EQ(request.cmdObj["filter"].Obj(), BSON("name" << nss.coll()));

            const std::vector<BSONObj> colls = {
                BSON("name" << nss.coll() << "options" << BSONObj() << "info"
                            << BSON("readOnly" << false << "uuid" << UUID::gen()) << "idIndex"
                            << BSON("v" << 2 << "key" << BSON("_id" << 1) << "name"
                                        << IndexConstants::kIdIndexName))};

            std::string listCollectionsNs = str::stream()
                << nss.db_forTest() << "$cmd.listCollections";
            return BSON(
                "ok" << 1 << "cursor"
                     << BSON("id" << 0LL << "ns" << listCollectionsNs << "firstBatch" << colls));
        });
    });

    MigrationDestinationManager::getCollectionOptions(
        operationContext(), nss, ShardId("0"), boost::none, boost::none);

    future.default_timed_get();

    future = launchAsync([&] {
        onCommand([&](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(request.cmdObj.firstElementFieldName(), "listIndexes"sv);
            ASSERT_EQUALS(request.target, HostAndPort("Host0:12345"));
            ASSERT_FALSE(request.cmdObj.hasField("readConcern"));
            ASSERT_FALSE(request.cmdObj.hasField("shardVersion"));

            const std::vector<BSONObj> indexes = {BSON(
                "v" << 2 << "key" << BSON("_id" << 1) << "name" << IndexConstants::kIdIndexName)};
            return BSON(
                "ok" << 1 << "cursor"
                     << BSON("id" << 0LL << "ns" << nss.ns_forTest() << "firstBatch" << indexes));
        });
    });

    MigrationDestinationManager::getCollectionIndexes(
        operationContext(), nss, ShardId("0"), boost::none, boost::none);
    future.default_timed_get();
}

// Tests that error message includes missing index names when collection is not empty.
TEST_F(MigrationDestinationManagerTest, ErrorMessageIncludesMissingIndexNames) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.foo");
    boost::optional<UUID> collectionUUID;

    auto opCtx = operationContext();
    DBDirectClient client(opCtx);

    // Create a collection with some data (so it's not empty).
    // Note that index on '_id' is built by default.
    client.insert(nss, BSON("_id" << 1 << "x" << 1));

    // Get the UUID of the created collection directly from the catalog.
    auto catalog = CollectionCatalog::get(opCtx);
    auto uuid = catalog->lookupUUIDByNSS(opCtx, nss);
    ASSERT_TRUE(uuid);
    collectionUUID.emplace(*uuid);

    // Prepare index specs that include indexes not on the local collection.
    // Use the actual UUID of the created collection.
    ASSERT(collectionUUID.has_value());

    CollectionOptionsAndIndexes collectionOptionsAndIndexes{
        *collectionUUID,
        {
            BSON("v" << 2 << "key" << BSON("y" << 1) << "name" << "y_1"),  // Missing index
            BSON("v" << 2 << "key" << BSON("z" << 1) << "name" << "z_1")   // Missing index
        },
        BSON("v" << 2 << "key" << BSON("_id" << 1) << "name" << IndexConstants::kIdIndexName),
        BSONObj()};

    LOGV2(11294300, "The error message should list the missing index names: y_1 and z_1.");
    ASSERT_THROWS_CODE_AND_WHAT(
        MigrationDestinationManager::cloneCollectionIndexesAndOptions(
            operationContext(), nss, collectionOptionsAndIndexes),
        DBException,
        ErrorCodes::CannotCreateCollection,
        "aborting, shard is missing 2 indexes and collection is not empty. Non-trivial index "
        "creation should be scheduled manually. Missing indexes: y_1, z_1");
}

// Shard key bounds reused across the PIT-reachable unowned chunk tests.
const BSONObj k0 = BSON("a" << 0);
const BSONObj k50 = BSON("a" << 50);
const BSONObj k100 = BSON("a" << 100);
const BSONObj k200 = BSON("a" << 200);
const BSONObj k300 = BSON("a" << 300);

// An empty shard catalog cannot conflict with any span.
TEST_F(MigrationDestinationManagerTest, PITReachableUnownedChunkEmptyCatalog) {
    ASSERT_FALSE(hasConflict(UUID::gen(), ChunkRange(k0, k100)));
}

// A reachable unowned entry that extends beyond the span (here past its max) loses the uncovered
// portion's PIT history when the span is refreshed, so it is a conflict. This is the pre-split
// move-back case: the source chunk is only [0, 50), but the stale entry [0, 100) reaches to 100.
TEST_F(MigrationDestinationManagerTest, PITReachableUnownedChunkExtendsPastSpanMax) {
    const auto collUuid = UUID::gen();
    insertShardCatalogChunk(collUuid,
                            k0,
                            k100,
                            kOtherShard,
                            {{Timestamp(20, 0), kOtherShard}, {Timestamp(10, 0), kRecipientShard}});
    setOldestTimestamp(Timestamp(5, 0));

    ASSERT_TRUE(hasConflict(collUuid, ChunkRange(k0, k50)));
}

// A reachable unowned entry extending below the span's min is likewise a conflict.
TEST_F(MigrationDestinationManagerTest, PITReachableUnownedChunkExtendsBelowSpanMin) {
    const auto collUuid = UUID::gen();
    insertShardCatalogChunk(collUuid,
                            k0,
                            k100,
                            kOtherShard,
                            {{Timestamp(20, 0), kOtherShard}, {Timestamp(10, 0), kRecipientShard}});
    setOldestTimestamp(Timestamp(5, 0));

    ASSERT_TRUE(hasConflict(collUuid, ChunkRange(k50, k100)));
}

// A reachable unowned entry exactly equal to the span is fully re-inserted, so its PIT history is
// preserved and it is not a conflict. This is the move-back case where the source chunk covers the
// whole stale entry.
TEST_F(MigrationDestinationManagerTest, PITReachableUnownedChunkEqualToSpan) {
    const auto collUuid = UUID::gen();
    insertShardCatalogChunk(collUuid,
                            k0,
                            k100,
                            kOtherShard,
                            {{Timestamp(20, 0), kOtherShard}, {Timestamp(10, 0), kRecipientShard}});
    setOldestTimestamp(Timestamp(5, 0));

    ASSERT_FALSE(hasConflict(collUuid, ChunkRange(k0, k100)));
}

// The merged-back case: the source chunk [0, 100) covers two narrower stale sub-range entries that
// each fall within it, so both are fully refreshed and neither is a conflict.
TEST_F(MigrationDestinationManagerTest, PITReachableUnownedChunkMergedSpanCoversSubRanges) {
    const auto collUuid = UUID::gen();
    insertShardCatalogChunk(collUuid,
                            k0,
                            k50,
                            kOtherShard,
                            {{Timestamp(20, 0), kOtherShard}, {Timestamp(10, 0), kRecipientShard}});
    insertShardCatalogChunk(collUuid,
                            k50,
                            k100,
                            kOtherShard,
                            {{Timestamp(20, 0), kOtherShard}, {Timestamp(10, 0), kRecipientShard}});
    setOldestTimestamp(Timestamp(5, 0));

    ASSERT_FALSE(hasConflict(collUuid, ChunkRange(k0, k100)));
}

// A reachable unowned entry strictly contained within the span is fully refreshed, so it is not a
// conflict.
TEST_F(MigrationDestinationManagerTest, PITReachableUnownedChunkContainedInSpan) {
    const auto collUuid = UUID::gen();
    insertShardCatalogChunk(collUuid,
                            k50,
                            k100,
                            kOtherShard,
                            {{Timestamp(20, 0), kOtherShard}, {Timestamp(10, 0), kRecipientShard}});
    setOldestTimestamp(Timestamp(5, 0));

    ASSERT_FALSE(hasConflict(collUuid, ChunkRange(k0, k200)));
}

// An entry extending beyond the span, but whose stale ownership has aged past the oldest timestamp,
// is no longer reachable by PIT reads and so is not a conflict.
TEST_F(MigrationDestinationManagerTest, PITReachableUnownedChunkAgedOut) {
    const auto collUuid = UUID::gen();
    insertShardCatalogChunk(collUuid,
                            k0,
                            k100,
                            kOtherShard,
                            {{Timestamp(20, 0), kOtherShard}, {Timestamp(10, 0), kRecipientShard}});
    setOldestTimestamp(Timestamp(25, 0));

    ASSERT_FALSE(hasConflict(collUuid, ChunkRange(k0, k50)));
}

// An entry currently owned by the recipient is not a conflict; the recipient re-owning a range it
// already owns does not create an inconsistent timeline, even when it extends beyond the span.
TEST_F(MigrationDestinationManagerTest, PITReachableUnownedChunkCurrentlyOwnedByRecipient) {
    const auto collUuid = UUID::gen();
    insertShardCatalogChunk(
        collUuid, k0, k100, kRecipientShard, {{Timestamp(20, 0), kRecipientShard}});
    setOldestTimestamp(Timestamp(5, 0));

    ASSERT_FALSE(hasConflict(collUuid, ChunkRange(k0, k50)));
}

// A reachable unowned entry that does not overlap the span is not a conflict.
TEST_F(MigrationDestinationManagerTest, PITReachableUnownedChunkNonOverlapping) {
    const auto collUuid = UUID::gen();
    insertShardCatalogChunk(collUuid,
                            k200,
                            k300,
                            kOtherShard,
                            {{Timestamp(20, 0), kOtherShard}, {Timestamp(10, 0), kRecipientShard}});
    setOldestTimestamp(Timestamp(5, 0));

    ASSERT_FALSE(hasConflict(collUuid, ChunkRange(k0, k100)));
}

}  // namespace
}  // namespace mongo
