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

    static QLock& q = *new QLock();

    //static RWLockRecursive excludeWrites("excludeWrites");
    //static mapsf<string,RWLockRecursive*> dblocks;

    // todo : report HLMutex status in db.currentOp() output
    HLMutex::HLMutex(const char *name) : SimpleMutex(name)
    { }

    // ' ', 'r', 'w', 'R', 'W'
    __declspec( thread ) char threadState;

    static void lock_W() { 
        assert( threadState == ' ' );
        threadState = 'W';
        q.lock_W();
    }
    static void unlock_W() { 
        assert( threadState == 'W' );
        threadState = ' ';
        q.unlock_W();
    }
    static void lock_R() { 
        assert( threadState == ' ' );
        threadState = 'R';
        q.lock_R();
    }
    static void unlock_R() { 
        assert( threadState == 'R' );
        threadState = ' ';
        q.unlock_R();
    }
    static void lock_w() { 
        assert( threadState == ' ' );
        threadState = 'w';
        q.lock_w();
    }
    static void unlock_w() { 
        assert( threadState == 'w' );
        threadState = ' ';
        q.unlock_w();
    }

    bool Lock::isLocked() {
        return threadState != ' ';
    }

    Lock::GlobalWrite::TempRelease::TempRelease() {
        unlock_W();
    }
    Lock::GlobalWrite::TempRelease::~TempRelease() {
        lock_W();
    }
    Lock::GlobalWrite::GlobalWrite() {
        lock_W();
    }
    Lock::GlobalWrite::~GlobalWrite() {
        unlock_W();
    }
    Lock::GlobalRead::GlobalRead() {
        lock_R();
    }
    Lock::GlobalRead::~GlobalRead() {
        unlock_R();
    }
    Lock::DBWrite::DBWrite(const StringData& ns) { 
        // TEMP : 
        lock_W();
        // todo: 
        //lock_w();
        //lock_the_db
    }
    Lock::DBWrite::~DBWrite() { 
        // TEMP : 
        unlock_W();
    }
    Lock::DBRead::DBRead(const StringData& ns) { 
        // TEMP : 
        lock_R();
    } 
    Lock::DBRead::~DBRead() { 
        unlock_R();
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
