// rocks_engine.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
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

#include "mongo/db/storage/rocks/rocks_engine.h"

#include <boost/filesystem/operations.hpp>

#include <rocksdb/db.h>
#include <rocksdb/slice.h>
#include <rocksdb/options.h>

#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/storage/rocks/rocks_collection_catalog_entry.h"
#include "mongo/db/storage/rocks/rocks_database_catalog_entry.h"
#include "mongo/db/storage/rocks/rocks_index_entry_comparator.h"
#include "mongo/db/storage/rocks/rocks_record_store.h"
#include "mongo/db/storage/rocks/rocks_recovery_unit.h"
#include "mongo/util/log.h"

#define ROCKS_TRACE log()

namespace mongo {

#define ROCK_STATUS_OK(sss) \
    if ( !(sss).ok() ){ error() << "rocks error: " << (sss).ToString(); invariant( false ); }

    /**
     * Create Entry's for all non-index column families in the database. This method is called by 
     * the constructor. It is necessary because information about indexes is needed before a
     * column family representing an index can be opened (specifically, the orderings used in the
     * comparators for these column families). This information is accessed through the 
     * RocksCollectionCatalogEntry class for each non-index column family in the database.
     * Hence, this method.
     */
    RocksEngine::EntryVector RocksEngine::_createNonIndexCatalogEntries( 
                                              const vector<string>& namespaces ) {
        EntryVector entries;

        for ( unsigned i = 0; i < namespaces.size(); ++i ) {
            string ns = namespaces[i];
            string collection = ns;
            if ( ns.find( '&' ) != string::npos ) {
                continue;
            }
            bool isIndex = ns.find( '$' ) != string::npos;
            if ( isIndex ) {
                continue;
            }

            // TODO is this the best way of doing this?
            boost::shared_ptr<Entry> entry = _map[collection];
            if ( !entry ) {
                _map[collection] = boost::shared_ptr<Entry>( new Entry() );
                entry = _map[collection];
            }

            entry->collectionEntry.reset( new RocksCollectionCatalogEntry( this, ns ) );

            entries.push_back(entry);
        }

        return entries;
    }

    RocksEngine::CfdVector RocksEngine::_generateCfds( const EntryVector& entries, 
                                                       const vector<string>& nsVec ) {
        set<string> namespaces( nsVec.begin(), nsVec.end() );
        
        CfdVector cfds;

        cfds.push_back( rocksdb::ColumnFamilyDescriptor( rocksdb::kDefaultColumnFamilyName, 
                                                         _collectionOptions() ) );

        for ( unsigned i = 0; i < entries.size(); ++i ) {
            string columnFamilyName = entries[i]->collectionEntry->metaDataKey();
            
            if ( namespaces.find( columnFamilyName ) == namespaces.end() ) {
                continue;
            }

            rocksdb::ColumnFamilyDescriptor cfd( columnFamilyName, _collectionOptions() );
            cfds.push_back( cfd );
        }

        return cfds;
    }

    map<string, Ordering> RocksEngine::_createIndexOrderingsHelper(
            const vector<string>& namespaces ) {
        map<string, Ordering> indexOrderings;

        for ( unsigned i = 0; i < namespaces.size(); i++ ) {
            string ns = namespaces[i];
            string collection = ns;
            if ( ns.find( '&' ) != string::npos ) {
                continue;
            }
            bool isIndex = ns.find( '$' ) != string::npos;

            if ( !isIndex ) {
                continue;
            }

            collection = ns.substr( 0, ns.find( '$' ) );

            // TODO is this the best way of doing this?
            boost::shared_ptr<Entry> entry = _map[collection];
            if ( !entry ) {
                _map[collection] = boost::shared_ptr<Entry>( new Entry() );
                entry = _map[collection];
            }

            string indexName = ns.substr( ns.find( '$' ) + 1 );
            BSONObj spec = entry->collectionEntry->getIndexSpec(indexName);
            Ordering order = Ordering::make( spec["key"].Obj().getOwned() );

            indexOrderings.insert( pair<string, Ordering> (indexName, order) );
        }

        return indexOrderings;
    }

    map<string, Ordering> RocksEngine::_createIndexOrderings( const vector<string>& namespaces,
                                                              const rocksdb::Options& options,
                                                              const string& path,
                                                              rocksdb::DB* const db ) {

        // first, go through and create RocksCollectionCatalogEntries for all non-indexes
        EntryVector nonIndexEntries = _createNonIndexCatalogEntries( namespaces );

        // open all the metadata column families so that we can retrieve information about
        // each index, which is needed in order to open the index column families
        CfdVector metaDataCfds = _generateCfds( nonIndexEntries, namespaces ); 
        vector<rocksdb::ColumnFamilyHandle*> metaDataHandles;
        rocksdb::Status openROStatus = rocksdb::DB::OpenForReadOnly( options, 
                                                                     path, 
                                                                     metaDataCfds, 
                                                                     &metaDataHandles, 
                                                                     &_db );

        ROCK_STATUS_OK( openROStatus );

        // find all the indexes for this database
        map<string, Ordering> indexOrderings = _createIndexOrderingsHelper( namespaces );

        // close the database
        delete db;

        return indexOrderings;
    } 

    RocksEngine::CfdVector RocksEngine::_createCfds( const string& path, 
                                                     const rocksdb::Options& options,
                                                     rocksdb::DB* const db ) {
        std::vector<rocksdb::ColumnFamilyDescriptor> families;

        std::vector<std::string> namespaces;
        if ( boost::filesystem::exists( path ) ) {
            rocksdb::Status status = rocksdb::DB::ListColumnFamilies(options, path, &namespaces);
            
            if ( status.IsIOError() ) {
                // DNE, ok
            } else {
                ROCK_STATUS_OK( status );
            }
        }

        if ( namespaces.empty() ) {
            return families;
        }

        map<string, Ordering> indexOrderings = _createIndexOrderings( namespaces,
                                                                      options,
                                                                      path,
                                                                      _db );

        for ( size_t i = 0; i < namespaces.size(); i++ ) {
            std::string ns = namespaces[i];
            bool isIndex = ns.find( '$' ) != string::npos;

            if ( isIndex ) {
                rocksdb::ColumnFamilyOptions options = _indexOptions();
                
                string indexName = ns.substr( ns.find( '$' ) + 1 );
                invariant( indexOrderings.find( indexName ) != indexOrderings.end() );

                options.comparator =
                    new RocksIndexEntryComparator( indexOrderings.find( indexName )->second );

                families.push_back( rocksdb::ColumnFamilyDescriptor( ns, options ) );

            } else {
                families.push_back( rocksdb::ColumnFamilyDescriptor( ns, _collectionOptions() ) );
            }
        }

        return families;
    }

    RocksEngine::RocksEngine( const std::string& path )
        : _path( path ), _db( NULL ) {

        rocksdb::Options options;

        // Optimize RocksDB. This is the easiest way to get RocksDB to perform well
        options.IncreaseParallelism();
        options.OptimizeLevelStyleCompaction();

        // create the DB if it's not already present
        options.create_if_missing = true;

        // get the column families out of the database
        CfdVector families = _createCfds( path, options, _db );

        if ( families.empty() ) {
            rocksdb::Status status = rocksdb::DB::Open(options, path, &_db);
            ROCK_STATUS_OK( status );
            return;
        }
        
        std::vector<rocksdb::ColumnFamilyHandle*> handles;
        rocksdb::Status status = rocksdb::DB::Open(options, path, families, &handles, &_db);
        ROCK_STATUS_OK( status );

        invariant( handles.size() == families.size() );

        std::map<string, int> metadataMap;
        for ( unsigned i = 0; i < families.size(); i++ ) {
            string ns = families[i].name;
            if ( ns.find( '&' ) == string::npos ) {
                continue;
            }
            string collection = ns.substr( 0, ns.find( '&' ) );
            metadataMap.emplace(collection, i);
        }

        for ( unsigned i = 0; i < families.size(); i++ ) {
            string ns = families[i].name;
            ROCKS_TRACE << "RocksEngine found ns: " << ns;
            string collection = ns;
            if ( ns.find( '&' ) != string::npos ) {
                continue;
            }
            bool isIndex = ns.find( '$' ) != string::npos;
            if ( isIndex ) {
                collection = ns.substr( 0, ns.find( '$' ) );
            }

            // TODO is this the best way of doing this?
            boost::shared_ptr<Entry> entry = _map[collection];
            invariant( entry );

            if ( isIndex ) {
                string indexName = ns.substr( ns.find( '$' ) + 1 );
                ROCKS_TRACE << " got index " << indexName << " for " << collection;
                entry->indexNameToCF[indexName] = handles[i];
            }
            else {
                entry->cfHandle.reset( handles[i] );
                entry->metaCfHandle.reset( handles[metadataMap[ns]] );
                entry->recordStore.reset( new RocksRecordStore( ns, _db, handles[i], 
                            handles[metadataMap[ns]]) );
                // entry->collectionEntry is set in _createNonIndexCatalogEntries()
                entry->collectionEntry.reset( new RocksCollectionCatalogEntry( this, ns ) );
            }
        }
    }

    RocksEngine::~RocksEngine() {
        _map = Map();
        delete _db;
    }

    RecoveryUnit* RocksEngine::newRecoveryUnit( OperationContext* opCtx ) {
        return new RocksRecoveryUnit( _db, true /* change to false when unit of work hooked up*/ );
    }

    void RocksEngine::listDatabases( std::vector<std::string>* out ) const {
        std::set<std::string> dbs;

        // todo: make this faster
        boost::mutex::scoped_lock lk( _mapLock );
        for ( Map::const_iterator i = _map.begin(); i != _map.end(); ++i ) {
            const StringData& ns = i->first;
            if ( dbs.insert( nsToDatabase( ns ) ).second )
                out->push_back( nsToDatabase( ns ) );
        }
    }

    DatabaseCatalogEntry* RocksEngine::getDatabaseCatalogEntry( OperationContext* opCtx,
                                                                const StringData& db ) {
        return new RocksDatabaseCatalogEntry( this, db );
    }

    int RocksEngine::flushAllFiles( bool sync ) {
        boost::mutex::scoped_lock lk( _mapLock );
        for ( Map::const_iterator i = _map.begin(); i != _map.end(); ++i ) {
            if ( i->second->cfHandle )
                _db->Flush( rocksdb::FlushOptions(), i->second->cfHandle.get() );
        }
        return _map.size();
    }

    Status RocksEngine::repairDatabase( OperationContext* tnx,
                                        const std::string& dbName,
                                        bool preserveClonedFilesOnFailure,
                                        bool backupOriginalFiles ) {
        return Status::OK();
    }

    // non public api

    const RocksEngine::Entry* RocksEngine::getEntry( const StringData& ns ) const {
        boost::mutex::scoped_lock lk( _mapLock );
        Map::const_iterator i = _map.find( ns );
        if ( i == _map.end() )
            return NULL;
        return i->second.get();
    }

    RocksEngine::Entry* RocksEngine::getEntry( const StringData& ns ) {
        boost::mutex::scoped_lock lk( _mapLock );
        Map::const_iterator i = _map.find( ns );
        if ( i == _map.end() )
            return NULL;
        return i->second.get();
    }

    rocksdb::ColumnFamilyHandle* RocksEngine::getIndexColumnFamily( const StringData& ns,
                                                           const StringData& indexName,
                                                           const boost::optional<Ordering> order ) {
        ROCKS_TRACE << "getIndexColumnFamily " << ns << "$" << indexName;

        boost::mutex::scoped_lock lk( _mapLock );
        Map::const_iterator i = _map.find( ns );
        if ( i == _map.end() )
            return NULL;
        shared_ptr<Entry> entry = i->second;

        {
            rocksdb::ColumnFamilyHandle* handle = entry->indexNameToCF[indexName];
            if ( handle != NULL )
                return handle;
        }

        // if we get here, then the column family doesn't exist, so we need to create it

        invariant( order && "need an ordering to create a comparator for the index" );

        string fullName = ns.toString() + string("$") + indexName.toString();
        rocksdb::ColumnFamilyHandle* cf;

        rocksdb::ColumnFamilyOptions options = rocksdb::ColumnFamilyOptions();

        options.comparator = new RocksIndexEntryComparator( order.get() );

        rocksdb::Status status = _db->CreateColumnFamily( options, fullName, &cf );
        ROCK_STATUS_OK( status );
        invariant( cf != NULL);
        entry->indexNameToCF[indexName] = cf;
        return cf;
    }

    void RocksEngine::removeColumnFamily( rocksdb::ColumnFamilyHandle*& cfh ) {
        rocksdb::Status s = _db->DropColumnFamily( cfh );
        invariant( s.ok() );
        delete cfh;
        cfh = nullptr;
    }

    void RocksEngine::getCollectionNamespaces( const StringData& dbName,
                                               std::list<std::string>* out ) const {
        string prefix = dbName.toString() + ".";
        boost::mutex::scoped_lock lk( _mapLock );
        for ( Map::const_iterator i = _map.begin(); i != _map.end(); ++i ) {
            const StringData& ns = i->first;
            if ( !ns.startsWith( prefix ) )
                continue;
            out->push_back( i->first );
        }
    }


    Status RocksEngine::createCollection( OperationContext* txn,
                                          const StringData& ns,
                                          const CollectionOptions& options ) {

        ROCKS_TRACE << "RocksEngine::createCollection: " << ns;

        boost::mutex::scoped_lock lk( _mapLock );
        if ( _map.find( ns ) != _map.end() )
            return Status( ErrorCodes::NamespaceExists, "collection already exists" );

        if ( options.capped ) {
            warning() << "RocksEngine doesn't support capped collections yet, using normal";
        }

        if (ns.toString().find('$') != string::npos ||
            ns.toString().find('&') != string::npos ) {
            return Status( ErrorCodes::NamespaceExists, "invalid character in namespace" );
        }

        boost::shared_ptr<Entry> entry( new Entry() );

        rocksdb::ColumnFamilyHandle* cf;
        rocksdb::Status status = _db->CreateColumnFamily( rocksdb::ColumnFamilyOptions(),
                                                          ns.toString(),
                                                          &cf );

        ROCK_STATUS_OK( status );
        rocksdb::ColumnFamilyHandle* cf_meta;
        string metadataName = ns.toString() + "&";
        status = _db->CreateColumnFamily( rocksdb::ColumnFamilyOptions(),
                                                          metadataName,
                                                          &cf_meta );
        ROCK_STATUS_OK( status );

        entry->cfHandle.reset( cf );
        entry->metaCfHandle.reset( cf_meta );
        entry->recordStore.reset( new RocksRecordStore( ns, _db, entry->cfHandle.get(), cf_meta ));
        entry->collectionEntry.reset( new RocksCollectionCatalogEntry( this, ns ) );
        entry->collectionEntry->createMetaData();

        _map[ns] = entry;
        return Status::OK();
    }

    Status RocksEngine::dropCollection( OperationContext* opCtx,
                                        const StringData& ns ) {
        // TODO delete metadata CF, indexes??
        boost::mutex::scoped_lock lk( _mapLock );
        if ( _map.find( ns ) == _map.end() )
            return Status( ErrorCodes::NamespaceNotFound, "can't find collection to drop" );
        Entry* entry = _map[ns].get();

        entry->recordStore.reset( NULL );
        entry->collectionEntry->dropMetaData();
        entry->collectionEntry.reset( NULL );

        rocksdb::Status status = _db->DropColumnFamily( entry->cfHandle.get() );
        ROCK_STATUS_OK( status );
        status = _db->DropColumnFamily( entry->metaCfHandle.get() );
        ROCK_STATUS_OK( status );

        entry->cfHandle.reset( NULL );
        entry->metaCfHandle.reset( NULL );

        _map.erase( ns );

        return Status::OK();
    }

    rocksdb::ColumnFamilyOptions RocksEngine::_collectionOptions() const {
        return rocksdb::ColumnFamilyOptions();
    }

    rocksdb::ColumnFamilyOptions RocksEngine::_indexOptions() const {
        return rocksdb::ColumnFamilyOptions();
    }

}
