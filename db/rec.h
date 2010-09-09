// rec.h
/*
 *    Copyright (C) 2010 10gen Inc.
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


/* TODO for _RECSTORE

   _ support > 2GB data per file
   _ multiple files, not just indexes.dat
   _ lazier writes? (may be done?)
   _ configurable cache size
   _ fix on abnormal terminations to be able to restart some
*/

#pragma once

#include "reci.h"
//#include "reccache.h"

namespace mongo { 

/* --------------------------------------------------------------------------
   A RecStoreInterface for the normal mongo mem mapped file (MongoDataFile) 
   storage
*/

NamespaceDetails* nsdetails_notinline(const char *ns);

class MongoMemMapped_RecStore : public RecStoreInterface { 
public:
    VIRT char* get(DiskLoc d, unsigned len) { return d.rec()->data; }

    VIRT DiskLoc insert(const char *ns, const void *obuf, int len, bool god) { 
        return theDataFileMgr.insert(ns, obuf, len, god);
    }

    VIRT void deleteRecord(const char *ns, DiskLoc d) { 
        theDataFileMgr._deleteRecord(nsdetails_notinline(ns), ns, d.rec(), d);
    }

    VIRT void modified(DiskLoc d) { }

    VIRT void drop(const char *ns) { 
        dropNS(ns);
    }

    VIRT void rename(const char *fromNs, const char *toNs) {
      renameNamespace( fromNs, toNs );
    }

    /* close datafiles associated with the db specified. */
    VIRT void closeFiles(string dbname, string path) {
        /* as this is only used for indexes so far, and we are in the same 
           PDFiles as the nonindex data, we just rely on them having been closed 
           at the same time.  one day this may need to change.
        */
    }

};

/* An in memory RecStoreInterface implementation ----------------------------
*/

#if 0
class InMem_RecStore : public RecStoreInterface { 
    enum InmemfileValue { INMEMFILE = 0x70000000 };
public:
    static char* get(DiskLoc d, unsigned len) { 
        assert( d.a() == INMEMFILE );
#ifdef __LP64__
		massert( 10372 , "64 bit not done", false);
		return 0;
#else
		return (char *) d.getOfs();
#endif
    }

    static DiskLoc insert(const char *ns, const void *obuf, int len, bool god) {
#ifdef __LP64__
      assert( 0 );
      throw -1;
#else
        char *p = (char *) malloc(len);
        assert( p );
        memcpy(p, obuf, len);
        int b = (int) p;
        assert( b > 0 );
        return DiskLoc(INMEMFILE, b);
#endif
    }

    static void modified(DiskLoc d) { }

    static void drop(const char *ns) { 
        log() << "warning: drop() not yet implemented for InMem_RecStore" << endl;
    }

    virtual void rename(const char *fromNs, const char *toNs) {
      massert( 10373 ,  "rename not yet implemented for InMem_RecStore", false );
    }
};
#endif

/* Glue btree to RecStoreInterface: ---------------------------- */

typedef MongoMemMapped_RecStore StoreToUse;

extern StoreToUse *btreeStore;

const int BucketSize = 8192;

inline BtreeBucket* DiskLoc::btree() const {
    assert( _a != -1 );
    return (BtreeBucket*) btreeStore->get(*this, BucketSize);
}

inline BtreeBucket* DiskLoc::btreemod() const {
    assert( _a != -1 );
    BtreeBucket *b = (BtreeBucket*) btreeStore->get(*this, BucketSize);
    btreeStore->modified(*this);
    return b;
}

}
