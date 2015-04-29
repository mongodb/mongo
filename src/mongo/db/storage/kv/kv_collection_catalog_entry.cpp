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

    using std::string;

    class KVCollectionCatalogEntry::AddIndexChange : public RecoveryUnit::Change {
    public:
        AddIndexChange(OperationContext* opCtx, KVCollectionCatalogEntry* cce,
                       StringData ident)
            : _opCtx(opCtx)
            , _cce(cce)
            , _ident(ident.toString())
        {}

        virtual void commit() {}
        virtual void rollback() {
            // Intentionally ignoring failure.
            _cce->_engine->dropIdent(_opCtx, _ident);
        }

        OperationContext* const _opCtx;
        KVCollectionCatalogEntry* const _cce;
        const std::string _ident;
    };

    class KVCollectionCatalogEntry::RemoveIndexChange : public RecoveryUnit::Change {
    public:
        RemoveIndexChange(OperationContext* opCtx, KVCollectionCatalogEntry* cce,
                          StringData ident)
            : _opCtx(opCtx)
            , _cce(cce)
            , _ident(ident.toString())
        {}

        virtual void rollback() {}
        virtual void commit() {
            // Intentionally ignoring failure here. Since we've removed the metadata pointing to the
            // index, we should never see it again anyway.
            _cce->_engine->dropIdent(_opCtx, _ident);
        }

        OperationContext* const _opCtx;
        KVCollectionCatalogEntry* const _cce;
        const std::string _ident;
    };


    KVCollectionCatalogEntry::KVCollectionCatalogEntry( KVEngine* engine,
                                                        KVCatalog* catalog,
                                                        StringData ns,
                                                        StringData ident,
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
                                                      StringData indexName,
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
                                                 StringData indexName,
                                                 const RecordId& newHead ) {
        MetaData md = _getMetaData( txn );
        int offset = md.findIndexOffset( indexName );
        invariant( offset >= 0 );
        md.indexes[offset].head = newHead;
        _catalog->putMetaData( txn,  ns().toString(), md );
    }

    Status KVCollectionCatalogEntry::removeIndex( OperationContext* txn,
                                                  StringData indexName ) {
        MetaData md = _getMetaData( txn );
        
        if (md.findIndexOffset(indexName) < 0)
            return Status::OK(); // never had the index so nothing to do.

        const string ident = _catalog->getIndexIdent( txn, ns().ns(), indexName );

        md.eraseIndex( indexName );
        _catalog->putMetaData( txn, ns().toString(), md );

        // Lazily remove to isolate underlying engine from rollback.
        txn->recoveryUnit()->registerChange(new RemoveIndexChange(txn, this, ident));
        return Status::OK();
    }

    Status KVCollectionCatalogEntry::prepareForIndexBuild( OperationContext* txn,
                                                           const IndexDescriptor* spec ) {
        MetaData md = _getMetaData( txn );
        md.indexes.push_back( IndexMetaData( spec->infoObj(), false, RecordId(), false ) );
        _catalog->putMetaData( txn, ns().toString(), md );

        string ident = _catalog->getIndexIdent( txn, ns().ns(), spec->indexName() );

        const Status status = _engine->createSortedDataInterface( txn, ident, spec );
        if (status.isOK()) {
            txn->recoveryUnit()->registerChange(new AddIndexChange(txn, this, ident));
        }

        return status;
    }

    void KVCollectionCatalogEntry::indexBuildSuccess( OperationContext* txn,
                                                      StringData indexName ) {
        MetaData md = _getMetaData( txn );
        int offset = md.findIndexOffset( indexName );
        invariant( offset >= 0 );
        md.indexes[offset].ready = true;
        _catalog->putMetaData( txn, ns().toString(), md );
    }

    void KVCollectionCatalogEntry::updateTTLSetting( OperationContext* txn,
                                                     StringData idxName,
                                                     long long newExpireSeconds ) {
        MetaData md = _getMetaData( txn );
        int offset = md.findIndexOffset( idxName );
        invariant( offset >= 0 );
        md.indexes[offset].updateTTLSetting( newExpireSeconds );
        _catalog->putMetaData( txn, ns().toString(), md );
    }

    void KVCollectionCatalogEntry::updateFlags(OperationContext* txn, int newValue) {
        MetaData md = _getMetaData( txn );
        md.options.flags = newValue;
        md.options.flagsSet = true;
        _catalog->putMetaData( txn, ns().toString(), md );
    }

    void KVCollectionCatalogEntry::updateValidator(OperationContext* txn,
                                                   const BSONObj& validator) {
        MetaData md = _getMetaData(txn);
        md.options.validator = validator;
        _catalog->putMetaData(txn, ns().toString(), md);
    }

    BSONCollectionCatalogEntry::MetaData KVCollectionCatalogEntry::_getMetaData( OperationContext* txn ) const {
        return _catalog->getMetaData( txn, ns().toString() );
    }

}
