// @file d_concurrency.h

#pragma once

//#include "mongomutex.h"

namespace mongo {

    /* these may be used recursively as long as you do not
       try to go from sharable to exclusive
    */

    class LockDatabaseSharable : boost::noncopyable { 
        bool already;
    public:
        LockDatabaseSharable();
        ~LockDatabaseSharable();
    };

    class LockCollectionForReading : boost::noncopyable {
        atleastreadlock globalrl;
        LockDatabaseSharable dbrl;
        bool already;
    public:
        LockCollectionForReading(const char *ns);
        ~LockCollectionForReading();
    };

}
