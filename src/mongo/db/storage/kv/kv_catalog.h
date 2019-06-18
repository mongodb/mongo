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
#include "mongo/db/storage/kv/kv_prefix.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

class OperationContext;
class RecordStore;
class KVStorageEngineInterface;

class KVCatalog {
public:
    class FeatureTracker;

    /**
     * The RecordStore must be thread-safe, in particular with concurrent calls to
     * RecordStore::find, updateRecord, insertRecord, deleteRecord and dataFor. The KVCatalog does
     * not utilize Cursors and those methods may omit further protection.
     */
    KVCatalog(RecordStore* rs,
              bool directoryPerDb,
              bool directoryForIndexes,
              KVStorageEngineInterface* engine);
    ~KVCatalog();

    void init(OperationContext* opCtx);

    std::vector<NamespaceString> getAllCollections() const;

    std::string getCollectionIdent(const NamespaceString& nss) const;

    std::string getIndexIdent(OperationContext* opCtx,
                              const NamespaceString& nss,
                              StringData idName) const;

    BSONCollectionCatalogEntry::MetaData getMetaData(OperationContext* opCtx,
                                                     const NamespaceString& nss) const;
    void putMetaData(OperationContext* opCtx,
                     const NamespaceString& nss,
                     BSONCollectionCatalogEntry::MetaData& md);

    std::vector<std::string> getAllIdentsForDB(StringData db) const;
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

    /**
     * Create an entry in the catalog for an orphaned collection found in the
     * storage engine. Return the generated ns of the collection.
     * Note that this function does not recreate the _id index on the collection because it does not
     * have access to index catalog.
     */
    StatusWith<std::string> newOrphanedIdent(OperationContext* opCtx, std::string ident);

    std::string getFilesystemPathForDb(const std::string& dbName) const;

    /**
     * Generate an internal ident name.
     */
    std::string newInternalIdent();

    std::unique_ptr<CollectionCatalogEntry> makeCollectionCatalogEntry(OperationContext* opCtx,
                                                                       const NamespaceString& nss,
                                                                       bool forRepair);

    StatusWith<std::unique_ptr<CollectionCatalogEntry>> createCollection(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const CollectionOptions& options,
        bool allocateDefaultSpace);

    Status renameCollection(OperationContext* opCtx,
                            const NamespaceString& fromNss,
                            const NamespaceString& toNss,
                            bool stayTemp);

    Status dropCollection(OperationContext* opCtx, const NamespaceString& nss);

private:
    class AddIdentChange;
    class RemoveIdentChange;

    friend class KVStorageEngine;
    friend class KVCatalogTest;
    friend class KVStorageEngineTest;

    BSONObj _findEntry(OperationContext* opCtx,
                       const NamespaceString& nss,
                       RecordId* out = nullptr) const;
    Status _addEntry(OperationContext* opCtx,
                     const NamespaceString& nss,
                     const CollectionOptions& options,
                     KVPrefix prefix);
    Status _replaceEntry(OperationContext* opCtx,
                         const NamespaceString& fromNss,
                         const NamespaceString& toNss,
                         bool stayTemp);
    Status _removeEntry(OperationContext* opCtx, const NamespaceString& nss);

    /**
     * Generates a new unique identifier for a new "thing".
     * @param nss - the containing namespace
     * @param kind - what this "thing" is, likely collection or index
     */
    std::string _newUniqueIdent(const NamespaceString& nss, const char* kind);

    // Helpers only used by constructor and init(). Don't call from elsewhere.
    static std::string _newRand();
    bool _hasEntryCollidingWithRand() const;

    RecordStore* _rs;  // not owned
    const bool _directoryPerDb;
    const bool _directoryForIndexes;

    // These two are only used for ident generation inside _newUniqueIdent.
    std::string _rand;  // effectively const after init() returns
    AtomicWord<unsigned long long> _next;

    struct Entry {
        Entry() {}
        Entry(std::string i, RecordId l) : ident(i), storedLoc(l) {}
        std::string ident;
        RecordId storedLoc;
    };
    typedef std::map<std::string, Entry> NSToIdentMap;
    NSToIdentMap _idents;
    mutable stdx::mutex _identsLock;

    // Manages the feature document that may be present in the KVCatalog. '_featureTracker' is
    // guaranteed to be non-null after KVCatalog::init() is called.
    std::unique_ptr<FeatureTracker> _featureTracker;

    KVStorageEngineInterface* const _engine;
};
}
