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

/* Database represents a database database 
   Each database database has its own set of files -- dbname.ns, dbname.0, dbname.1, ...
*/

class Database { 
public:
	Database(const char *nm, bool& justCreated) : name(nm) { 
		justCreated = namespaceIndex.init(dbpath, nm);
		profile = 0;
		profileName = name + ".system.profile";
	} 
	~Database() { 
		int n = files.size();
		for( int i = 0; i < n; i++ )
			delete files[i];
	}

	PhysicalDataFile* getFile(int n) { 
		assert(this);

		if( n < 0 || n >= DiskLoc::MaxFiles ) {
			cout << "getFile(): n=" << n << endl;
			assert( n >= 0 && n < DiskLoc::MaxFiles );
		}
		DEV { 
			if( n > 100 )
				cout << "getFile(): n=" << n << "?" << endl;
		}
		while( n >= (int) files.size() )
			files.push_back(0);
		PhysicalDataFile* p = files[n];
		if( p == 0 ) {
			p = new PhysicalDataFile(n);
			files[n] = p;
			stringstream out;
			out << dbpath << name << '.' << n;
			p->open(n, out.str().c_str());
		}
		return p;
	}

	PhysicalDataFile* addAFile() {
		int n = (int) files.size();
		return getFile(n);
	}

    PhysicalDataFile* suitableFile(int sizeNeeded) { 
        PhysicalDataFile* f = newestFile();
        for( int i = 0; i < 8; i++ ) {
            if( f->getHeader()->unusedLength >= sizeNeeded ) 
                break;
            f = addAFile();
            if( f->getHeader()->fileLength > 1500000000 ) // this is as big as they get so might as well stop
                break;
        }
        return f;
    }

	PhysicalDataFile* newestFile() { 
		int n = (int) files.size();
		if( n > 0 ) n--;
		return getFile(n);
	}

    void finishInit(); // ugly...

	vector<PhysicalDataFile*> files;
	string name; // "alleyinsider"
	NamespaceIndex namespaceIndex;
	int profile; // 0=off.
	string profileName; // "alleyinsider.system.profile"
    QueryOptimizer optimizer;

    bool haveLogged() { return _haveLogged; }
    void setHaveLogged();

private:
    // see dbinfo.h description.  if true, we have logged to the replication log.
    bool _haveLogged; 
};

// tempish...move to TLS or pass all the way down as a parm
extern map<string,Database*> databases;
extern Database *database;
extern const char *curNs;
extern int dbLocked;
extern bool master;

/* returns true if the database ("database") did not exist, and it was created on this call */
inline bool setClient(const char *ns) { 
    /* we must be in critical section at this point as these are global 
       variables. 
    */
    assert( dbLocked == 1 );

	char cl[256];
	curNs = ns;
	nsToClient(ns, cl);
	map<string,Database*>::iterator it = databases.find(cl);
	if( it != databases.end() ) {
		database = it->second;
		return false;
	}

    // when master for replication, we advertise all the db's, and that 
    // looks like a 'first operation'. so that breaks this log message's 
    // meaningfulness.  instead of fixing (which would be better), we just
    // stop showing for now.
    if( !master )
        log() << "first operation for database " << cl << endl;

	bool justCreated;
	Database *c = new Database(cl, justCreated);
	databases[cl] = c;
	database = c;
    database->finishInit();
	return justCreated;
}

/* We normally keep around a curNs ptr -- if this ns is temporary, 
   use this instead so we don't have a bad ptr.  we could have made a copy,
   but trying to be fast as we call setClient this for every single operation.
*/
inline bool setClientTempNs(const char *ns) { 
	bool jc = setClient(ns); 
	curNs = "";
	return jc;
}

#include "dbinfo.h"
