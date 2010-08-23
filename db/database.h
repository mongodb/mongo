// database.h

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

#pragma once

#include "cmdline.h"

namespace mongo {

    class ClientCursor;

    /**
     * Database represents a database database
     * Each database database has its own set of files -- dbname.ns, dbname.0, dbname.1, ...
     * NOT memory mapped
    */
    class Database {
    public:
        static bool _openAllFiles;
        
        Database(const char *nm, bool& newDb, const string& _path = dbpath);
        
        ~Database() {
            magic = 0;
            btreeStore->closeFiles(name, path);
            size_t n = files.size();
            for ( size_t i = 0; i < n; i++ )
                delete files[i];
        }
        
        /**
         * tries to make sure that this hasn't been deleted
         */
        bool isOk(){
            return magic == 781231;
        }

        bool isEmpty(){
            return ! namespaceIndex.allocated();
        }

        boost::filesystem::path fileName( int n ) {
            stringstream ss;
            ss << name << '.' << n;
            boost::filesystem::path fullName;
            fullName = boost::filesystem::path(path);
            if ( directoryperdb )
                fullName /= name;
            fullName /= ss.str();
            return fullName;
        }
        
        bool exists(int n) { 
            return boost::filesystem::exists( fileName( n ) );
        }

        void openAllFiles() { 
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

        MongoDataFile* getFile( int n, int sizeNeeded = 0, bool preallocateOnly = false ) {
            assert(this);

            namespaceIndex.init();
            if ( n < 0 || n >= DiskLoc::MaxFiles ) {
                out() << "getFile(): n=" << n << endl;
#if 0
                if( n >= RecCache::Base && n <= RecCache::Base+1000 )
                    massert( 10294 , "getFile(): bad file number - using recstore db w/nonrecstore db build?", false);
#endif
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

        MongoDataFile* addAFile( int sizeNeeded, bool preallocateNextFile ) {
            int n = (int) files.size();
            MongoDataFile *ret = getFile( n, sizeNeeded );
            if ( preallocateNextFile )
                preallocateAFile();
            return ret;
        }
        
        // safe to call this multiple times - the implementation will only preallocate one file
        void preallocateAFile() {
            int n = (int) files.size();
            getFile( n, 0, true );
        }

        MongoDataFile* suitableFile( int sizeNeeded, bool preallocate ) {
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

        Extent* allocExtent( const char *ns, int size, bool capped ) { 
            Extent *e = DataFileMgr::allocFromFreeList( ns, size, capped );
            if( e ) return e;
            return suitableFile( size, !capped )->createExtent( ns, size, capped );
        }
        
        MongoDataFile* newestFile() {
            int n = (int) files.size();
            if ( n > 0 ) {
                n--;
            } else {
                return 0;   
            }
            return getFile(n);
        }
        
        /**
         * @return true if success, false otherwise
         */
        bool setProfilingLevel( int newLevel , string& errmsg );

        void finishInit();

        static bool validDBName( const string& ns );

        long long fileSize(){
            long long size=0;
            for (int n=0; exists(n); n++)
                size += boost::filesystem::file_size( fileName(n) );
            return size;
        }

        void flushFiles( bool sync );
        
        vector<MongoDataFile*> files;
        string name; // "alleyinsider"
        string path;
        NamespaceIndex namespaceIndex;
        int profile; // 0=off.
        string profileName; // "alleyinsider.system.profile"

        multimap<DiskLoc, ClientCursor*> ccByLoc;

        int magic; // used for making sure the object is still loaded in memory 
    };

} // namespace mongo
