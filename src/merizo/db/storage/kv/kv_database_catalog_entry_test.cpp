/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#include "merizo/platform/basic.h"

#include "merizo/db/storage/kv/kv_database_catalog_entry_mock.h"

#include "merizo/base/string_data.h"
#include "merizo/db/catalog/collection_options.h"
#include "merizo/db/operation_context_noop.h"
#include "merizo/db/service_context_test_fixture.h"
#include "merizo/db/storage/devnull/devnull_kv_engine.h"
#include "merizo/db/storage/kv/kv_prefix.h"
#include "merizo/db/storage/kv/kv_storage_engine.h"
#include "merizo/stdx/memory.h"
#include "merizo/unittest/death_test.h"
#include "merizo/unittest/unittest.h"

namespace merizo {
namespace {

TEST_F(ServiceContextTest, CreateCollectionValidNamespace) {
    KVStorageEngine storageEngine(
        new DevNullKVEngine(), KVStorageEngineOptions{}, kvDatabaseCatalogEntryMockFactory);
    storageEngine.finishInit();
    KVDatabaseCatalogEntryMock dbEntry("mydb", &storageEngine);
    OperationContextNoop ctx;
    ASSERT_OK(
        dbEntry.createCollection(&ctx, NamespaceString("mydb.mycoll"), CollectionOptions(), true));
    std::list<std::string> collectionNamespaces;
    dbEntry.getCollectionNamespaces(&collectionNamespaces);
    ASSERT_FALSE(collectionNamespaces.empty());
}

/**
 * Derived class of devnull KV engine where createRecordStore is overridden to fail
 * on an empty namespace (provided by the test).
 */
class InvalidRecordStoreKVEngine : public DevNullKVEngine {
public:
    virtual Status createRecordStore(OperationContext* opCtx,
                                     StringData ns,
                                     StringData ident,
                                     const CollectionOptions& options) {
        if (ns == "fail.me") {
            return Status(ErrorCodes::BadValue, "failed to create record store");
        }
        return DevNullKVEngine::createRecordStore(opCtx, ns, ident, options);
    }
};

// After createCollection fails, collection namespaces should remain empty.
TEST_F(ServiceContextTest, CreateCollectionInvalidRecordStore) {
    KVStorageEngine storageEngine(new InvalidRecordStoreKVEngine(),
                                  KVStorageEngineOptions{},
                                  kvDatabaseCatalogEntryMockFactory);
    storageEngine.finishInit();
    KVDatabaseCatalogEntryMock dbEntry("fail", &storageEngine);
    OperationContextNoop ctx;
    ASSERT_NOT_OK(
        dbEntry.createCollection(&ctx, NamespaceString("fail.me"), CollectionOptions(), true));
    std::list<std::string> collectionNamespaces;
    dbEntry.getCollectionNamespaces(&collectionNamespaces);
    ASSERT_TRUE(collectionNamespaces.empty());
}

DEATH_TEST_F(ServiceContextTest, CreateCollectionEmptyNamespace, "Invariant failure") {
    KVStorageEngine storageEngine(
        new DevNullKVEngine(), KVStorageEngineOptions{}, kvDatabaseCatalogEntryMockFactory);
    storageEngine.finishInit();
    KVDatabaseCatalogEntryMock dbEntry("mydb", &storageEngine);
    OperationContextNoop ctx;
    Status status = dbEntry.createCollection(&ctx, NamespaceString(""), CollectionOptions(), true);
}


}  // namespace
}  // namespace merizo
