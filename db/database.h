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

/* Database represents a database database
   Each database database has its own set of files -- dbname.ns, dbname.0, dbname.1, ...
*/

namespace mongo {

    class Database {
    public:
        static bool _openAllFiles;

        Database(const char *nm, bool& newDb, const string& _path = dbpath) :
        name(nm),
        path(_path),
        namespaceIndex( path, name )
        {
            {
                int L = strlen(nm);
                uassert( 10028 ,  "db name is empty", L > 0 );
                uassert( 10029 ,  "bad db name [1]", *nm != '.' );
                uassert( 10030 ,  "bad db name [2]", nm[L-1] != '.' );
                uassert( 10031 ,  "bad char(s) in db name", strchr(nm, ' ') == 0 );
                uassert( 10032 ,  "db name too long", L < 64 );
            }

            newDb = namespaceIndex.exists();
            profile = 0;
            profileName = name + ".system.profile";

            // If already exists, open.  Otherwise behave as if empty until
            // there's a write, then open.
            if ( !newDb ) {
                namespaceIndex.init();
                if( _openAllFiles )
                    openAllFiles();
            }
        }

        ~Database() {
            btreeStore->closeFiles(name, path);
            int n = files.size();
            for ( int i = 0; i < n; i++ )
                delete files[i];
        }

        bool exists(int n) { 
            stringstream ss;
            ss << name << '.' << n;
            boost::filesystem::path fullName;
            fullName = boost::filesystem::path(path) / ss.str();
            return boost::filesystem::exists(fullName);
        }

        void openAllFiles() { 
            int n = 0;
            while( exists(n) ) { 
                getFile(n);
                n++;
            }
        }

        MongoDataFile* getFile( int n, int sizeNeeded = 0, bool preallocateOnly = false ) {
            assert(this);

            namespaceIndex.init();
            if ( n < 0 || n >= DiskLoc::MaxFiles ) {
                out() << "getFile(): n=" << n << endl;
#if !defined(_RECSTORE)
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
                stringstream ss;
                ss << name << '.' << n;
                boost::filesystem::path fullName;
                fullName = boost::filesystem::path(path) / ss.str();
                string fullNameString = fullName.string();
                p = new MongoDataFile(n);
                int minSize = 0;
                if ( n != 0 && files[ n - 1 ] )
                    minSize = files[ n - 1 ]->getHeader()->fileLength;
                if ( sizeNeeded + MDFHeader::headerSize() > minSize )
                    minSize = sizeNeeded + MDFHeader::headerSize();
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

        MongoDataFile* addAFile( int sizeNeeded = 0, bool preallocateNextFile = false ) {
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

        MongoDataFile* suitableFile( int sizeNeeded ) {
            MongoDataFile* f = newestFile();
            for ( int i = 0; i < 8; i++ ) {
                if ( f->getHeader()->unusedLength >= sizeNeeded )
                    break;
                f = addAFile( sizeNeeded );
                if ( f->getHeader()->fileLength >= MongoDataFile::maxSize() ) // this is as big as they get so might as well stop
                    break;
            }
            return f;
        }

        Extent* allocExtent( const char *ns, int size, bool capped ) { 
            Extent *e = DataFileMgr::allocFromFreeList( ns, size, capped );
            if( e ) return e;
            return suitableFile( size )->createExtent( ns, size, capped );
        }
        
        MongoDataFile* newestFile() {
            int n = (int) files.size();
            if ( n > 0 ) n--;
            return getFile(n);
        }
        
        /**
         * @return true if success, false otherwise
         */
        bool setProfilingLevel( int newLevel , string& errmsg );

        vector<MongoDataFile*> files;
        string name; // "alleyinsider"
        string path;
        NamespaceIndex namespaceIndex;
        int profile; // 0=off.
        string profileName; // "alleyinsider.system.profile"

    };

} // namespace mongo
