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

    RocksEngine::RocksEngine( const std::string& path ): _path( path ), _db( NULL ) {
        // get ColumnFamilyDescriptors for all the column families
        CfdVector families = _createCfds( path, _db );

        // If there are no column families, then just open the database and return
        if ( families.empty() ) {
            rocksdb::Status s = rocksdb::DB::Open( _dbOptions(), path, &_db );
            ROCK_STATUS_OK( s );
            return;
        }

        // Open the database, getting handles for every column family
        std::vector<rocksdb::ColumnFamilyHandle*> handles;
        rocksdb::Status s = rocksdb::DB::Open( _dbOptions(), path, families, &handles, &_db );
        ROCK_STATUS_OK( s );

        invariant( handles.size() == families.size() );

        // Create an Entry object for every ColumnFamilyHandle
        _createEntries( families, handles );
    }

    RocksEngine::~RocksEngine() {
    }

    RecoveryUnit* RocksEngine::newRecoveryUnit( OperationContext* opCtx ) {
        return new RocksRecoveryUnit( _db, true /* TODO change to false when unit of work hooked up*/ );
    }

    void RocksEngine::listDatabases( std::vector<std::string>* out ) const {
        std::set<std::string> dbs;

        // TODO: make this faster
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
        boost::mutex::scoped_lock lk( _mapLock );
        rocksdb::Status s = RepairDB( dbName, rocksdb::Options() );
        if ( s.ok() )
            return Status::OK();
        else
            return Status( ErrorCodes::InternalError, "Repair Failed: " + s.ToString() );
    }

    void RocksEngine::cleanShutdown(OperationContext* txn) {
        typedef StringMap<rocksdb::ColumnFamilyHandle*>::const_iterator CfhIt;

        boost::mutex::scoped_lock lk( _mapLock );
        for ( Map::const_iterator i = _map.begin(); i != _map.end(); ++i ) {
            boost::shared_ptr<Entry> entry = i->second;

            for ( CfhIt j = entry->indexNameToCF.begin(); j != entry->indexNameToCF.end(); ++j ) {
                rocksdb::ColumnFamilyHandle* index_handle = j->second;
                delete index_handle;
            }

            entry->metaCfHandle.reset();
            entry->cfHandle.reset();
        }
        _map = Map();
        delete _db;
    }

    // non public api

    rocksdb::ReadOptions RocksEngine::readOptionsWithSnapshot( OperationContext* opCtx ) {
        rocksdb::ReadOptions options = rocksdb::ReadOptions();
        options.snapshot = dynamic_cast<RocksRecoveryUnit*>( opCtx->recoveryUnit() )->snapshot();
        return options;
    }

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
        rocksdb::ColumnFamilyHandle* cf = NULL;

        rocksdb::ColumnFamilyOptions options = rocksdb::ColumnFamilyOptions();

        options.comparator = new RocksIndexEntryComparator( order.get() );

        rocksdb::Status status = _db->CreateColumnFamily( options, fullName, &cf );
        ROCK_STATUS_OK( status );
        invariant( cf != NULL);
        entry->indexNameToCF[indexName] = cf;
        return cf;
    }

    void RocksEngine::removeColumnFamily( rocksdb::ColumnFamilyHandle*& cfh,
                                            const StringData& indexName,
                                            const StringData& ns ) {
        Map::const_iterator i = _map.find( ns );
        if ( i != _map.end() ) {
            shared_ptr<Entry> entry = i->second;
            entry->indexNameToCF.erase(indexName);
        }
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

        if (ns.toString().find('$') != string::npos || ns.toString().find('&') != string::npos ) {
            return Status( ErrorCodes::NamespaceExists, "invalid character in namespace" );
        }

        rocksdb::ColumnFamilyOptions rocksOptions;

        boost::shared_ptr<Entry> entry( new Entry() );

        rocksdb::ColumnFamilyHandle* cf;
        rocksdb::Status status = _db->CreateColumnFamily( rocksOptions, ns.toString(), &cf );

        ROCK_STATUS_OK( status );
        rocksdb::ColumnFamilyHandle* cf_meta;
        string metadataName = ns.toString() + "&";
        status = _db->CreateColumnFamily( rocksdb::ColumnFamilyOptions(), metadataName, &cf_meta );
        ROCK_STATUS_OK( status );

        BSONObj optionsObj = options.toBSON();
        rocksdb::Slice key( "options" );
        rocksdb::Slice value( optionsObj.objdata(), optionsObj.objsize() );
        status = _db->Put(rocksdb::WriteOptions(), cf_meta, key, value);
        ROCK_STATUS_OK( status );

        entry->cfHandle.reset( cf );
        entry->metaCfHandle.reset( cf_meta );
        if ( options.capped )
            entry->recordStore.reset( new RocksRecordStore( ns, _db, entry->cfHandle.get(), cf_meta,
                                    true,
                                    options.cappedSize ? options.cappedSize : 4096, // default size
                                    options.cappedMaxDocs ? options.cappedMaxDocs : -1 ));
        else
            entry->recordStore.reset( new RocksRecordStore( ns, _db,
                                                entry->cfHandle.get(), cf_meta ) );

        entry->collectionEntry.reset( new RocksCollectionCatalogEntry( this, ns ) );
        entry->collectionEntry->createMetaData();

        _map[ns] = entry;
        return Status::OK();
    }

    Status RocksEngine::dropCollection( OperationContext* opCtx,
                                        const StringData& ns ) {
        boost::mutex::scoped_lock lk( _mapLock );
        if ( _map.find( ns ) == _map.end() )
            return Status( ErrorCodes::NamespaceNotFound, "can't find collection to drop" );
        Entry* entry = _map[ns].get();

        for ( auto it = entry->indexNameToCF.begin(); it != entry->indexNameToCF.end(); ++it ) {
            entry->collectionEntry->removeIndex( opCtx, it->first );
        }

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

    rocksdb::Options RocksEngine::_dbOptions() const {
        rocksdb::Options options;

        // Optimize RocksDB. This is the easiest way to get RocksDB to perform well
        options.IncreaseParallelism();
        options.OptimizeLevelStyleCompaction();

        // create the DB if it's not already present
        options.create_if_missing = true;

        return options;
    }

    rocksdb::ColumnFamilyOptions RocksEngine::_collectionOptions() const {
        return rocksdb::ColumnFamilyOptions();
    }

    rocksdb::ColumnFamilyOptions RocksEngine::_indexOptions() const {
        return rocksdb::ColumnFamilyOptions();
    }

    /**
     * Create Entry's for all non-index column families in the database. This method is called by
     * the constructor. It is necessary because information about indexes is needed before a
     * column family representing an index can be opened (specifically, the orderings used in the
     * comparators for these column families needs to be known). This information is accessed
     * through the RocksCollectionCatalogEntry class for each non-index column family in the
     * database. Hence, this method.
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

            boost::shared_ptr<Entry> entry = _map[collection];
            // this works because a shared_ptr's default constructor leaves it uninitialized
            if ( !entry ) {
                _map[collection] = boost::shared_ptr<Entry>( new Entry() );
                entry = _map[collection];
            }

            // We'll use this RocksCollectionCatalogEntry to open the column families representing
            // indexes
            entry->collectionEntry.reset( new RocksCollectionCatalogEntry( this, ns ) );

            entries.push_back(entry);
        }

        return entries;
    }

    RocksEngine::CfdVector RocksEngine::_generateMetaDataCfds( const EntryVector& entries,
                                                               const vector<string>& nsVec ) const {
        set<string> namespaces( nsVec.begin(), nsVec.end() );

        CfdVector cfds;

        // the default column family must always be included, as per rocksdb specifications
        cfds.push_back( rocksdb::ColumnFamilyDescriptor( rocksdb::kDefaultColumnFamilyName,
                                                         _collectionOptions() ) );

        for ( unsigned i = 0; i < entries.size(); ++i ) {
            string columnFamilyName = entries[i]->collectionEntry->metaDataKey();

            // some column families don't have corresponding metadata column families,
            // so before blindly opening a metadata column family, we check to see that it exists
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
            if ( ns.find( '&' ) != string::npos || ns.find( '$' ) == string::npos ) {
                continue;
            }

            collection = ns.substr( 0, ns.find( '$' ) );

            boost::shared_ptr<Entry> entry = _map[collection];
            // this works because a shared_ptr's default constructor leaves it uninitialized
            if ( !entry ) {
                _map[collection] = boost::shared_ptr<Entry>( new Entry() );
                entry = _map[collection];
            }

            // Generate the Ordering object for each index, allowing the column families
            // representing these indexes to eventually be opened
            string indexName = ns.substr( ns.find( '$' ) + 1 );
            BSONObj spec = entry->collectionEntry->getIndexSpec(indexName);
            Ordering order = Ordering::make( spec["key"].Obj().getOwned() );

            indexOrderings.insert( pair<string, Ordering> (indexName, order) );
        }

        return indexOrderings;
    }

    map<string, Ordering> RocksEngine::_createIndexOrderings( const vector<string>& namespaces,
                                                              const string& path,
                                                              rocksdb::DB* const db ) {

        // first, go through and create RocksCollectionCatalogEntries for all non-indexes
        EntryVector nonIndexEntries = _createNonIndexCatalogEntries( namespaces );

        // open all the metadata column families so that we can retrieve information about
        // each index, which is needed in order to open the index column families
        CfdVector metaDataCfds = _generateMetaDataCfds( nonIndexEntries, namespaces );
        vector<rocksdb::ColumnFamilyHandle*> metaDataHandles;
        rocksdb::Status openROStatus = rocksdb::DB::OpenForReadOnly( _dbOptions(),
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

    RocksEngine::CfdVector RocksEngine::_createCfds( const string& path, rocksdb::DB* const db ) {
        std::vector<rocksdb::ColumnFamilyDescriptor> families;

        std::vector<std::string> namespaces;
        if ( boost::filesystem::exists( path ) ) {
            rocksdb::Status s = rocksdb::DB::ListColumnFamilies(_dbOptions(), path, &namespaces);

            if ( s.IsIOError() ) {
                // DNE, ok
            } else {
                ROCK_STATUS_OK( s );
            }
        }

        if ( namespaces.empty() ) {
            return families;
        }

        // Create a mapping from index names to the Ordering object for each index. These Ordering
        // objects will be used to create RocksIndexEntryComparators to be used with each
        // column family representing a namespace
        map<string, Ordering> indexOrderings = _createIndexOrderings( namespaces, path, _db );

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

    void RocksEngine::_createEntries( const CfdVector& families,
                                      const vector<rocksdb::ColumnFamilyHandle*> handles ) {
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

            boost::shared_ptr<Entry> entry = _map[collection];
            invariant( entry );

            if ( isIndex ) {
                string indexName = ns.substr( ns.find( '$' ) + 1 );
                ROCKS_TRACE << " got index " << indexName << " for " << collection;
                entry->indexNameToCF[indexName] = handles[i];
            } else {
                if ( ns.compare("default") == 0 )
                    continue;

                CollectionOptions options;
                rocksdb::Slice optionsKey( "options" );
                std::string value;
                rocksdb::Status status = _db->Get(rocksdb::ReadOptions(), handles[metadataMap[ns]],
                                        optionsKey, &value);

                ROCK_STATUS_OK( status );
                BSONObj optionsObj( value.data() );
                invariant( optionsObj.isValid() );
                options.parse( optionsObj );

                entry->cfHandle.reset( handles[i] );
                entry->metaCfHandle.reset( handles[metadataMap[ns]] );
                if ( options.capped ) {
                    entry->recordStore.reset( new RocksRecordStore( ns, _db, handles[i],
                            handles[metadataMap[ns]],
                            options.capped,
                            options.cappedSize ? options.cappedSize : 4096, // default size
                            options.cappedMaxDocs ? options.cappedMaxDocs : -1) );
                } else {
                    entry->recordStore.reset( new RocksRecordStore( ns, _db, handles[i],
                            handles[metadataMap[ns]] ) );
                }
                // entry->collectionEntry is set in _createNonIndexCatalogEntries()
                entry->collectionEntry.reset( new RocksCollectionCatalogEntry( this, ns ) );
            }
        }
    }
}
