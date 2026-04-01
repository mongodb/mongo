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

#include "mongo/db/replicated_fast_count/size_count_timestamp_store.h"

#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_init.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"

namespace mongo::replicated_fast_count {
namespace {

void write(OperationContext* opCtx, Timestamp timestamp, SizeCountTimestampStore& store) {
    WriteUnitOfWork wuow(opCtx);
    store.write(opCtx, timestamp);
    wuow.commit();
}

class SizeCountTimestampStoreTest : public CatalogTestFixture {};

TEST_F(SizeCountTimestampStoreTest, WriteMassertsWithoutWriteUnitOfWork) {
    ASSERT_OK(createReplicatedFastCountTimestampCollection(storageInterface(), operationContext()));
    SizeCountTimestampStore store;

    ASSERT_THROWS_CODE(store.write(operationContext(), Timestamp(10, 1)), DBException, 12280400);
}

TEST_F(SizeCountTimestampStoreTest, ReadReturnsNoneWhenDocumentDoesNotExist) {
    ASSERT_OK(createReplicatedFastCountTimestampCollection(storageInterface(), operationContext()));
    const SizeCountTimestampStore store;

    EXPECT_FALSE(store.read(operationContext()).has_value());
}

TEST_F(SizeCountTimestampStoreTest, ReadWriteRoundTripNewEntry) {
    ASSERT_OK(createReplicatedFastCountTimestampCollection(storageInterface(), operationContext()));
    SizeCountTimestampStore store;

    write(operationContext(), Timestamp(10, 1), store);

    const auto result = store.read(operationContext());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(Timestamp(10, 1), *result);
}

TEST_F(SizeCountTimestampStoreTest, WriteUpdatesExistingDocument) {
    ASSERT_OK(createReplicatedFastCountTimestampCollection(storageInterface(), operationContext()));
    SizeCountTimestampStore store;
    write(operationContext(), Timestamp(10, 1), store);

    write(operationContext(), Timestamp(20, 2), store);

    const auto result = store.read(operationContext());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(Timestamp(20, 2), *result);
}

TEST_F(SizeCountTimestampStoreTest, WriteWithSameTimestampIsIdempotent) {
    ASSERT_OK(createReplicatedFastCountTimestampCollection(storageInterface(), operationContext()));
    SizeCountTimestampStore store;

    write(operationContext(), Timestamp(10, 1), store);
    write(operationContext(), Timestamp(10, 1), store);

    const auto result = store.read(operationContext());
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(Timestamp(10, 1), *result);
}
}  // namespace
}  // namespace mongo::replicated_fast_count
