/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <map>
#include <memory>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/bson_collection_catalog_entry.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/storage/kv/kv_prefix.h"
#include "mongo/platform/mutex.h"

namespace mongo {

class OperationContext;
class RecordStore;
class StorageEngineInterface;

class DurableCatalogImpl : public DurableCatalog {
public:
    class FeatureTracker;

    /**
     * The RecordStore must be thread-safe, in particular with concurrent calls to
     * RecordStore::find, updateRecord, insertRecord, deleteRecord and dataFor. The
     * DurableCatalogImpl does not utilize Cursors and those methods may omit further protection.
     */
    DurableCatalogImpl(RecordStore* rs,
                       bool directoryPerDb,
                       bool directoryForIndexes,
                       StorageEngineInterface* engine);
    ~DurableCatalogImpl();

    void init(OperationContext* opCtx);

    std::vector<Entry> getAllCatalogEntries(OperationContext* opCtx) const;

    Entry getEntry(RecordId catalogId) const;

    std::string getCollectionIdent(RecordId catalogId) const;

    std::string getIndexIdent(OperationContext* opCtx,
                              RecordId catalogId,
                              StringData idxName) const;

    BSONCollectionCatalogEntry::MetaData getMetaData(OperationContext* opCtx,
                                                     RecordId catalogId) const;
    void putMetaData(OperationContext* opCtx,
                     RecordId catalogId,
                     BSONCollectionCatalogEntry::MetaData& md);

    std::vector<std::string> getAllIdents(OperationContext* opCtx) const;

    bool isUserDataIdent(StringData ident) const;

    bool isInternalIdent(StringData ident) const;

    bool isCollectionIdent(StringData ident) const;

    FeatureTracker* getFeatureTracker() const {
        invariant(_featureTracker);
        return _featureTracker.get();
    }

    RecordStore* getRecordStore() {
        return _rs;
    }

    StatusWith<std::string> newOrphanedIdent(OperationContext* opCtx, std::string ident);

    std::string getFilesystemPathForDb(const std::string& dbName) const;

    std::string newInternalIdent();

    StatusWith<std::pair<RecordId, std::unique_ptr<RecordStore>>> createCollection(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const CollectionOptions& options,
        bool allocateDefaultSpace);

    Status renameCollection(OperationContext* opCtx,
                            RecordId catalogId,
                            const NamespaceString& toNss,
                            bool stayTemp);

    Status dropCollection(OperationContext* opCtx, RecordId catalogId);

    void updateCappedSize(OperationContext* opCtx, RecordId catalogId, long long size);

    void updateTTLSetting(OperationContext* opCtx,
                          RecordId catalogId,
                          StringData idxName,
                          long long newExpireSeconds);

    void updateHiddenSetting(OperationContext* opCtx,
                             RecordId catalogId,
                             StringData idxName,
                             bool hidden);

    bool isEqualToMetadataUUID(OperationContext* opCtx,
                               RecordId catalogId,
                               OptionalCollectionUUID uuid);

    void setIsTemp(OperationContext* opCtx, RecordId catalogId, bool isTemp);

    void setRecordPreImages(OperationContext* opCtx, RecordId catalogId, bool val) override;

    void updateValidator(OperationContext* opCtx,
                         RecordId catalogId,
                         const BSONObj& validator,
                         StringData validationLevel,
                         StringData validationAction);

    Status removeIndex(OperationContext* opCtx, RecordId catalogId, StringData indexName);

    Status prepareForIndexBuild(OperationContext* opCtx,
                                RecordId catalogId,
                                const IndexDescriptor* spec,
                                boost::optional<UUID> buildUUID,
                                bool isBackgroundSecondaryBuild);

    boost::optional<UUID> getIndexBuildUUID(OperationContext* opCtx,
                                            RecordId catalogId,
                                            StringData indexName) const;

    void indexBuildSuccess(OperationContext* opCtx, RecordId catalogId, StringData indexName);

    bool isIndexMultikey(OperationContext* opCtx,
                         RecordId catalogId,
                         StringData indexName,
                         MultikeyPaths* multikeyPaths) const;

    bool setIndexIsMultikey(OperationContext* opCtx,
                            RecordId catalogId,
                            StringData indexName,
                            const MultikeyPaths& multikeyPaths);

    CollectionOptions getCollectionOptions(OperationContext* opCtx, RecordId catalogId) const;

    int getTotalIndexCount(OperationContext* opCtx, RecordId catalogId) const;

    int getCompletedIndexCount(OperationContext* opCtx, RecordId catalogId) const;

    BSONObj getIndexSpec(OperationContext* opCtx, RecordId catalogId, StringData indexName) const;

    void getAllIndexes(OperationContext* opCtx,
                       RecordId catalogId,
                       std::vector<std::string>* names) const;

    void getReadyIndexes(OperationContext* opCtx,
                         RecordId catalogId,
                         std::vector<std::string>* names) const;

    bool isIndexPresent(OperationContext* opCtx, RecordId catalogId, StringData indexName) const;

    bool isIndexReady(OperationContext* opCtx, RecordId catalogId, StringData indexName) const;

    KVPrefix getIndexPrefix(OperationContext* opCtx,
                            RecordId catalogId,
                            StringData indexName) const;

private:
    class AddIdentChange;
    class RemoveIdentChange;
    class AddIndexChange;
    class RemoveIndexChange;

    friend class StorageEngineImpl;
    friend class DurableCatalogImplTest;
    friend class StorageEngineTest;

    BSONObj _findEntry(OperationContext* opCtx, RecordId catalogId) const;
    StatusWith<Entry> _addEntry(OperationContext* opCtx,
                                NamespaceString nss,
                                const CollectionOptions& options,
                                KVPrefix prefix);
    Status _replaceEntry(OperationContext* opCtx,
                         RecordId catalogId,
                         const NamespaceString& toNss,
                         bool stayTemp);
    Status _removeEntry(OperationContext* opCtx, RecordId catalogId);

    /**
     * Generates a new unique identifier for a new "thing".
     * @param nss - the containing namespace
     * @param kind - what this "thing" is, likely collection or index
     */
    std::string _newUniqueIdent(NamespaceString nss, const char* kind);

    // Helpers only used by constructor and init(). Don't call from elsewhere.
    static std::string _newRand();
    bool _hasEntryCollidingWithRand() const;

    RecordStore* _rs;  // not owned
    const bool _directoryPerDb;
    const bool _directoryForIndexes;

    // These two are only used for ident generation inside _newUniqueIdent.
    std::string _rand;  // effectively const after init() returns
    AtomicWord<unsigned long long> _next;

    std::map<RecordId, Entry> _catalogIdToEntryMap;
    mutable Mutex _catalogIdToEntryMapLock =
        MONGO_MAKE_LATCH("DurableCatalogImpl::_catalogIdToEntryMap");

    // Manages the feature document that may be present in the DurableCatalogImpl. '_featureTracker'
    // is guaranteed to be non-null after DurableCatalogImpl::init() is called.
    std::unique_ptr<FeatureTracker> _featureTracker;

    StorageEngineInterface* const _engine;
};
}  // namespace mongo
