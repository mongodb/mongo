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

#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/catalog_repair.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/collection_impl.h"
#include "mongo/db/local_catalog/durable_catalog.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/mdb_catalog.h"
#include "mongo/db/storage/spill_table.h"
#include "mongo/db/storage/storage_engine_impl.h"
#include "mongo/db/storage/storage_repair_observer.h"
#include "mongo/logv2/log.h"

#include <boost/iterator/transform_iterator.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {

class StorageEngineTest : public ServiceContextMongoDTest {
public:
    explicit StorageEngineTest(Options options = {})
        : ServiceContextMongoDTest(std::move(options)),
          _storageEngine(getServiceContext()->getStorageEngine()) {}

    MDBCatalog::EntryIdentifier createCollection(OperationContext* opCtx,
                                                 NamespaceString ns,
                                                 CollectionOptions options = {}) {
        Lock::GlobalWrite lk(opCtx);
        AutoGetDb db(opCtx, ns.dbName(), LockMode::MODE_X);
        if (!options.uuid) {
            options.uuid = UUID::gen();
        }
        RecordId catalogId;
        std::unique_ptr<RecordStore> rs;
        auto mdbCatalog = _storageEngine->getMDBCatalog();
        {
            WriteUnitOfWork wuow(opCtx);
            const auto ident = _storageEngine->generateNewCollectionIdent(ns.dbName());
            catalogId = mdbCatalog->reserveCatalogId(opCtx);
            rs = unittest::assertGet(durable_catalog::createCollection(
                opCtx, catalogId, ns, ident, options, mdbCatalog));
            wuow.commit();
        }
        std::shared_ptr<Collection> coll = std::make_shared<CollectionImpl>(
            opCtx,
            ns,
            catalogId,
            durable_catalog::getParsedCatalogEntry(opCtx, catalogId, mdbCatalog)->metadata,
            std::move(rs));
        coll->init(opCtx);

        CollectionCatalog::write(opCtx, [&](CollectionCatalog& catalog) {
            catalog.registerCollection(opCtx, std::move(coll), /*ts=*/boost::none);
        });

        return mdbCatalog->getEntry(catalogId);
    }

    MDBCatalog::EntryIdentifier createTempCollection(OperationContext* opCtx, NamespaceString ns) {
        CollectionOptions options;
        options.temp = true;
        return createCollection(opCtx, ns, options);
    }

    std::unique_ptr<SpillTable> makeSpillTable(OperationContext* opCtx,
                                               KeyFormat keyFormat,
                                               int64_t thresholdBytes) {
        return _storageEngine->makeSpillTable(opCtx, keyFormat, thresholdBytes);
    }

    std::unique_ptr<TemporaryRecordStore> makeTemporary(OperationContext* opCtx) {
        return _storageEngine->makeTemporaryRecordStore(
            opCtx, _storageEngine->generateNewInternalIdent(), KeyFormat::Long);
    }

    std::unique_ptr<TemporaryRecordStore> makeTemporaryClustered(OperationContext* opCtx) {
        return _storageEngine->makeTemporaryRecordStore(
            opCtx, _storageEngine->generateNewInternalIdent(), KeyFormat::String);
    }

    /**
     * Create a collection table in the KVEngine not reflected in the MDBCatalog / durable_catalog.
     */
    Status createCollTable(OperationContext* opCtx, NamespaceString collName) {
        const std::string identName = _storageEngine->generateNewCollectionIdent(collName.dbName());
        auto& provider = rss::ReplicatedStorageService::get(opCtx).getPersistenceProvider();
        return _storageEngine->getEngine()->createRecordStore(
            provider, collName, identName, RecordStore::Options{});
    }

    Status dropIndexTable(OperationContext* opCtx, NamespaceString nss, StringData indexName) {
        RecordId catalogId =
            CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss)->getCatalogId();
        std::string indexIdent =
            _storageEngine->getMDBCatalog()->getIndexIdent(opCtx, catalogId, indexName);
        return dropIdent(*shard_role_details::getRecoveryUnit(opCtx), indexIdent, false);
    }

    Status dropIdent(RecoveryUnit& ru, StringData ident, bool identHasSizeInfo) {
        return _storageEngine->getEngine()->dropIdent(ru, ident, identHasSizeInfo);
    }

    StatusWith<StorageEngine::ReconcileResult> reconcile(OperationContext* opCtx) {
        Lock::GlobalLock globalLock{opCtx, MODE_IX};
        return catalog_repair::reconcileCatalogAndIdents(
            opCtx,
            _storageEngine,
            Timestamp::min(),
            StorageEngine::LastShutdownState::kClean,
            reinterpret_cast<StorageEngineImpl*>(_storageEngine)->_options.forRepair);
    }

    StatusWith<StorageEngine::ReconcileResult> reconcileAfterUncleanShutdown(
        OperationContext* opCtx) {
        return catalog_repair::reconcileCatalogAndIdents(
            opCtx,
            _storageEngine,
            Timestamp::min(),
            StorageEngine::LastShutdownState::kUnclean,
            reinterpret_cast<StorageEngineImpl*>(_storageEngine)->_options.forRepair);
    }

    std::vector<std::string> getAllKVEngineIdents(OperationContext* opCtx) {
        return _storageEngine->getEngine()->getAllIdents(
            *shard_role_details::getRecoveryUnit(opCtx));
    }

    std::vector<std::string> getAllSpillKVEngineIdents(OperationContext* opCtx) {
        return _storageEngine->getSpillEngine()->getAllIdents(
            *_storageEngine->getSpillEngine()->newRecoveryUnit());
    }

    bool collectionExists(OperationContext* opCtx, const NamespaceString& nss) {
        std::vector<MDBCatalog::EntryIdentifier> allCollections =
            _storageEngine->getMDBCatalog()->getAllCatalogEntries(opCtx);
        return std::count_if(allCollections.begin(), allCollections.end(), [&](auto& entry) {
            return nss == entry.nss;
        });
    }

    bool identExists(OperationContext* opCtx, StringData ident) {
        auto idents = getAllKVEngineIdents(opCtx);
        return std::find(idents.begin(), idents.end(), ident) != idents.end();
    }

    bool spillIdentExists(OperationContext* opCtx, StringData ident) {
        auto idents = getAllSpillKVEngineIdents(opCtx);
        return std::find(idents.begin(), idents.end(), ident) != idents.end();
    }

    /**
     * Create an index with a key of `{<key>: 1}` and a `name` of <key>.
     */
    Status createIndex(OperationContext* opCtx, NamespaceString collNs, StringData key) {
        auto buildUUID = UUID::gen();
        auto ret = startIndexBuild(opCtx, collNs, key, buildUUID);
        if (!ret.isOK()) {
            return ret;
        }

        indexBuildSuccess(opCtx, collNs, key);
        return Status::OK();
    }

    Status startIndexBuild(OperationContext* opCtx,
                           NamespaceString collNs,
                           StringData key,
                           boost::optional<UUID> buildUUID) {
        BSONObjBuilder builder;
        BSONObj spec = BSON("v" << 2 << "key" << BSON(key << 1) << "name" << key);

        CollectionWriter writer{opCtx, collNs};
        Collection* collection = writer.getWritableCollection(opCtx);
        IndexDescriptor descriptor(IndexNames::BTREE, spec);
        return collection->prepareForIndexBuild(
            opCtx, &descriptor, _storageEngine->generateNewIndexIdent(collNs.dbName()), buildUUID);
    }

    void indexBuildSuccess(OperationContext* opCtx, NamespaceString collNs, StringData key) {
        CollectionWriter writer{opCtx, collNs};
        Collection* collection = writer.getWritableCollection(opCtx);
        auto writableEntry = collection->getIndexCatalog()->getWritableEntryByName(
            opCtx,
            key,
            IndexCatalog::InclusionPolicy::kReady | IndexCatalog::InclusionPolicy::kUnfinished);
        ASSERT(writableEntry);
        collection->indexBuildSuccess(opCtx, writableEntry);
    }

    Status removeEntry(OperationContext* opCtx, StringData collNs, MDBCatalog* catalog) {
        const Collection* collection = CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(
            opCtx, NamespaceString::createNamespaceString_forTest(collNs));
        return catalog->removeEntry(opCtx, collection->getCatalogId());
    }

    StorageEngine* _storageEngine;
};

class StorageEngineRepairTest : public StorageEngineTest {
public:
    StorageEngineRepairTest() : StorageEngineTest(Options{}.enableRepair().inMemory(false)) {
        repl::StorageInterface::set(getServiceContext(),
                                    std::make_unique<repl::StorageInterfaceImpl>());
    }

    void tearDown() override {
        auto repairObserver = StorageRepairObserver::get(getGlobalServiceContext());
        ASSERT(repairObserver->isDone());

        auto asString = [](const StorageRepairObserver::Modification& mod) {
            return mod.getDescription();
        };
        auto modifications = repairObserver->getModifications();
        LOGV2(24150,
              "Modifications",
              "modifications"_attr =
                  logv2::seqLog(boost::make_transform_iterator(modifications.begin(), asString),
                                boost::make_transform_iterator(modifications.end(), asString)));
    }
};

class StorageEngineTestNotEphemeral : public StorageEngineTest {
public:
    StorageEngineTestNotEphemeral() : StorageEngineTest(Options{}.inMemory(false)) {}
};

}  // namespace mongo

#undef MONGO_LOGV2_DEFAULT_COMPONENT
