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

#include <boost/filesystem/operations.hpp>

#include "mongo/db/auth/auth_index_d.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/databaseholder.h"
#include "mongo/db/instance.h"
#include "mongo/db/introspect.h"
#include "mongo/db/pdfile.h"

namespace mongo {

    void assertDbAtLeastReadLocked(const Database *db) {
        if( db ) {
            Lock::assertAtLeastReadLocked(db->name());
        }
        else {
            verify( Lock::isLocked() );
        }
    }

    void assertDbWriteLocked(const Database *db) {
        if( db ) {
            Lock::assertWriteLocked(db->name());
        }
        else {
            verify( Lock::isW() );
        }
    }

    Database::~Database() {
        verify( Lock::isW() );
        _magic = 0;
        size_t n = _files.size();
        for ( size_t i = 0; i < n; i++ )
            delete _files[i];
        if( _ccByLoc.size() ) {
            log() << "\n\n\nWARNING: ccByLoc not empty on database close! "
                  << _ccByLoc.size() << ' ' << _name << endl;
        }
    }

    Database::Database(const char *nm, bool& newDb, const string& path )
        : _name(nm), _path(path), _namespaceIndex( _path, _name ),
          _profileName(_name + ".system.profile")
    {
        try {
            {
                // check db name is valid
                size_t L = strlen(nm);
                uassert( 10028 ,  "db name is empty", L > 0 );
                uassert( 10032 ,  "db name too long", L < 64 );
                uassert( 10029 ,  "bad db name [1]", *nm != '.' );
                uassert( 10030 ,  "bad db name [2]", nm[L-1] != '.' );
                uassert( 10031 ,  "bad char(s) in db name", strchr(nm, ' ') == 0 );
#ifdef _WIN32
                static const char* windowsReservedNames[] = {
                    "con", "prn", "aux", "nul",
                    "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
                    "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"
                };
                for ( size_t i = 0; i < (sizeof(windowsReservedNames) / sizeof(char*)); ++i ) {
                    if ( strcasecmp( nm, windowsReservedNames[i] ) == 0 ) {
                        stringstream errorString;
                        errorString << "db name \"" << nm << "\" is a reserved name";
                        uassert( 16185 , errorString.str(), false );
                    }
                }
#endif
            }
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
            // since destructor won't be called:
            for ( size_t i = 0; i < _files.size(); i++ ) {
                delete _files[i];
            }
            _files.clear();
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

    boost::filesystem::path Database::fileName( int n ) const {
        stringstream ss;
        ss << _name << '.' << n;
        boost::filesystem::path fullName;
        fullName = boost::filesystem::path(_path);
        if ( directoryperdb )
            fullName /= _name;
        fullName /= ss.str();
        return fullName;
    }

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
                dlog(2) << "openExistingFile " << n << " is already open" << endl;
                return true;
            }
        }

        {
            boost::filesystem::path fullName = fileName( n );
            string fullNameString = fullName.string();
            MongoDataFile *df = new MongoDataFile(n);
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

    // todo : we stop once a datafile dne.
    //        if one datafile were missing we should keep going for 
    //        repair purposes yet we do not.
    void Database::openAllFiles() {
        verify(this);
        int n = 0;
        while( openExistingFile(n) ) {
            n++;
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
        shared_ptr<Cursor> cursor = theDataFileMgr.findAll(systemNamespaces);
        while ( cursor && cursor->ok() ) {
            BSONObj nsObj = cursor->current();
            cursor->advance();

            BSONElement e = nsObj.getFieldDotted( "options.temp" );
            if ( !e.trueValue() )
                continue;

            string ns = nsObj["name"].String();

            // Do not attempt to drop indexes
            if ( !NamespaceString::normal(ns.c_str()) )
                continue;

            toDelete.push_back(ns);
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

    // todo: this is called a lot. streamline the common case
    MongoDataFile* Database::getFile( int n, int sizeNeeded , bool preallocateOnly) {
        verify(this);
        DEV assertDbAtLeastReadLocked(this);

        _namespaceIndex.init();
        if ( n < 0 || n >= DiskLoc::MaxFiles ) {
            out() << "getFile(): n=" << n << endl;
            massert( 10295 , "getFile(): bad file number value (corrupt db?): run repair", false);
        }
        DEV {
            if ( n > 100 ) {
                out() << "getFile(): n=" << n << endl;
            }
        }
        MongoDataFile* p = 0;
        if ( !preallocateOnly ) {
            while ( n >= (int) _files.size() ) {
                verify(this);
                if( !Lock::isWriteLocked(this->_name) ) {
                    log() << "error: getFile() called in a read lock, yet file to return is not yet open" << endl;
                    log() << "       getFile(" << n << ") _files.size:" <<_files.size() << ' ' << fileName(n).string() << endl;
                    log() << "       context ns: " << cc().ns() << endl;
                    verify(false);
                }
                _files.push_back(0);
            }
            p = _files[n];
        }
        if ( p == 0 ) {
            assertDbWriteLocked(this);
            boost::filesystem::path fullName = fileName( n );
            string fullNameString = fullName.string();
            p = new MongoDataFile(n);
            int minSize = 0;
            if ( n != 0 && _files[ n - 1 ] )
                minSize = _files[ n - 1 ]->getHeader()->fileLength;
            if ( sizeNeeded + DataFileHeader::HeaderSize > minSize )
                minSize = sizeNeeded + DataFileHeader::HeaderSize;
            try {
                p->open( fullNameString.c_str(), minSize, preallocateOnly );
            }
            catch ( AssertionException& ) {
                delete p;
                throw;
            }
            if ( preallocateOnly )
                delete p;
            else
                _files[n] = p;
        }
        return preallocateOnly ? 0 : p;
    }

    MongoDataFile* Database::addAFile( int sizeNeeded, bool preallocateNextFile ) {
        assertDbWriteLocked(this);
        int n = (int) _files.size();
        MongoDataFile *ret = getFile( n, sizeNeeded );
        if ( preallocateNextFile )
            preallocateAFile();
        return ret;
    }

    bool fileIndexExceedsQuota( const char *ns, int fileIndex, bool enforceQuota ) {
        return
            cmdLine.quota &&
            enforceQuota &&
            fileIndex >= cmdLine.quotaFiles &&
            // we don't enforce the quota on "special" namespaces as that could lead to problems -- e.g.
            // rejecting an index insert after inserting the main record.
            !NamespaceString::special( ns ) &&
            NamespaceString( ns ).db != "local";
    }
    
    MongoDataFile* Database::suitableFile( const char *ns, int sizeNeeded, bool preallocate, bool enforceQuota ) {

        // check existing files
        for ( int i=numFiles()-1; i>=0; i-- ) {
            MongoDataFile* f = getFile( i );
            if ( f->getHeader()->unusedLength >= sizeNeeded ) {
                if ( fileIndexExceedsQuota( ns, i-1, enforceQuota ) ) // NOTE i-1 is the value used historically for this check.
                    ;
                else
                    return f;
            }
        }

        if ( fileIndexExceedsQuota( ns, numFiles(), enforceQuota ) ) {
            if ( cc().hasWrittenThisPass() ) {
                warning() << "quota exceeded, but can't assert, probably going over quota for: " << ns << endl;
            }
            else {
                log() << "quota exceeded for namespace: " << ns << endl;
                uasserted(12501, "quota exceeded");
            }
        }

        // allocate files until we either get one big enough or hit maxSize
        for ( int i = 0; i < 8; i++ ) {
            MongoDataFile* f = addAFile( sizeNeeded, preallocate );

            if ( f->getHeader()->unusedLength >= sizeNeeded )
                return f;

            if ( f->getHeader()->fileLength >= MongoDataFile::maxSize() ) // this is as big as they get so might as well stop
                return f;
        }

        uasserted(14810, "couldn't allocate space (suitableFile)"); // callers don't check for null return code
        return 0;
    }

    MongoDataFile* Database::newestFile() {
        int n = numFiles();
        if ( n == 0 )
            return 0;
        return getFile(n-1);
    }


    Extent* Database::allocExtent( const char *ns, int size, bool capped, bool enforceQuota ) {
        // todo: when profiling, these may be worth logging into profile collection
        bool fromFreeList = true;
        Extent *e = DataFileMgr::allocFromFreeList( ns, size, capped );
        if( e == 0 ) {
            fromFreeList = false;
            e = suitableFile( ns, size, !capped, enforceQuota )->createExtent( ns, size, capped );
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

    bool Database::exists(int n) const { 
        return boost::filesystem::exists( fileName( n ) ); 
    }

    int Database::numFiles() const { 
        DEV assertDbAtLeastReadLocked(this);
        return (int) _files.size(); 
    }

    void Database::flushFiles( bool sync ) {
        assertDbAtLeastReadLocked(this);
        for( vector<MongoDataFile*>::iterator i = _files.begin(); i != _files.end(); i++ ) { 
            MongoDataFile *f = *i;
            f->flush(sync);
        }
    }

    long long Database::fileSize() const {
        long long size=0;
        for (int n=0; exists(n); n++)
            size += boost::filesystem::file_size( fileName(n) );
        return size;
    }

    Database* DatabaseHolder::getOrCreate( const string& ns , const string& path , bool& justCreated ) {
        string dbname = _todb( ns );
        {
            SimpleMutex::scoped_lock lk(_m);
            Lock::assertAtLeastReadLocked(ns);
            DBs& m = _paths[path];
            {
                DBs::iterator i = m.find(dbname); 
                if( i != m.end() ) {
                    justCreated = false;
                    return i->second;
                }
            }

            // todo: protect against getting sprayed with requests for different db names that DNE - 
            //       that would make the DBs map very large.  not clear what to do to handle though, 
            //       perhaps just log it, which is what we do here with the "> 40" : 
            bool cant = !Lock::isWriteLocked(ns);
            if( logLevel >= 1 || m.size() > 40 || cant || DEBUG_BUILD ) {
                log() << "opening db: " << (path==dbpath?"":path) << ' ' << dbname << endl;
            }
            massert(15927, "can't open database in a read lock. if db was just closed, consider retrying the query. might otherwise indicate an internal error", !cant);
        }

        // we mark our thread as having done writes now as we do not want any exceptions
        // once we start creating a new database
        cc().writeHappened();

        // this locks _m for defensive checks, so we don't want to be locked right here : 
        Database *db = new Database( dbname.c_str() , justCreated , path );

        {
            SimpleMutex::scoped_lock lk(_m);
            DBs& m = _paths[path];
            verify( m[dbname] == 0 );
            m[dbname] = db;
            _size++;
        }

        authindex::configureSystemIndexes(dbname);

        db->clearTmpCollections();

        return db;
    }

} // namespace mongo
