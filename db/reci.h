// reci.h

#pragma once

#include "storage.h"

namespace mongo { 

/* Subclass this and implement your real storage interface.
*/
class RecStoreInterface {
public:
    virtual ~RecStoreInterface() {}
    /* Get a pointer to the data at diskloc d.  Pointer guaranteed to stay in
       scope through the current database operation's life.
    */
    virtual char* get(DiskLoc d, unsigned len) = 0;

    /* indicate that the diskloc specified has been updated. note that as-is today,tl he modification may come AFTER this 
       call -- we handle that currently -- until the dblock finishes.
    */
    virtual void modified(DiskLoc d) = 0;

    /* insert specified data as a record */
    virtual DiskLoc insert(const char *ns, const void *obuf, int len, bool god) = 0;

    virtual void deleteRecord(const char *ns, DiskLoc d) { massert("not implemented RecStoreInterface::deleteRecord", false); }

    /* drop the collection */
    virtual void drop(const char *ns) = 0;

    /* rename collection */
    virtual void rename(const char *fromNs, const char *toNs) = 0;

    /* close datafiles associated with the db specified. */
    virtual void closeFiles(string dbname, string path) = 0;

    /* todo add: 
       closeFiles(dbname)
       eraseFiles(dbname)
    */
};

}
