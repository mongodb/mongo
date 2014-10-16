// kv_database_catalog_entry.cpp

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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/db/storage/kv/kv_database_catalog_entry.h"

#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/index/2d_access_method.h"
#include "mongo/db/index/btree_access_method.h"
#include "mongo/db/index/fts_access_method.h"
#include "mongo/db/index/hash_access_method.h"
#include "mongo/db/index/haystack_access_method.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/s2_access_method.h"
#include "mongo/db/storage/kv/kv_collection_catalog_entry.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/kv/kv_storage_engine.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {
    KVDatabaseCatalogEntry::KVDatabaseCatalogEntry( const StringData& db, KVStorageEngine* engine )
        : DatabaseCatalogEntry( db ), _engine( engine ) {

    }

    KVDatabaseCatalogEntry::~KVDatabaseCatalogEntry() {
        for ( CollectionMap::const_iterator it = _collections.begin(); it != _collections.end(); ++it ) {
            delete it->second;
        }
        _collections.clear();
    }

    bool KVDatabaseCatalogEntry::exists() const {
        return !isEmpty();
    }

    bool KVDatabaseCatalogEntry::isEmpty() const {
        boost::mutex::scoped_lock lk( _collectionsLock );
        return _collections.empty();
    }

    int64_t KVDatabaseCatalogEntry::sizeOnDisk( OperationContext* opCtx ) const {
        int64_t size = 0;

        boost::mutex::scoped_lock lk( _collectionsLock );
        for ( CollectionMap::const_iterator it = _collections.begin(); it != _collections.end(); ++it ) {
            const KVCollectionCatalogEntry* coll = it->second;
            size += coll->getRecordStore()->storageSize( opCtx );
            // todo: indexes
        }

        return size;
    }

    void KVDatabaseCatalogEntry::appendExtraStats( OperationContext* opCtx,
                                                   BSONObjBuilder* out,
                                                   double scale ) const {
        // todo
    }

    bool KVDatabaseCatalogEntry::currentFilesCompatible( OperationContext* opCtx ) const {
        // todo
        return true;
    }

    void KVDatabaseCatalogEntry::getCollectionNamespaces( std::list<std::string>* out ) const {
        boost::mutex::scoped_lock lk( _collectionsLock );
        for ( CollectionMap::const_iterator it = _collections.begin(); it != _collections.end(); ++it ) {
            out->push_back( it->first );
        }
    }

    CollectionCatalogEntry* KVDatabaseCatalogEntry::getCollectionCatalogEntry( OperationContext* txn,
                                                                               const StringData& ns ) const {
        boost::mutex::scoped_lock lk( _collectionsLock );
        CollectionMap::const_iterator it = _collections.find( ns.toString() );
        if ( it == _collections.end() )
            return NULL;
        return it->second;
    }

    RecordStore* KVDatabaseCatalogEntry::getRecordStore( OperationContext* txn,
                                                         const StringData& ns ) {
        boost::mutex::scoped_lock lk( _collectionsLock );
        CollectionMap::const_iterator it = _collections.find( ns.toString() );
        if ( it == _collections.end() )
            return NULL;
        return it->second->getRecordStore();
    }

    IndexAccessMethod* KVDatabaseCatalogEntry::getIndex( OperationContext* txn,
                                                         const CollectionCatalogEntry* collection,
                                                         IndexCatalogEntry* index ) {
        IndexDescriptor* desc = index->descriptor();

        const string& type = desc->getAccessMethodName();

        string ident = _engine->getCatalog()->getIndexIdent( txn,
                                                             collection->ns().ns(),
                                                             desc->indexName() );

        SortedDataInterface* sdi =
            _engine->getEngine()->getSortedDataInterface( txn, ident, desc );

        if ("" == type)
            return new BtreeAccessMethod( index, sdi );

        if (IndexNames::HASHED == type)
            return new HashAccessMethod( index, sdi );

        if (IndexNames::GEO_2DSPHERE == type)
            return new S2AccessMethod( index, sdi );

        if (IndexNames::TEXT == type)
            return new FTSAccessMethod( index, sdi );

        if (IndexNames::GEO_HAYSTACK == type)
            return new HaystackAccessMethod( index, sdi );

        if (IndexNames::GEO_2D == type)
            return new TwoDAccessMethod( index, sdi );

        log() << "Can't find index for keyPattern " << desc->keyPattern();
        invariant( false );
    }

    Status KVDatabaseCatalogEntry::createCollection( OperationContext* txn,
                                                     const StringData& ns,
                                                     const CollectionOptions& options,
                                                     bool allocateDefaultSpace ) {
        // we assume there is a logical lock on the collection name above

        {
            boost::mutex::scoped_lock lk( _collectionsLock );
            if ( _collections[ns.toString()] ) {
                return Status( ErrorCodes::NamespaceExists,
                               "collection already exists" );
            }
        }

        // need to create it
        Status status = _engine->getCatalog()->newCollection( txn, ns, options );
        if ( !status.isOK() )
            return status;

        string ident = _engine->getCatalog()->getCollectionIdent( ns );

        status = _engine->getEngine()->createRecordStore( txn, ident, options );
        if ( !status.isOK() )
            return status;

        RecordStore* rs = _engine->getEngine()->getRecordStore( txn, ns, ident, options );
        invariant( rs );
        boost::mutex::scoped_lock lk( _collectionsLock );
        _collections[ns.toString()] =
            new KVCollectionCatalogEntry( _engine->getEngine(), _engine->getCatalog(),
                                          ns, ident, rs );

        return Status::OK();
    }

    void KVDatabaseCatalogEntry::initCollection( OperationContext* opCtx,
                                                 const std::string& ns ) {
        string ident = _engine->getCatalog()->getCollectionIdent( ns );
        BSONCollectionCatalogEntry::MetaData md = _engine->getCatalog()->getMetaData( opCtx, ns );

        RecordStore* rs = _engine->getEngine()->getRecordStore( opCtx, ns, ident, md.options );
        invariant( rs );

        boost::mutex::scoped_lock lk( _collectionsLock );
        invariant( !_collections[ns] );
        _collections[ns] = new KVCollectionCatalogEntry( _engine->getEngine(),
                                                         _engine->getCatalog(),
                                                         ns,
                                                         ident,
                                                         rs );
    }

    Status KVDatabaseCatalogEntry::renameCollection( OperationContext* txn,
                                                     const StringData& fromNS,
                                                     const StringData& toNS,
                                                     bool stayTemp ) {
        {
            boost::mutex::scoped_lock lk( _collectionsLock );
            CollectionMap::const_iterator it = _collections.find( fromNS.toString() );
            if ( it == _collections.end() )
                return Status( ErrorCodes::NamespaceNotFound, "rename cannot find collection" );
            KVCollectionCatalogEntry* fromEntry = it->second;

            it = _collections.find( toNS.toString() );
            if ( it != _collections.end() )
                return Status( ErrorCodes::NamespaceExists, "for rename to already exists" );

            _collections.erase( fromNS.toString() );
            delete fromEntry;
        }

        Status status = _engine->getCatalog()->renameCollection( txn, fromNS, toNS, stayTemp );
        if ( !status.isOK() )
            return status;

        string ident = _engine->getCatalog()->getCollectionIdent( toNS );
        BSONCollectionCatalogEntry::MetaData md = _engine->getCatalog()->getMetaData( txn, toNS );
        RecordStore* rs = _engine->getEngine()->getRecordStore( txn, toNS, ident, md.options );

        boost::mutex::scoped_lock lk( _collectionsLock );
        _collections[toNS.toString()] =
            new KVCollectionCatalogEntry( _engine->getEngine(), _engine->getCatalog(),
                                          toNS, ident, rs );

        return Status::OK();
    }

    Status KVDatabaseCatalogEntry::dropCollection( OperationContext* opCtx,
                                                   const StringData& ns ) {
        KVCollectionCatalogEntry* entry;
        {
            boost::mutex::scoped_lock lk( _collectionsLock );
            CollectionMap::const_iterator it = _collections.find( ns.toString() );
            if ( it == _collections.end() )
                return Status( ErrorCodes::NamespaceNotFound, "cannnot find collection to drop" );
            entry = it->second;
        }

        invariant( entry->getTotalIndexCount( opCtx ) == entry->getCompletedIndexCount( opCtx ) );
        {
            std::vector<std::string> indexNames;
            entry->getAllIndexes( opCtx, &indexNames );
            for ( size_t i = 0; i < indexNames.size(); i++ ) {
                entry->removeIndex( opCtx, indexNames[i] );
            }
        }
        invariant( entry->getTotalIndexCount( opCtx ) == 0 );

        string ident = _engine->getCatalog()->getCollectionIdent( ns );

        boost::mutex::scoped_lock lk( _collectionsLock );

        Status status = _engine->getEngine()->dropRecordStore( opCtx, ident );
        if ( !status.isOK() )
            return status;

        status = _engine->getCatalog()->dropCollection( opCtx, ns );
        if ( !status.isOK() )
            return status;

        _collections.erase( ns.toString() );

        return Status::OK();
    }

}
