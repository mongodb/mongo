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
        Database(const char *nm, bool& newDb, const char *_path = dbpath) :
        name(nm),
        path(_path),
        namespaceIndex( path, name )
        {
            {
                int L = strlen(nm);
                uassert( "db name is empty", L > 0 );
                uassert( "bad db name [1]", *nm != '.' );
                uassert( "bad db name [2]", nm[L-1] != '.' );
                uassert( "bad char(s) in db name", strchr(nm, ' ') == 0 );
                uassert( "db name too long", L < 64 );
            }

            newDb = namespaceIndex.exists();
            // If already exists, open.  Otherwise behave as if empty until
            // there's a write, then open.
            if ( !newDb )
                namespaceIndex.init();            
            profile = 0;
            profileName = name + ".system.profile";
        }
        ~Database() {
            int n = files.size();
            for ( int i = 0; i < n; i++ )
                delete files[i];
        }

        MongoDataFile* getFile( int n, int sizeNeeded = 0 ) {
            assert(this);

            namespaceIndex.init();
            if ( n < 0 || n >= DiskLoc::MaxFiles ) {
                out() << "getFile(): n=" << n << endl;
                assert( n >= 0 && n < DiskLoc::MaxFiles );
            }
            DEV {
                if ( n > 100 )
                    out() << "getFile(): n=" << n << "?" << endl;
            }
            while ( n >= (int) files.size() )
                files.push_back(0);
            MongoDataFile* p = files[n];
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
                    p->open( fullNameString.c_str(), minSize );
                }
                catch ( AssertionException& ) {
                    delete p;
                    throw;
                }
                files[n] = p;
            }
            return p;
        }

        MongoDataFile* addAFile( int sizeNeeded = 0 ) {
            int n = (int) files.size();
            return getFile( n, sizeNeeded );
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

        MongoDataFile* newestFile() {
            int n = (int) files.size();
            if ( n > 0 ) n--;
            return getFile(n);
        }

        void finishInit(); // ugly...

        vector<MongoDataFile*> files;
        string name; // "alleyinsider"
        string path;
        NamespaceIndex namespaceIndex;
        int profile; // 0=off.
        string profileName; // "alleyinsider.system.profile"

    };

    extern Database *database;

} // namespace mongo
