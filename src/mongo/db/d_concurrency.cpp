// @file d_concurrency.cpp 

#include "pch.h"
#include "../util/concurrency/qlock.h"
#include "d_concurrency.h"
#include "../util/concurrency/threadlocal.h"
#include "../util/concurrency/rwlock.h"
#include "../util/concurrency/mapsf.h"
#include "../util/assert_util.h"
#include "client.h"
#include "namespacestring.h"
#include "d_globals.h"
#include "mongomutex.h"

// oplog locking
// no top level read locks
// system.profile writing
// oplog now
// yielding
// commitIfNeeded

namespace mongo { 

    //static RWLockRecursive excludeWrites("excludeWrites");
    //static mapsf<string,RWLockRecursive*> dblocks;

    HLMutex::HLMutex(const char *name) : SimpleMutex(name)
    { }

    bool Lock::isLocked() {
        return d.dbMutex.atLeastReadLocked();
    }

    Lock::Global::TempRelease::TempRelease() {
    }

    Lock::Global::TempRelease::~TempRelease() {
    }

    Lock::Global::Global() {
        d.dbMutex.lock();
    }
    Lock::Global::~Global() {
        DESTRUCTOR_GUARD(
          d.dbMutex.unlock();
        )
    }
    Lock::DBWrite::DBWrite(const StringData& ns) { 
        d.dbMutex.lock();
    }
    Lock::DBWrite::~DBWrite() { 
        DESTRUCTOR_GUARD(
          d.dbMutex.unlock();
        )
    }
    Lock::DBRead::DBRead(const StringData& ns) { 
        d.dbMutex.lock_shared();
    } 
    Lock::DBRead::~DBRead() { 
        DESTRUCTOR_GUARD(
          d.dbMutex.unlock_shared();
        )
    }

}

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

}
