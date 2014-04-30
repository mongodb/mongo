// database.cpp

/**
*    Copyright (C) 2008 10gen Inc.
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

#include "mongo/pch.h"

#include "mongo/db/catalog/database.h"

#include <algorithm>
#include <boost/filesystem/operations.hpp>

#include "mongo/db/audit.h"
#include "mongo/db/auth/auth_index_d.h"
#include "mongo/db/background.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/structure/catalog/index_details.h"
#include "mongo/db/instance.h"
#include "mongo/db/introspect.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/storage/data_file.h"
#include "mongo/db/storage/extent.h"
#include "mongo/db/storage/extent_manager.h"
#include "mongo/db/storage/mmap_v1/dur_transaction.h"
#include "mongo/db/storage_options.h"
#include "mongo/db/structure/catalog/namespace_details.h"
#include "mongo/db/catalog/collection.h"

namespace mongo {

    MONGO_EXPORT_SERVER_PARAMETER(newCollectionsUsePowerOf2Sizes, bool, true);

    Status CollectionOptions::parse( const BSONObj& options ) {
        reset();

        BSONObjIterator i( options );
        while ( i.more() ) {
            BSONElement e = i.next();
            StringData fieldName = e.fieldName();

            if ( fieldName == "capped" ) {
                capped = e.trueValue();
            }
            else if ( fieldName == "size" ) {
                if ( !e.isNumber() )
                    return Status( ErrorCodes::BadValue, "size has to be a number" );
                cappedSize = e.numberLong();
                if ( cappedSize < 0 )
                    return Status( ErrorCodes::BadValue, "size has to be >= 0" );
                cappedSize += 0xff;
                cappedSize &= 0xffffffffffffff00LL;
                if ( cappedSize < Extent::minSize() )
                    cappedSize = Extent::minSize();
            }
            else if ( fieldName == "max" ) {
                if ( !e.isNumber() )
                    return Status( ErrorCodes::BadValue, "max has to be a number" );
                cappedMaxDocs = e.numberLong();
                if ( !NamespaceDetails::validMaxCappedDocs( &cappedMaxDocs ) )
                    return Status( ErrorCodes::BadValue,
                                   "max in a capped collection has to be < 2^31 or not set" );
            }
            else if ( fieldName == "$nExtents" ) {
                if ( e.type() == Array ) {
                    BSONObjIterator j( e.Obj() );
                    while ( j.more() ) {
                        BSONElement inner = j.next();
                        initialExtentSizes.push_back( inner.numberInt() );
                    }
                }
                else {
                    initialNumExtents = e.numberLong();
                }
            }
            else if ( fieldName == "autoIndexId" ) {
                if ( e.trueValue() )
                    autoIndexId = YES;
                else
                    autoIndexId = NO;
            }
            else if ( fieldName == "flags" ) {
                flags = e.numberInt();
                flagsSet = true;
            }
            else if ( fieldName == "temp" ) {
                temp = e.trueValue();
            }
        }

        return Status::OK();
    }

    BSONObj CollectionOptions::toBSON() const {
        BSONObjBuilder b;
        if ( capped ) {
            b.appendBool( "capped", true );
            if ( cappedSize )
                b.appendNumber( "size", cappedSize );
            if ( cappedMaxDocs )
                b.appendNumber( "max", cappedMaxDocs );
        }

        if ( initialNumExtents )
            b.appendNumber( "$nExtents", initialNumExtents );
        if ( !initialExtentSizes.empty() )
            b.append( "$nExtents", initialExtentSizes );

        if ( autoIndexId != DEFAULT )
            b.appendBool( "autoIndexId", autoIndexId == YES );

        if ( flagsSet )
            b.append( "flags", flags );

        if ( temp )
            b.appendBool( "temp", true );

        return b.obj();
    }

    void massertNamespaceNotIndex( const StringData& ns, const StringData& caller ) {
        massert( 17320,
                 str::stream() << "cannot do " << caller
                 << " on namespace with a $ in it: " << ns,
                 NamespaceString::normal( ns ) );
    }

    Database::~Database() {
        verify( Lock::isW() );
        _magic = 0;

        for ( CollectionMap::const_iterator i = _collections.begin(); i != _collections.end(); ++i )
            delete i->second;
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

    Database::Database(TransactionExperiment* txn, const char *nm, bool& newDb, const string& path )
        : _name(nm), _path(path),
          _namespaceIndex( _path, _name ),
          _extentManager(new ExtentManager(_name, _path, storageGlobalParams.directoryperdb)),
          _profileName(_name + ".system.profile"),
          _namespacesName(_name + ".system.namespaces"),
          _indexesName(_name + ".system.indexes"),
          _collectionLock( "Database::_collectionLock" )
    {
        Status status = validateDBName( _name );
        if ( !status.isOK() ) {
            warning() << "tried to open invalid db: " << _name << endl;
            uasserted( 10028, status.toString() );
        }

        try {
            newDb = _namespaceIndex.exists();
            _profile = serverGlobalParams.defaultProfile;
            checkDuplicateUncasedNames(true);

            // If already exists, open.  Otherwise behave as if empty until
            // there's a write, then open.
            if (!newDb) {
                _namespaceIndex.init();
                openAllFiles(txn);

                // upgrade freelist
                string oldFreeList = _name + ".$freelist";
                NamespaceDetails* details = _namespaceIndex.details( oldFreeList );
                if ( details ) {
                    if ( !details->firstExtent().isNull() ) {
                        _extentManager->freeExtents(txn,
                                                    details->firstExtent(),
                                                    details->lastExtent());
                    }
                    _namespaceIndex.kill_ns( oldFreeList );
                }
            }
            _magic = 781231;
        }
        catch(std::exception& e) {
            log() << "warning database " << path << " " << nm << " could not be opened" << endl;
            DBException* dbe = dynamic_cast<DBException*>(&e);
            if ( dbe != 0 ) {
                log() << "DBException " << dbe->getCode() << ": " << e.what() << endl;
            }
            else {
                log() << e.what() << endl;
            }
            _extentManager.reset();
            throw;
        }
    }

    void Database::checkDuplicateUncasedNames(bool inholderlock) const {
        string duplicate = duplicateUncasedName(inholderlock, _name, _path );
        if ( !duplicate.empty() ) {
            stringstream ss;
            ss << "db already exists with different case already have: [" << duplicate
               << "] trying to create [" << _name << "]";
            uasserted( DatabaseDifferCaseCode , ss.str() );
        }
    }

    /*static*/
    string Database::duplicateUncasedName( bool inholderlock, const string &name, const string &path, set< string > *duplicates ) {
        Lock::assertAtLeastReadLocked(name);

        if ( duplicates ) {
            duplicates->clear();
        }

        vector<string> others;
        getDatabaseNames( others , path );

        set<string> allShortNames;
        dbHolder().getAllShortNames( allShortNames );

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

    // todo : we stop once a datafile dne.
    //        if one datafile were missing we should keep going for
    //        repair purposes yet we do not.
    void Database::openAllFiles(TransactionExperiment* txn) {
        verify(this);
        Status s = _extentManager->init(txn);
        if ( !s.isOK() ) {
            msgasserted( 16966, str::stream() << "_extentManager.init failed: " << s.toString() );
        }
    }

    void Database::clearTmpCollections(TransactionExperiment* txn) {

        Lock::assertWriteLocked( _name );

        // Note: we build up a toDelete vector rather than dropping the collection inside the loop
        // to avoid modifying the system.namespaces collection while iterating over it since that
        // would corrupt the cursor.
        vector<string> toDelete;
        {
            Collection* coll = getCollection( txn, _namespacesName );
            if ( coll ) {
                scoped_ptr<RecordIterator> it( coll->getIterator() );
                DiskLoc next;
                while ( !( next = it->getNext() ).isNull() ) {
                    BSONObj nsObj = coll->docFor( next );

                    BSONElement e = nsObj.getFieldDotted( "options.temp" );
                    if ( !e.trueValue() )
                        continue;

                    string ns = nsObj["name"].String();

                    // Do not attempt to drop indexes
                    if ( !NamespaceString::normal(ns.c_str()) )
                        continue;

                    toDelete.push_back(ns);
                }
            }
        }

        for (size_t i=0; i < toDelete.size(); i++) {
            BSONObj info;
            // using DBDirectClient to ensure this ends up in opLog
            bool ok = DBDirectClient().dropCollection(toDelete[i], &info);
            if (!ok)
                warning() << "could not drop temp collection '" << toDelete[i] << "': " << info;
        }
    }

    long long Database::fileSize() const { return _extentManager->fileSize(); }

    int Database::numFiles() const { return _extentManager->numFiles(); }

    void Database::flushFiles( bool sync ) { return _extentManager->flushFiles( sync ); }

    bool Database::setProfilingLevel( TransactionExperiment* txn, int newLevel , string& errmsg ) {
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

    Status Database::dropCollection( TransactionExperiment* txn, const StringData& fullns ) {
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

        verify( collection->_details->getTotalIndexCount() == 0 );
        LOG(1) << "\t dropIndexes done" << endl;

        Top::global.collectionDropped( fullns );

        Status s = _dropNS( txn, fullns );

        _clearCollectionCache( fullns ); // we want to do this always

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

    void Database::_clearCollectionCache( const StringData& fullns ) {
        scoped_lock lk( _collectionLock );
        _clearCollectionCache_inlock( fullns );
    }

    void Database::_clearCollectionCache_inlock( const StringData& fullns ) {
        verify( _name == nsToDatabaseSubstring( fullns ) );
        CollectionMap::const_iterator it = _collections.find( fullns.toString() );
        if ( it == _collections.end() )
            return;

        delete it->second; // this also deletes all cursors + runners
        _collections.erase( it );
    }

    Collection* Database::getCollection( const StringData& ns ) {
        DurTransaction txn; // TODO remove once we require reads to have transactions
        return getCollection(&txn, ns);
    }

    Collection* Database::getCollection( TransactionExperiment* txn, const StringData& ns ) {
        verify( _name == nsToDatabaseSubstring( ns ) );

        scoped_lock lk( _collectionLock );

        CollectionMap::const_iterator it = _collections.find( ns );
        if ( it != _collections.end() ) {
            if ( it->second ) {
                DEV {
                    NamespaceDetails* details = _namespaceIndex.details( ns );
                    if ( details != it->second->_details ) {
                        log() << "about to crash for mismatch on ns: " << ns
                              << " current: " << (void*)details
                              << " cached: " << (void*)it->second->_details;
                    }
                    verify( details == it->second->_details );
                }
                return it->second;
            }
        }

        NamespaceDetails* details = _namespaceIndex.details( ns );
        if ( !details ) {
            return NULL;
        }

        Collection* c = new Collection( txn, ns, details, this );
        _collections[ns] = c;
        return c;
    }



    Status Database::renameCollection( TransactionExperiment* txn,
                                       const StringData& fromNS,
                                       const StringData& toNS,
                                       bool stayTemp ) {

        // move data namespace
        Status s = _renameSingleNamespace( txn, fromNS, toNS, stayTemp );
        if ( !s.isOK() )
            return s;

        NamespaceDetails* details = _namespaceIndex.details( toNS );
        verify( details );

        audit::logRenameCollection( currentClient.get(), fromNS, toNS );

        Collection* systemIndexCollection = getCollection(txn, _indexesName);

        // move index namespaces
        BSONObj oldIndexSpec;
        while (Helpers::findOne(systemIndexCollection, BSON("ns" << fromNS), oldIndexSpec)) {
            oldIndexSpec = oldIndexSpec.getOwned();

            BSONObj newIndexSpec;
            {
                BSONObjBuilder b;
                BSONObjIterator i( oldIndexSpec );
                while( i.more() ) {
                    BSONElement e = i.next();
                    if ( strcmp( e.fieldName(), "ns" ) != 0 )
                        b.append( e );
                    else
                        b << "ns" << toNS;
                }
                newIndexSpec = b.obj();
            }

            StatusWith<DiskLoc> newIndexSpecLoc =
                systemIndexCollection->insertDocument( txn, newIndexSpec, false );
            if ( !newIndexSpecLoc.isOK() )
                return newIndexSpecLoc.getStatus();

            const string& indexName = oldIndexSpec.getStringField( "name" );

            {
                // fix IndexDetails pointer
                int indexI = details->_catalogFindIndexByName(
                                        systemIndexCollection, indexName, false);

                IndexDetails& indexDetails = details->idx(indexI);
                *txn->writing(&indexDetails.info) = newIndexSpecLoc.getValue(); // XXX: dur
            }

            {
                // move underlying namespac
                string oldIndexNs = IndexDescriptor::makeIndexNamespace( fromNS, indexName );
                string newIndexNs = IndexDescriptor::makeIndexNamespace( toNS, indexName );

                Status s = _renameSingleNamespace( txn, oldIndexNs, newIndexNs, false );
                if ( !s.isOK() )
                    return s;
            }

            deleteObjects( txn, _indexesName, oldIndexSpec, true, false, true );
        }

        Top::global.collectionDropped( fromNS.toString() );

        return Status::OK();
    }

    Status Database::_renameSingleNamespace( TransactionExperiment* txn,
                                             const StringData& fromNS,
                                             const StringData& toNS,
                                             bool stayTemp ) {

        // TODO: make it so we dont't need to do this
        string fromNSString = fromNS.toString();
        string toNSString = toNS.toString();

        // some sanity checking
        NamespaceDetails* fromDetails = _namespaceIndex.details( fromNS );
        if ( !fromDetails )
            return Status( ErrorCodes::BadValue, "from namespace doesn't exist" );

        if ( _namespaceIndex.details( toNS ) )
            return Status( ErrorCodes::BadValue, "to namespace already exists" );

        // remove anything cached
        {
            scoped_lock lk( _collectionLock );
            _clearCollectionCache_inlock( fromNSString );
            _clearCollectionCache_inlock( toNSString );
        }

        // at this point, we haven't done anything destructive yet

        // ----
        // actually start moving
        // ----

        // this could throw, but if it does we're ok
        _namespaceIndex.add_ns( toNS, fromDetails );
        NamespaceDetails* toDetails = _namespaceIndex.details( toNS );

        try {
            toDetails->copyingFrom(toNSString.c_str(),
                                   _namespaceIndex,
                                   fromDetails); // fixes extraOffset
        }
        catch( DBException& ) {
            // could end up here if .ns is full - if so try to clean up / roll back a little
            _namespaceIndex.kill_ns( toNSString );
            _clearCollectionCache(toNSString);
            throw;
        }

        // at this point, code .ns stuff moved

        _namespaceIndex.kill_ns( fromNSString );
        _clearCollectionCache(fromNSString);
        fromDetails = NULL;

        // fix system.namespaces
        BSONObj newSpec;
        {

            BSONObj oldSpec;
            if ( !Helpers::findOne( getCollection( txn, _namespacesName ),
                                    BSON( "name" << fromNS ),
                                    oldSpec ) )
                return Status( ErrorCodes::InternalError, "can't find system.namespaces entry" );

            BSONObjBuilder b;
            BSONObjIterator i( oldSpec.getObjectField( "options" ) );
            while( i.more() ) {
                BSONElement e = i.next();
                if ( strcmp( e.fieldName(), "create" ) != 0 ) {
                    if (stayTemp || (strcmp(e.fieldName(), "temp") != 0))
                        b.append( e );
                }
                else {
                    b << "create" << toNS;
                }
            }
            newSpec = b.obj();
        }

        _addNamespaceToCatalog( txn, toNSString, newSpec.isEmpty() ? 0 : &newSpec );

        deleteObjects( txn, _namespacesName, BSON( "name" << fromNS ), false, false, true );

        return Status::OK();
    }

    Collection* Database::getOrCreateCollection( const StringData& ns ) {
        DurTransaction txn; // TODO remove once we require reads to have transactions
        return getOrCreateCollection(&txn, ns);
    }
    Collection* Database::getOrCreateCollection(TransactionExperiment* txn, const StringData& ns) {
        Collection* c = getCollection( txn, ns );
        if ( !c ) {
            c = createCollection( txn, ns );
        }
        return c;
    }

    namespace {
        int _massageExtentSize( long long size ) {
            if ( size < Extent::minSize() )
                return Extent::minSize();
            if ( size > Extent::maxSize() )
                return Extent::maxSize();
            return static_cast<int>( size );
        }
    }

    Collection* Database::createCollection( TransactionExperiment* txn,
                                            const StringData& ns,
                                            const CollectionOptions& options,
                                            bool allocateDefaultSpace,
                                            bool createIdIndex ) {
        massert( 17399, "collection already exists", _namespaceIndex.details( ns ) == NULL );
        massertNamespaceNotIndex( ns, "createCollection" );
        _namespaceIndex.init();

        if ( serverGlobalParams.configsvr &&
             !( ns.startsWith( "config." ) ||
                ns.startsWith( "local." ) ||
                ns.startsWith( "admin." ) ) ) {
            uasserted(14037, "can't create user databases on a --configsvr instance");
        }

        if (NamespaceString::normal(ns)) {
            // This check only applies for actual collections, not indexes or other types of ns.
            uassert(17381, str::stream() << "fully qualified namespace " << ns << " is too long "
                                         << "(max is " << Namespace::MaxNsColletionLen << " bytes)",
                    ns.size() <= Namespace::MaxNsColletionLen);
        }

        NamespaceString nss( ns );
        uassert( 17316, "cannot create a blank collection", nss.coll() > 0 );

        audit::logCreateCollection( currentClient.get(), ns );

        _namespaceIndex.add_ns( ns, DiskLoc(), options.capped );
        BSONObj optionsAsBSON = options.toBSON();
        _addNamespaceToCatalog( txn, ns, &optionsAsBSON );

        Collection* collection = getCollection( txn, ns );
        massert( 17400, "_namespaceIndex.add_ns failed?", collection );

        // allocation strategy set explicitly in flags or by server-wide default
        if ( !options.capped ) {
            if ( options.flagsSet ) {
                collection->setUserFlag( txn, options.flags );
            }
            else if ( newCollectionsUsePowerOf2Sizes ) {
                collection->setUserFlag( txn, NamespaceDetails::Flag_UsePowerOf2Sizes );
            }
        }

        if ( options.cappedMaxDocs > 0 )
            collection->setMaxCappedDocs( options.cappedMaxDocs );

        if ( allocateDefaultSpace ) {
            if ( options.initialNumExtents > 0 ) {
                int size = _massageExtentSize( options.cappedSize );
                for ( int i = 0; i < options.initialNumExtents; i++ ) {
                    collection->increaseStorageSize( txn, size, false );
                }
            }
            else if ( !options.initialExtentSizes.empty() ) {
                for ( size_t i = 0; i < options.initialExtentSizes.size(); i++ ) {
                    int size = options.initialExtentSizes[i];
                    size = _massageExtentSize( size );
                    collection->increaseStorageSize( txn, size, false );
                }
            }
            else if ( options.capped ) {
                // normal
                while ( collection->storageSize() < options.cappedSize ) {
                    int sz = _massageExtentSize( options.cappedSize - collection->storageSize() );
                    sz &= 0xffffff00;
                    collection->increaseStorageSize( txn, sz, true );
                }
            }
            else {
                collection->increaseStorageSize( txn, Extent::initialSize( 128 ), false );
            }
        }

        if ( createIdIndex ) {
            if ( collection->requiresIdIndex() ) {
                if ( options.autoIndexId == CollectionOptions::YES ||
                     options.autoIndexId == CollectionOptions::DEFAULT ) {
                    uassertStatusOK( collection->getIndexCatalog()->ensureHaveIdIndex(txn) );
                }
            }

            if ( nss.isSystem() ) {
                authindex::createSystemIndexes( txn, collection );
            }

        }

        return collection;
    }


    void Database::_addNamespaceToCatalog( TransactionExperiment* txn,
                                           const StringData& ns,
                                           const BSONObj* options ) {
        LOG(1) << "Database::_addNamespaceToCatalog ns: " << ns << endl;
        if ( nsToCollectionSubstring( ns ) == "system.namespaces" ) {
            // system.namespaces holds all the others, so it is not explicitly listed in the catalog.
            return;
        }

        BSONObjBuilder b;
        b.append("name", ns);
        if ( options && !options->isEmpty() )
            b.append("options", *options);
        BSONObj obj = b.done();

        Collection* collection = getCollection( txn, _namespacesName );
        if ( !collection )
            collection = createCollection( txn, _namespacesName );
        StatusWith<DiskLoc> loc = collection->insertDocument( txn, obj, false );
        uassertStatusOK( loc.getStatus() );
    }

    Status Database::_dropNS( TransactionExperiment* txn, const StringData& ns ) {

        NamespaceDetails* d = _namespaceIndex.details( ns );
        if ( !d )
            return Status( ErrorCodes::NamespaceNotFound,
                           str::stream() << "ns not found: " << ns );

        BackgroundOperation::assertNoBgOpInProgForNs( ns );

        {
            // remove from the system catalog
            BSONObj cond = BSON( "name" << ns );   // { name: "colltodropname" }
            deleteObjects( txn, _namespacesName, cond, false, false, true);
        }

        // free extents
        if( !d->firstExtent().isNull() ) {
            _extentManager->freeExtents(txn, d->firstExtent(), d->lastExtent());
            d->setFirstExtentInvalid();
            d->setLastExtentInvalid();
        }

        // remove from the catalog hashtable
        _namespaceIndex.kill_ns( ns );

        return Status::OK();
    }

    void Database::getFileFormat( int* major, int* minor ) {
        if ( _extentManager->numFiles() == 0 ) {
            *major = 0;
            *minor = 0;
            return;
        }
        DurTransaction txn; // TODO get rid of this once reads need transactions
        const DataFile* df = _extentManager->getFile( &txn, 0 );
        *major = df->getHeader()->version;
        *minor = df->getHeader()->versionMinor;
    }

} // namespace mongo
