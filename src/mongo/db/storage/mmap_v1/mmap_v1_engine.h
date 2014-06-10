// mmap_v1_engine.h

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

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/storage/mmap_v1/mmap_v1_extent_manager.h"
#include "mongo/db/structure/catalog/namespace_index.h"

namespace mongo {

    class CollectionCatalogEntry;
    struct CollectionOptions;
    class IndexAccessMethod;
    class IndexCatalogEntry;
    class IndexDescriptor;
    class RecordStore;
    class RecordStoreV1Base;
    class OperationContext;

    class MMAP1DatabaseCatalogEntry : public DatabaseCatalogEntry {
    public:
        MMAP1DatabaseCatalogEntry( OperationContext* txn,
                                   const StringData& name,
                                   const StringData& path,
                                   bool directoryperdb );

        virtual ~MMAP1DatabaseCatalogEntry();

        // these two seem the same and yet different
        // TODO(ERH): consolidate into one ideally
        bool exists() const { return _namespaceIndex.pathExists(); }
        bool isEmpty() const { return !_namespaceIndex.allocated(); }

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
         * ownership passes to caller
         * will return NULL if ns does not exist
         */
        CollectionCatalogEntry* getCollectionCatalogEntry( OperationContext* txn,
                                                           const StringData& ns );

        // TODO(ERH): ownership passes to caller (i think this is wrong)
        RecordStore* getRecordStore( OperationContext* txn,
                                     const StringData& ns );

        // TODO(ERH): ownership passes to caller (i think this is wrong)
        IndexAccessMethod* getIndex( OperationContext* txn,
                                     const CollectionCatalogEntry* collection,
                                     IndexCatalogEntry* index );

        const MmapV1ExtentManager* getExtentManager() const { return &_extentManager; } // TODO(ERH): remove
        MmapV1ExtentManager* getExtentManager() { return &_extentManager; } // TODO(ERH): remove

    private:

        RecordStoreV1Base* _getIndexRecordStore( OperationContext* txn );
        RecordStoreV1Base* _getNamespaceRecordStore( OperationContext* txn,
                                                     const StringData& whosAsking );

        RecordStoreV1Base* _getRecordStore( OperationContext* txn,
                                            const StringData& ns );

        void _addNamespaceToNamespaceCollection( OperationContext* txn,
                                                 const StringData& ns,
                                                 const BSONObj* options );

        void _removeNamespaceFromNamespaceCollection( OperationContext* txn,
                                                      const StringData& ns );

        Status _renameSingleNamespace( OperationContext* txn,
                                       const StringData& fromNS,
                                       const StringData& toNS,
                                       bool stayTemp );

        /**
         * @throws DatabaseDifferCaseCode if the name is a duplicate based on
         * case insensitive matching.
         */
        void _checkDuplicateUncasedNames() const;

        std::string _path;

        MmapV1ExtentManager _extentManager;
        NamespaceIndex _namespaceIndex;

        friend class NamespaceDetailsCollectionCatalogEntry;
    };
}
