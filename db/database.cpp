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

#include "pch.h"
#include "pdfile.h"
#include "database.h"
#include "instance.h"
#include "clientcursor.h"

namespace mongo {

    bool Database::_openAllFiles = false;

    Database::~Database() {
        magic = 0;
        size_t n = files.size();
        for ( size_t i = 0; i < n; i++ )
            delete files[i];
        if( ccByLoc.size() ) {
            log() << "\n\n\nWARNING: ccByLoc not empty on database close! " << ccByLoc.size() << ' ' << name << endl;
        }
    }

    Database::Database(const char *nm, bool& newDb, const string& _path )
        : name(nm), path(_path), namespaceIndex( path, name ),
          profileName(name + ".system.profile") {
        try {

        {
            // check db name is valid
            size_t L = strlen(nm);
            uassert( 10028 ,  "db name is empty", L > 0 );
            uassert( 10029 ,  "bad db name [1]", *nm != '.' );
            uassert( 10030 ,  "bad db name [2]", nm[L-1] != '.' );
            uassert( 10031 ,  "bad char(s) in db name", strchr(nm, ' ') == 0 );
            uassert( 10032 ,  "db name too long", L < 64 );
        }

        newDb = namespaceIndex.exists();
        profile = cmdLine.defaultProfile;

        checkDuplicateUncasedNames();

        // If already exists, open.  Otherwise behave as if empty until
        // there's a write, then open.
        if ( ! newDb || cmdLine.defaultProfile ) {
            namespaceIndex.init();
            if( _openAllFiles )
                openAllFiles();

        }


        magic = 781231;
        } catch(...) { 
            // since destructor won't be called:
            for ( size_t i = 0; i < files.size(); i++ )
                delete files[i];
            throw;
        }
    }
    
    void Database::checkDuplicateUncasedNames() const {
        string duplicate = duplicateUncasedName( name, path );
        if ( !duplicate.empty() ) {
            stringstream ss;
            ss << "db already exists with different case other: [" << duplicate << "] me [" << name << "]";
            uasserted( DatabaseDifferCaseCode , ss.str() );
        }
    }

    string Database::duplicateUncasedName( const string &name, const string &path, set< string > *duplicates ) {
        if ( duplicates ) {
            duplicates->clear();   
        }
        
        vector<string> others;
        getDatabaseNames( others , path );
        
        set<string> allShortNames;
        dbHolder.getAllShortNames( allShortNames );
        
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
        ss << name << '.' << n;
        boost::filesystem::path fullName;
        fullName = boost::filesystem::path(path);
        if ( directoryperdb )
            fullName /= name;
        fullName /= ss.str();
        return fullName;
    }

    void Database::openAllFiles() {
        int n = 0;
        while( exists(n) ) {
            getFile(n);
            n++;
        }
        // If last file is empty, consider it preallocated and make sure it's not mapped
        // until a write is requested
        if ( n > 1 && getFile( n - 1 )->getHeader()->isEmpty() ) {
            delete files[ n - 1 ];
            files.pop_back();
        }
    }

    MongoDataFile* Database::getFile( int n, int sizeNeeded , bool preallocateOnly) {
        assert(this);

        namespaceIndex.init();
        if ( n < 0 || n >= DiskLoc::MaxFiles ) {
            out() << "getFile(): n=" << n << endl;
            massert( 10295 , "getFile(): bad file number value (corrupt db?): run repair", false);
        }
        DEV {
            if ( n > 100 )
                out() << "getFile(): n=" << n << "?" << endl;
        }
        MongoDataFile* p = 0;
        if ( !preallocateOnly ) {
            while ( n >= (int) files.size() )
                files.push_back(0);
            p = files[n];
        }
        if ( p == 0 ) {
            boost::filesystem::path fullName = fileName( n );
            string fullNameString = fullName.string();
            p = new MongoDataFile(n);
            int minSize = 0;
            if ( n != 0 && files[ n - 1 ] )
                minSize = files[ n - 1 ]->getHeader()->fileLength;
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
                files[n] = p;
        }
        return preallocateOnly ? 0 : p;
    }

    MongoDataFile* Database::addAFile( int sizeNeeded, bool preallocateNextFile ) {
        int n = (int) files.size();
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

        if ( fileIndexExceedsQuota( ns, numFiles(), enforceQuota ) )
            uasserted(12501, "quota exceeded");

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
        if ( profile == newLevel )
            return true;

        if ( newLevel < 0 || newLevel > 2 ) {
            errmsg = "profiling level has to be >=0 and <= 2";
            return false;
        }

        if ( newLevel == 0 ) {
            profile = 0;
            return true;
        }

        assert( cc().database() == this );

        if ( ! namespaceIndex.details( profileName.c_str() ) ) {
            log() << "creating profile collection: " << profileName << endl;
            BSONObjBuilder spec;
            spec.appendBool( "capped", true );
            spec.append( "size", 1024*1024 );
            if ( ! userCreateNS( profileName.c_str(), spec.done(), errmsg , false /* we don't replica profile messages */ ) ) {
                return false;
            }
        }
        profile = newLevel;
        return true;
    }

    void Database::flushFiles( bool sync ) const {
        dbMutex.assertAtLeastReadLocked();
        for ( unsigned i=0; i<files.size(); i++ ) {
            files[i]->flush( sync );
        }
    }

    long long Database::fileSize() const {
        long long size=0;
        for (int n=0; exists(n); n++)
            size += boost::filesystem::file_size( fileName(n) );
        return size;
    }

    Database* DatabaseHolder::getOrCreate( const string& ns , const string& path , bool& justCreated ) {
        dbMutex.assertWriteLocked();
        DBs& m = _paths[path];

        string dbname = _todb( ns );

        Database* & db = m[dbname];
        if ( db ) {
            justCreated = false;
            return db;
        }

        log(1) << "Accessing: " << dbname << " for the first time" << endl;
        try {
            db = new Database( dbname.c_str() , justCreated , path );
        }
        catch ( ... ) {
            m.erase( dbname );
            throw;
        }
        _size++;
        return db;
    }

} // namespace mongo
