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

#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_init.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_test_helpers.h"
#include "mongo/db/replicated_fast_count/size_count_store.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"

namespace mongo::replicated_fast_count {
namespace {

class ReadAndIncrementSizeCountsTest : public CatalogTestFixture {};

TEST_F(ReadAndIncrementSizeCountsTest, IncrementZeros) {
    ASSERT_OK(createReplicatedFastCountCollection(storageInterface(), operationContext()));
    SizeCountStore store;

    const UUID uuid = UUID::gen();
    absl::flat_hash_map<UUID, CollectionSizeCount> deltas;
    deltas[uuid] = CollectionSizeCount{0, 0};

    // Read before the document exists.
    readAndIncrementSizeCounts(operationContext(), deltas);

    EXPECT_EQ(deltas.size(), 1);
    EXPECT_EQ(deltas[uuid].size, 0);
    EXPECT_EQ(deltas[uuid].count, 0);

    test_helpers::insertSizeCountEntry(
        operationContext(), store, uuid, {.timestamp = Timestamp(1, 1), .size = 0, .count = 0});

    // Read after (0,0) document exists.
    readAndIncrementSizeCounts(operationContext(), deltas);

    EXPECT_EQ(deltas.size(), 1);
    EXPECT_EQ(deltas[uuid].size, 0);
    EXPECT_EQ(deltas[uuid].count, 0);
}

TEST_F(ReadAndIncrementSizeCountsTest, NegativeResult) {
    ASSERT_OK(createReplicatedFastCountCollection(storageInterface(), operationContext()));
    SizeCountStore store;

    const UUID uuid = UUID::gen();
    test_helpers::insertSizeCountEntry(
        operationContext(), store, uuid, {.timestamp = Timestamp(1, 1), .size = 200, .count = 10});

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
    ASSERT_OK(createReplicatedFastCountCollection(storageInterface(), operationContext()));
    SizeCountStore store;

    const UUID uuid1 = UUID::gen();
    test_helpers::insertSizeCountEntry(
        operationContext(), store, uuid1, {.timestamp = Timestamp(1, 1), .size = 200, .count = 10});

    const UUID uuid2 = UUID::gen();
    test_helpers::insertSizeCountEntry(
        operationContext(), store, uuid2, {.timestamp = Timestamp(1, 1), .size = 100, .count = 5});

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
    ASSERT_OK(createReplicatedFastCountCollection(storageInterface(), operationContext()));
    SizeCountStore store;

    const UUID uuid1 = UUID::gen();
    test_helpers::insertSizeCountEntry(
        operationContext(), store, uuid1, {.timestamp = Timestamp(1, 1), .size = 200, .count = 10});

    const UUID uuid2 = UUID::gen();
    test_helpers::insertSizeCountEntry(
        operationContext(), store, uuid2, {.timestamp = Timestamp(1, 1), .size = 100, .count = 5});

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
    ASSERT_OK(createReplicatedFastCountCollection(storageInterface(), operationContext()));
    SizeCountStore store;

    const UUID uuid1 = UUID::gen();
    test_helpers::insertSizeCountEntry(
        operationContext(), store, uuid1, {.timestamp = Timestamp(1, 1), .size = 200, .count = 10});

    const UUID uuid2 = UUID::gen();
    test_helpers::insertSizeCountEntry(
        operationContext(), store, uuid2, {.timestamp = Timestamp(1, 1), .size = 100, .count = 5});

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
    ASSERT_OK(createReplicatedFastCountCollection(storageInterface(), operationContext()));
    SizeCountStore store;

    const UUID uuid1 = UUID::gen();
    test_helpers::insertSizeCountEntry(
        operationContext(), store, uuid1, {.timestamp = Timestamp(1, 1), .size = 200, .count = 10});

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
    ASSERT_OK(createReplicatedFastCountCollection(storageInterface(), operationContext()));
    SizeCountStore store;

    const UUID uuid1 = UUID::gen();
    test_helpers::insertSizeCountEntry(
        operationContext(), store, uuid1, {.timestamp = Timestamp(1, 1), .size = 200, .count = 10});

    const UUID uuid2 = UUID::gen();
    test_helpers::insertSizeCountEntry(
        operationContext(), store, uuid2, {.timestamp = Timestamp(1, 1), .size = 100, .count = 5});

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
