// mmap_v1_database_catalog_entry.h

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
#include "mongo/db/storage/mmap_v1/catalog/namespace_index.h"
#include "mongo/db/storage/mmap_v1/mmap_v1_extent_manager.h"

namespace mongo {

    class CollectionCatalogEntry;
    struct CollectionOptions;
    class IndexAccessMethod;
    class IndexCatalogEntry;
    class IndexDescriptor;
    class RecordStore;
    class RecordStoreV1Base;
    class OperationContext;

    class MMAPV1DatabaseCatalogEntry : public DatabaseCatalogEntry {
    public:
        MMAPV1DatabaseCatalogEntry( OperationContext* txn,
                                    const StringData& name,
                                    const StringData& path,
                                    bool directoryperdb,
                                    bool transient );

        virtual ~MMAPV1DatabaseCatalogEntry();

        // these two seem the same and yet different
        // TODO(ERH): consolidate into one ideally
        bool exists() const { return _namespaceIndex.pathExists(); }
        bool isEmpty() const { return !_namespaceIndex.allocated(); }

        virtual int64_t sizeOnDisk( OperationContext* opCtx ) const;

        virtual bool isOlderThan24( OperationContext* opCtx ) const;
        virtual void markIndexSafe24AndUp( OperationContext* opCtx );

        virtual bool currentFilesCompatible( OperationContext* opCtx ) const;

        virtual void appendExtraStats( OperationContext* opCtx,
                                       BSONObjBuilder* out,
                                       double scale ) const;

        Status createCollection( OperationContext* txn,
                                 const StringData& ns,
                                 const CollectionOptions& options,
                                 bool allocateDefaultSpace );

        Status dropCollection( OperationContext* txn, const StringData& ns );

        Status renameCollection( OperationContext* txn,
                                 const StringData& fromNS,
                                 const StringData& toNS,
                                 bool stayTemp );

        void getCollectionNamespaces( std::list<std::string>* tofill ) const;

        /*
         * will return NULL if ns does not exist
         */
        CollectionCatalogEntry* getCollectionCatalogEntry( OperationContext* txn,
                                                           const StringData& ns ) const;

        RecordStore* getRecordStore( OperationContext* txn,
                                     const StringData& ns );

        IndexAccessMethod* getIndex( OperationContext* txn,
                                     const CollectionCatalogEntry* collection,
                                     IndexCatalogEntry* index );

        const MmapV1ExtentManager* getExtentManager() const { return &_extentManager; }
        MmapV1ExtentManager* getExtentManager() { return &_extentManager; }

        CollectionOptions getCollectionOptions( OperationContext* txn,
                                                const StringData& ns ) const;


        //
        // Remaining methods are MMAPv1 specific
        //

        /**
         * Creates a CollectionCatalogEntry in the for an index rather than a collection. MMAPv1
         * puts both indexes and collections into CCEs. A namespace named 'name' must not exist.
         */
        void createNamespaceForIndex(OperationContext* txn, const StringData& name);

    private:
        RecordStoreV1Base* _getIndexRecordStore_inlock();
        RecordStoreV1Base* _getIndexRecordStore();
        RecordStoreV1Base* _getNamespaceRecordStore_inlock() const;
        RecordStoreV1Base* _getNamespaceRecordStore() const;

        RecordStoreV1Base* _getRecordStore( OperationContext* txn,
                                            const StringData& ns );

        void _addNamespaceToNamespaceCollection( OperationContext* txn,
                                                 const StringData& ns,
                                                 const BSONObj* options );
        void _addNamespaceToNamespaceCollection_inlock( OperationContext* txn,
                                                        const StringData& ns,
                                                        const BSONObj* options );


        void _removeNamespaceFromNamespaceCollection( OperationContext* txn,
                                                      const StringData& ns );

        Status _renameSingleNamespace( OperationContext* txn,
                                       const StringData& fromNS,
                                       const StringData& toNS,
                                       bool stayTemp );

        void _ensureSystemCollection_inlock( OperationContext* txn,
                                             const StringData& ns );
        void _lazyInit( OperationContext* txn );

        std::string _path;

        MmapV1ExtentManager _extentManager;
        NamespaceIndex _namespaceIndex;

        //  The _collections map is a cache for efficiently looking up namespace information.
        //  Access to the cache is protected by the _collectionsLock mutex.
        //  Once initialized, the cache must remain consistent with the data in the memory-mapped
        //  database files through _removeFromCache and _insertInCache_inlock. These methods
        //  use the RecoveryUnit to ensure correct handling of rollback.

        struct Entry {
            scoped_ptr<CollectionCatalogEntry> catalogEntry;
            scoped_ptr<RecordStoreV1Base> recordStore;
        };

        /**
         * Populate the _collections cache.
         */
        void _insertInCache_inlock(OperationContext* opCtx, const StringData& ns, Entry* entry);

        /**
         * Drop cached information for specified namespace. If a RecoveryUnit is specified,
         * use it to allow rollback. When ru is null, removal is unconditional.
         */
        void _removeFromCache(RecoveryUnit* ru, const StringData& ns);

        mutable boost::mutex _collectionsLock;
        typedef std::map<std::string, Entry*> CollectionMap;
        CollectionMap _collections;

        class EntryInsertion;
        class EntryRemoval;

        friend class NamespaceDetailsCollectionCatalogEntry;
    };
}
