// @file d_concurrency.cpp 

#include "pch.h"
#include "d_concurrency.h"
#include "../util/concurrency/threadlocal.h"
#include "../util/concurrency/rwlock.h"
#include "../util/concurrency/mapsf.h"
#include "../util/assert_util.h"
#include "client.h"
#include "namespacestring.h"
#include "d_globals.h"

// oplog locking
// no top level read locks
// system.profile writing
// oplog now
// yielding
// commitIfNeeded

namespace mongo {
    // this tie-in temporary until MongoMutex is folded in more directly.Exclud
    // called when the lock has been achieved
    void MongoMutex::lockedExclusively() {
        Client& c = cc();
        curopGotLock(&c); // hopefully lockStatus replaces one day
        _minfo.entered(); // hopefully eliminate one day 
    }

    void MongoMutex::unlockingExclusively() {
        Client& c = cc();
        _minfo.leaving();
    }

    MongoMutex::MongoMutex(const char *name) : _m(name) {
        static int n = 0;
        assert( ++n == 1 ); // below releasingWriteLock we assume MongoMutex is a singleton, and uses dbMutex ref above
        _remapPrivateViewRequested = false;
    }

    /** Notes on d.writeExcluder
        we want to be able to block any attempted write while allowing reads; additionally 
        force non-greedy acquisition so that reads can continue -- 
        that is, disallow greediness of write lock acquisitions.  This is for that purpose.  The 
        #1 need is by groupCommitWithLimitedLocks() but useful elsewhere such as for lock and fsync.
    */
    
    ExcludeAllWrites::ExcludeAllWrites() : 
    lk(d.writeExcluder)
    {
        LOG(3) << "ExcludeAllWrites" << endl;
        wassert( !d.dbMutex.isWriteLocked() );
    };
    /*
    ExcludeAllWrites::~ExcludeAllWrites() {
    }*/

}
