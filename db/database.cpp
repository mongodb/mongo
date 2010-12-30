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
          profileName(name + ".system.profile")
    {
        
        { // check db name is valid
            size_t L = strlen(nm);
            uassert( 10028 ,  "db name is empty", L > 0 );
            uassert( 10029 ,  "bad db name [1]", *nm != '.' );
            uassert( 10030 ,  "bad db name [2]", nm[L-1] != '.' );
            uassert( 10031 ,  "bad char(s) in db name", strchr(nm, ' ') == 0 );
            uassert( 10032 ,  "db name too long", L < 64 );
        }
        
        newDb = namespaceIndex.exists();
        profile = 0;

        {
            vector<string> others;
            getDatabaseNames( others , path );
            
            for ( unsigned i=0; i<others.size(); i++ ){

                if ( strcasecmp( others[i].c_str() , nm ) )
                    continue;

                if ( strcmp( others[i].c_str() , nm ) == 0 )
                    continue;
                
                stringstream ss;
                ss << "db already exists with different case other: [" << others[i] << "] me [" << nm << "]";
                uasserted( DatabaseDifferCaseCode , ss.str() );
            }
        }

        
        // If already exists, open.  Otherwise behave as if empty until
        // there's a write, then open.
        if ( ! newDb || cmdLine.defaultProfile ) {
            namespaceIndex.init();
            if( _openAllFiles )
                openAllFiles();
            
        }
       

        magic = 781231;
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

    MongoDataFile* Database::suitableFile( int sizeNeeded, bool preallocate ) {
        MongoDataFile* f = newestFile();
        if ( !f ) {
            f = addAFile( sizeNeeded, preallocate );                
        }
        for ( int i = 0; i < 8; i++ ) {
            if ( f->getHeader()->unusedLength >= sizeNeeded )
                break;
            f = addAFile( sizeNeeded, preallocate );
            if ( f->getHeader()->fileLength >= MongoDataFile::maxSize() ) // this is as big as they get so might as well stop
                break;
        }
        return f;
    }

    MongoDataFile* Database::newestFile() {
        int n = numFiles();
        if ( n == 0 )
            return 0;
        return getFile(n-1);
    }

    
    Extent* Database::allocExtent( const char *ns, int size, bool capped ) { 
        Extent *e = DataFileMgr::allocFromFreeList( ns, size, capped );
        if( e ) 
            return e;
        return suitableFile( size, !capped )->createExtent( ns, size, capped );
    }
    
    
    bool Database::setProfilingLevel( int newLevel , string& errmsg ){
        if ( profile == newLevel )
            return true;
        
        if ( newLevel < 0 || newLevel > 2 ){
            errmsg = "profiling level has to be >=0 and <= 2";
            return false;
        }
        
        if ( newLevel == 0 ){
            profile = 0;
            return true;
        }
        
        assert( cc().database() == this );

        if ( ! namespaceIndex.details( profileName.c_str() ) ){
            log(1) << "creating profile ns: " << profileName << endl;
            BSONObjBuilder spec;
            spec.appendBool( "capped", true );
            spec.append( "size", 131072.0 );
            if ( ! userCreateNS( profileName.c_str(), spec.done(), errmsg , true ) ){
                return false;
            }
        }
        profile = newLevel;
        return true;
    }

    void Database::finishInit(){
        if ( cmdLine.defaultProfile == profile )
            return;
        
        string errmsg;
        massert( 12506 , errmsg , setProfilingLevel( cmdLine.defaultProfile , errmsg ) );
    }

    bool Database::validDBName( const string& ns ){
        if ( ns.size() == 0 || ns.size() > 64 )
            return false;
        size_t good = strcspn( ns.c_str() , "/\\. \"" );
        return good == ns.size();
    }

    void Database::flushFiles( bool sync ) const {
        dbMutex.assertAtLeastReadLocked();
        for ( unsigned i=0; i<files.size(); i++ ){
            files[i]->flush( sync );
        }
    }

    long long Database::fileSize() const {
        long long size=0;
        for (int n=0; exists(n); n++)
            size += boost::filesystem::file_size( fileName(n) );
        return size;
    }

    Database* DatabaseHolder::getOrCreate( const string& ns , const string& path , bool& justCreated ){
        dbMutex.assertWriteLocked();
        DBs& m = _paths[path];
        
        string dbname = _todb( ns );
        
        Database* & db = m[dbname];
        if ( db ){
            justCreated = false;
            return db;
        }
        
        log(1) << "Accessing: " << dbname << " for the first time" << endl;
        try {
            db = new Database( dbname.c_str() , justCreated , path );
        }
        catch ( ... ){
            m.erase( dbname );
            throw;
        }
        _size++;
        return db;
    }
    
} // namespace mongo
