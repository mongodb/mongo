// bson_collection_catalog_entry.cpp

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

#include "mongo/db/storage/bson_collection_catalog_entry.h"

namespace mongo {

    BSONCollectionCatalogEntry::BSONCollectionCatalogEntry( const StringData& ns )
        : CollectionCatalogEntry( ns ) {
    }

    CollectionOptions BSONCollectionCatalogEntry::getCollectionOptions( OperationContext* txn ) const {
        // TODO: support everything
        return CollectionOptions();
    }

    int BSONCollectionCatalogEntry::getTotalIndexCount( OperationContext* txn ) const {
        MetaData md = _getMetaData( txn );

        return static_cast<int>( md.indexes.size() );
    }

    int BSONCollectionCatalogEntry::getCompletedIndexCount( OperationContext* txn ) const {
        MetaData md = _getMetaData( txn );

        int num = 0;
        for ( unsigned i = 0; i < md.indexes.size(); i++ ) {
            if ( md.indexes[i].ready )
                num++;
        }
        return num;
    }

    BSONObj BSONCollectionCatalogEntry::getIndexSpec( OperationContext* txn,
                                                      const StringData& indexName ) const {
        MetaData md = _getMetaData( txn );

        int offset = md.findIndexOffset( indexName );
        invariant( offset >= 0 );
        return md.indexes[offset].spec.getOwned();
    }


    void BSONCollectionCatalogEntry::getAllIndexes( OperationContext* txn,
                                                    std::vector<std::string>* names ) const {
        MetaData md = _getMetaData( txn );

        for ( unsigned i = 0; i < md.indexes.size(); i++ ) {
            names->push_back( md.indexes[i].spec["name"].String() );
        }
    }

    bool BSONCollectionCatalogEntry::isIndexMultikey( OperationContext* txn,
                                                      const StringData& indexName) const {
        MetaData md = _getMetaData( txn );

        int offset = md.findIndexOffset( indexName );
        invariant( offset >= 0 );
        return md.indexes[offset].multikey;
    }

    DiskLoc BSONCollectionCatalogEntry::getIndexHead( OperationContext* txn,
                                                      const StringData& indexName ) const {
        MetaData md = _getMetaData( txn );

        int offset = md.findIndexOffset( indexName );
        invariant( offset >= 0 );
        return md.indexes[offset].head;
    }

    bool BSONCollectionCatalogEntry::isIndexReady( OperationContext* txn,
                                                   const StringData& indexName ) const {
        MetaData md = _getMetaData( txn );

        int offset = md.findIndexOffset( indexName );
        invariant( offset >= 0 );
        return md.indexes[offset].ready;
    }

    int BSONCollectionCatalogEntry::MetaData::findIndexOffset( const StringData& name ) const {
        for ( unsigned i = 0; i < indexes.size(); i++ )
            if ( indexes[i].spec["name"].String() == name )
                return i;
        return -1;
    }

    bool BSONCollectionCatalogEntry::MetaData::eraseIndex( const StringData& name ) {
        int indexOffset = findIndexOffset( name );

        if ( indexOffset < 0 ) {
            return false;
        }

        indexes.erase( indexes.begin() + indexOffset );
        return true;
    }

    BSONObj BSONCollectionCatalogEntry::MetaData::toBSON() const {
        BSONObjBuilder b;
        b.append( "ns", ns );
        {
            BSONArrayBuilder arr( b.subarrayStart( "indexes" ) );
            for ( unsigned i = 0; i < indexes.size(); i++ ) {
                BSONObjBuilder sub( arr.subobjStart() );
                sub.append( "spec", indexes[i].spec );
                sub.appendBool( "ready", indexes[i].ready );
                sub.appendBool( "multikey", indexes[i].multikey );
                sub.append( "head_a", indexes[i].head.a() );
                sub.append( "head_b", indexes[i].head.getOfs() );
                sub.done();
            }
            arr.done();
        }
        return b.obj();
    }

    void BSONCollectionCatalogEntry::MetaData::parse( const BSONObj& obj ) {
        ns = obj["ns"].valuestrsafe();

        BSONElement e = obj["indexes"];
        if ( e.isABSONObj() ) {
            std::vector<BSONElement> entries = e.Array();
            for ( unsigned i = 0; i < entries.size(); i++ ) {
                BSONObj idx = entries[i].Obj();
                IndexMetaData imd;
                imd.spec = idx["spec"].Obj().getOwned();
                imd.ready = idx["ready"].trueValue();
                imd.head = DiskLoc( idx["head_a"].Int(),
                                    idx["head_b"].Int() );
                imd.multikey = idx["multikey"].trueValue();
                indexes.push_back( imd );
            }
        }
    }
}
