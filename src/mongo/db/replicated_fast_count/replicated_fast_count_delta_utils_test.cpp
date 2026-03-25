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

#include "mongo/db/replicated_fast_count/replicated_fast_count_delta_utils.h"

#include "mongo/db/dbhelpers.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_util.h"
#include "mongo/db/storage/write_unit_of_work.h"

namespace mongo::replicated_fast_count {
namespace {

/**
 * Creates a replicated fast count collection using the global namespace string
 * kReplicatedFastCountStore.
 */
void createReplicatedFastCountCollection(repl::StorageInterface* storageInterface,
                                         OperationContext* opCtx) {
    ASSERT_OK(storageInterface->createCollection(
        opCtx,
        NamespaceString::makeGlobalConfigCollection(NamespaceString::kReplicatedFastCountStore),
        CollectionOptions{.clusteredIndex = clustered_util::makeDefaultClusteredIdIndex()}));
}

/**
 * Inserts a document directly into the replicated fast count collection with the given UUID,
 * size, and count.
 */
void insertSizeCountDocument(OperationContext* opCtx, UUID uuid, int64_t size, int64_t count) {
    AutoGetCollection fastCountColl(
        opCtx,
        NamespaceString::makeGlobalConfigCollection(NamespaceString::kReplicatedFastCountStore),
        LockMode::MODE_IX);
    ASSERT(fastCountColl);

    const Timestamp timestamp(1, 1);
    WriteUnitOfWork wuow{opCtx, WriteUnitOfWork::kGroupForPossiblyRetryableOperations};
    ASSERT_OK(Helpers::insert(opCtx,
                              *fastCountColl,
                              BSON("_id" << uuid << kValidAsOfKey << timestamp << kMetadataKey
                                         << BSON(kCountKey << count << kSizeKey << size))));
    wuow.commit();
}

class ReadAndIncrementSizeCountsTest : public CatalogTestFixture {};

TEST_F(ReadAndIncrementSizeCountsTest, IncrementZeros) {
    createReplicatedFastCountCollection(storageInterface(), operationContext());

    const UUID uuid = UUID::gen();
    absl::flat_hash_map<UUID, CollectionSizeCount> deltas;
    deltas[uuid] = CollectionSizeCount{0, 0};

    // Read before the document exists.
    readAndIncrementSizeCounts(operationContext(), deltas);

    EXPECT_EQ(deltas.size(), 1);
    EXPECT_EQ(deltas[uuid].size, 0);
    EXPECT_EQ(deltas[uuid].count, 0);

    insertSizeCountDocument(operationContext(), uuid, 0, 0);

    // Read after (0,0) document exists.
    readAndIncrementSizeCounts(operationContext(), deltas);

    EXPECT_EQ(deltas.size(), 1);
    EXPECT_EQ(deltas[uuid].size, 0);
    EXPECT_EQ(deltas[uuid].count, 0);
}

TEST_F(ReadAndIncrementSizeCountsTest, NegativeResult) {
    createReplicatedFastCountCollection(storageInterface(), operationContext());

    const UUID uuid = UUID::gen();
    insertSizeCountDocument(operationContext(), uuid, 200, 10);

    absl::flat_hash_map<UUID, CollectionSizeCount> deltas;
    deltas[uuid] = CollectionSizeCount{-400, -20};

    readAndIncrementSizeCounts(operationContext(), deltas);

    EXPECT_EQ(deltas.size(), 1);
    EXPECT_EQ(deltas[uuid].size, -200);
    EXPECT_EQ(deltas[uuid].count, -10);
}

/**
 * document UUIDs:  {uuid1, uuid2}
 * delta UUIDs:     {}
 * document UUIDs ∩ delta UUIDs = {}
 */
TEST_F(ReadAndIncrementSizeCountsTest, ReadEmptySet) {
    createReplicatedFastCountCollection(storageInterface(), operationContext());

    const UUID uuid1 = UUID::gen();
    insertSizeCountDocument(operationContext(), uuid1, 200, 10);

    const UUID uuid2 = UUID::gen();
    insertSizeCountDocument(operationContext(), uuid2, 100, 5);

    absl::flat_hash_map<UUID, CollectionSizeCount> deltas;

    readAndIncrementSizeCounts(operationContext(), deltas);

    EXPECT_TRUE(deltas.empty());
}

/**
 * document UUIDs:  {uuid1, uuid2}
 * delta UUIDs:     {uuid1, uuid2}
 * document UUIDs ∩ delta UUIDs = {uuid1, uuid2}
 */
TEST_F(ReadAndIncrementSizeCountsTest, ReadDocumentEqualSet) {
    createReplicatedFastCountCollection(storageInterface(), operationContext());

    const UUID uuid1 = UUID::gen();
    insertSizeCountDocument(operationContext(), uuid1, 200, 10);

    const UUID uuid2 = UUID::gen();
    insertSizeCountDocument(operationContext(), uuid2, 100, 5);

    absl::flat_hash_map<UUID, CollectionSizeCount> deltas;
    deltas[uuid1] = CollectionSizeCount{5, 1};
    deltas[uuid2] = CollectionSizeCount{50, 10};

    readAndIncrementSizeCounts(operationContext(), deltas);

    EXPECT_EQ(deltas.size(), 2);
    EXPECT_EQ(deltas[uuid1].size, 205);
    EXPECT_EQ(deltas[uuid1].count, 11);
    EXPECT_EQ(deltas[uuid2].size, 150);
    EXPECT_EQ(deltas[uuid2].count, 15);
}

/**
 * document UUIDs:  {uuid1, uuid2}
 * delta UUIDs:     {uuid1}
 * document UUIDs ∩ delta UUIDs = {uuid1}
 */
TEST_F(ReadAndIncrementSizeCountsTest, ReadDocumentSubset) {
    createReplicatedFastCountCollection(storageInterface(), operationContext());

    const UUID uuid1 = UUID::gen();
    insertSizeCountDocument(operationContext(), uuid1, 200, 10);

    const UUID uuid2 = UUID::gen();
    insertSizeCountDocument(operationContext(), uuid2, 100, 5);

    absl::flat_hash_map<UUID, CollectionSizeCount> deltas;
    deltas[uuid1] = CollectionSizeCount{5, 1};

    readAndIncrementSizeCounts(operationContext(), deltas);

    EXPECT_EQ(deltas.size(), 1);
    EXPECT_EQ(deltas[uuid1].size, 205);
    EXPECT_EQ(deltas[uuid1].count, 11);
}

/**
 * document UUIDs:  {uuid1}
 * delta UUIDs:     {uuid1, uuid2}
 * document UUIDs ∩ delta UUIDs = {uuid1}
 */
TEST_F(ReadAndIncrementSizeCountsTest, ReadDocumentSuperset) {
    createReplicatedFastCountCollection(storageInterface(), operationContext());

    const UUID uuid1 = UUID::gen();
    insertSizeCountDocument(operationContext(), uuid1, 200, 10);

    const UUID uuid2 = UUID::gen();
    absl::flat_hash_map<UUID, CollectionSizeCount> deltas;
    deltas[uuid1] = CollectionSizeCount{5, 1};
    deltas[uuid2] = CollectionSizeCount{50, 10};

    readAndIncrementSizeCounts(operationContext(), deltas);

    EXPECT_EQ(deltas.size(), 2);
    EXPECT_EQ(deltas[uuid1].size, 205);
    EXPECT_EQ(deltas[uuid1].count, 11);
    EXPECT_EQ(deltas[uuid2].size, 50);
    EXPECT_EQ(deltas[uuid2].count, 10);
}

/**
 * document UUIDs:  {uuid1, uuid2}
 * delta UUIDs:     {uuid3}
 * document UUIDs ∩ delta UUIDs = {}
 */
TEST_F(ReadAndIncrementSizeCountsTest, ReadDocumentsDisjointSet) {
    createReplicatedFastCountCollection(storageInterface(), operationContext());

    const UUID uuid1 = UUID::gen();
    insertSizeCountDocument(operationContext(), uuid1, 200, 10);

    const UUID uuid2 = UUID::gen();
    insertSizeCountDocument(operationContext(), uuid2, 100, 5);

    const UUID uuid3 = UUID::gen();
    absl::flat_hash_map<UUID, CollectionSizeCount> deltas;
    deltas[uuid3] = CollectionSizeCount{5, 1};

    readAndIncrementSizeCounts(operationContext(), deltas);

    EXPECT_EQ(deltas.size(), 1);
    EXPECT_EQ(deltas[uuid3].size, 5);
    EXPECT_EQ(deltas[uuid3].count, 1);
}
}  // namespace
}  // namespace mongo::replicated_fast_count
