// wiredtiger_database_catalog_entry.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *    Copyright (C) 2014 WiredTiger Inc.
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
#include "mongo/db/json.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_collection_catalog_entry.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_index.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"

#include "mongo/db/storage_options.h"
#include "mongo/util/log.h"

namespace mongo {

    WiredTigerDatabaseCatalogEntry::WiredTigerDatabaseCatalogEntry(
                WiredTigerDatabase &db,
                const StringData& name )
        : DatabaseCatalogEntry( name ), _db(db) {

            _loadAllCollections();
    }

    WiredTigerDatabaseCatalogEntry::~WiredTigerDatabaseCatalogEntry() {
        for ( EntryMap::const_iterator i = _entryMap.begin(); i != _entryMap.end(); ++i ) {
            delete i->second;
        }
        _entryMap.clear();
    }

    bool WiredTigerDatabaseCatalogEntry::isEmpty() const {
        boost::mutex::scoped_lock lk( _entryMapLock );
        return _entryMap.empty();
    }

    void WiredTigerDatabaseCatalogEntry::appendExtraStats( OperationContext* txn,
                                                      BSONObjBuilder* out,
                                                      double scale ) const {
    }

    CollectionCatalogEntry* WiredTigerDatabaseCatalogEntry::getCollectionCatalogEntry(
            OperationContext* txn,
            const StringData& ns ) const {
        boost::mutex::scoped_lock lk( _entryMapLock );
        EntryMap::const_iterator i = _entryMap.find( ns.toString() );
        if ( i == _entryMap.end() )
            return NULL;
        return i->second;
    }

    RecordStore* WiredTigerDatabaseCatalogEntry::getRecordStore( OperationContext* txn,
                                                            const StringData& ns ) {
        boost::mutex::scoped_lock lk( _entryMapLock );
        EntryMap::iterator i = _entryMap.find( ns.toString() );
        if ( i == _entryMap.end() )
            return NULL;
        return i->second->rs.get();
    }

    void WiredTigerDatabaseCatalogEntry::_loadAllCollections( ) {
        boost::mutex::scoped_lock lk( _entryMapLock );

        /* Only do this once. */
        if (!_entryMap.empty())
            return;

        WiredTigerSession swrap(_db);
        WiredTigerCursor cursor("metadata:", swrap);
        WT_CURSOR *c = cursor.Get();
        invariant(c != NULL);

        int ret;
        const char *key;
        std::string table_prefix = "table:" + name() + ".";
        while ((ret = c->next(c)) == 0) {
            ret = c->get_key(c, &key);
            invariant(ret == 0);
            if (strcmp("metadata:", key) == 0)
                continue;
            if (strncmp(table_prefix.c_str(), key, table_prefix.length()) != 0)
                continue;
            if (strstr(key, ".$") != NULL)
                continue;

            // Initialize the namespace we found
            std::string name = WiredTigerRecordStore::_fromURI(key);
            WiredTigerCollectionCatalogEntry *entry =
                new WiredTigerCollectionCatalogEntry(swrap, StringData(name));
            _entryMap[name.c_str()] = entry;
            //_loadCollection(swrap, key);
        }
        invariant(ret == WT_NOTFOUND || ret == 0);
        name();
    }

    void WiredTigerDatabaseCatalogEntry::getCollectionNamespaces(
            std::list<std::string>* out ) const {
        boost::mutex::scoped_lock lk( _entryMapLock );
        for ( EntryMap::const_iterator i = _entryMap.begin(); i != _entryMap.end(); ++i ) {
            out->push_back( i->first );
        }
    }

    Status WiredTigerDatabaseCatalogEntry::createCollection( OperationContext* txn,
                                                        const StringData& ns,
                                                        const CollectionOptions& options,
                                                        bool allocateDefaultSpace ) {
        boost::mutex::scoped_lock lk( _entryMapLock );
        WiredTigerCollectionCatalogEntry*& entry = _entryMap[ ns.toString() ];
        if ( entry )
            return Status( ErrorCodes::NamespaceExists,
                           "cannot create collection, already exists" );

        int ret = WiredTigerRecordStore::Create(_db, ns, options, allocateDefaultSpace);
        invariant(ret == 0);

        entry = new WiredTigerCollectionCatalogEntry( ns, options );

        WiredTigerRecordStore *rs = new WiredTigerRecordStore( ns, _db );
        if ( options.capped )
            rs->setCapped(options.cappedSize ? options.cappedSize : 4096,
                options.cappedMaxDocs ? options.cappedMaxDocs : -1);
        entry->rs.reset( rs );

        _entryMap[ns.toString()] = entry;

        return Status::OK();
    }

    Status WiredTigerDatabaseCatalogEntry::dropAllCollections( OperationContext* txn ) {
        // Keep setting the iterator to the beginning since drop is removing entries
        // as it goes along
        for ( EntryMap::const_iterator i = _entryMap.begin();
            i != _entryMap.end(); i = _entryMap.begin() ) {
            Status status = dropCollection(txn, i->first);
            if (!status.isOK()) 
                return status;
        }
        return Status::OK();
    }

    Status WiredTigerDatabaseCatalogEntry::dropCollection( OperationContext* txn,
                                                      const StringData& ns ) {
        //TODO: invariant( txn->lockState()->isWriteLocked( ns ) );

        boost::mutex::scoped_lock lk( _entryMapLock );
        EntryMap::iterator i = _entryMap.find( ns.toString() );

        if ( i == _entryMap.end() ) {
            return Status( ErrorCodes::NamespaceNotFound, "namespace not found" );
        }

        WiredTigerCollectionCatalogEntry *entry = i->second;
        // Drop the underlying table from WiredTiger
        // XXX use a temporary session for drops: WiredTiger doesn't allow transactional drops
        // and can't deal with rolling back a drop.
        WiredTigerSession &swrap_real = WiredTigerRecoveryUnit::Get(txn).GetSession();
        WiredTigerSession swrap(swrap_real.GetDatabase());

        // Close any cached cursors.
        swrap_real.GetContext().CloseAllCursors();
        swrap.GetContext().CloseAllCursors();

        WT_SESSION *session = swrap.Get();
        int ret = session->drop(session, WiredTigerRecordStore::_getURI(ns).c_str(), "force");
        if (ret != 0)
            return Status( ErrorCodes::OperationFailed, "Collection drop failed" );

        std::vector<std::string> names;
        entry->getAllIndexes( txn, &names );
        std::vector<std::string>::const_iterator idx;
        for (idx = names.begin(); idx != names.end(); ++idx) {
            Status s = entry->removeIndex(txn, StringData(*idx));
            invariant(s.isOK());
        }
        delete entry;
        _entryMap.erase( i );

        return Status::OK();
    }


    IndexAccessMethod* WiredTigerDatabaseCatalogEntry::getIndex( OperationContext* txn,
                        const CollectionCatalogEntry* collection,
                        IndexCatalogEntry* index ) {
        const WiredTigerCollectionCatalogEntry* entry =
            dynamic_cast<const WiredTigerCollectionCatalogEntry*>( collection );

        if ( !entry->indexExists( index->descriptor()->indexName() ) ) {
            // index doesn't exist
            return NULL;
        }

        const string& type = index->descriptor()->getAccessMethodName();

        // Need the Head to be non-Null to avoid asserts. TODO remove the asserts.
        index->headManager()->setHead(txn, DiskLoc(0xDEAD, 0xBEAF));

        std::auto_ptr<SortedDataInterface> wtidx(getWiredTigerIndex(
                _db, collection->ns().ns(),
                index->descriptor()->indexName(),
                *index));

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
        int ret;
        boost::mutex::scoped_lock lk( _entryMapLock );
        EntryMap::iterator i = _entryMap.find( toNS.toString() );
        if ( i != _entryMap.end() )
            return Status( ErrorCodes::NamespaceNotFound, "to namespace already exists for rename" );

        i = _entryMap.find( fromNS.toString() );
        if ( i == _entryMap.end() )
            return Status( ErrorCodes::NamespaceNotFound, "namespace not found for rename" );

        WiredTigerCollectionCatalogEntry *entry = i->second;

        // Remove the entry from the entryMap
        _entryMap.erase( i );

        // XXX use a temporary session for renames: WiredTiger doesn't allow transactional renames
        // and can't deal with rolling back a rename.
        WiredTigerSession &swrap_real = WiredTigerRecoveryUnit::Get(txn).GetSession();
        WiredTigerSession swrap(swrap_real.GetDatabase());
        WT_SESSION *session = swrap.Get();

        // Close any cached cursors we can...
        swrap_real.GetContext().CloseAllCursors();
        swrap.GetDatabase().ClearCache();

        // Rename all indexes in the entry
        std::vector<std::string> names;
        entry->getAllIndexes( txn, &names );
        std::vector<std::string>::const_iterator idx;
        for (idx = names.begin(); idx != names.end(); ++idx) {
            //std::string fromName = *idx;
            //std::string toName(toNS.toString() + fromName.substr(fromNS.toString().length()));
            ret = session->rename(session,
                    WiredTigerIndex::_getURI(fromNS.toString(), *idx).c_str(),
                    WiredTigerIndex::_getURI(toNS.toString(), *idx).c_str(),
                    "force");
            invariant(ret == 0);
        }

        // Rename the primary WiredTiger table
        std::string fromUri = WiredTigerRecordStore::_getURI(fromNS);
        std::string toUri = WiredTigerRecordStore::_getURI(toNS);
        ret = session->rename(session, fromUri.c_str(), toUri.c_str(), NULL);
        if (ret != 0)
            return Status( ErrorCodes::OperationFailed, "Collection rename failed" );

        bool was_capped = entry->rs->isCapped();
        int64_t maxDocs, maxSize;
        if (was_capped) {
            maxSize = entry->rs->cappedMaxSize();
            maxDocs = entry->rs->cappedMaxDocs();
        }
        // Now delete the old entry.
        delete entry;

        // Load the newly renamed collection into memory
        WiredTigerCollectionCatalogEntry *newEntry =
            new WiredTigerCollectionCatalogEntry(swrap, toNS);
        _entryMap[toNS.toString().c_str()] = newEntry;
        //_loadCollection(swrap, toUri, stayTemp);

        return Status::OK();
    }

}
