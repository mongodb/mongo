// wiredtiger_database_catalog_entry.cpp

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

#include "mongo/db/storage/wiredtiger/wiredtiger_database_catalog_entry.h"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/index/2d_access_method.h"
#include "mongo/db/index/btree_access_method.h"
#include "mongo/db/index/fts_access_method.h"
#include "mongo/db/index/hash_access_method.h"
#include "mongo/db/index/haystack_access_method.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/s2_access_method.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_index.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"

#include "mongo/db/storage_options.h"

namespace mongo {

    WiredTigerDatabaseCatalogEntry::WiredTigerDatabaseCatalogEntry( const StringData& name, WiredTigerDatabase &db )
        : DatabaseCatalogEntry( name ), _db(db) {
            // If the catalog hasn't been created, nothing to do.
            boost::filesystem::path dbpath =
                boost::filesystem::path(storageGlobalParams.dbpath) / name.toString();
            if ( !boost::filesystem::exists( dbpath ) )
                return;

            initCollectionNamespaces();
    }

    WiredTigerDatabaseCatalogEntry::~WiredTigerDatabaseCatalogEntry() {
        for ( EntryMap::const_iterator i = _entryMap.begin(); i != _entryMap.end(); ++i ) {
            delete i->second;
        }
        _entryMap.clear();
        delete &_db;
    }

    bool WiredTigerDatabaseCatalogEntry::isEmpty() const {
        boost::mutex::scoped_lock lk( _entryMapLock );
        return _entryMap.empty();
    }

    void WiredTigerDatabaseCatalogEntry::appendExtraStats( OperationContext* opCtx,
                                                      BSONObjBuilder* out,
                                                      double scale ) const {
    }

    CollectionCatalogEntry* WiredTigerDatabaseCatalogEntry::getCollectionCatalogEntry( OperationContext* opCtx,
                                                                                  const StringData& ns ) const {
        boost::mutex::scoped_lock lk( _entryMapLock );
        EntryMap::const_iterator i = _entryMap.find( ns.toString() );
        if ( i == _entryMap.end() )
            return NULL;
        return i->second;
    }

    RecordStore* WiredTigerDatabaseCatalogEntry::getRecordStore( OperationContext* opCtx,
                                                            const StringData& ns ) {
        boost::mutex::scoped_lock lk( _entryMapLock );
        EntryMap::iterator i = _entryMap.find( ns.toString() );
        if ( i == _entryMap.end() )
            return NULL;
        return i->second->rs.get();
    }

    void WiredTigerDatabaseCatalogEntry::initCollectionNamespaces() {
        int ret;
        const char *key;
        WT_SESSION *session = _db.GetSession();
        boost::mutex::scoped_lock lk( _entryMapLock );

        /* Only do this once. */
        if (!_entryMap.empty())
            return;

        WT_CURSOR *c;
        ret = session->open_cursor(session, "metadata:", NULL, NULL, &c);
        invariant(ret == 0);
        while ((ret = c->next(c)) == 0) {
            ret = c->get_key(c, &key);
            invariant(ret == 0);
            if (strcmp("metadata:", key) == 0)
                continue;
            if (strncmp("table:", key, 6) != 0)
                continue;
            if (strstr(key, "_idx") != NULL)
                continue;

            // Move the pointer past table:NAME.
            key = key + 6;
            // TODO: retrieve options? Filter indexes? manage memory?
            CollectionOptions *options = new CollectionOptions();
            Entry *entry = new Entry(mongo::StringData(key), *options);
            entry->rs.reset(new WiredTigerRecordStore(key, _db));

            _entryMap[key] = entry;
        }
        invariant(ret == WT_NOTFOUND);
        _db.ReleaseSession(session);
        name();
    }

    void WiredTigerDatabaseCatalogEntry::getCollectionNamespaces( std::list<std::string>* out ) const {
        boost::mutex::scoped_lock lk( _entryMapLock );
        for ( EntryMap::const_iterator i = _entryMap.begin(); i != _entryMap.end(); ++i ) {
            out->push_back( i->first );
        }
    }

    Status WiredTigerDatabaseCatalogEntry::createCollection( OperationContext* opCtx,
                                                        const StringData& ns,
                                                        const CollectionOptions& options,
                                                        bool allocateDefaultSpace ) {
        initCollectionNamespaces();
        dynamic_cast<WiredTigerRecoveryUnit*>( opCtx->recoveryUnit() )->rollbackPossible = false;
        boost::mutex::scoped_lock lk( _entryMapLock );
        Entry*& entry = _entryMap[ ns.toString() ];
        if ( entry )
            return Status( ErrorCodes::NamespaceExists,
                           "cannot create collection, already exists" );

	int ret = WiredTigerRecordStore::Create(_db, ns, options, allocateDefaultSpace);
	invariant(ret == 0);

        entry = new Entry( ns, options );

        if ( options.capped ) {
            entry->rs.reset(new WiredTigerRecordStore(ns,
				                _db,
                                                true,
                                                options.cappedSize
                                                     ? options.cappedSize : 4096, // default size
                                                options.cappedMaxDocs
                                                     ? options.cappedMaxDocs : -1)); // no limit
        } else {
            entry->rs.reset( new WiredTigerRecordStore( ns, _db ) );
        }
        _entryMap[ns.toString()] = entry;

        return Status::OK();
    }

    Status WiredTigerDatabaseCatalogEntry::dropCollection( OperationContext* opCtx,
                                                      const StringData& ns ) {
        //TODO: invariant( opCtx->lockState()->isWriteLocked( ns ) );

        dynamic_cast<WiredTigerRecoveryUnit*>( opCtx->recoveryUnit() )->rollbackPossible = false;
        boost::mutex::scoped_lock lk( _entryMapLock );
        EntryMap::iterator i = _entryMap.find( ns.toString() );

        if ( i == _entryMap.end() )
            return Status( ErrorCodes::NamespaceNotFound, "namespace not found" );

        delete i->second;
        _entryMap.erase( i );

        return Status::OK();
    }


    IndexAccessMethod* WiredTigerDatabaseCatalogEntry::getIndex( OperationContext* txn,
                                                            const CollectionCatalogEntry* collection,
                                                            IndexCatalogEntry* index ) {
        const Entry* entry = dynamic_cast<const Entry*>( collection );

        Entry::Indexes::const_iterator i = entry->indexes.find( index->descriptor()->indexName() );
        if ( i == entry->indexes.end() ) {
            // index doesn't exist
            return NULL;
        }

        const string& type = index->descriptor()->getAccessMethodName();

        // Need the Head to be non-Null to avoid asserts. TODO remove the asserts.
        index->headManager()->setHead(txn, DiskLoc(0xDEAD, 0xBEAF));

        std::auto_ptr<SortedDataInterface> wtidx(getWiredTigerIndex(_db, collection->ns().ns(), index->descriptor()->indexName(), *index, &i->second->data));

        if ("" == type)
            return new BtreeAccessMethod( index, wtidx.release() );

        if (IndexNames::HASHED == type)
            return new HashAccessMethod( index, wtidx.release() );

        if (IndexNames::GEO_2DSPHERE == type)
            return new S2AccessMethod( index, wtidx.release() );

        if (IndexNames::TEXT == type)
            return new FTSAccessMethod( index, wtidx.release() );

        if (IndexNames::GEO_HAYSTACK == type)
            return new HaystackAccessMethod( index, wtidx.release() );

        if (IndexNames::GEO_2D == type)
            return new TwoDAccessMethod( index, wtidx.release() );

        log() << "Can't find index for keyPattern " << index->descriptor()->keyPattern();
        fassertFailed(28518);
    }

    Status WiredTigerDatabaseCatalogEntry::renameCollection( OperationContext* txn,
                                                        const StringData& fromNS,
                                                        const StringData& toNS,
                                                        bool stayTemp ) {
        invariant( false );
    }

    // ------------------

    WiredTigerDatabaseCatalogEntry::Entry::Entry( const StringData& ns, const CollectionOptions& o )
        : CollectionCatalogEntry( ns ), options( o ) {
    }

    WiredTigerDatabaseCatalogEntry::Entry::~Entry() {
        for ( Indexes::const_iterator i = indexes.begin(); i != indexes.end(); ++i )
            delete i->second;
        indexes.clear();
    }

    int WiredTigerDatabaseCatalogEntry::Entry::getTotalIndexCount() const {
        return static_cast<int>( indexes.size() );
    }

    int WiredTigerDatabaseCatalogEntry::Entry::getCompletedIndexCount() const {
        int ready = 0;
        for ( Indexes::const_iterator i = indexes.begin(); i != indexes.end(); ++i )
            if ( i->second->ready )
                ready++;
        return ready;
    }

    void WiredTigerDatabaseCatalogEntry::Entry::getAllIndexes( std::vector<std::string>* names ) const {
        for ( Indexes::const_iterator i = indexes.begin(); i != indexes.end(); ++i )
            names->push_back( i->second->name );
    }

    BSONObj WiredTigerDatabaseCatalogEntry::Entry::getIndexSpec( const StringData& idxName ) const {
        Indexes::const_iterator i = indexes.find( idxName.toString() );
        invariant( i != indexes.end() );
        return i->second->spec; 
    }

    bool WiredTigerDatabaseCatalogEntry::Entry::isIndexMultikey( const StringData& idxName) const {
        Indexes::const_iterator i = indexes.find( idxName.toString() );
        invariant( i != indexes.end() );
        return i->second->isMultikey;
    }

    bool WiredTigerDatabaseCatalogEntry::Entry::setIndexIsMultikey(OperationContext* txn,
                                                              const StringData& idxName,
                                                              bool multikey ) {
        Indexes::const_iterator i = indexes.find( idxName.toString() );
        invariant( i != indexes.end() );
        if (i->second->isMultikey == multikey)
            return false;

        i->second->isMultikey = multikey;
        return true;
    }

    DiskLoc WiredTigerDatabaseCatalogEntry::Entry::getIndexHead( const StringData& idxName ) const {
        Indexes::const_iterator i = indexes.find( idxName.toString() );
        invariant( i != indexes.end() );
        return i->second->head;
    }

    void WiredTigerDatabaseCatalogEntry::Entry::setIndexHead( OperationContext* txn,
                                                         const StringData& idxName,
                                                         const DiskLoc& newHead ) {
        Indexes::const_iterator i = indexes.find( idxName.toString() );
        invariant( i != indexes.end() );
        i->second->head = newHead;
    }

    bool WiredTigerDatabaseCatalogEntry::Entry::isIndexReady( const StringData& idxName ) const {
        Indexes::const_iterator i = indexes.find( idxName.toString() );
        invariant( i != indexes.end() );
        return i->second->ready;
    }

    Status WiredTigerDatabaseCatalogEntry::Entry::removeIndex( OperationContext* txn,
                                                          const StringData& idxName ) {
        indexes.erase( idxName.toString() );
        return Status::OK();
    }

    Status WiredTigerDatabaseCatalogEntry::Entry::prepareForIndexBuild( OperationContext* txn,
                                                                   const IndexDescriptor* spec ) {
        auto_ptr<IndexEntry> newEntry( new IndexEntry() );
        newEntry->name = spec->indexName();
        newEntry->spec = spec->infoObj();
        newEntry->ready = false;
        newEntry->isMultikey = false;

        indexes[spec->indexName()] = newEntry.release();
        return Status::OK();
    }

    void WiredTigerDatabaseCatalogEntry::Entry::indexBuildSuccess( OperationContext* txn,
                                                              const StringData& idxName ) {
        Indexes::const_iterator i = indexes.find( idxName.toString() );
        invariant( i != indexes.end() );
        i->second->ready = true;
    }

    void WiredTigerDatabaseCatalogEntry::Entry::updateTTLSetting( OperationContext* txn,
                                                             const StringData& idxName,
                                                             long long newExpireSeconds ) {
        invariant( false );
    }

}
