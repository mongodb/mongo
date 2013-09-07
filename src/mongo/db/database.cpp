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

#include "mongo/db/database.h"

#include <algorithm>
#include <boost/filesystem/operations.hpp>

#include "mongo/db/auth/auth_index_d.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/database_holder.h"
#include "mongo/db/instance.h"
#include "mongo/db/introspect.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/structure/collection.h"

namespace mongo {


    Database::~Database() {
        verify( Lock::isW() );
        _magic = 0;
        if( _ccByLoc.size() ) {
            log() << "\n\n\nWARNING: ccByLoc not empty on database close! "
                  << _ccByLoc.size() << ' ' << _name << endl;
        }

        for ( CollectionMap::iterator i = _collections.begin(); i != _collections.end(); ++i ) {
            delete i->second;
        }
        _collections.clear();

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

    Database::Database(const char *nm, bool& newDb, const string& path )
        : _name(nm), _path(path),
          _namespaceIndex( _path, _name ),
          _extentManager( _name, _path, directoryperdb /* this is a global right now */ ),
          _profileName(_name + ".system.profile"),
          _namespacesName(_name + ".system.namespaces"),
          _collectionLock( "Database::_collectionLock" )
    {
        Status status = validateDBName( _name );
        if ( !status.isOK() ) {
            warning() << "tried to open invalid db: " << _name << endl;
            uasserted( 10028, status.toString() );
        }

        try {
            newDb = _namespaceIndex.exists();
            _profile = cmdLine.defaultProfile;
            checkDuplicateUncasedNames(true);

            // If already exists, open.  Otherwise behave as if empty until
            // there's a write, then open.
            if (!newDb) {
                _namespaceIndex.init();
                openAllFiles();
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
            ss << "db already exists with different case other: [" << duplicate << "] me [" << _name << "]";
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
    void Database::openAllFiles() {
        verify(this);
        Status s = _extentManager.init();
        if ( !s.isOK() ) {
            msgasserted( 16966, str::stream() << "_extentManager.init failed: " << s.toString() );
        }
    }

    void Database::clearTmpCollections() {

        Lock::assertWriteLocked( _name );
        Client::Context ctx( _name );

        string systemNamespaces =  _name + ".system.namespaces";

        // Note: we build up a toDelete vector rather than dropping the collection inside the loop
        // to avoid modifying the system.namespaces collection while iterating over it since that
        // would corrupt the cursor.
        vector<string> toDelete;
        auto_ptr<Runner> runner(InternalPlanner::collectionScan(systemNamespaces));
        BSONObj nsObj;
        Runner::RunnerState state;
        while (Runner::RUNNER_ADVANCED == (state = runner->getNext(&nsObj, NULL))) {
            BSONElement e = nsObj.getFieldDotted( "options.temp" );
            if ( !e.trueValue() )
                continue;

            string ns = nsObj["name"].String();

            // Do not attempt to drop indexes
            if ( !NamespaceString::normal(ns.c_str()) )
                continue;

            toDelete.push_back(ns);
        }

        if (Runner::RUNNER_EOF != state) {
            warning() << "Internal error while reading collection " << systemNamespaces << endl;
        }

        for (size_t i=0; i < toDelete.size(); i++) {
            const string& ns = toDelete[i];

            string errmsg;
            BSONObjBuilder result;
            mongo::dropCollection(ns, errmsg, result); // XXX-ERH

            if ( errmsg.size() > 0 ) {
                warning() << "could not delete temp collection: " << ns
                          << " because of: " << errmsg << endl;
            }
        }
    }

    Extent* Database::allocExtent( const char *ns, int size, bool capped, bool enforceQuota ) {
        bool fromFreeList = true;
        Extent *e = DataFileMgr::allocFromFreeList( ns, size, capped );
        if( e == 0 ) {
            fromFreeList = false;
            e = _extentManager.createExtent( ns, size, capped, enforceQuota );
        }
        LOG(1) << "allocExtent " << ns << " size " << size << ' ' << fromFreeList << endl;
        return e;
    }


    bool Database::setProfilingLevel( int newLevel , string& errmsg ) {
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

        verify( cc().database() == this );

        if (!getOrCreateProfileCollection(this, true, &errmsg))
            return false;

        _profile = newLevel;
        return true;
    }

    void Database::dropCollection( const StringData& fullns ) {
        // TODO: XXX-ERH
        // move impl from pdfile.cpp here

        scoped_lock lk( _collectionLock );
        _collections.erase( fullns.toString() );
    }

    CollectionTemp* Database::getCollectionTemp( const StringData& ns ) {
        StringData dbName = nsToDatabaseSubstring( ns );
        verify( dbName == _name);

        scoped_lock lk( _collectionLock );

        string myns = ns.toString();

        CollectionMap::const_iterator it = _collections.find( myns );
        if ( it != _collections.end() ) {
            if ( it->second ) {
                //DEV {
                    NamespaceDetails* details = _namespaceIndex.details( ns );
                    verify( details == it->second->_details );
                    //}
                return it->second;
            }
        }

        NamespaceDetails* details = _namespaceIndex.details( ns );
        if ( !details ) {
            return NULL;
        }

        CollectionTemp* c = new CollectionTemp( ns, details, this );
        _collections[myns] = c;
        return c;
    }



    void addNewNamespaceToCatalog(const char *ns, const BSONObj *options = 0); // XXX

    Status Database::renameCollection( const StringData& fromNS, const StringData& toNS,
                                       bool stayTemp ) {

        // move data namespace
        Status s = _renameSingleNamespace( fromNS, toNS, stayTemp );
        if ( !s.isOK() )
            return s;

        NamespaceDetails* details = _namespaceIndex.details( toNS );
        verify( details );

        // move index namespaces
        string indexName = _name + ".system.indexes";
        BSONObj oldIndexSpec;
        while( Helpers::findOne( indexName, BSON( "ns" << fromNS ), oldIndexSpec ) ) {
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

            DiskLoc newIndexSpecLoc = theDataFileMgr.insert( indexName.c_str(),
                                                             newIndexSpec.objdata(),
                                                             newIndexSpec.objsize(),
                                                             false,
                                                             true,
                                                             false );
            int indexI = details->findIndexByName( oldIndexSpec.getStringField( "name" ) );
            IndexDetails &indexDetails = details->idx(indexI);
            string oldIndexNs = indexDetails.indexNamespace();
            indexDetails.info = newIndexSpecLoc;
            string newIndexNs = indexDetails.indexNamespace();

            Status s = _renameSingleNamespace( oldIndexNs, newIndexNs, false );
            if ( !s.isOK() )
                return s;

            deleteObjects( indexName.c_str(), oldIndexSpec, true, false, true );
        }

        Top::global.collectionDropped( fromNS.toString() );

        return Status::OK();
    }

    Status Database::_renameSingleNamespace( const StringData& fromNS, const StringData& toNS,
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
            _collections.erase( fromNSString );
            _collections.erase( toNSString );
        }

        ClientCursor::invalidate( fromNSString.c_str() );
        ClientCursor::invalidate( toNSString.c_str() );
        NamespaceDetailsTransient::eraseCollection( fromNSString ); // XXX
        NamespaceDetailsTransient::eraseCollection( toNSString ); // XXX

        // at this point, we haven't done anything destructive yet

        // ----
        // actually start moving
        // ----

        // this could throw, but if it does we're ok
        _namespaceIndex.add_ns( toNS, fromDetails );
        NamespaceDetails* toDetails = _namespaceIndex.details( toNS );

        try {
            toDetails->copyingFrom(toNSString.c_str(), fromDetails); // fixes extraOffset
        }
        catch( DBException& ) {
            // could end up here if .ns is full - if so try to clean up / roll back a little
            _namespaceIndex.kill_ns(toNSString.c_str());
            throw;
        }

        // at this point, code .ns stuff moved

        _namespaceIndex.kill_ns( fromNSString.c_str() );
        fromDetails = NULL;

        // fix system.namespaces
        BSONObj newSpec;
        {

            BSONObj oldSpec;
            if ( !Helpers::findOne( _namespacesName, BSON( "name" << fromNS ), oldSpec ) )
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
        addNewNamespaceToCatalog( toNSString.c_str(), newSpec.isEmpty() ? 0 : &newSpec );

        deleteObjects( _namespacesName.c_str(), BSON( "name" << fromNS ), false, false, true );

        return Status::OK();
    }

} // namespace mongo
