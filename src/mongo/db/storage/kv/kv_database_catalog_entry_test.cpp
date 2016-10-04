/**
 *    Copyright 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/storage/kv/kv_database_catalog_entry.h"

#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/storage/devnull/devnull_kv_engine.h"
#include "mongo/db/storage/kv/kv_storage_engine.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;

TEST(KVDatabaseCatalogEntryTest, CreateCollectionValidNamespace) {
    KVStorageEngine storageEngine(new DevNullKVEngine());
    storageEngine.finishInit();
    KVDatabaseCatalogEntry dbEntry("mydb", &storageEngine);
    OperationContextNoop ctx;
    ASSERT_OK(dbEntry.createCollection(&ctx, "mydb.mycoll", CollectionOptions(), true));
    std::list<std::string> collectionNamespaces;
    dbEntry.getCollectionNamespaces(&collectionNamespaces);
    ASSERT_FALSE(collectionNamespaces.empty());
}

TEST(KVDatabaseCatalogEntryTest, CreateCollectionEmptyNamespace) {
    KVStorageEngine storageEngine(new DevNullKVEngine());
    storageEngine.finishInit();
    KVDatabaseCatalogEntry dbEntry("mydb", &storageEngine);
    OperationContextNoop ctx;
    ASSERT_NOT_OK(dbEntry.createCollection(&ctx, "", CollectionOptions(), true));
    std::list<std::string> collectionNamespaces;
    dbEntry.getCollectionNamespaces(&collectionNamespaces);
    ASSERT_TRUE(collectionNamespaces.empty());
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
TEST(KVDatabaseCatalogEntryTest, CreateCollectionInvalidRecordStore) {
    KVStorageEngine storageEngine(new InvalidRecordStoreKVEngine());
    storageEngine.finishInit();
    KVDatabaseCatalogEntry dbEntry("fail", &storageEngine);
    OperationContextNoop ctx;
    ASSERT_NOT_OK(dbEntry.createCollection(&ctx, "fail.me", CollectionOptions(), true));
    std::list<std::string> collectionNamespaces;
    dbEntry.getCollectionNamespaces(&collectionNamespaces);
    ASSERT_TRUE(collectionNamespaces.empty());
}

}  // namespace
