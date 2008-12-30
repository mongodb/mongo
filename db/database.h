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

class Database {
public:
    Database(const char *nm, bool& justCreated, const char *_path = dbpath) :
            name(nm),
            path(_path) 
    {
        {
            int L = strlen(nm);
            uassert( "db name is empty", L > 0 );
            uassert( "bad db name [1]", *nm != '.' );
            uassert( "bad db name [2]", nm[L-1] != '.' );
            uassert( "bad char(s) in db name", strchr(nm, ' ') == 0 );
            uassert( "db name too long", L < 64 );
        }

        justCreated = namespaceIndex.init(_path, nm);
        profile = 0;
        profileName = name + ".system.profile";
    }
    ~Database() {
        int n = files.size();
        for ( int i = 0; i < n; i++ )
            delete files[i];
    }

    PhysicalDataFile* getFile(int n) {
        assert(this);

        if ( n < 0 || n >= DiskLoc::MaxFiles ) {
            cout << "getFile(): n=" << n << endl;
            assert( n >= 0 && n < DiskLoc::MaxFiles );
        }
        DEV {
            if ( n > 100 )
                cout << "getFile(): n=" << n << "?" << endl;
        }
        while ( n >= (int) files.size() )
            files.push_back(0);
        PhysicalDataFile* p = files[n];
        if ( p == 0 ) {
            stringstream ss;
            ss << name << '.' << n;
            boost::filesystem::path fullName;
            fullName = boost::filesystem::path(path) / ss.str();
            string fullNameString = fullName.string();
            p = new PhysicalDataFile(n);
            try {
                p->open(n, fullNameString.c_str() );
            }
            catch ( AssertionException& u ) {
                delete p;
                throw u;
            }
            files[n] = p;
        }
        return p;
    }

    PhysicalDataFile* addAFile() {
        int n = (int) files.size();
        return getFile(n);
    }

    PhysicalDataFile* suitableFile(int sizeNeeded) {
        PhysicalDataFile* f = newestFile();
        for ( int i = 0; i < 8; i++ ) {
            if ( f->getHeader()->unusedLength >= sizeNeeded )
                break;
            f = addAFile();
            if ( f->getHeader()->fileLength > 1500000000 ) // this is as big as they get so might as well stop
                break;
        }
        return f;
    }

    PhysicalDataFile* newestFile() {
        int n = (int) files.size();
        if ( n > 0 ) n--;
        return getFile(n);
    }

    void finishInit(); // ugly...

    vector<PhysicalDataFile*> files;
    string name; // "alleyinsider"
    string path;
    NamespaceIndex namespaceIndex;
    int profile; // 0=off.
    string profileName; // "alleyinsider.system.profile"
    QueryOptimizer optimizer;

    bool haveLogged() {
        return _haveLogged;
    }
    void setHaveLogged();

private:
    // see dbinfo.h description.  if true, we have logged to the replication log.
    bool _haveLogged;
};

