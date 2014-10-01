// wiredtiger_collection_catalog_entry.h

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

#include <string>
#include <vector>

#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/storage/bson_collection_catalog_entry.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"

namespace mongo {
    class WiredTigerDatabase;
    class WiredTigerSession;

    class WiredTigerCollectionCatalogEntry : public CollectionCatalogEntry {
    public:
        WiredTigerCollectionCatalogEntry(
                const StringData& ns, const CollectionOptions& options );
        WiredTigerCollectionCatalogEntry(
            WiredTigerDatabase& db, const StringData& ns, bool stayTemp = false);
        virtual ~WiredTigerCollectionCatalogEntry();

        int getTotalIndexCount( OperationContext* txn ) const;

        int getCompletedIndexCount( OperationContext* txn ) const;

        int getMaxAllowedIndexes() const { return 64; }

        void getAllIndexes( OperationContext* txn, std::vector<std::string>* names ) const;

        BSONObj getIndexSpec( OperationContext* txn, const StringData& idxName ) const;

        bool isIndexMultikey( OperationContext* txn, const StringData& indexName) const;

        bool setIndexIsMultikey(OperationContext* txn,
                                const StringData& indexName,
                                bool multikey = true);

        DiskLoc getIndexHead( OperationContext* txn, const StringData& indexName ) const;

        void setIndexHead( OperationContext* txn,
                           const StringData& indexName,
                           const DiskLoc& newHead );

        bool isIndexReady( OperationContext* txn, const StringData& indexName ) const;

        Status removeIndex( OperationContext* txn,
                            const StringData& indexName );

        Status prepareForIndexBuild( OperationContext* txn,
                                     const IndexDescriptor* spec );

        void indexBuildSuccess( OperationContext* txn,
                                const StringData& indexName );

        void updateTTLSetting( OperationContext* txn,
                               const StringData& idxName,
                               long long newExpireSeconds );

        CollectionOptions getCollectionOptions(OperationContext* txn) const { return options; }

        // WiredTiger specific APIs
        bool indexExists( const StringData &indexName ) const;

        CollectionOptions options;
        scoped_ptr<WiredTigerRecordStore> rs;

    private:
        // Is this an IndexMetaData in the BSONCollectionCatalogEntry
        struct IndexEntry {
            std::string name;
            BSONObj spec;
            DiskLoc head;
            bool ready;
            bool isMultikey;
            
            // Only one of these will be in use. See getIndex() implementation.
            scoped_ptr<RecordStore> rs;
            shared_ptr<void> data;
        };
        // Not currently used, but needed to implement interface.
        BSONCollectionCatalogEntry::MetaData _metaData;
        BSONObj _getSavedMetadata(WiredTigerCursor &cursor);
    public:
        typedef std::map<std::string,IndexEntry*> Indexes;
        Indexes indexes;
    protected:
    };

} // namespace mongo
