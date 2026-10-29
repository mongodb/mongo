// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/lazy_record_store.h"

#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_engine_test_fixture.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

class LazyRecordStoreTest : public StorageEngineTest {};
using LazyRecordStoreDeathTest = LazyRecordStoreTest;

TEST_F(LazyRecordStoreTest, DeferredDoesNotCreateTableUntilAccess) {
    auto opCtx = makeOperationContext();
    auto ident = _storageEngine->generateNewInternalIdent();

    LazyRecordStore lrs(opCtx.get(), ident, LazyRecordStore::CreateMode::deferred);
    EXPECT_FALSE(lrs.tableExists());

    auto& rs = lrs.getOrCreateTable(opCtx.get());
    EXPECT_TRUE(lrs.tableExists());
    EXPECT_EQ(&lrs.getTableOrThrow(), &rs);
}

TEST_F(LazyRecordStoreTest, ImmediateCreatesTableRightAway) {
    auto opCtx = makeOperationContext();
    auto ident = _storageEngine->generateNewInternalIdent();

    LazyRecordStore lrs(opCtx.get(), ident, LazyRecordStore::CreateMode::immediate);
    EXPECT_TRUE(lrs.tableExists());
}

TEST_F(LazyRecordStoreTest, GetOrCreateTableIsIdempotent) {
    auto opCtx = makeOperationContext();
    auto ident = _storageEngine->generateNewInternalIdent();

    LazyRecordStore lrs(opCtx.get(), ident, LazyRecordStore::CreateMode::deferred);
    auto& rs1 = lrs.getOrCreateTable(opCtx.get());
    auto& rs2 = lrs.getOrCreateTable(opCtx.get());
    EXPECT_EQ(&rs1, &rs2);
}

TEST_F(LazyRecordStoreTest, DropResetsToUninitialized) {
    auto opCtx = makeOperationContext();
    auto ident = _storageEngine->generateNewInternalIdent();

    LazyRecordStore lrs(opCtx.get(), ident, LazyRecordStore::CreateMode::immediate);
    EXPECT_TRUE(lrs.tableExists());

    lrs.drop(opCtx.get(), StorageEngine::Immediate{});
    EXPECT_FALSE(lrs.tableExists());
}

TEST_F(LazyRecordStoreTest, DropOnDeferredIsNoOp) {
    auto opCtx = makeOperationContext();
    auto ident = _storageEngine->generateNewInternalIdent();

    LazyRecordStore lrs(opCtx.get(), ident, LazyRecordStore::CreateMode::deferred);
    lrs.drop(opCtx.get(), StorageEngine::Immediate{});
    EXPECT_FALSE(lrs.tableExists());
}

TEST_F(LazyRecordStoreTest, OpenExistingOpensExistingTable) {
    auto opCtx = makeOperationContext();
    auto ident = _storageEngine->generateNewInternalIdent();

    {
        LazyRecordStore lrs(opCtx.get(), ident, LazyRecordStore::CreateMode::immediate);
    }

    LazyRecordStore lrs(opCtx.get(), ident, LazyRecordStore::CreateMode::openExisting);
    EXPECT_TRUE(lrs.tableExists());
}

TEST_F(LazyRecordStoreTest, GetTableOrThrowSucceedsWhenTableExists) {
    auto opCtx = makeOperationContext();
    auto ident = _storageEngine->generateNewInternalIdent();

    LazyRecordStore lrs(opCtx.get(), ident, LazyRecordStore::CreateMode::immediate);
    ASSERT_NO_THROW(lrs.getTableOrThrow());
    EXPECT_EQ(&lrs.getTableOrThrow(), &lrs.getOrCreateTable(opCtx.get()));
}

TEST_F(LazyRecordStoreTest, GetTableOrThrowFailsWhenTableDoesNotExist) {
    auto opCtx = makeOperationContext();
    auto ident = _storageEngine->generateNewInternalIdent();

    LazyRecordStore lrs(opCtx.get(), ident, LazyRecordStore::CreateMode::deferred);
    ASSERT_THROWS_WITH_CHECK(lrs.getTableOrThrow(), DBException, [](const DBException& ex) {
        EXPECT_EQ(ex.code(), 12129700);
        assertionCount.tripwire.subtractAndFetch(1);
    });
}

TEST_F(LazyRecordStoreTest, GetOrCreateTableInsideWUOW) {
    auto opCtx = makeOperationContext();
    auto ident = _storageEngine->generateNewInternalIdent();

    LazyRecordStore lrs(opCtx.get(), ident, LazyRecordStore::CreateMode::deferred);

    // Table should exist inside the pending WUOW, but it should be cleared on rollback
    {
        WriteUnitOfWork wuow(opCtx.get());
        lrs.getOrCreateTable(opCtx.get());
        EXPECT_TRUE(lrs.tableExists());
    }
    EXPECT_FALSE(lrs.tableExists());

    // Table should continue to exist after the parent WUOW is committed
    {
        WriteUnitOfWork wuow(opCtx.get());
        lrs.getOrCreateTable(opCtx.get());
        EXPECT_TRUE(lrs.tableExists());
        wuow.commit();
    }
    EXPECT_TRUE(lrs.tableExists());
}

DEATH_TEST_F(LazyRecordStoreDeathTest, WriteUnitOfWorkOutlivesLazyRecordStore, "invariant") {
    auto opCtx = makeOperationContext();
    auto ident = _storageEngine->generateNewInternalIdent();
    WriteUnitOfWork wuow(opCtx.get());
    (void)LazyRecordStore(opCtx.get(), ident, LazyRecordStore::CreateMode::immediate);
    wuow.commit();
}

}  // namespace
}  // namespace mongo
