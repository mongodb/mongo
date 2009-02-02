// reci.h

#pragma once

#include "storage.h"

namespace mongo { 

class RecStoreInterface {
public:
    static char* get(DiskLoc d, unsigned len) { assert(false); return 0; }

    static void modified(DiskLoc d) { assert(false); }

    /* insert specified data as a record */
    static DiskLoc insert(const char *ns, const void *obuf, int len, bool god) { assert(false); return DiskLoc(); }

};

}
