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
#include "mongo/db/storage/rocks/rocks_record_store.h"
#include "mongo/db/storage/rocks/rocks_recovery_unit.h"
#include "mongo/util/log.h"

#define ROCKS_TRACE log()

namespace mongo {

#define ROCK_STATUS_OK(sss) \
    if ( !(sss).ok() ){ error() << "rocks error: " << (sss).ToString(); invariant( false ); }

    RocksEngine::RocksEngine( const std::string& path )
        : _path( path ), _db( NULL ) {

        rocksdb::Options options;

        // Optimize RocksDB. This is the easiest way to get RocksDB to perform well
        options.IncreaseParallelism();
        options.OptimizeLevelStyleCompaction();

        // create the DB if it's not already present
        options.create_if_missing = true;

        // get the column families out
        std::vector<rocksdb::ColumnFamilyDescriptor> families;

        if ( boost::filesystem::exists( path ) ) {
            std::vector<std::string> namespaces;
            rocksdb::Status status = rocksdb::DB::ListColumnFamilies(options, path, &namespaces);
            if ( status.IsIOError() ) {
                // DNE, ok
            }
            else {
                ROCK_STATUS_OK( status );

                for ( size_t i = 0; i < namespaces.size(); i++ ) {
                    std::string ns = namespaces[i];
                    bool isIndex = ns.find( '$' ) != string::npos;
                    families.push_back( rocksdb::ColumnFamilyDescriptor( ns,
                                                                         isIndex ? _indexOptions() : _collectionOptions() ) );
                }
            }
        }

        if ( families.empty() ) {
            rocksdb::Status status = rocksdb::DB::Open(options, path, &_db);
            ROCK_STATUS_OK( status );
        }
        else {
            std::vector<rocksdb::ColumnFamilyHandle*> handles;
            rocksdb::Status status = rocksdb::DB::Open(options, path, families, &handles, &_db);
            ROCK_STATUS_OK( status );

            invariant( handles.size() == families.size() );

            for ( unsigned i = 0; i < families.size(); i++ ) {
                string ns = families[i].name;
                ROCKS_TRACE << "RocksEngine found ns: " << ns;
                string collection = ns;
                bool isIndex = ns.find( '$' ) != string::npos;
                if ( isIndex ) {
                    collection = ns.substr( 0, ns.find( '$' ) );
                }

                boost::shared_ptr<Entry> entry = _map[collection];
                if ( !entry ) {
                    _map[collection] = boost::shared_ptr<Entry>( new Entry() );
                    entry = _map[collection];
                }

                if ( isIndex ) {
                    string indexName = ns.substr( ns.find( '$' ) + 1 );
                    ROCKS_TRACE << " got index " << indexName << " for " << collection;
                    entry->indexNameToCF[indexName] =
                        boost::shared_ptr<rocksdb::ColumnFamilyHandle>( handles[i] );
                }
                else {
                    entry->cfHandle.reset( handles[i] );
                    entry->recordStore.reset( new RocksRecordStore( ns, _db, handles[i] ) );
                    entry->collectionEntry.reset( new RocksCollectionCatalogEntry( this, ns ) );
                }
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
                                                                    const StringData& indexName ) {
        ROCKS_TRACE << "getIndexColumnFamily " << ns << "$" << indexName;

        boost::mutex::scoped_lock lk( _mapLock );
        Map::const_iterator i = _map.find( ns );
        if ( i == _map.end() )
            return NULL;
        shared_ptr<Entry> entry = i->second;

        {
            boost::shared_ptr<rocksdb::ColumnFamilyHandle> handle = entry->indexNameToCF[indexName];
            if ( handle )
                return handle.get();
        }

        string fullName = ns.toString() + string("$") + indexName.toString();
        rocksdb::ColumnFamilyHandle* cf;
        rocksdb::Status status = _db->CreateColumnFamily( rocksdb::ColumnFamilyOptions(),
                                                          fullName,
                                                          &cf );
        ROCK_STATUS_OK( status );
        entry->indexNameToCF[indexName] = boost::shared_ptr<rocksdb::ColumnFamilyHandle>( cf );
        return cf;

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

        boost::shared_ptr<Entry> entry( new Entry() );

        rocksdb::ColumnFamilyHandle* cf;
        rocksdb::Status status = _db->CreateColumnFamily( rocksdb::ColumnFamilyOptions(),
                                                          ns.toString(),
                                                          &cf );
        ROCK_STATUS_OK( status );

        entry->cfHandle.reset( cf );
        entry->recordStore.reset( new RocksRecordStore( ns, _db, entry->cfHandle.get() ) );
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
        boost::shared_ptr<Entry> entry = _map[ns];

        entry->recordStore.reset( NULL );
        entry->collectionEntry->dropMetaData();
        entry->collectionEntry.reset( NULL );

        rocksdb::Status status = _db->DropColumnFamily( entry->cfHandle.get() );
        ROCK_STATUS_OK( status );

        entry->cfHandle.reset( NULL );

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
