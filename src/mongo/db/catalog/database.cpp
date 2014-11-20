// database.cpp

/**
*    Copyright (C) 2008-2014 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/database.h"

#include <algorithm>
#include <boost/filesystem/operations.hpp>

#include "mongo/db/audit.h"
#include "mongo/db/auth/auth_index_d.h"
#include "mongo/db/background.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/global_environment_experiment.h"
#include "mongo/db/global_environment_d.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/instance.h"
#include "mongo/db/introspect.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/storage_options.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/util/log.h"

namespace mongo {

    void massertNamespaceNotIndex( const StringData& ns, const StringData& caller ) {
        massert( 17320,
                 str::stream() << "cannot do " << caller
                 << " on namespace with a $ in it: " << ns,
                 NamespaceString::normal( ns ) );
    }

    class Database::AddCollectionChange : public RecoveryUnit::Change {
    public:
        AddCollectionChange(Database* db, const StringData& ns)
            : _db(db)
            , _ns(ns.toString())
        {}

        virtual void commit() {}
        virtual void rollback() {
            scoped_lock lk( _db->_collectionLock );
            CollectionMap::const_iterator it = _db->_collections.find(_ns);
            if ( it == _db->_collections.end() )
                return;

            delete it->second;
            _db->_collections.erase( it );
        }

        Database* const _db;
        const std::string _ns;
    };

    class Database::RemoveCollectionChange : public RecoveryUnit::Change {
    public:
        // Takes ownership of coll (but not db).
        RemoveCollectionChange(Database* db, Collection* coll)
            : _db(db)
            , _coll(coll)
        {}

        virtual void commit() {
            delete _coll;
        }

        virtual void rollback() {
            mongo::mutex::scoped_lock lk(_db->_collectionLock);
            Collection*& inMap = _db->_collections[_coll->ns().ns()];
            invariant(!inMap);
            inMap = _coll;
        }

        Database* const _db;
        Collection* const _coll;
    };

    Database::~Database() {
        for (CollectionMap::const_iterator i = _collections.begin(); i != _collections.end(); ++i)
            delete i->second;
    }

    void Database::close(OperationContext* txn ) {
        // XXX? - Do we need to close database under global lock or just DB-lock is sufficient ?
        invariant(txn->lockState()->isW());

        repl::oplogCheckCloseDatabase(txn, this); // oplog caches some things, dirty its caches

        if ( BackgroundOperation::inProgForDb( _name ) ) {
            log() << "warning: bg op in prog during close db? " << _name << endl;
        }
    }

    Status Database::validateDBName( const StringData& dbname ) {

        if ( dbname.size() <= 0 )
            return Status( ErrorCodes::BadValue, "db name is empty" );

        if ( dbname.size() >= 64 )
            return Status( ErrorCodes::BadValue, "db name is too long" );

        if ( dbname.find( '.' ) != string::npos )
            return Status( ErrorCodes::BadValue, "db name cannot contain a ." );

        if ( dbname.find( ' ' ) != string::npos )
            return Status( ErrorCodes::BadValue, "db name cannot contain a space" );

#ifdef _WIN32
        static const char* windowsReservedNames[] = {
            "con", "prn", "aux", "nul",
            "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
            "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"
        };

        string lower( dbname.toString() );
        std::transform( lower.begin(), lower.end(), lower.begin(), ::tolower );
        for ( size_t i = 0; i < (sizeof(windowsReservedNames) / sizeof(char*)); ++i ) {
            if ( lower == windowsReservedNames[i] ) {
                stringstream errorString;
                errorString << "db name \"" << dbname.toString() << "\" is a reserved name";
                return Status( ErrorCodes::BadValue, errorString.str() );
            }
        }
#endif

        return Status::OK();
    }

    Database::Database(const StringData& name, DatabaseCatalogEntry* dbEntry)
        : _name(name.toString()),
          _dbEntry( dbEntry ),
          _profileName(_name + ".system.profile"),
          _indexesName(_name + ".system.indexes"),
          _collectionLock( "Database::_collectionLock" )
    {
        Status status = validateDBName( _name );
        if ( !status.isOK() ) {
            warning() << "tried to open invalid db: " << _name << endl;
            uasserted( 10028, status.toString() );
        }

        _profile = serverGlobalParams.defaultProfile;
    }


    /*static*/
    string Database::duplicateUncasedName(const string &name, set< string > *duplicates) {
        if ( duplicates ) {
            duplicates->clear();
        }

        vector<string> others;
        StorageEngine* storageEngine = getGlobalEnvironment()->getGlobalStorageEngine();
        storageEngine->listDatabases(&others);

        set<string> allShortNames;
        dbHolder().getAllShortNames(allShortNames);

        others.insert( others.end(), allShortNames.begin(), allShortNames.end() );

        for ( unsigned i=0; i<others.size(); i++ ) {

            if ( strcasecmp( others[i].c_str() , name.c_str() ) )
                continue;

            if ( strcmp( others[i].c_str() , name.c_str() ) == 0 )
                continue;

            if ( duplicates ) {
                duplicates->insert( others[i] );
            } else {
                return others[i];
            }
        }
        if ( duplicates ) {
            return duplicates->empty() ? "" : *duplicates->begin();
        }
        return "";
    }

    void Database::clearTmpCollections(OperationContext* txn) {

        txn->lockState()->assertWriteLocked( _name );

        list<string> collections;
        _dbEntry->getCollectionNamespaces( &collections );

        for ( list<string>::iterator i = collections.begin(); i != collections.end(); ++i ) {
            string ns = *i;
            invariant( NamespaceString::normal( ns ) );

            CollectionCatalogEntry* coll = _dbEntry->getCollectionCatalogEntry( txn, ns );

            CollectionOptions options = coll->getCollectionOptions( txn );
            if ( !options.temp )
                continue;

            WriteUnitOfWork wunit(txn);
            Status status = dropCollection( txn, ns );
            if ( !status.isOK() ) {
                warning() << "could not drop temp collection '" << ns << "': " << status;
                continue;
            }

            string cmdNs = _name + ".$cmd";
            repl::logOp( txn,
                         "c",
                         cmdNs.c_str(),
                         BSON( "drop" << nsToCollectionSubstring( ns ) ) );
            wunit.commit();
        }
    }

    bool Database::setProfilingLevel( OperationContext* txn, int newLevel , string& errmsg ) {
        if ( _profile == newLevel )
            return true;

        if ( newLevel < 0 || newLevel > 2 ) {
            errmsg = "profiling level has to be >=0 and <= 2";
            return false;
        }

        if ( newLevel == 0 ) {
            _profile = 0;
            return true;
        }

        if (!getOrCreateProfileCollection(txn, this, true, &errmsg))
            return false;

        _profile = newLevel;
        return true;
    }

    void Database::getStats( OperationContext* opCtx, BSONObjBuilder* output, double scale ) {
        list<string> collections;
        _dbEntry->getCollectionNamespaces( &collections );

        long long ncollections = 0;
        long long objects = 0;
        long long size = 0;
        long long storageSize = 0;
        long long numExtents = 0;
        long long indexes = 0;
        long long indexSize = 0;

        for (list<string>::const_iterator it = collections.begin(); it != collections.end(); ++it) {
            const string ns = *it;

            Collection* collection = getCollection( opCtx, ns );
            if ( !collection )
                continue;

            ncollections += 1;
            objects += collection->numRecords(opCtx);
            size += collection->dataSize(opCtx);

            BSONObjBuilder temp;
            storageSize += collection->getRecordStore()->storageSize( opCtx, &temp );
            numExtents += temp.obj()["numExtents"].numberInt(); // XXX

            indexes += collection->getIndexCatalog()->numIndexesTotal( opCtx );
            indexSize += collection->getIndexSize(opCtx);
        }

        output->appendNumber( "collections" , ncollections );
        output->appendNumber( "objects" , objects );
        output->append      ( "avgObjSize" , objects == 0 ? 0 : double(size) / double(objects) );
        output->appendNumber( "dataSize" , size / scale );
        output->appendNumber( "storageSize" , storageSize / scale);
        output->appendNumber( "numExtents" , numExtents );
        output->appendNumber( "indexes" , indexes );
        output->appendNumber( "indexSize" , indexSize / scale );

        _dbEntry->appendExtraStats( opCtx, output, scale );
    }

    Status Database::dropCollection( OperationContext* txn, const StringData& fullns ) {
        LOG(1) << "dropCollection: " << fullns << endl;
        massertNamespaceNotIndex( fullns, "dropCollection" );

        Collection* collection = getCollection( txn, fullns );
        if ( !collection ) {
            // collection doesn't exist
            return Status::OK();
        }

        {
            NamespaceString s( fullns );
            verify( s.db() == _name );

            if( s.isSystem() ) {
                if( s.coll() == "system.profile" ) {
                    if ( _profile != 0 )
                        return Status( ErrorCodes::IllegalOperation,
                                       "turn off profiling before dropping system.profile collection" );
                }
                else {
                    return Status( ErrorCodes::IllegalOperation, "can't drop system ns" );
                }
            }
        }

        BackgroundOperation::assertNoBgOpInProgForNs( fullns );

        audit::logDropCollection( currentClient.get(), fullns );

        try {
            Status s = collection->getIndexCatalog()->dropAllIndexes(txn, true);
            if ( !s.isOK() ) {
                warning() << "could not drop collection, trying to drop indexes"
                          << fullns << " because of " << s.toString();
                return s;
            }
        }
        catch( DBException& e ) {
            stringstream ss;
            ss << "drop: dropIndexes for collection failed. cause: " << e.what();
            ss << ". See http://dochub.mongodb.org/core/data-recovery";
            warning() << ss.str() << endl;
            return Status( ErrorCodes::InternalError, ss.str() );
        }

        verify( collection->_details->getTotalIndexCount( txn ) == 0 );
        LOG(1) << "\t dropIndexes done" << endl;

        Top::global.collectionDropped( fullns );

        Status s = _dbEntry->dropCollection( txn, fullns );

        _clearCollectionCache( txn, fullns ); // we want to do this always

        if ( !s.isOK() )
            return s;

        DEV {
            // check all index collection entries are gone
            string nstocheck = fullns.toString() + ".$";
            scoped_lock lk( _collectionLock );
            for ( CollectionMap::const_iterator i = _collections.begin();
                  i != _collections.end();
                  ++i ) {
                string temp = i->first;
                if ( temp.find( nstocheck ) != 0 )
                    continue;
                log() << "after drop, bad cache entries for: "
                      << fullns << " have " << temp;
                verify(0);
            }
        }

        return Status::OK();
    }

    void Database::_clearCollectionCache(OperationContext* txn, const StringData& fullns ) {
        scoped_lock lk( _collectionLock );
        _clearCollectionCache_inlock( txn, fullns );
    }

    void Database::_clearCollectionCache_inlock(OperationContext* txn, const StringData& fullns ) {
        verify( _name == nsToDatabaseSubstring( fullns ) );
        CollectionMap::const_iterator it = _collections.find( fullns.toString() );
        if ( it == _collections.end() )
            return;

        // Takes ownership of the collection
        txn->recoveryUnit()->registerChange(new RemoveCollectionChange(this, it->second));

        it->second->_cursorCache.invalidateAll(false);
        _collections.erase( it );
    }

    Collection* Database::getCollection( OperationContext* txn, const StringData& ns ) {
        invariant( _name == nsToDatabaseSubstring( ns ) );

        scoped_lock lk( _collectionLock );

        CollectionMap::const_iterator it = _collections.find( ns );
        if ( it != _collections.end() && it->second ) {
            return it->second;
        }

        auto_ptr<CollectionCatalogEntry> catalogEntry( _dbEntry->getCollectionCatalogEntry( txn, ns ) );
        if ( !catalogEntry.get() )
            return NULL;

        auto_ptr<RecordStore> rs( _dbEntry->getRecordStore( txn, ns ) );
        invariant( rs.get() ); // if catalogEntry exists, so should this

        // Not registering AddCollectionChange since this is for collections that already exist.
        Collection* c = new Collection( txn, ns, catalogEntry.release(), rs.release(), this );
        _collections[ns] = c;
        return c;
    }



    Status Database::renameCollection( OperationContext* txn,
                                       const StringData& fromNS,
                                       const StringData& toNS,
                                       bool stayTemp ) {

        audit::logRenameCollection( currentClient.get(), fromNS, toNS );

        { // remove anything cached
            Collection* coll = getCollection( txn, fromNS );
            if ( !coll )
                return Status( ErrorCodes::NamespaceNotFound, "collection not found to rename" );
            IndexCatalog::IndexIterator ii = coll->getIndexCatalog()->getIndexIterator( txn, true );
            while ( ii.more() ) {
                IndexDescriptor* desc = ii.next();
                _clearCollectionCache( txn, desc->indexNamespace() );
            }

            {
                scoped_lock lk( _collectionLock );
                _clearCollectionCache_inlock( txn, fromNS );
                _clearCollectionCache_inlock( txn, toNS );
            }

            Top::global.collectionDropped( fromNS.toString() );
        }

        txn->recoveryUnit()->registerChange( new AddCollectionChange(this, toNS) );
        return _dbEntry->renameCollection( txn, fromNS, toNS, stayTemp );
    }

    Collection* Database::getOrCreateCollection(OperationContext* txn, const StringData& ns) {
        Collection* c = getCollection( txn, ns );
        if ( !c ) {
            c = createCollection( txn, ns );
        }
        return c;
    }

    Collection* Database::createCollection( OperationContext* txn,
                                            const StringData& ns,
                                            const CollectionOptions& options,
                                            bool allocateDefaultSpace,
                                            bool createIdIndex ) {
        massert( 17399, "collection already exists", getCollection( txn, ns ) == NULL );
        massertNamespaceNotIndex( ns, "createCollection" );

        if ( serverGlobalParams.configsvr &&
             !( ns.startsWith( "config." ) ||
                ns.startsWith( "local." ) ||
                ns.startsWith( "admin." ) ) ) {
            uasserted(14037, "can't create user databases on a --configsvr instance");
        }

        if (NamespaceString::normal(ns)) {
            // This check only applies for actual collections, not indexes or other types of ns.
            uassert(17381, str::stream() << "fully qualified namespace " << ns << " is too long "
                                         << "(max is " << NamespaceString::MaxNsCollectionLen << " bytes)",
                    ns.size() <= NamespaceString::MaxNsCollectionLen);
        }

        NamespaceString nss( ns );
        uassert( 17316, "cannot create a blank collection", nss.coll() > 0 );

        audit::logCreateCollection( currentClient.get(), ns );

        txn->recoveryUnit()->registerChange( new AddCollectionChange(this, ns) );

        Status status = _dbEntry->createCollection(txn, ns, options, allocateDefaultSpace);
        massertNoTraceStatusOK(status);

        Collection* collection = getCollection(txn, ns);
        invariant(collection);

        if ( createIdIndex ) {
            if ( collection->requiresIdIndex() ) {
                if ( options.autoIndexId == CollectionOptions::YES ||
                     options.autoIndexId == CollectionOptions::DEFAULT ) {
                    IndexCatalog* ic = collection->getIndexCatalog();
                    uassertStatusOK(
                        ic->createIndexOnEmptyCollection(txn, ic->getDefaultIdIndexSpec()));
                }
            }

            if ( nss.isSystem() ) {
                authindex::createSystemIndexes( txn, collection );
            }

        }

        return collection;
    }

    const DatabaseCatalogEntry* Database::getDatabaseCatalogEntry() const {
        return _dbEntry;
    }

    void dropAllDatabasesExceptLocal(OperationContext* txn) {
        ScopedTransaction transaction(txn, MODE_X);
        Lock::GlobalWrite lk(txn->lockState());

        vector<string> n;
        StorageEngine* storageEngine = getGlobalEnvironment()->getGlobalStorageEngine();
        storageEngine->listDatabases(&n);

        if( n.size() == 0 ) return;
        log() << "dropAllDatabasesExceptLocal " << n.size() << endl;

        for (vector<string>::iterator i = n.begin(); i != n.end(); i++) {
            if (*i != "local") {
                Database* db = dbHolder().get(txn, *i);
                invariant(db);

                dropDatabase(txn, db);
            }
        }
    }

    void dropDatabase(OperationContext* txn, Database* db ) {
        invariant( db );

        string name = db->name(); // just to have safe
        LOG(1) << "dropDatabase " << name << endl;

        txn->lockState()->assertWriteLocked( name );

        BackgroundOperation::assertNoBgOpInProgForDb(name.c_str());

        audit::logDropDatabase( currentClient.get(), name );

        dbHolder().close( txn, name );
        db = NULL; // d is now deleted

        getGlobalEnvironment()->getGlobalStorageEngine()->dropDatabase( txn, name );
    }

namespace {
    /*
     * Extracts the storageEngine bson from the CollectionOptions provided.  Loops through each
     * provided storageEngine and asks the matching registered storage engine if the collection
     * options are valid.  Returns an error if the collection options are invalid.
     * If no matching registered storage engine is found, return an error.
     */
    Status validateStorageOptions(const CollectionOptions& options) {
        BSONObjIterator storageIt(options.storageEngine);

        while (storageIt.more()) {
            BSONElement storageElement = storageIt.next();
            StringData storageEngineName = storageElement.fieldNameStringData();
            invariant(storageElement.type() == mongo::Object);

            boost::scoped_ptr<StorageFactoriesIterator> sfi(getGlobalEnvironment()->
                                                            makeStorageFactoriesIterator());
            invariant(sfi);
            bool found = false;
            while (sfi->more()) {
                const StorageEngine::Factory* const& factory = sfi->next();
                if (storageEngineName != factory->getCanonicalName()) {
                    continue;
                }
                Status status = factory->validateCollectionStorageOptions(storageElement.Obj());
                if ( !status.isOK() ) {
                    return status;
                }
                found = true;
            }
            if (!found) {
                return Status(ErrorCodes::InvalidOptions, str::stream() << storageEngineName <<
                              " is not a registered storage engine for this server");
            }
        }
        return Status::OK();
    }
}

    /** { ..., capped: true, size: ..., max: ... }
     * @param createDefaultIndexes - if false, defers id (and other) index creation.
     * @return true if successful
    */
    Status userCreateNS( OperationContext* txn,
                         Database* db,
                         const StringData& ns,
                         BSONObj options,
                         bool logForReplication,
                         bool createDefaultIndexes ) {

        invariant( db );

        LOG(1) << "create collection " << ns << ' ' << options;

        if ( !NamespaceString::validCollectionComponent(ns) )
            return Status( ErrorCodes::InvalidNamespace,
                           str::stream() << "invalid ns: " << ns );

        Collection* collection = db->getCollection( txn, ns );

        if ( collection )
            return Status( ErrorCodes::NamespaceExists,
                           "collection already exists" );

        CollectionOptions collectionOptions;
        Status status = collectionOptions.parse(options);
        if ( !status.isOK() )
            return status;

        status = validateStorageOptions(collectionOptions);
        if ( !status.isOK() )
            return status;

        invariant( db->createCollection( txn, ns, collectionOptions, true, createDefaultIndexes ) );

        if ( logForReplication ) {
            if ( options.getField( "create" ).eoo() ) {
                BSONObjBuilder b;
                b << "create" << nsToCollectionSubstring( ns );
                b.appendElements( options );
                options = b.obj();
            }
            string logNs = nsToDatabase(ns) + ".$cmd";
            repl::logOp(txn, "c", logNs.c_str(), options);
        }

        return Status::OK();
    }
} // namespace mongo
