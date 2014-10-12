// kv_collection_catalog_entry.cpp

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

#include "mongo/db/storage/kv/kv_collection_catalog_entry.h"

#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/storage/kv/kv_catalog.h"
#include "mongo/db/storage/kv/kv_engine.h"

namespace mongo {

    KVCollectionCatalogEntry::KVCollectionCatalogEntry( KVEngine* engine,
                                                        KVCatalog* catalog,
                                                        const StringData& ns,
                                                        const StringData& ident,
                                                        RecordStore* rs)
        : BSONCollectionCatalogEntry( ns ),
          _engine( engine ),
          _catalog( catalog ),
          _ident( ident.toString() ),
          _recordStore( rs ) {
    }

    KVCollectionCatalogEntry::~KVCollectionCatalogEntry() {
    }

    bool KVCollectionCatalogEntry::setIndexIsMultikey(OperationContext* txn,
                                                      const StringData& indexName,
                                                      bool multikey ) {
        MetaData md = _getMetaData(txn);

        int offset = md.findIndexOffset( indexName );
        invariant( offset >= 0 );
        if ( md.indexes[offset].multikey == multikey )
            return false;
        md.indexes[offset].multikey = multikey;
        _catalog->putMetaData( txn, ns().toString(), md );
        return true;
    }

    void KVCollectionCatalogEntry::setIndexHead( OperationContext* txn,
                                                 const StringData& indexName,
                                                 const DiskLoc& newHead ) {
        MetaData md = _getMetaData( txn );
        int offset = md.findIndexOffset( indexName );
        invariant( offset >= 0 );
        md.indexes[offset].head = newHead;
        _catalog->putMetaData( txn,  ns().toString(), md );
    }

    Status KVCollectionCatalogEntry::removeIndex( OperationContext* txn,
                                                  const StringData& indexName ) {
        string ident = _catalog->getIndexIdent( txn, ns().ns(), indexName );

        MetaData md = _getMetaData( txn );
        md.eraseIndex( indexName );
        _catalog->putMetaData( txn, ns().toString(), md );

        return _engine->dropSortedDataInterface( txn, ident );
    }

    Status KVCollectionCatalogEntry::prepareForIndexBuild( OperationContext* txn,
                                                           const IndexDescriptor* spec ) {
        MetaData md = _getMetaData( txn );
        md.indexes.push_back( IndexMetaData( spec->infoObj(), false, DiskLoc(), false ) );
        _catalog->putMetaData( txn, ns().toString(), md );

        string ident = _catalog->getIndexIdent( txn, ns().ns(), spec->indexName() );

        return _engine->createSortedDataInterface( txn, ident, spec );
    }

    void KVCollectionCatalogEntry::indexBuildSuccess( OperationContext* txn,
                                                      const StringData& indexName ) {
        MetaData md = _getMetaData( txn );
        int offset = md.findIndexOffset( indexName );
        invariant( offset >= 0 );
        md.indexes[offset].ready = true;
        _catalog->putMetaData( txn, ns().toString(), md );
    }

    void KVCollectionCatalogEntry::updateTTLSetting( OperationContext* txn,
                                                     const StringData& idxName,
                                                     long long newExpireSeconds ) {
        MetaData md = _getMetaData( txn );
        int offset = md.findIndexOffset( idxName );
        invariant( offset >= 0 );
        md.indexes[offset].updateTTLSetting( newExpireSeconds );
        _catalog->putMetaData( txn, ns().toString(), md );
    }

    BSONCollectionCatalogEntry::MetaData KVCollectionCatalogEntry::_getMetaData( OperationContext* txn ) const {
        return _catalog->getMetaData( txn, ns().toString() );
    }

}
