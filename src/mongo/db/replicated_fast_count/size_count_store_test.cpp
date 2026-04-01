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

#include "mongo/db/replicated_fast_count/size_count_store.h"

#include "mongo/db/dbhelpers.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_delta_utils.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_init.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_test_helpers.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_util.h"

namespace mongo::replicated_fast_count {
namespace {

class SizeCountStoreTest : public CatalogTestFixture {};

TEST_F(SizeCountStoreTest, ReadReturnsNoneWhenDocumentDoesNotExist) {
    ASSERT_OK(createReplicatedFastCountCollection(storageInterface(), operationContext()));
    const SizeCountStore store;

    EXPECT_FALSE(store.read(operationContext(), UUID::gen()).has_value());
}

TEST_F(SizeCountStoreTest, ReadWriteRoundTripNewEntry) {
    ASSERT_OK(createReplicatedFastCountCollection(storageInterface(), operationContext()));
    SizeCountStore store;
    const UUID uuid = UUID::gen();
    const SizeCountStore::Entry entry{.timestamp = Timestamp(10, 1), .size = 42, .count = 7};

    test_helpers::insertSizeCountEntry(operationContext(), store, uuid, entry);

    const auto result = store.read(operationContext(), uuid);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(entry, *result);
}

TEST_F(SizeCountStoreTest, WriteUpdateExistingEntry) {
    ASSERT_OK(createReplicatedFastCountCollection(storageInterface(), operationContext()));
    SizeCountStore store;
    const UUID uuid = UUID::gen();
    const SizeCountStore::Entry initialEntry{.timestamp = Timestamp(10, 1), .size = 42, .count = 7};
    test_helpers::insertSizeCountEntry(operationContext(), store, uuid, initialEntry);

    // Update initial entry.
    const SizeCountStore::Entry updatedEntry{.timestamp = initialEntry.timestamp + 1,
                                             .size = initialEntry.size - 2,
                                             .count = initialEntry.count - 1};
    ASSERT_NE(initialEntry, updatedEntry);

    test_helpers::insertSizeCountEntry(operationContext(), store, uuid, updatedEntry);
    const auto result = store.read(operationContext(), uuid);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(updatedEntry, *result);
}

TEST_F(SizeCountStoreTest, ReadWriteTwoEntries) {
    ASSERT_OK(createReplicatedFastCountCollection(storageInterface(), operationContext()));
    SizeCountStore store;
    const UUID uuid0 = UUID::gen();
    const UUID uuid1 = UUID::gen();
    const SizeCountStore::Entry entry0{.timestamp = Timestamp(10, 1), .size = 42, .count = 7};
    const SizeCountStore::Entry entry1{.timestamp = Timestamp(20, 2), .size = 100, .count = 3};

    test_helpers::insertSizeCountEntry(operationContext(), store, uuid0, entry0);
    test_helpers::insertSizeCountEntry(operationContext(), store, uuid1, entry1);

    const auto result0 = store.read(operationContext(), uuid0);
    ASSERT_TRUE(result0.has_value());
    EXPECT_EQ(entry0, *result0);

    const auto result1 = store.read(operationContext(), uuid1);
    ASSERT_TRUE(result1.has_value());
    EXPECT_EQ(entry1, *result1);
}

TEST_F(SizeCountStoreTest, WriterUpdateToOneOfTwoEntries) {
    ASSERT_OK(createReplicatedFastCountCollection(storageInterface(), operationContext()));
    SizeCountStore store;
    const UUID uuid0 = UUID::gen();
    const UUID uuid1 = UUID::gen();
    const SizeCountStore::Entry entry0{.timestamp = Timestamp(10, 1), .size = 42, .count = 7};
    const SizeCountStore::Entry entry1{.timestamp = Timestamp(20, 2), .size = 100, .count = 3};

    test_helpers::insertSizeCountEntry(operationContext(), store, uuid0, entry0);
    test_helpers::insertSizeCountEntry(operationContext(), store, uuid1, entry1);

    const SizeCountStore::Entry updatedEntry0{
        .timestamp = entry0.timestamp + 1, .size = entry0.size + 10, .count = entry0.count + 2};
    test_helpers::insertSizeCountEntry(operationContext(), store, uuid0, updatedEntry0);

    const auto result0 = store.read(operationContext(), uuid0);
    ASSERT_TRUE(result0.has_value());
    EXPECT_EQ(updatedEntry0, *result0);

    const auto result1 = store.read(operationContext(), uuid1);
    ASSERT_TRUE(result1.has_value());
    EXPECT_EQ(entry1, *result1);
}

}  // namespace
}  // namespace mongo::replicated_fast_count
