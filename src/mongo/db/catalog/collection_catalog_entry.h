// collection_catalog_entry.h

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

#include "mongo/base/string_data.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/diskloc.h"
#include "mongo/db/namespace_string.h"

namespace mongo {

    class IndexDescriptor;
    class OperationContext;

    class CollectionCatalogEntry {
    public:
        CollectionCatalogEntry( const StringData& ns )
            : _ns( ns ){
        }
        virtual ~CollectionCatalogEntry(){}

        const NamespaceString& ns() const { return _ns; }

        // ------- indexes ----------

        virtual CollectionOptions getCollectionOptions( OperationContext* txn ) const = 0;

        virtual int getTotalIndexCount() const = 0;

        virtual int getCompletedIndexCount() const = 0;

        virtual int getMaxAllowedIndexes() const = 0;

        virtual void getAllIndexes( std::vector<std::string>* names ) const = 0;

        virtual BSONObj getIndexSpec( const StringData& idxName ) const = 0;

        virtual bool isIndexMultikey( const StringData& indexName) const = 0;

        virtual bool setIndexIsMultikey(OperationContext* txn,
                                        const StringData& indexName,
                                        bool multikey = true) = 0;

        virtual DiskLoc getIndexHead( const StringData& indexName ) const = 0;

        virtual void setIndexHead( OperationContext* txn,
                                   const StringData& indexName,
                                   const DiskLoc& newHead ) = 0;

        virtual bool isIndexReady( const StringData& indexName ) const = 0;

        virtual Status removeIndex( OperationContext* txn,
                                    const StringData& indexName ) = 0;

        virtual Status prepareForIndexBuild( OperationContext* txn,
                                             const IndexDescriptor* spec ) = 0;

        virtual void indexBuildSuccess( OperationContext* txn,
                                        const StringData& indexName ) = 0;

        /* Updates the expireAfterSeconds field of the given index to the value in newExpireSecs.
         * The specified index must already contain an expireAfterSeconds field, and the value in
         * that field and newExpireSecs must both be numeric.
         */
        virtual void updateTTLSetting( OperationContext* txn,
                                       const StringData& idxName,
                                       long long newExpireSeconds ) = 0;
    private:
        NamespaceString _ns;
    };

}
