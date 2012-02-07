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

    Lock::Nongreedy::Nongreedy() {
        q.stop_greed();
    }
    Lock::Nongreedy::~Nongreedy() {
        q.start_greed();
    }

    //static RWLockRecursive excludeWrites("excludeWrites");
    //static mapsf<string,RWLockRecursive*> dblocks;

    // todo : report HLMutex status in db.currentOp() output
    HLMutex::HLMutex(const char *name) : SimpleMutex(name)
    { }

    // 0, 'r', 'w', 'R', 'W'
    __declspec( thread ) char threadState;
    __declspec( thread ) unsigned recursive;
    
    static bool lock_R_try(int ms) { 
        assert( threadState == 0 );
        bool got = q.lock_R(ms);
        if( got ) 
            threadState = 'R';
        return got;
    }
    static bool lock_W_try(int ms) { 
        assert( threadState == 0 );
        bool got = q.lock_W(ms);
        if( got ) 
            threadState = 'W';
        return got;
    }
    static void lock_W() { 
        assert( threadState == 0 );
        threadState = 'W';
        q.lock_W();
    }
    static void unlock_W() { 
        wassert( threadState == 'W' );
        threadState = 0;
        q.unlock_W();
    }
    static void lock_R() { 
        assert( threadState == 0 );
        threadState = 'R';
        q.lock_R();
    }
    static void unlock_R() { 
        wassert( threadState == 'R' );
        threadState = 0;
        q.unlock_R();
    }
    static void lock_w() { 
        assert( threadState == 0 );
        threadState = 'w';
        q.lock_w();
    }
    static void unlock_w() { 
        wassert( threadState == 'w' );
        threadState = 0;
        q.unlock_w();
    }

    // these are safe for use ACROSS threads.  i.e. one thread can lock and 
    // another unlock
    void Lock::ThreadSpan::setWLockedNongreedy() { 
        assert( threadState == 0 ); // as this spans threads the tls wouldn't make sense
        q.lock_W();
        q.stop_greed();
    }
    void Lock::ThreadSpan::W_to_R() { 
        assert( threadState == 0 );
        q.W_to_R();
    }
    void Lock::ThreadSpan::unsetW() {
        assert( threadState == 0 );
        q.unlock_W();
        q.start_greed();
    }
    void Lock::ThreadSpan::unsetR() {
        assert( threadState == 0 );
        q.unlock_R();
        q.start_greed();
    }

    int Lock::isLocked() {
        return threadState;
    }
    int Lock::isWriteLocked() { // w or W
        return threadState & 'W'; // ascii assumed
    }
    bool Lock::isW() { 
        return threadState == 'W';
    }
    bool Lock::nested() { 
        return recursive != 0;
    }

    Lock::TempRelease::TempRelease() : 
        cant( recursive ), type(threadState)
    {
        if( cant )
            return;
        if( threadState == 'W' ) {
            unlock_W();
        }
        else if( threadState == 'R' ) { 
            unlock_R();
        }
        else {
            error() << "TempRelease called but threadState=" << threadState << endl;
            assert(false);
        }
    }
    Lock::TempRelease::~TempRelease() {
        if( cant )
            return;
        DESTRUCTOR_GUARD( 
            fassert(0, threadState == 0);
            if( type == 'W' ) {
                lock_W();
            }
            else if( type == 'R' ) { 
                lock_R();
            }
            else {
                wassert(false);
            }
        )
    }
    Lock::GlobalWrite::GlobalWrite() {
        if( threadState == 'W' ) { 
            recursive++;
        }
        else {
            lock_W();
        }
    }
    Lock::GlobalWrite::~GlobalWrite() {
        if( recursive ) { 
            recursive--;
        }
        else {
            if( threadState == 'R' ) // downgraded
                unlock_R();
            else
                unlock_W();
        }
    }
    void Lock::GlobalWrite::downgrade() { 
        assert( !recursive );
        assert( threadState == 'W' );
        q.W_to_R();
        threadState = 'R';
    }
    // you will deadlock if 2 threads doing this
    void Lock::GlobalWrite::upgrade() { 
        assert( !recursive );
        assert( threadState == 'R' );
        q.R_to_W();
        threadState = 'W';
    }

    Lock::GlobalRead::GlobalRead() {
        if( threadState == 'R' || threadState == 'W' ) { 
            recursive++;
        }
        else {
            lock_R();
        }
    }
    Lock::GlobalRead::~GlobalRead() {
        if( recursive ) { 
            recursive--;
        }
        else {
            unlock_R();
        }
    }
    Lock::DBWrite::DBWrite(const StringData& ns) { 
    }
    Lock::DBWrite::~DBWrite() { 
    }
    Lock::DBRead::DBRead(const StringData& ns) { 
    } 
    Lock::DBRead::~DBRead() { 
    }

}

// legacy hooks
namespace mongo { 

    writelock::writelock() { 
        lk1.reset( new Lock::GlobalWrite() );
    }
    writelock::writelock(const string& ns) { 
        if( ns.empty() ) { 
            lk1.reset( new Lock::GlobalWrite() );
        }
        else {
            lk2.reset( new Lock::DBWrite(ns) );
        }
    }

    writelocktry::writelocktry( int tryms ) : 
        _got( lock_W_try(tryms) )
    { }
    writelocktry::~writelocktry() { 
        if( _got )
            unlock_W();
    }

    readlocktry::readlocktry( int tryms )      
    {
        if( threadState == 'R' ) {
            _already = true;
            _got = true;
        }
        else {
            _already = false;
            _got = lock_R_try(tryms);
        }
    }
    readlocktry::~readlocktry() { 
        if( !_already && _got )
            unlock_R();
    }

    readlock::readlock() {
        lk1.reset( new Lock::GlobalRead() );
    }
    readlock::readlock(const string& ns) {
        if( ns.empty() ) { 
            lk1.reset( new Lock::GlobalRead() );
        }
        else {
            lk2.reset( new Lock::DBRead(ns) );
        }
    }

}

// legacy MongoMutex glue
namespace mongo {

    /* backward compatible glue. it could be that the assumption was that 
       it's a global read lock, so 'r' and 'w' don't qualify.
       */ 
    bool MongoMutex::atLeastReadLocked() { 
        int x = Lock::isLocked();
        return x == 'R' || x == 'W';
    }
    bool MongoMutex::isWriteLocked() { 
        return Lock::isW();
    }

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
    }

}
