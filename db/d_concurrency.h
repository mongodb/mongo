// @file d_concurrency.h

#pragma once

//#include "mongomutex.h"

namespace mongo {

    /* these may be used recursively as long as you do not
       try to go from sharable to exclusive
    */

    //void assertDbLocked();
    //void assertCollectionLocked();

    /*class LockDatabaseExclusively : boost::noncopyable { 
    public:
        LockDatabaseExclusively(const char *db);
    };*/

    /*class LockCollectionUpgradably : boost::noncopyable { 
    public:
        LockCollectionUpgradably(const char *ns);
    };*/

    class LockCollectionForReading : boost::noncopyable { 
        Database *db;
    public:
        LockCollectionForReading(const char *ns);
        ~LockCollectionForReading();
    };

    class LockCollectionExclusively : boost::noncopyable { 
    public:
        LockCollectionExclusively(const char *ns);
    };

}
