// kv_collection_catalog_entry.h

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

#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/storage/bson_collection_catalog_entry.h"
#include "mongo/db/storage/record_store.h"

namespace mongo {

    class KVCatalog;
    class KVEngine;

    class KVCollectionCatalogEntry : public BSONCollectionCatalogEntry {
    public:
        KVCollectionCatalogEntry( KVEngine* engine,
                                  KVCatalog* catalog,
                                  const StringData& ns,
                                  const StringData& ident,
                                  RecordStore* rs );

        virtual ~KVCollectionCatalogEntry();

        virtual int getMaxAllowedIndexes() const { return 64; };

        virtual bool setIndexIsMultikey(OperationContext* txn,
                                        const StringData& indexName,
                                        bool multikey = true);

        virtual void setIndexHead( OperationContext* txn,
                                   const StringData& indexName,
                                   const DiskLoc& newHead );

        virtual Status removeIndex( OperationContext* txn,
                                    const StringData& indexName );

        virtual Status prepareForIndexBuild( OperationContext* txn,
                                             const IndexDescriptor* spec );

        virtual void indexBuildSuccess( OperationContext* txn,
                                        const StringData& indexName );

        /* Updates the expireAfterSeconds field of the given index to the value in newExpireSecs.
         * The specified index must already contain an expireAfterSeconds field, and the value in
         * that field and newExpireSecs must both be numeric.
         */
        virtual void updateTTLSetting( OperationContext* txn,
                                       const StringData& idxName,
                                       long long newExpireSeconds );

        RecordStore* getRecordStore() { return _recrodStore.get(); }

    protected:
        virtual MetaData _getMetaData( OperationContext* txn ) const;

    private:
        KVEngine* _engine; // not owned
        KVCatalog* _catalog; // not owned
        std::string _ident;
        boost::scoped_ptr<RecordStore> _recrodStore; // owned
    };

}
