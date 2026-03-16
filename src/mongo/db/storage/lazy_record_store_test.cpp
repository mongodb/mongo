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

#include "mongo/db/storage/lazy_record_store.h"

#include "mongo/db/storage/storage_engine_test_fixture.h"

namespace mongo {
namespace {

class LazyRecordStoreTest : public StorageEngineTest {};

TEST_F(LazyRecordStoreTest, DeferredDoesNotCreateTableUntilAccess) {
    auto opCtx = makeOperationContext();
    auto ident = _storageEngine->generateNewInternalIdent();

    LazyRecordStore lrs(opCtx.get(), ident, LazyRecordStore::CreateMode::deferred);
    ASSERT_EQ(lrs.getTableIfExists(), nullptr);

    auto& rs = lrs.getOrCreateTable(opCtx.get());
    ASSERT_NE(lrs.getTableIfExists(), nullptr);
    ASSERT_EQ(lrs.getTableIfExists(), &rs);
}

TEST_F(LazyRecordStoreTest, ImmediateCreatesTableRightAway) {
    auto opCtx = makeOperationContext();
    auto ident = _storageEngine->generateNewInternalIdent();

    LazyRecordStore lrs(opCtx.get(), ident, LazyRecordStore::CreateMode::immediate);
    ASSERT_NE(lrs.getTableIfExists(), nullptr);
}

TEST_F(LazyRecordStoreTest, GetOrCreateTableIsIdempotent) {
    auto opCtx = makeOperationContext();
    auto ident = _storageEngine->generateNewInternalIdent();

    LazyRecordStore lrs(opCtx.get(), ident, LazyRecordStore::CreateMode::deferred);
    auto& rs1 = lrs.getOrCreateTable(opCtx.get());
    auto& rs2 = lrs.getOrCreateTable(opCtx.get());
    ASSERT_EQ(&rs1, &rs2);
}

TEST_F(LazyRecordStoreTest, DropResetsToUninitialized) {
    auto opCtx = makeOperationContext();
    auto ident = _storageEngine->generateNewInternalIdent();

    LazyRecordStore lrs(opCtx.get(), ident, LazyRecordStore::CreateMode::immediate);
    ASSERT_NE(lrs.getTableIfExists(), nullptr);

    lrs.drop();
    ASSERT_EQ(lrs.getTableIfExists(), nullptr);
}

TEST_F(LazyRecordStoreTest, DropOnDeferredIsNoOp) {
    auto opCtx = makeOperationContext();
    auto ident = _storageEngine->generateNewInternalIdent();

    LazyRecordStore lrs(opCtx.get(), ident, LazyRecordStore::CreateMode::deferred);
    lrs.drop();
    ASSERT_EQ(lrs.getTableIfExists(), nullptr);
}

TEST_F(LazyRecordStoreTest, OpenExistingOpensExistingTable) {
    auto opCtx = makeOperationContext();
    auto ident = _storageEngine->generateNewInternalIdent();

    {
        LazyRecordStore lrs(opCtx.get(), ident, LazyRecordStore::CreateMode::immediate);
        lrs.keepTemporaryTable(opCtx.get());
    }

    LazyRecordStore lrs(opCtx.get(), ident, LazyRecordStore::CreateMode::openExisting);
    ASSERT_NE(lrs.getTableIfExists(), nullptr);
}

}  // namespace
}  // namespace mongo
