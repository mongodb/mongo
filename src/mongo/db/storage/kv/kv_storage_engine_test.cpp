/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_names.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/repair_database.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_engine.h"
#include "mongo/db/storage/kv/kv_database_catalog_entry.h"
#include "mongo/db/storage/kv/kv_database_catalog_entry_mock.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/kv/kv_storage_engine.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

class KVStorageEngineTest : public unittest::Test {
public:
    KVStorageEngineTest()
        : _storageEngine(stdx::make_unique<KVStorageEngine>(new EphemeralForTestEngine(),
                                                            KVStorageEngineOptions())) {}

    void setUp() final {
        _serviceContext.setUp();
    }

    void tearDown() final {
        _storageEngine->cleanShutdown();
        _serviceContext.tearDown();
    }

    /**
     * Create a collection in the catalog and in the KVEngine. Return the storage engine's `ident`.
     */
    StatusWith<std::string> createCollection(OperationContext* opCtx, NamespaceString ns) {
        AutoGetDb db(opCtx, ns.db(), LockMode::MODE_X);
        DatabaseCatalogEntry* dbce = _storageEngine->getDatabaseCatalogEntry(opCtx, ns.db());
        auto ret = dbce->createCollection(opCtx, ns.ns(), CollectionOptions(), false);
        if (!ret.isOK()) {
            return ret;
        }

        return _storageEngine->getCatalog()->getCollectionIdent(ns.ns());
    }


    /**
     * Create a collection table in the KVEngine not reflected in the KVCatalog.
     */
    Status createCollTable(OperationContext* opCtx, NamespaceString collName) {
        const std::string identName = "collection-" + collName.ns();
        return _storageEngine->getEngine()->createGroupedRecordStore(
            opCtx, collName.ns(), identName, CollectionOptions(), KVPrefix::kNotPrefixed);
    }

    Status dropIndexTable(OperationContext* opCtx, NamespaceString nss, std::string indexName) {
        std::string indexIdent =
            _storageEngine->getCatalog()->getIndexIdent(opCtx, nss.ns(), indexName);
        return dropIdent(opCtx, indexIdent);
    }

    Status dropIdent(OperationContext* opCtx, StringData ident) {
        return _storageEngine->getEngine()->dropIdent(opCtx, ident);
    }

    StatusWith<std::vector<StorageEngine::CollectionIndexNamePair>> reconcile(
        OperationContext* opCtx) {
        return _storageEngine->reconcileCatalogAndIdents(opCtx);
    }

    std::vector<std::string> getAllKVEngineIdents(OperationContext* opCtx) {
        return _storageEngine->getEngine()->getAllIdents(opCtx);
    }

    /**
     * Create an index with a key of `{<key>: 1}` and a `name` of <key>.
     */
    Status createIndex(OperationContext* opCtx, NamespaceString collNs, std::string key) {
        Collection* coll = nullptr;
        BSONObjBuilder builder;
        {
            BSONObjBuilder keyObj;
            builder.append("key", keyObj.append(key, 1).done());
        }
        BSONObj spec = builder.append("name", key).append("ns", collNs.ns()).append("v", 2).done();

        auto descriptor =
            stdx::make_unique<IndexDescriptor>(coll, IndexNames::findPluginName(spec), spec);

        DatabaseCatalogEntry* dbce = _storageEngine->getDatabaseCatalogEntry(opCtx, collNs.db());
        CollectionCatalogEntry* cce = dbce->getCollectionCatalogEntry(collNs.ns());
        auto ret = cce->prepareForIndexBuild(opCtx, descriptor.get());
        if (!ret.isOK()) {
            return ret;
        }

        cce->indexBuildSuccess(opCtx, key);
        return Status::OK();
    }

    ServiceContextMongoDTest _serviceContext;
    std::unique_ptr<KVStorageEngine> _storageEngine;
};

TEST_F(KVStorageEngineTest, ReconcileIdentsTest) {
    auto opCtx = cc().makeOperationContext();

    // Add a collection, `db.coll1` to both the KVCatalog and KVEngine. The returned value is the
    // `ident` name given to the collection.
    auto swIdentName = createCollection(opCtx.get(), NamespaceString("db.coll1"));
    ASSERT_OK(swIdentName);
    // Create a table in the KVEngine not reflected in the KVCatalog. This should be dropped when
    // reconciling.
    ASSERT_OK(createCollTable(opCtx.get(), NamespaceString("db.coll2")));
    ASSERT_OK(reconcile(opCtx.get()).getStatus());
    auto identsVec = getAllKVEngineIdents(opCtx.get());
    auto idents = std::set<std::string>(identsVec.begin(), identsVec.end());
    // There are two idents. `_mdb_catalog` and the ident for `db.coll1`.
    ASSERT_EQUALS(static_cast<const unsigned long>(2), idents.size());
    ASSERT_TRUE(idents.find(swIdentName.getValue()) != idents.end());
    ASSERT_TRUE(idents.find("_mdb_catalog") != idents.end());

    // Create a catalog entry for the `_id` index. Drop the created the table.
    ASSERT_OK(createIndex(opCtx.get(), NamespaceString("db.coll1"), "_id"));
    ASSERT_OK(dropIndexTable(opCtx.get(), NamespaceString("db.coll1"), "_id"));
    // The reconcile response should include this index as needing to be rebuilt.
    auto reconcileStatus = reconcile(opCtx.get());
    ASSERT_OK(reconcileStatus.getStatus());
    ASSERT_EQUALS(static_cast<const unsigned long>(1), reconcileStatus.getValue().size());
    StorageEngine::CollectionIndexNamePair& toRebuild = reconcileStatus.getValue()[0];
    ASSERT_EQUALS("db.coll1", toRebuild.first);
    ASSERT_EQUALS("_id", toRebuild.second);

    // Now drop the `db.coll1` table, while leaving the KVCatalog entry.
    ASSERT_OK(dropIdent(opCtx.get(), swIdentName.getValue()));
    ASSERT_EQUALS(static_cast<const unsigned long>(1), getAllKVEngineIdents(opCtx.get()).size());

    // Reconciling this should result in an error.
    reconcileStatus = reconcile(opCtx.get());
    ASSERT_NOT_OK(reconcileStatus.getStatus());
    ASSERT_EQUALS(ErrorCodes::UnrecoverableRollbackError, reconcileStatus.getStatus());
}

TEST_F(KVStorageEngineTest, RecreateIndexes) {
    repl::setGlobalReplicationCoordinator(
        new repl::ReplicationCoordinatorMock(getGlobalServiceContext(), repl::ReplSettings()));

    auto opCtx = cc().makeOperationContext();

    // Create two indexes for `db.coll1` in the catalog named `foo` and `bar`. Verify the indexes
    // appear as idents in the KVEngine.
    ASSERT_OK(createCollection(opCtx.get(), NamespaceString("db.coll1")).getStatus());
    ASSERT_OK(createIndex(opCtx.get(), NamespaceString("db.coll1"), "foo"));
    ASSERT_OK(createIndex(opCtx.get(), NamespaceString("db.coll1"), "bar"));
    auto kvIdents = getAllKVEngineIdents(opCtx.get());
    ASSERT_EQUALS(2, std::count_if(kvIdents.begin(), kvIdents.end(), [](const std::string& str) {
                      return str.find("index-") == 0;
                  }));

    // Use the `getIndexNameObjs` to find the `foo` index in the IndexCatalog.
    DatabaseCatalogEntry* dbce = _storageEngine->getDatabaseCatalogEntry(opCtx.get(), "db");
    CollectionCatalogEntry* cce = dbce->getCollectionCatalogEntry("db.coll1");
    auto swIndexNameObjs = getIndexNameObjs(
        opCtx.get(), dbce, cce, [](const std::string& indexName) { return indexName == "foo"; });
    ASSERT_OK(swIndexNameObjs.getStatus());
    auto& indexNameObjs = swIndexNameObjs.getValue();
    // There's one index that matched the name `foo`.
    ASSERT_EQUALS(static_cast<const unsigned long>(1), indexNameObjs.first.size());
    // Assert the parallel vectors have matching sizes.
    ASSERT_EQUALS(static_cast<const unsigned long>(1), indexNameObjs.second.size());
    // The index that matched should be named `foo`.
    ASSERT_EQUALS("foo", indexNameObjs.first[0]);
    ASSERT_EQUALS("db.coll1"_sd, indexNameObjs.second[0].getStringField("ns"));
    ASSERT_EQUALS("foo"_sd, indexNameObjs.second[0].getStringField("name"));
    ASSERT_EQUALS(2, indexNameObjs.second[0].getIntField("v"));
    ASSERT_EQUALS(1, indexNameObjs.second[0].getObjectField("key").getIntField("foo"));

    // Drop the `foo` index table. Count one remaining index ident according to the KVEngine.
    ASSERT_OK(dropIndexTable(opCtx.get(), NamespaceString("db.coll1"), "foo"));
    kvIdents = getAllKVEngineIdents(opCtx.get());
    ASSERT_EQUALS(1, std::count_if(kvIdents.begin(), kvIdents.end(), [](const std::string& str) {
                      return str.find("index-") == 0;
                  }));

    AutoGetCollection coll(opCtx.get(), NamespaceString("db.coll1"), LockMode::MODE_X);
    // Find the `foo` index in the catalog. Rebuild it. Count two indexes in the KVEngine.
    ASSERT_OK(rebuildIndexesOnCollection(opCtx.get(), dbce, cce, indexNameObjs));
    ASSERT_TRUE(cce->isIndexReady(opCtx.get(), "foo"));
    kvIdents = getAllKVEngineIdents(opCtx.get());
    ASSERT_EQUALS(2, std::count_if(kvIdents.begin(), kvIdents.end(), [](const std::string& str) {
                      return str.find("index-") == 0;
                  }));
}
}  // namespace mongo
