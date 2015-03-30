// namespace_details_collection_entry.h

#pragma once

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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/storage/mmap_v1/diskloc.h"

namespace mongo {

    class NamespaceDetails;

    class MMAPV1DatabaseCatalogEntry;;
    class RecordStore;
    class OperationContext;

    class NamespaceDetailsCollectionCatalogEntry : public CollectionCatalogEntry {
    public:
        NamespaceDetailsCollectionCatalogEntry( StringData ns,
                                                NamespaceDetails* details,
                                                RecordStore* namespacesRecordStore,
                                                RecordStore* indexRecordStore,
                                                MMAPV1DatabaseCatalogEntry* db );

        virtual ~NamespaceDetailsCollectionCatalogEntry(){}

        virtual CollectionOptions getCollectionOptions(OperationContext* txn) const;

        virtual int getTotalIndexCount(OperationContext* txn) const;

        virtual int getCompletedIndexCount(OperationContext* txn) const;

        virtual int getMaxAllowedIndexes() const;

        virtual void getAllIndexes( OperationContext* txn,
                                    std::vector<std::string>* names ) const;

        virtual BSONObj getIndexSpec( OperationContext* txn,
                                      StringData idxName ) const;

        virtual bool isIndexMultikey(OperationContext* txn,
                                     StringData indexName) const;
        virtual bool isIndexMultikey(int idxNo) const;

        virtual bool setIndexIsMultikey(OperationContext* txn,
                                        int idxNo,
                                        bool multikey = true);
        virtual bool setIndexIsMultikey(OperationContext* txn,
                                        StringData indexName,
                                        bool multikey = true);

        virtual RecordId getIndexHead( OperationContext* txn,
                                       StringData indexName ) const;

        virtual void setIndexHead( OperationContext* txn,
                                   StringData indexName,
                                   const RecordId& newHead );

        virtual bool isIndexReady( OperationContext* txn,
                                   StringData indexName ) const;

        virtual Status removeIndex( OperationContext* txn,
                                    StringData indexName );

        virtual Status prepareForIndexBuild( OperationContext* txn,
                                             const IndexDescriptor* spec );

        virtual void indexBuildSuccess( OperationContext* txn,
                                        StringData indexName );

        virtual void updateTTLSetting( OperationContext* txn,
                                       StringData idxName,
                                       long long newExpireSeconds );

        virtual void updateFlags(OperationContext* txn, int newValue);

        // not part of interface, but available to my storage engine

        int _findIndexNumber( OperationContext* txn, StringData indexName) const;

    private:
        NamespaceDetails* _details;
        RecordStore* _namespacesRecordStore;
        RecordStore* _indexRecordStore;
        MMAPV1DatabaseCatalogEntry* _db;

        friend class MMAPV1DatabaseCatalogEntry;
    };
}
