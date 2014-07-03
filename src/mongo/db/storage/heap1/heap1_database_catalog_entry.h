// heap1_database_catalog_entry.h

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

#include <list>
#include <map>
#include <string>

#include <boost/thread/mutex.hpp>

#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/database_catalog_entry.h"

namespace mongo {

    class HeapRecordStore;

    class Heap1DatabaseCatalogEntry : public DatabaseCatalogEntry {
    public:
        Heap1DatabaseCatalogEntry( const StringData& name );

        virtual ~Heap1DatabaseCatalogEntry();

        virtual bool exists() const { return _everHadACollection; }
        virtual bool isEmpty() const;

        virtual void appendExtraStats( OperationContext* opCtx,
                                       BSONObjBuilder* out,
                                       double scale ) const;

        // these are hacks :(
        virtual bool isOlderThan24( OperationContext* opCtx ) const { return false; }
        virtual void markIndexSafe24AndUp( OperationContext* opCtx ) { }

        /**
         * @return true if current files on disk are compatibile with the current version.
         *              if we return false, then an upgrade will be required
         */
        virtual bool currentFilesCompatible( OperationContext* opCtx ) const { return true; }

        // ----

        virtual void getCollectionNamespaces( std::list<std::string>* out ) const;

        virtual CollectionCatalogEntry* getCollectionCatalogEntry( OperationContext* txn,
                                                                   const StringData& ns ) const;

        virtual RecordStore* getRecordStore( OperationContext* txn,
                                             const StringData& ns );


        virtual IndexAccessMethod* getIndex( OperationContext* txn,
                                             const CollectionCatalogEntry* collection,
                                             IndexCatalogEntry* index );

        virtual Status createCollection( OperationContext* txn,
                                         const StringData& ns,
                                         const CollectionOptions& options,
                                         bool allocateDefaultSpace );

        virtual Status renameCollection( OperationContext* txn,
                                         const StringData& fromNS,
                                         const StringData& toNS,
                                         bool stayTemp );

        virtual Status dropCollection( OperationContext* opCtx,
                                       const StringData& ns );

    private:

        struct IndexEntry {
            std::string name;
            BSONObj spec;
            DiskLoc head;
            bool ready;
            bool isMultikey;
            
            // Only one of these will be in use. See getIndex() implementation.
            scoped_ptr<RecordStore> rs; // used by Btree on HeapRecordStore
            shared_ptr<void> data; // used by Heap1BtreeImpl
        };

        class Entry : public CollectionCatalogEntry {
        public:
            Entry( const StringData& ns, const CollectionOptions& options );
            virtual ~Entry();

            int getTotalIndexCount() const;

            int getCompletedIndexCount() const;

            int getMaxAllowedIndexes() const { return 64; }

            void getAllIndexes( std::vector<std::string>* names ) const;

            BSONObj getIndexSpec( const StringData& idxName ) const;

            bool isIndexMultikey( const StringData& indexName) const;

            bool setIndexIsMultikey(OperationContext* txn,
                                    const StringData& indexName,
                                    bool multikey = true);

            DiskLoc getIndexHead( const StringData& indexName ) const;

            void setIndexHead( OperationContext* txn,
                               const StringData& indexName,
                               const DiskLoc& newHead );

            bool isIndexReady( const StringData& indexName ) const;

            Status removeIndex( OperationContext* txn,
                                const StringData& indexName );

            Status prepareForIndexBuild( OperationContext* txn,
                                         const IndexDescriptor* spec );

            void indexBuildSuccess( OperationContext* txn,
                                    const StringData& indexName );

            void updateTTLSetting( OperationContext* txn,
                                   const StringData& idxName,
                                   long long newExpireSeconds );

            CollectionOptions getCollectionOptions() const { return options; }

            CollectionOptions options;
            scoped_ptr<HeapRecordStore> rs;
            typedef std::map<std::string,IndexEntry*> Indexes;
            Indexes indexes;
        };

        bool _everHadACollection;

        mutable boost::mutex _entryMapLock;
        typedef std::map<std::string,Entry*> EntryMap;
        EntryMap _entryMap;

    };
}
