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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/db/storage/rocks/rocks_engine.h"

#include <boost/filesystem/operations.hpp>
#include <boost/make_shared.hpp>
#include <boost/shared_ptr.hpp>

#include <rocksdb/comparator.h>
#include <rocksdb/db.h>
#include <rocksdb/slice.h>
#include <rocksdb/options.h>

#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/rocks/rocks_collection_catalog_entry.h"
#include "mongo/db/storage/rocks/rocks_database_catalog_entry.h"
#include "mongo/db/storage/rocks/rocks_record_store.h"
#include "mongo/db/storage/rocks/rocks_recovery_unit.h"
#include "mongo/db/storage/rocks/rocks_sorted_data_impl.h"
#include "mongo/util/log.h"

#define ROCKS_TRACE if(0) log()

#define ROCKS_STATUS_OK( s ) if ( !( s ).ok() ) { error() << "rocks error: " << ( s ).ToString(); \
    invariant( false ); }

namespace mongo {

    RocksEngine::RocksEngine( const std::string& path ) : _path( path ), _defaultHandle( NULL ) {
        // TODO make this more fine-grained?
        boost::mutex::scoped_lock lk( _entryMapMutex );
        std::vector<rocksdb::ColumnFamilyDescriptor> families;

        vector<string> familyNames = _listFamilyNames( path );

        // Create the shared collection comparator
        _collectionComparator.reset( RocksRecordStore::newRocksCollectionComparator() );

        if ( !familyNames.empty() ) {
            // go through and create RocksCollectionCatalogEntries for all non-indexes
            _createNonIndexCatalogEntries( familyNames );

            // Create a mapping from index names to the Ordering object for each index.
            // These Ordering objects will be used to create RocksIndexEntryComparators to be used
            // with each column family representing a namespace
            map<string, Ordering> indexOrderings = _createIndexOrderings( familyNames, path );

            // get ColumnFamilyDescriptors for all the column families
            families = _createCfds( familyNames, indexOrderings );
        }

        rocksdb::DB* dbPtr;

        // If there are no column families, then just open the database and return
        if ( families.empty() ) {
            rocksdb::Status s = rocksdb::DB::Open( dbOptions(), path, &dbPtr );
            _db.reset( dbPtr );
            ROCKS_STATUS_OK( s );

            _defaultHandle = _db->DefaultColumnFamily();
            return;
        }

        // Open the database, getting handles for every column family
        std::vector<rocksdb::ColumnFamilyHandle*> handles;

        rocksdb::Status s = rocksdb::DB::Open( dbOptions(), path, families, &handles, &dbPtr );
        ROCKS_STATUS_OK( s );
        _db.reset( dbPtr );

        invariant( handles.size() == families.size() );

        _defaultHandle = _db->DefaultColumnFamily();

        // Create an Entry object for every ColumnFamilyHandle
        _createEntries( families, handles );
    }

    RocksEngine::~RocksEngine() {
    }

    RecoveryUnit* RocksEngine::newRecoveryUnit( OperationContext* opCtx ) {
        /* TODO change to false when unit of work hooked up*/
        return new RocksRecoveryUnit( _db.get(), true );
    }

    void RocksEngine::listDatabases( std::vector<std::string>* out ) const {
        std::set<std::string> dbs;

        // TODO: make this faster
        boost::mutex::scoped_lock lk( _entryMapMutex );
        for ( EntryMap::const_iterator i = _entryMap.begin(); i != _entryMap.end(); ++i ) {
            const StringData& ns = i->first;
            if ( dbs.insert( nsToDatabase( ns ) ).second )
                out->push_back( nsToDatabase( ns ) );
        }
    }

    DatabaseCatalogEntry* RocksEngine::getDatabaseCatalogEntry( OperationContext* opCtx,
                                                                const StringData& db ) {
        boost::mutex::scoped_lock lk( _dbCatalogMapMutex );

        boost::shared_ptr<RocksDatabaseCatalogEntry>& dbce = _dbCatalogMap[db.toString()];
        if ( !dbce ) {
            dbce = boost::make_shared<RocksDatabaseCatalogEntry>( this, db );
        }

        return dbce.get();
    }

    int RocksEngine::flushAllFiles( bool sync ) {
        boost::mutex::scoped_lock lk( _entryMapMutex );
        for ( EntryMap::const_iterator i = _entryMap.begin(); i != _entryMap.end(); ++i ) {
            if ( i->second->cfHandle )
                _db->Flush( rocksdb::FlushOptions(), i->second->cfHandle.get() );
        }
        return _entryMap.size();
    }

    Status RocksEngine::repairDatabase( OperationContext* tnx,
                                        const std::string& dbName,
                                        bool preserveClonedFilesOnFailure,
                                        bool backupOriginalFiles ) {
        // TODO implement
        return Status::OK();
    }

    void RocksEngine::cleanShutdown(OperationContext* txn) {
        // no locking here because this is only called while single-threaded.
        _entryMap = EntryMap();
        _dbCatalogMap = DbCatalogMap();
        _collectionComparator.reset();
        _db.reset();
    }

    Status RocksEngine::closeDatabase( OperationContext* txn, const StringData& db ) {
        boost::mutex::scoped_lock lk( _dbCatalogMapMutex );
        _dbCatalogMap.erase( db.toString() );
        return Status::OK();
    }

    Status RocksEngine::dropDatabase( OperationContext* txn, const StringData& db ) {
        const string prefix = db.toString() + ".";
        boost::mutex::scoped_lock lk( _entryMapMutex );
        vector<string> toDrop;

        // TODO don't iterate through everything
        for (EntryMap::const_iterator it = _entryMap.begin(); it != _entryMap.end(); ++it ) {
            const StringData& ns = it->first;
            if ( ns.startsWith( prefix ) ) {
                toDrop.push_back( ns.toString() );
            }
        }

        for (vector<string>::const_iterator it = toDrop.begin(); it != toDrop.end(); ++it ) {
            _dropCollection_inlock( txn, *it );
        }

        return closeDatabase( txn, db );
    }

    // non public api

    rocksdb::ReadOptions RocksEngine::readOptionsWithSnapshot( OperationContext* opCtx ) {
        rocksdb::ReadOptions options;
        options.snapshot = dynamic_cast<RocksRecoveryUnit*>( opCtx->recoveryUnit() )->snapshot();
        return options;
    }

    const RocksEngine::Entry* RocksEngine::getEntry( const StringData& ns ) const {
        boost::mutex::scoped_lock lk( _entryMapMutex );
        EntryMap::const_iterator i = _entryMap.find( ns );
        if ( i == _entryMap.end() )
            return NULL;
        return i->second.get();
    }

    RocksEngine::Entry* RocksEngine::getEntry( const StringData& ns ) {
        boost::mutex::scoped_lock lk( _entryMapMutex );
        EntryMap::const_iterator i = _entryMap.find( ns );
        if ( i == _entryMap.end() )
            return NULL;
        return i->second.get();
    }

    rocksdb::ColumnFamilyHandle* RocksEngine::getIndexColumnFamily(
                                                           const StringData& ns,
                                                           const StringData& indexName,
                                                           const boost::optional<Ordering> order ) {
        ROCKS_TRACE << "getIndexColumnFamily " << ns << "$" << indexName;

        boost::mutex::scoped_lock lk( _entryMapMutex );
        EntryMap::const_iterator i = _entryMap.find( ns );
        if ( i == _entryMap.end() )
            return NULL;
        shared_ptr<Entry> entry = i->second;

        {
            rocksdb::ColumnFamilyHandle* handle = entry->indexNameToCF[indexName].get();
            if ( handle )
                return handle;
        }

        // if we get here, then the column family doesn't exist, so we need to create it

        invariant( order && "need an Ordering to create a comparator for the index" );

        const string fullName = ns.toString() + string("$") + indexName.toString();
        rocksdb::ColumnFamilyHandle* cf = NULL;

        typedef boost::shared_ptr<const rocksdb::Comparator> SharedComparatorPtr;
        SharedComparatorPtr& comparator = entry->indexNameToComparator[indexName];

        comparator.reset( RocksSortedDataImpl::newRocksComparator( order.get() ) );
        invariant( comparator );

        rocksdb::ColumnFamilyOptions options;
        options.comparator = comparator.get();

        rocksdb::Status status = _db->CreateColumnFamily( options, fullName, &cf );
        ROCKS_STATUS_OK( status );
        invariant( cf != NULL);
        entry->indexNameToCF[indexName].reset( cf );
        return cf;
    }

    void RocksEngine::removeColumnFamily( rocksdb::ColumnFamilyHandle** cfh,
                                            const StringData& indexName,
                                            const StringData& ns ) {
        boost::mutex::scoped_lock lk( _entryMapMutex );
        EntryMap::const_iterator i = _entryMap.find( ns );
        const rocksdb::Status s = _db->DropColumnFamily( *cfh );
        invariant( s.ok() );
        if ( i != _entryMap.end() ) {
            i->second->indexNameToCF.erase(indexName);
        }
        *cfh = NULL;
    }

    void RocksEngine::getCollectionNamespaces( const StringData& dbName,
                                               std::list<std::string>* out ) const {
        const string prefix = dbName.toString() + ".";
        boost::mutex::scoped_lock lk( _entryMapMutex );
        for (EntryMap::const_iterator i = _entryMap.begin(); i != _entryMap.end(); ++i ) {
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

        boost::mutex::scoped_lock lk( _entryMapMutex );
        if ( _entryMap.find( ns ) != _entryMap.end() )
            return Status( ErrorCodes::NamespaceExists, "collection already exists" );

        if (ns.toString().find('$') != string::npos ) {
            return Status( ErrorCodes::BadValue, "invalid character in namespace" );
        }

        boost::shared_ptr<Entry> entry( new Entry() );

        rocksdb::ColumnFamilyHandle* cf;
        rocksdb::Status s = _db->CreateColumnFamily( _collectionOptions(), ns.toString(), &cf );
        ROCKS_STATUS_OK( s );

        const BSONObj optionsObj = options.toBSON();
        const rocksdb::Slice key( ns.toString() + "-options" );
        const rocksdb::Slice value( optionsObj.objdata(), optionsObj.objsize() );

        dynamic_cast<RocksRecoveryUnit*>( txn->recoveryUnit() )->writeBatch()->Put(_defaultHandle,
                                                                                   key,
                                                                                   value);
        entry->cfHandle.reset( cf );

        invariant(_defaultHandle != NULL);

        if ( options.capped )
            entry->recordStore.reset( new RocksRecordStore(
                                        ns,
                                        _db.get(),
                                        entry->cfHandle.get(),
                                        _defaultHandle,
                                        true,
                                        options.cappedSize ? options.cappedSize : 4096,
                                        options.cappedMaxDocs ? options.cappedMaxDocs : -1 ) );
        else
            entry->recordStore.reset( new RocksRecordStore(
                                          ns, _db.get(), entry->cfHandle.get(), _defaultHandle ) );

        entry->collectionEntry.reset( new RocksCollectionCatalogEntry( this, ns ) );
        entry->collectionEntry->createMetaData();

        _entryMap[ns] = entry;
        return Status::OK();
    }

    Status RocksEngine::dropCollection( OperationContext* opCtx, const StringData& ns ) {
        boost::mutex::scoped_lock lk( _entryMapMutex );
        return _dropCollection_inlock( opCtx, ns );
    }

    Status RocksEngine::_dropCollection_inlock( OperationContext* opCtx, const StringData& ns ) {
        // XXX not using a snapshot here (anywhere in the method, really)
        if ( _entryMap.find( ns ) == _entryMap.end() )
            return Status( ErrorCodes::NamespaceNotFound, "can't find collection to drop" );
        Entry* entry = _entryMap[ns].get();

        for ( auto it = entry->indexNameToCF.begin(); it != entry->indexNameToCF.end(); ++it ) {
            entry->collectionEntry->removeIndex( opCtx, it->first );
        }

        entry->recordStore->dropRsMetaData( opCtx );
        entry->recordStore.reset( NULL );
        entry->collectionEntry->dropMetaData();
        entry->collectionEntry.reset( NULL );

        rocksdb::Status status = _db->DropColumnFamily( entry->cfHandle.get() );
        ROCKS_STATUS_OK( status );

        entry->cfHandle.reset( NULL );

        _entryMap.erase( ns );

        return Status::OK();
    }

    rocksdb::Options RocksEngine::dbOptions() {
        rocksdb::Options options;

        // Optimize RocksDB. This is the easiest way to get RocksDB to perform well
        options.IncreaseParallelism();
        options.OptimizeLevelStyleCompaction();

        // create the DB if it's not already present
        options.create_if_missing = true;

        return options;
    }

    rocksdb::ColumnFamilyOptions RocksEngine::_collectionOptions() const {
        rocksdb::ColumnFamilyOptions options;
        invariant( _collectionComparator.get() );
        options.comparator = _collectionComparator.get();
        return options;
    }

    rocksdb::ColumnFamilyOptions RocksEngine::_indexOptions() const {
        return rocksdb::ColumnFamilyOptions();
    }

    vector<std::string> RocksEngine::_listFamilyNames( string filepath ) {
        std::vector<std::string> familyNames;
        if ( boost::filesystem::exists( filepath ) ) {
            rocksdb::Status s = rocksdb::DB::ListColumnFamilies( dbOptions(),
                                                                 filepath,
                                                                 &familyNames );

            if ( s.IsIOError() ) {
                // DNE, this means the directory exists but is empty, which is fine
                // because it means no rocks database exists yet
            } else {
                ROCKS_STATUS_OK( s );
            }
        }

        return familyNames;
    }

    bool RocksEngine::_isDefaultFamily( const string& name ) {
        return name == rocksdb::kDefaultColumnFamilyName;
    }

    /**
     * Create Entry's for all non-index column families in the database. This method is called by
     * the constructor. It is necessary because information about indexes is needed before a
     * column family representing an index can be opened (specifically, the orderings used in the
     * comparators for these column families needs to be known). This information is accessed
     * through the RocksCollectionCatalogEntry class for each non-index column family in the
     * database. Hence, this method.
     */
    void RocksEngine::_createNonIndexCatalogEntries( const vector<string>& namespaces ) {
        for ( unsigned i = 0; i < namespaces.size(); ++i ) {
            const string ns = namespaces[i];

            // ignore the default column family, which RocksDB makes us keep around.
            if ( _isDefaultFamily( ns ) ) {
                continue;
            }

            string collection = ns;
            if ( ns.find( '&' ) != string::npos || ns.find( '$' ) != string::npos ) {
                continue;
            }

            boost::shared_ptr<Entry>& entry = _entryMap[collection];
            invariant( !entry );
            entry = boost::make_shared<Entry>();

            // We'll use this RocksCollectionCatalogEntry to open the column families representing
            // indexes
            invariant( !entry->collectionEntry );
            entry->collectionEntry.reset( new RocksCollectionCatalogEntry( this, ns ) );
        }
    }

    map<string, Ordering> RocksEngine::_createIndexOrderings( const vector<string>& namespaces,
                                                              const string& filepath ) {
        // open the default column family so that we can retrieve information about
        // each index, which is needed in order to open the index column families
        rocksdb::DB* db;
        rocksdb::Status status = rocksdb::DB::OpenForReadOnly( dbOptions(), filepath, &db );
        boost::scoped_ptr<rocksdb::DB> dbPtr( db );

        ROCKS_STATUS_OK( status );

        map<string, Ordering> indexOrderings;

        // populate indexOrderings
        for ( unsigned i = 0; i < namespaces.size(); i++ ) {
            const string ns = namespaces[i];
            const size_t sepPos = ns.find( '$' );
            if ( sepPos == string::npos ) {
                continue;
            }

            // the default family, which we want to ignore, should be caught in the above if
            // statement.
            invariant( !_isDefaultFamily( ns ) );

            string collection = ns.substr( 0, sepPos );

            boost::shared_ptr<Entry>& entry = _entryMap[collection];
            invariant( entry );

            // Generate the Ordering object for each index, allowing the column families
            // representing these indexes to eventually be opened
            const string indexName = ns.substr( sepPos + 1 );
            const BSONObj spec = entry->collectionEntry->getOtherIndexSpec( indexName, db );
            const Ordering order = Ordering::make( spec["key"].Obj() );

            indexOrderings.insert( std::make_pair( indexName, order ) );
        }

        return indexOrderings;
    }

    RocksEngine::CfdVector RocksEngine::_createCfds( const std::vector<std::string>& namespaces,
                                                     const map<string, Ordering>& indexOrderings ) {
        CfdVector families;

        for ( size_t i = 0; i < namespaces.size(); i++ ) {
            const std::string ns = namespaces[i];
            const size_t sepPos = ns.find( '$' );
            const bool isIndex = sepPos != string::npos;

            if ( isIndex ) {
                rocksdb::ColumnFamilyOptions options = _indexOptions();

                const string indexName = ns.substr( sepPos + 1 );

                const map<string, Ordering>::const_iterator it = indexOrderings.find( indexName );
                invariant( it != indexOrderings.end() );

                const Ordering order = it->second;
                options.comparator = RocksSortedDataImpl::newRocksComparator( order );

                families.push_back( rocksdb::ColumnFamilyDescriptor( ns, options ) );

            } else if ( _isDefaultFamily( ns ) ) {
                families.push_back( rocksdb::ColumnFamilyDescriptor( ns,
                                                                rocksdb::ColumnFamilyOptions() ) );
            } else {
                families.push_back( rocksdb::ColumnFamilyDescriptor( ns, _collectionOptions() ) );
            }
        }

        return families;
    }

    void RocksEngine::_createEntries( const CfdVector& families,
                                      const vector<rocksdb::ColumnFamilyHandle*> handles ) {
        invariant(_defaultHandle != NULL);

        for ( unsigned i = 0; i < families.size(); i++ ) {
            const string ns = families[i].name;

            ROCKS_TRACE << "RocksEngine found ns: " << ns;

            // RocksDB specifies that the default column family must be included in the database.
            // We want to ignore this column family.
            if ( _isDefaultFamily( ns ) ) {
                continue;
            }

            string collection = ns;

            const size_t sepPos = ns.find( '$' );

            const bool isIndex = sepPos != string::npos;
            if ( isIndex ) {
                collection = ns.substr( 0, sepPos );
            }

            boost::shared_ptr<Entry> entry = _entryMap[collection];
            invariant( entry );

            if ( isIndex ) {
                string indexName = ns.substr( sepPos + 1 );
                ROCKS_TRACE << " got index " << indexName << " for " << collection;
                entry->indexNameToCF[indexName].reset( handles[i] );

                invariant( families[i].options.comparator );
                entry->indexNameToComparator[indexName].reset( families[i].options.comparator );
            } else {
                rocksdb::Slice optionsKey( ns + "-options" );
                std::string value;
                rocksdb::Status status = _db->Get(rocksdb::ReadOptions(), optionsKey, &value);

                ROCKS_STATUS_OK( status );
                BSONObj optionsObj( value.data() );

                CollectionOptions options;
                options.parse( optionsObj );

                entry->cfHandle.reset( handles[i] );
                if ( options.capped ) {
                    entry->recordStore.reset(new RocksRecordStore(
                                ns,
                                _db.get(),
                                handles[i],
                                _defaultHandle,
                                options.capped,
                                options.cappedSize ? options.cappedSize : 4096, // default size
                                options.cappedMaxDocs ? options.cappedMaxDocs : -1) );
                } else {
                    entry->recordStore.reset( new RocksRecordStore( ns, _db.get(), handles[i],
                                                                    _defaultHandle ) );
                }
                // entry->collectionEntry is set in _createNonIndexCatalogEntries()
                invariant( entry->collectionEntry );
            }
        }
    }

    Status toMongoStatus( rocksdb::Status s ) {
        if ( s.ok() )
            return Status::OK();
        else
            return Status( ErrorCodes::InternalError, s.ToString() );
    }
}
