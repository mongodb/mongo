// rec.h

/* TODO for _RECSTORE

   _ support > 2GB data per file
   _ multiple files, not just indexes.dat
   _ lazier writes? (may be done?)
   _ configurable cache size
   _ fix on abnormal terminations to be able to restart some
*/

#pragma once

#include "reci.h"
#include "reccache.h"

namespace mongo { 

/* --------------------------------------------------------------------------
   A RecStoreInterface for the normal mongo mem mapped file (MongoDataFile) 
   storage
*/

class MongoMemMapped_RecStore : public RecStoreInterface { 
public:
    static char* get(DiskLoc d, unsigned len) { return d.rec()->data; }

    static DiskLoc insert(const char *ns, const void *obuf, int len, bool god) { 
        return theDataFileMgr.insert(ns, obuf, len, god);
    }

    static void modified(DiskLoc d) { }

    static void drop(const char *ns) { 
        dropNS(ns);
    }
};

/* An in memory RecStoreInterface implementation ----------------------------
*/

class InMem_RecStore : public RecStoreInterface { 
    enum { INMEMFILE = 0x70000000 };
public:
    static char* get(DiskLoc d, unsigned len) { 
        assert( d.a() == INMEMFILE );
#ifdef __LP64__
		massert("64 bit not done", false);
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
};

/* Glue btree to RecStoreInterface: ---------------------------- */

// pick your store for indexes by setting this typedef
#if defined(_RECSTORE)
typedef Cached_RecStore BtreeStore;
#else
typedef MongoMemMapped_RecStore BtreeStore;
#endif

const int BucketSize = 8192;

inline BtreeBucket* DiskLoc::btree() const {
    assert( fileNo != -1 );
    return (BtreeBucket*) BtreeStore::get(*this, BucketSize);
}

inline BtreeBucket* DiskLoc::btreemod() const {
    assert( fileNo != -1 );
    BtreeBucket *b = (BtreeBucket*) BtreeStore::get(*this, BucketSize);
    BtreeStore::modified(*this);
    return b;
}

}
