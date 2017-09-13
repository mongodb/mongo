/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#pragma once

#include <map>
#include <string>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/storage/mmap_v1/catalog/namespace_details_collection_entry.h"
#include "mongo/db/storage/mmap_v1/catalog/namespace_index.h"
#include "mongo/db/storage/mmap_v1/mmap_v1_extent_manager.h"

namespace mongo {

class CollectionCatalogEntry;
struct CollectionOptions;
class IndexAccessMethod;
class IndexCatalogEntry;
class IndexDescriptor;
class RecordId;
class RecordStore;
class RecordStoreV1Base;
class RecoveryUnit;
class OperationContext;

class MMAPV1DatabaseCatalogEntry : public DatabaseCatalogEntry {
public:
    MMAPV1DatabaseCatalogEntry(OperationContext* opCtx,
                               StringData name,
                               StringData path,
                               bool directoryperdb,
                               bool transient,
                               std::unique_ptr<ExtentManager> extentManager);

    virtual ~MMAPV1DatabaseCatalogEntry();

    /**
     * Must be called before destruction.
     */
    virtual void close(OperationContext* opCtx) {
        _extentManager->close(opCtx);
        _namespaceIndex.close(opCtx);
    }

    // these two seem the same and yet different
    // TODO(ERH): consolidate into one ideally
    virtual bool exists() const {
        return _namespaceIndex.pathExists();
    }
    virtual bool isEmpty() const {
        return !_namespaceIndex.allocated();
    }
    virtual bool hasUserData() const {
        // The two collections which exist and can't be removed are:
        //    system.indexes
        //    system.namespaces
        return _collections.size() > 2;
    }

    virtual int64_t sizeOnDisk(OperationContext* opCtx) const;

    virtual bool isOlderThan24(OperationContext* opCtx) const;
    virtual void markIndexSafe24AndUp(OperationContext* opCtx);

    // Records in the data file version bits that an index or collection may have an associated
    // collation.
    void markCollationFeatureAsInUse(OperationContext* opCtx);

    virtual Status currentFilesCompatible(OperationContext* opCtx) const;

    virtual void appendExtraStats(OperationContext* opCtx, BSONObjBuilder* out, double scale) const;

    Status createCollection(OperationContext* opCtx,
                            StringData ns,
                            const CollectionOptions& options,
                            bool allocateDefaultSpace);

    Status dropCollection(OperationContext* opCtx, StringData ns);

    Status renameCollection(OperationContext* opCtx,
                            StringData fromNS,
                            StringData toNS,
                            bool stayTemp);

    void getCollectionNamespaces(std::list<std::string>* tofill) const;

    /**
     * will return NULL if ns does not exist
     */
    NamespaceDetailsCollectionCatalogEntry* getCollectionCatalogEntry(StringData ns) const;

    RecordStore* getRecordStore(StringData ns) const;

    IndexAccessMethod* getIndex(OperationContext* opCtx,
                                const CollectionCatalogEntry* collection,
                                IndexCatalogEntry* index);

    const ExtentManager* getExtentManager() const {
        return _extentManager.get();
    }
    ExtentManager* getExtentManager() {
        return _extentManager.get();
    }

    CollectionOptions getCollectionOptions(OperationContext* opCtx, StringData ns) const;

    CollectionOptions getCollectionOptions(OperationContext* opCtx, RecordId nsRid) const;

    /**
     * Creates a CollectionCatalogEntry in the form of an index rather than a collection.
     * MMAPv1 puts both indexes and collections into CCEs. A namespace named 'name' must not
     * exist.
     */
    void createNamespaceForIndex(OperationContext* opCtx, StringData name);
    static void invalidateSystemCollectionRecord(OperationContext* opCtx,
                                                 NamespaceString systemCollectionNamespace,
                                                 RecordId record);

private:
    class EntryInsertion;
    class EntryRemoval;

    friend class NamespaceDetailsCollectionCatalogEntry;

    // The _collections map is a cache for efficiently looking up namespace information. Access
    // to the cache is protected by holding the appropriate DB lock. Regular operations
    // (insert/update/delete/query) hold intent locks on the database and they access the cache
    // directly. Metadata operations, such as create db/collection, etc acquire exclusive lock
    // on the database, which protects against concurrent readers of the cache.
    //
    // Once initialized, the cache must remain consistent with the data in the memory-mapped
    // database files through _removeFromCache and _insertInCache. These methods use the
    // RecoveryUnit to ensure correct handling of rollback.

    struct Entry {
        std::unique_ptr<NamespaceDetailsCollectionCatalogEntry> catalogEntry;
        std::unique_ptr<RecordStoreV1Base> recordStore;
    };

    typedef std::map<std::string, Entry*> CollectionMap;


    RecordStoreV1Base* _getIndexRecordStore();
    RecordStoreV1Base* _getNamespaceRecordStore() const;
    RecordStoreV1Base* _getRecordStore(StringData ns) const;

    RecordId _addNamespaceToNamespaceCollection(OperationContext* opCtx,
                                                StringData ns,
                                                const BSONObj* options);

    void _removeNamespaceFromNamespaceCollection(OperationContext* opCtx, StringData ns);

    Status _renameSingleNamespace(OperationContext* opCtx,
                                  StringData fromNS,
                                  StringData toNS,
                                  bool stayTemp);

    void _ensureSystemCollection(OperationContext* opCtx, StringData ns);

    void _init(OperationContext* opCtx);

    /**
     * Populate the _collections cache.
     */
    void _insertInCache(OperationContext* opCtx, StringData ns, RecordId rid, Entry* entry);

    /**
     * Drop cached information for specified namespace. If a RecoveryUnit is specified,
     * use it to allow rollback. When ru is null, removal is unconditional.
     */
    void _removeFromCache(RecoveryUnit* ru, StringData ns);


    const std::string _path;

    NamespaceIndex _namespaceIndex;
    std::unique_ptr<ExtentManager> _extentManager;
    CollectionMap _collections;
};
}
