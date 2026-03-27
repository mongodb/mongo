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
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_util.h"
#include "mongo/db/storage/write_unit_of_work.h"

namespace mongo::replicated_fast_count {
namespace {

void createReplicatedFastCountCollection(repl::StorageInterface* storageInterface,
                                         OperationContext* opCtx) {
    ASSERT_OK(storageInterface->createCollection(
        opCtx,
        NamespaceString::makeGlobalConfigCollection(NamespaceString::kReplicatedFastCountStore),
        CollectionOptions{.clusteredIndex = clustered_util::makeDefaultClusteredIdIndex()}));
}

void insertSizeCountDocument(OperationContext* opCtx, UUID uuid, SizeCountStore::Entry entry) {
    AutoGetCollection fastCountColl(
        opCtx,
        NamespaceString::makeGlobalConfigCollection(NamespaceString::kReplicatedFastCountStore),
        LockMode::MODE_IX);
    ASSERT(fastCountColl);

    WriteUnitOfWork wuow{opCtx, WriteUnitOfWork::kGroupForPossiblyRetryableOperations};
    ASSERT_OK(
        Helpers::insert(opCtx,
                        *fastCountColl,
                        BSON("_id" << uuid << kValidAsOfKey << entry.timestamp << kMetadataKey
                                   << BSON(kCountKey << entry.count << kSizeKey << entry.size))));
    wuow.commit();
}

class SizeCountStoreTest : public CatalogTestFixture {};

TEST_F(SizeCountStoreTest, ReadReturnsNoneWhenDocumentDoesNotExist) {
    createReplicatedFastCountCollection(storageInterface(), operationContext());
    const SizeCountStore store;

    EXPECT_FALSE(store.read(operationContext(), UUID::gen()).has_value());
}

TEST_F(SizeCountStoreTest, ReadReturnsEntryWhenDocumentExists) {
    createReplicatedFastCountCollection(storageInterface(), operationContext());
    const SizeCountStore store;
    const UUID uuid = UUID::gen();

    insertSizeCountDocument(
        operationContext(), uuid, {.timestamp = Timestamp(10, 1), .size = 42, .count = 7});

    const auto result = store.read(operationContext(), uuid);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->timestamp, Timestamp(10, 1));
    EXPECT_EQ(result->size, 42);
    EXPECT_EQ(result->count, 7);
}
}  // namespace
}  // namespace mongo::replicated_fast_count
