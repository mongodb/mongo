// rec.h

#pragma once

#include "reci.h"

namespace mongo { 

class MongoMemMapped_RecStore : public RecStoreInterface { 
public:
    static char* get(DiskLoc d, unsigned len) { return d.rec()->data; }

    static DiskLoc insert(const char *ns, const void *obuf, int len, bool god) { 
        return theDataFileMgr.insert(ns, obuf, len, god);
    }
};

typedef MongoMemMapped_RecStore BtreeStore;

const int BucketSize = 8192;

inline BtreeBucket* DiskLoc::btree() const {
    assert( fileNo != -1 );
    return (BtreeBucket*) BtreeStore::get(*this, BucketSize);
}

}
