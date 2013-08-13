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

namespace mongo {


    Database::~Database() {
        verify( Lock::isW() );
        _magic = 0;
        if( _ccByLoc.size() ) {
            log() << "\n\n\nWARNING: ccByLoc not empty on database close! "
                  << _ccByLoc.size() << ' ' << _name << endl;
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

    Database::Database(const char *nm, bool& newDb, const string& path )
        : _name(nm), _path(path),
          _namespaceIndex( _path, _name ),
          _extentManager( _name, _path, directoryperdb /* this is a global right now */ ),
          _profileName(_name + ".system.profile")
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

    /*
    bool Database::openExistingFile( int n ) {
        verify(this);
        Lock::assertWriteLocked(_name);
        {
            // must not yet be visible to others as we aren't in the db's write lock and
            // we will write to _files vector - thus this assert.
            bool loaded = dbHolder().__isLoaded(_name, _path);
            verify( !loaded );
        }
        // additionally must be in the dbholder mutex (no assert for that yet)

        // todo: why here? that could be bad as we may be read locked only here
        _namespaceIndex.init();

        if ( n < 0 || n >= DiskLoc::MaxFiles ) {
            massert( 15924 , str::stream() << "getFile(): bad file number value " << n << " (corrupt db?): run repair", false);
        }

        {
            if( n < (int) _files.size() && _files[n] ) {
                MONGO_DLOG(2) << "openExistingFile " << n << " is already open" << endl;
                return true;
            }
        }

        {
            boost::filesystem::path fullName = fileName( n );
            string fullNameString = fullName.string();
            DataFile *df = new DataFile(n);
            try {
                if( !df->openExisting( fullNameString.c_str() ) ) { 
                    delete df;
                    return false;
                }
            }
            catch ( AssertionException& ) {
                delete df;
                throw;
            }
            while ( n >= (int) _files.size() ) {
                _files.push_back(0);
            }
            _files[n] = df;
        }

        return true;
    }
    */
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
        auto_ptr<Runner> runner(InternalPlanner::findAll(systemNamespaces));
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
            dropCollection(ns, errmsg, result);

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


} // namespace mongo
