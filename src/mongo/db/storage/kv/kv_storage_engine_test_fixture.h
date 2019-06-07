/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#pragma once

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_mock.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/storage/kv/kv_catalog.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/kv/kv_storage_engine.h"
#include "mongo/db/storage/storage_repair_observer.h"

namespace mongo {

class KVStorageEngineTest : public ServiceContextMongoDTest {
public:
    KVStorageEngineTest(RepairAction repair)
        : ServiceContextMongoDTest("ephemeralForTest", repair),
          _storageEngine(checked_cast<KVStorageEngine*>(getServiceContext()->getStorageEngine())) {}

    KVStorageEngineTest() : KVStorageEngineTest(RepairAction::kNoRepair) {}

    /**
     * Create a collection in the catalog and in the KVEngine. Return the storage engine's `ident`.
     */
    StatusWith<std::string> createCollection(OperationContext* opCtx, NamespaceString ns) {
        AutoGetDb db(opCtx, ns.db(), LockMode::MODE_X);
        CollectionOptions options;
        options.uuid = UUID::gen();
        auto catalogEntry = unittest::assertGet(
            _storageEngine->getCatalog()->createCollection(opCtx, ns, options, true));
        CollectionCatalog::get(opCtx).registerCollection(
            options.uuid.get(), std::move(catalogEntry), std::make_unique<CollectionMock>(ns));

        return _storageEngine->getCatalog()->getCollectionIdent(ns);
    }

    std::unique_ptr<TemporaryRecordStore> makeTemporary(OperationContext* opCtx) {
        return _storageEngine->makeTemporaryRecordStore(opCtx);
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
        std::string indexIdent = _storageEngine->getCatalog()->getIndexIdent(opCtx, nss, indexName);
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

    bool collectionExists(OperationContext* opCtx, const NamespaceString& nss) {
        auto allCollections = _storageEngine->getCatalog()->getAllCollections();
        return std::count(allCollections.begin(), allCollections.end(), nss);
    }
    bool identExists(OperationContext* opCtx, const std::string& ident) {
        auto idents = getAllKVEngineIdents(opCtx);
        return std::find(idents.begin(), idents.end(), ident) != idents.end();
    }

    /**
     * Create an index with a key of `{<key>: 1}` and a `name` of <key>.
     */
    Status createIndex(OperationContext* opCtx,
                       NamespaceString collNs,
                       std::string key,
                       bool isBackgroundSecondaryBuild) {
        auto ret = startIndexBuild(opCtx, collNs, key, isBackgroundSecondaryBuild);
        if (!ret.isOK()) {
            return ret;
        }

        indexBuildSuccess(opCtx, collNs, key);
        return Status::OK();
    }

    Status startIndexBuild(OperationContext* opCtx,
                           NamespaceString collNs,
                           std::string key,
                           bool isBackgroundSecondaryBuild) {
        BSONObjBuilder builder;
        {
            BSONObjBuilder keyObj;
            builder.append("key", keyObj.append(key, 1).done());
        }
        BSONObj spec = builder.append("name", key).append("ns", collNs.ns()).append("v", 2).done();

        auto collection = std::make_unique<CollectionMock>(collNs);
        auto descriptor = std::make_unique<IndexDescriptor>(
            collection.get(), IndexNames::findPluginName(spec), spec);

        CollectionCatalogEntry* cce =
            CollectionCatalog::get(opCtx).lookupCollectionCatalogEntryByNamespace(collNs);
        const auto protocol = IndexBuildProtocol::kTwoPhase;
        auto ret = cce->prepareForIndexBuild(
            opCtx, descriptor.get(), protocol, isBackgroundSecondaryBuild);
        return ret;
    }

    void indexBuildScan(OperationContext* opCtx,
                        NamespaceString collNs,
                        std::string key,
                        std::string sideWritesIdent,
                        std::string constraintViolationsIdent) {
        CollectionCatalogEntry* cce =
            CollectionCatalog::get(opCtx).lookupCollectionCatalogEntryByNamespace(collNs);
        cce->setIndexBuildScanning(opCtx, key, sideWritesIdent, constraintViolationsIdent);
    }

    void indexBuildDrain(OperationContext* opCtx, NamespaceString collNs, std::string key) {
        CollectionCatalogEntry* cce =
            CollectionCatalog::get(opCtx).lookupCollectionCatalogEntryByNamespace(collNs);
        cce->setIndexBuildDraining(opCtx, key);
    }

    void indexBuildSuccess(OperationContext* opCtx, NamespaceString collNs, std::string key) {
        CollectionCatalogEntry* cce =
            CollectionCatalog::get(opCtx).lookupCollectionCatalogEntryByNamespace(collNs);
        cce->indexBuildSuccess(opCtx, key);
    }

    Status removeEntry(OperationContext* opCtx, StringData ns, KVCatalog* catalog) {
        return catalog->_removeEntry(opCtx, NamespaceString(ns));
    }

    KVStorageEngine* _storageEngine;
};

class KVStorageEngineRepairTest : public KVStorageEngineTest {
public:
    KVStorageEngineRepairTest() : KVStorageEngineTest(RepairAction::kRepair) {}

    void tearDown() {
        auto repairObserver = StorageRepairObserver::get(getGlobalServiceContext());
        ASSERT(repairObserver->isDone());

        unittest::log() << "Modifications: ";
        for (const auto& mod : repairObserver->getModifications()) {
            unittest::log() << "  " << mod;
        }
    }
};
}
