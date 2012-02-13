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
#include "server.h"

// oplog locking
// no top level read locks
// system.profile writing
// oplog now
// yielding
// commitIfNeeded

namespace mongo { 

    // e.g. externalobjsortmutex uses hlmutex as it can be locked for very long times
    // todo : report HLMutex status in db.currentOp() output
    HLMutex::HLMutex(const char *name) : SimpleMutex(name) { }

    /* dbname->lock
       Currently these are never deleted - will linger if db was closed. (that should be fine.)
       We don't put the lock inside the Database object as those can come and go with open and 
       closes and that would just add complexity. 
       Note there is no path concept for where the database is; if somehow you had two db's open 
       in different directories with the same name, it will be ok but they are sharing a lock 
       then.
    */
    static mapsf<string,SimpleRWLock*> dblocks;
    SimpleRWLock localDBLock("localDBLock");

    void lockedExclusively();
    void unlockingExclusively();
    static QLock& q = *new QLock();
    TSP_DEFINE(LockState,ls);

    inline LockState& lockState() { 
        LockState *p = ls.get();
        if( unlikely( p == 0 ) ) { 
            ls.reset(p = new LockState());
        }
        return *p;
    }
    void LockState::dump() {
        char s = threadState;
        stringstream ss;
        ss << "lock status: ";
        if( s == 0 ){
            ss << "unlocked"; 
        }
        else {
            ss << s;
            if( recursive ) { 
                ss << " recursive:" << recursive;
            }
            if( other ) {
                ss << " db:" << otherName;
            }
            if( local ) {
                ss << " local:" << local;
            }
        }
        log() << ss.str() << endl;
    }
    inline char& threadState() { 
        return lockState().threadState;
    }
    inline unsigned& recursive() { // the nested locking counter for the big outer QLock
        return lockState().recursive;
    }

    static bool lock_R_try(int ms) { 
        assert( threadState() == 0 );
        bool got = q.lock_R(ms);
        if( got ) 
            threadState() = 'R';
        return got;
    }
    static bool lock_W_try(int ms) { 
        assert( threadState() == 0 );
        bool got = q.lock_W(ms);
        if( got ) {
            threadState() = 'W';
            lockedExclusively();
        }
        return got;
    }
    static void lock_W_stop_greed() { 
        assert( threadState() == 0 );
        threadState() = 'W';
        q.lock_W_stop_greed();
        lockedExclusively();
    }
    static void lock_W() { 
        assert( threadState() == 0 );
        threadState() = 'W';
        q.lock_W();
        lockedExclusively();
    }
    static void unlock_W() { 
        wassert( threadState() == 'W' );
        unlockingExclusively();
        threadState() = 0;
        q.unlock_W();
    }
    static void lock_R() { 
        assert( threadState() == 0 );
        threadState() = 'R';
        q.lock_R();
    }
    static void unlock_R() { 
        wassert( threadState() == 'R' );
        threadState() = 0;
        q.unlock_R();
    }
    static void lock_w() { 
        assert( threadState() == 0 );
        threadState() = 'w';
        q.lock_w();
    }
    static void unlock_w() { 
        wassert( threadState() == 'w' );
        threadState() = 0;
        q.unlock_w();
    }
    static void lock_r(char& thrState) {
        assert(thrState == 0);
        thrState = 'r';
        q.lock_r();
    }
    static void unlock_r() { 
        wassert( threadState() == 'r' );
        threadState() = 0;
        q.unlock_r();
    }

    // these are safe for use ACROSS threads.  i.e. one thread can lock and 
    // another unlock
    void Lock::ThreadSpanningOp::setWLockedNongreedy() { 
        assert( threadState() == 0 ); // as this spans threads the tls wouldn't make sense
        q.lock_W_stop_greed();
    }
    void Lock::ThreadSpanningOp::W_to_R() { 
        assert( threadState() == 0 );
        q.W_to_R();
    }
    void Lock::ThreadSpanningOp::unsetW() {
        assert( threadState() == 0 );
        q.unlock_W();
        q.start_greed();
    }
    void Lock::ThreadSpanningOp::unsetR() {
        assert( threadState() == 0 );
        q.unlock_R();
        q.start_greed();
    }

    int Lock::isLocked() {
        return threadState();
    }
    int Lock::isReadLocked() {
        return threadState() & 'R'; // ascii assumed
    }
    int Lock::somethingWriteLocked() { // w or W
        return threadState() & 'W'; // ascii assumed
    }
    bool Lock::isRW() {
        return threadState() == 'W' || threadState() == 'R';
    }
    bool Lock::isW() { 
        return threadState() == 'W';
    }
    bool Lock::isR() { 
        return threadState() == 'R';
    }
    bool Lock::nested() { 
        return recursive() != 0;
    }
    static bool weLocked(const LockState &ls, const StringData& ns) { 
        string db = nsToDatabase(ns.data());
        if( db == "local" ) {
            return ls.local;
        }
        return db == ls.otherName && ls.other;
    }
    bool Lock::isWriteLocked(const StringData& ns) { 
        LockState &ls = lockState();
        if( ls.threadState == 'W' ) 
            return true;
        if( ls.threadState != 'w' ) 
            return false;
        return weLocked(ls,ns);
    }
    bool Lock::atLeastReadLocked(const StringData& ns)
    { 
        LockState &ls = lockState();
        if( ls.threadState == 'R' || ls.threadState == 'W' ) 
            return true; // global
        if( ls.threadState == 0 ) 
            return false;
        return weLocked(ls,ns);
    }
    void Lock::assertAtLeastReadLocked(const StringData& ns) { 
        if( !atLeastReadLocked(ns) ) { 
            LockState &ls = lockState();
            log() << ns << endl;
            msgasserted(0, str::stream() << "expected to be read locked for " << ns);
        }
    }
    void Lock::assertWriteLocked(const StringData& ns) { 
        if( !Lock::isWriteLocked(ns) ) { 
            lockState().dump();
            msgasserted(0, str::stream() << "expected to be write locked for " << ns);
        }
    }

    Lock::TempRelease::TempRelease() : 
        cant( recursive() ), type(threadState())
    {
        if( cant )
            return;
        if( threadState() == 'W' ) {
            unlock_W();
        }
        else if( threadState() == 'R' ) { 
            unlock_R();
        }
        else {
            error() << "TempRelease called but threadState()=" << threadState() << endl;
            assert(false);
        }
    }
    Lock::TempRelease::~TempRelease() {
        if( cant )
            return;
        DESTRUCTOR_GUARD( 
            fassert(0, threadState() == 0);
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
    Lock::GlobalWrite::GlobalWrite(bool sg) : stopGreed(sg) {
        if( threadState() == 'W' ) { 
            recursive()++;
            DEV if( stopGreed ) { 
                log() << "info Lock::GlobalWrite does not stop greed on recursive invocation" << endl;
            }
        }
        else {
            if( stopGreed )
                lock_W_stop_greed();
            else
                lock_W();
        }
    }
    Lock::GlobalWrite::~GlobalWrite() {
        if( recursive() ) { 
            recursive()--;
        }
        else {
            if( threadState() == 'R' ) // downgraded
                unlock_R();
            else
                unlock_W();
            if( stopGreed )
                q.start_greed();
        }
    }
    void Lock::GlobalWrite::downgrade() { 
        assert( !recursive() );
        assert( threadState() == 'W' );
        q.W_to_R();
        threadState() = 'R';
    }
    // you will deadlock if 2 threads doing this
    void Lock::GlobalWrite::upgrade() { 
        assert( !recursive() );
        assert( threadState() == 'R' );
        q.R_to_W();
        threadState() = 'W';
    }

    Lock::GlobalRead::GlobalRead() {
        if( threadState() == 'R' || threadState() == 'W' ) { 
            recursive()++;
        }
        else {
            lock_R();
        }
    }
    Lock::GlobalRead::~GlobalRead() {
        if( recursive() ) { 
            recursive()--;
        }
        else {
            unlock_R();
        }
    }

    void Lock::DBWrite::lockTop(LockState& ls) { 
        switch( ls.threadState ) { 
        case 'R' : 
            assert(false);
        case 'r' : 
            assert(false);
        case 'w' :
        case 'W' :
            ls.recursive++;
            break;
        case  0  : 
            lock_w();
            locked_w = true;
        }
    }
    void Lock::DBWrite::lockLocal() { 
        LockState& ls = lockState();
        lockTop(ls);
        if( ls.local ) { 
            // we are nested in our locking of local.
            // we could increment ls.local here; that would be recommended if 
            // temprelease is supported here - but it is not yet, so we don't need to.
            assert( ls.local > 0 );
        }
        else {
            ourCounter = &ls.local;
            ls.local++;
            localDBLock.lock();
            weLocked = &localDBLock;
        }
    }
    void Lock::DBWrite::lock(const string& db) {
        LockState& ls = lockState();

        // we do checks first, as on assert destructor won't be called so don't want to be half finished with our work.
        bool same = (db == ls.otherName);
        if( ls.other ) { 
            // nested. if/when we do temprelease with DBWrite we will need to increment here
            // (so we can not release or assert if nested).
            massert(0, str::stream() << "internal error tried to lock two databases at the same time. old:" << ls.otherName << " new:" << db,same);

            // we do the top lock though so its temprelease semantics are preserved:
            lockTop(ls);
            return;
        }

        // first lock for this db. check consistent order with local db lock so we never deadlock. local always comes last
        massert(0, str::stream() << "can't dblock:" << db << " when local is already locked", ls.local == 0);

        lockTop(ls);

        ourCounter = &ls.other;
        dassert( ls.other == 0 );
        ls.other++;
        if( !same ) {
            ls.otherName = db;
            mapsf<string,SimpleRWLock*>::ref r(dblocks);
            SimpleRWLock*& lock = r[db];
            if( lock == 0 )
                lock = new SimpleRWLock();
            ls.otherLock = lock;
        }
        ls.otherLock->lock();
        weLocked = ls.otherLock;
    }
    Lock::DBWrite::DBWrite(const StringData& ns) {
        locked_w=false; weLocked=0; ourCounter = 0;
        if( str::equals(ns.data(),"local") ) {
            lockLocal();
        } else { 
            lock(nsToDatabase(ns.data()));
        }
    }
    Lock::DBWrite::~DBWrite() {
        if( ourCounter ) {
            (*ourCounter)--;
            wassert( *ourCounter >= 0 );
        }
        if( weLocked ) {
            wassert( ourCounter && *ourCounter == 0 );
            weLocked->unlock();
        }
        if( locked_w ) {
            unlock_w();
        }
        else { 
            recursive()--;
            dassert( recursive() >= 0 );
        }
    }

    void Lock::DBRead::lockTop(LockState& ls) { 
        switch( ls.threadState ) { 
        case 'W' :
        case 'R' : 
        case 'r' :
        case 'w' :
            ls.recursive++;
            break;
        case  0  : 
            lock_r(ls.threadState);
            locked_r = true;
        }
    }
    void Lock::DBRead::lockLocal() { 
        LockState& ls = lockState();
        lockTop(ls);
        if( ls.local ) { 
            // we are nested in our locking of local.  previous lock could be read or write.
            // we could increment/decrement ls.local here; that would be recommended if 
            // temprelease is supported here - but it is not yet, so we don't need to.
        }
        else {
            ourCounter = &ls.local;
            ls.local--;
            localDBLock.lock_shared();
            weLocked = &localDBLock;
        }
    }
    void Lock::DBRead::lock(const string& db) {
        LockState& ls = lockState();

        // we do checks first, as on assert destructor won't be called so don't want to be half finished with our work.
        bool same = (db == ls.otherName);
        if( ls.other ) { 
            // nested. prev could be read or write. if/when we do temprelease with DBRead/DBWrite we will need to increment/decrement here
            // (so we can not release or assert if nested).  temprelease we should avoid if we can though, it's a bit of an anti-pattern.
            massert(0, str::stream() << "internal error tried to lock two databases at the same time. old:" << ls.otherName << " new:" << db,same);

            // we do the top lock though so its temprelease semantics are preserved:
            lockTop(ls);
            return;
        }

        // first lock for this db. check consistent order with local db lock so we never deadlock. local always comes last
        massert(0, str::stream() << "can't dblock:" << db << " when local is already locked", ls.local == 0);

        lockTop(ls);

        ourCounter = &ls.other;
        dassert( ls.other == 0 );
        ls.other--;
        if( !same ) {
            ls.otherName = db;
            mapsf<string,SimpleRWLock*>::ref r(dblocks);
            SimpleRWLock*& lock = r[db];
            if( lock == 0 )
                lock = new SimpleRWLock();
            ls.otherLock = lock;
        }
        ls.otherLock->lock_shared();
        weLocked = ls.otherLock;
    }
    Lock::DBRead::DBRead(const StringData& ns) {
        locked_r=false; weLocked=0; ourCounter = 0;
        if( str::equals(ns.data(),"local") ) {
            lockLocal();
        } else { 
            lock(nsToDatabase(ns.data()));
        }
    }
    Lock::DBRead::~DBRead() {
        if( ourCounter ) {
            (*ourCounter)++;
            wassert( *ourCounter <= 0 );
        }
        if( weLocked ) {
            wassert( ourCounter && *ourCounter == 0 );
            weLocked->unlock_shared();
        }
        if( locked_r ) {
            unlock_r();
        } else { 
            recursive()--;
            dassert( recursive() >= 0 );
        }
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

    // note: the 'already' concept here might be a bad idea as a temprelease wouldn't notice it is nested then
    readlocktry::readlocktry( int tryms )      
    {
        if( threadState() == 'R' || threadState() == 'W' ) {
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
    void MongoMutex::assertWriteLocked() const { 
        if( !isWriteLocked() ) { 
            lockState().dump();
            msgasserted(0, "expected write lock");
        }
    }

    void curopGotLock(Client*);
    void lockedExclusively() {
        Client& c = cc();
        curopGotLock(&c);
        d.dbMutex._minfo.entered(); // hopefully eliminate one day 
    }

    namespace dur {
        void releasingWriteLock(); // because it's hard to include dur.h here
    }

    void unlockingExclusively() {
        dur::releasingWriteLock();
        d.dbMutex._minfo.leaving();
    }

    MongoMutex::MongoMutex() {
        static int n = 0;
        assert( ++n == 1 ); // below releasingWriteLock we assume MongoMutex is a singleton, and uses dbMutex ref above
    }

}
