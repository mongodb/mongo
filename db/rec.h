// rec.h

#pragma once

#include "reci.h"

namespace mongo { 

/* A RecStoreInterface for the normal mongo mem mapped file (MongoDataFile) storage
*/

class MongoMemMapped_RecStore : public RecStoreInterface { 
public:
    static char* get(DiskLoc d, unsigned len) { return d.rec()->data; }

    static DiskLoc insert(const char *ns, const void *obuf, int len, bool god) { 
        return theDataFileMgr.insert(ns, obuf, len, god);
    }
};

/* An in memory RecStoreInterface implementation
*/

class InMem_RecStore : public RecStoreInterface { 
    enum { INMEMFILE = 0x70000000 };
public:
    static char* get(DiskLoc d, unsigned len) { 
        assert( d.a() == INMEMFILE );
		//return (char *) d.getOfs();
		massert("64 bit not done", false);
		return 0;
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
};

/* Glue btree to RecStoreInterface
*/

// pick your store for indexes by setting this typedef
typedef MongoMemMapped_RecStore BtreeStore;
//typedef InMem_RecStore BtreeStore;

const int BucketSize = 8192;

inline BtreeBucket* DiskLoc::btree() const {
    assert( fileNo != -1 );
    return (BtreeBucket*) BtreeStore::get(*this, BucketSize);
}

}
