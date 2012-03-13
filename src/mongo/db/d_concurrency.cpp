// @file d_concurrency.cpp 

#include "pch.h"
#include "d_concurrency.h"
#include "../util/concurrency/qlock.h"
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

    Client* curopWaitingForLock( char type );
    void curopGotLock(Client*);
    struct Acquiring { 
        Client* c;
        ~Acquiring() { curopGotLock(c); }
        Acquiring(char type) { 
            c = curopWaitingForLock(type);
        }
    };

    namespace dur { 
        void assertNothingSpooled();
        void releasingWriteLock();
    }

    // e.g. externalobjsortmutex uses hlmutex as it can be locked for very long times
    // todo : report HLMutex status in db.currentOp() output
    // perhaps move this elsewhere as this could be used in mongos and this file is for mongod
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

    static void locked_W();
    static void unlocking_w();
    static void unlocking_W();
    static QLock& q = *new QLock();
    TSP_DEFINE(LockState,ls);

    void runExclusively(void (*f)(void)) { 
        q.runExclusively(f);
    }

    /** commitIfNeeded(), we have to do work when no one else is writing, and do it at a 
        point where there is data consistency.  yet we have multiple writers so what to do.
        this is the solution chosen.  we wait until all writers either finish (quick ones) 
        or also call commitIfNeeded (long ones) -- a little like a synchronization barrier.
        a more elegant solution likely is best long term.
    */
    void QLock::runExclusively(void (*f)(void)) { 
        dlog(1) << "QLock::runExclusively" << endl;
        boost::mutex::scoped_lock lk( q.m );
        assert( w.n > 0 );
        greed++; // stop new acquisitions
        X.n++;
        while( X.n ) { 
            if( X.n == w.n ) {
                // we're all here
                f();
                X.n = 0; // sentinel, tell everyone we're done
                X.c.notify_all();
            }
            else { 
                X.c.wait(lk);
            }
        }
        greed--;
        dlog(1) << "run exclusively end" << endl;
    }

    inline LockState& lockState() { 
        LockState *p = ls.get();
        if( unlikely( p == 0 ) ) { 
            ls.reset(p = new LockState());
        }
        return *p;
    }
    void LockState::Dump() {
        lockState().dump();
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
        bool got = q.lock_R_try(ms);
        if( got ) 
            threadState() = 'R';
        return got;
    }
    static bool lock_W_try(int ms) { 
        assert( threadState() == 0 );
        bool got = q.lock_W_try(ms);
        if( got ) {
            threadState() = 'W';
            locked_W();
        }
        return got;
    }
    static void lock_W_stop_greed() { 
        assert( threadState() == 0 );
        threadState() = 'W';
        {
            Acquiring a('W');
            q.lock_W_stop_greed();
        }
        locked_W();
    }
    static void lock_W() { 
        LockState& ls = lockState();
        if( ls.threadState == 't' ) { 
            DEV warning() << "W locking inside a temprelease, seems nonideal" << endl;
        }
        else if(  ls.threadState ) {
            log() << "can't lock_W, threadState=" << (int) ls.threadState << endl;
            fassert(0,false);
        }
        getDur().commitIfNeeded(); // check before locking - will use an R lock for the commit if need to do one, which is better than W
        threadState() = 'W';
        {
            Acquiring a('W');
            q.lock_W();
        }
        locked_W();
    }
    static void unlock_W(char oldState = 0) { 
        wassert( threadState() == 'W' );
        unlocking_W();
        dassert( oldState == 0 ||  oldState == 't' );
        threadState() = oldState;
        q.unlock_W();
    }
    static void lock_R() { 
        LockState& ls = lockState();
        massert(16094, str::stream() << "can't lock_R, threadState=" << (int) ls.threadState, ls.threadState == 0);
        ls.threadState = 'R';
        Acquiring a('R');
        q.lock_R();
    }
    static void unlock_R() { 
        wassert( threadState() == 'R' );
        threadState() = 0;
        q.unlock_R();
    }    
    static void lock_w() { 
        char &ts = threadState();
        assert( ts == 0 || ts == 't' );
        getDur().commitIfNeeded();
        ts = 'w';
        Acquiring a('w');
        q.lock_w();
    }
    static void unlock_w() { 
        unlocking_w();
        wassert( threadState() == 'w' );
        threadState() = 0;
        q.unlock_w();
    }
    static void lock_r() {
        char& ts = threadState();
        assert( ts == 0 || ts == 't' );
        ts = 'r';
        Acquiring a('r');
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
        dur::assertNothingSpooled();
        q.W_to_R();
    }
    void Lock::ThreadSpanningOp::unsetW() { // note there is no unlocking_W() call here
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
        return threadState() == 'R' || threadState() == 'r';
    }
    int Lock::somethingWriteLocked() {
        return threadState() == 'W' || threadState() == 'w';
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
        char db[MaxDatabaseNameLen];
        nsToDatabase(ns.data(), db);
        if( str::equals(db,"local") ) {
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
            log() << "error expected " << ns << " to be locked " << endl;
            ls.dump();
            msgasserted(16095, str::stream() << "expected to be read locked for " << ns);
        }
    }
    void Lock::assertWriteLocked(const StringData& ns) { 
        if( !Lock::isWriteLocked(ns) ) { 
            lockState().dump();
            msgasserted(16096, str::stream() << "expected to be write locked for " << ns);
        }
    }

    Lock::TempRelease::TempRelease() : 
        cant( recursive() ), type(threadState())
    {
        LockState& ls = lockState();
        cant = ls.recursive;
        type = ls.threadState;
        local = ls.local;

        if( cant )
            return;

        switch(type) {
        case 'W':
            unlock_W();
            break;
        case 'R':
            unlock_R();
            break;
        case 'w':
            if( local ) {
                assert(local==1);
                assert(ls.other==0);
                localDBLock.unlock();
            }
            else {
                assert(local==0);
                assert(ls.other==1);
                ls.otherLock->unlock();
            }
            unlock_w();
            break;
        case 'r':
            if( local ) {
                assert(local==-1);
                assert(ls.other==0);
                localDBLock.unlock_shared();
            }
            else {
                assert(local==0);
                assert(ls.other==-1);
                ls.otherLock->unlock_shared();
            }
            unlock_r();
            break;
        default:
            error() << "TempRelease called but threadState()=" << type << endl;
            assert(false);
        }
        ls.threadState = 't';
        dassert( ls.recursive == 0 );
    }
    Lock::TempRelease::~TempRelease() {
        if( cant )
            return;

        LockState& ls = lockState();
        dassert( ls.recursive == 0 );
        DESTRUCTOR_GUARD( 
            fassert(0, ls.threadState == 't' || ls.threadState == 0);
            ls.threadState = 0;
            switch( type ) {
            case 'W':
                lock_W();
                break;
            case 'R':        
                lock_R();
                break;
            case 'w':
                lock_w();
                if( local ) 
                    localDBLock.lock();
                else
                    ls.otherLock->lock();
                break;
            case 'r':
                lock_r();
                if( local ) 
                    localDBLock.lock_shared();
                else
                    ls.otherLock->lock_shared();
                break;
            default:
                wassert(false);
            }
        )
    }
    Lock::GlobalWrite::GlobalWrite(bool sg) : stopGreed(sg), old(threadState()) {
        if( old == 'W' ) { 
            recursive()++;
            DEV if( stopGreed ) { 
                log() << "info Lock::GlobalWrite does not stop greed on recursive invocation" << endl;
            }
        }
        else {
            if( stopGreed ) {
                lock_W_stop_greed();
            } else {
                lock_W();
            }
        }
    }
    Lock::GlobalWrite::~GlobalWrite() {
        if( recursive() ) { 
            recursive()--;
        }
        else {
            if( threadState() == 'R' ) { // downgraded
                unlock_R();
            }
            else {
                unlock_W(old);
            }
            if( stopGreed ) {
                q.start_greed();
            }
        }
        fassert( 0, recursive() < 1000 );
    }
    void Lock::GlobalWrite::downgrade() { 
        assert( !recursive() );
        assert( threadState() == 'W' );
        q.W_to_R();
        threadState() = 'R';
    }
    // you will deadlock if 2 threads doing this
    bool Lock::GlobalWrite::upgrade() { 
        assert( !recursive() );
        assert( threadState() == 'R' );
        if( q.R_to_W() ) {
            threadState() = 'W';
            return true;
        }
        return false;
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
        fassert( 0, recursive() < 1000 );
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
        default: // 't'
            assert(false);
        case 't':
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
            massert(16097, str::stream() << "internal error tried to lock two databases at the same time. old:" << ls.otherName << " new:" << db,same);

            // we do the top lock though so its temprelease semantics are preserved:
            lockTop(ls);
            return;
        }

        // first lock for this db. check consistent order with local db lock so we never deadlock. local always comes last
        massert(16098, str::stream() << "can't dblock:" << db << " when local is already locked", ls.local == 0);

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
        char db[MaxDatabaseNameLen];
        nsToDatabase(ns.data(), db);
        if( str::equals(db,"local") ) {
            lockLocal();
        } else { 
            lock(db);
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
        default:
            assert(false);
        case 't' :
            {
                LOG(2) << "info relocking inside a temprelease" << endl;
            }
        case  0  : 
            lock_r();
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
            massert(16099, str::stream() << "internal error tried to lock two databases at the same time. old:" << ls.otherName << " new:" << db,same);

            // we do the top lock though so its temprelease semantics are preserved:
            lockTop(ls);
            return;
        }

        // first lock for this db. check consistent order with local db lock so we never deadlock. local always comes last
        massert(16100, str::stream() << "can't dblock:" << db << " when local is already locked", ls.local == 0);

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
        char db[MaxDatabaseNameLen];
        nsToDatabase(ns.data(), db);
        if( str::equals(db,"local") ) {
            lockLocal();
        } else { 
            lock(db);
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
            dassert(false); // dassert will terminate buildbot
            msgasserted(16101, "expected write lock");
        }
    }
    void MongoMutex::assertAtLeastReadLocked() const { 
        if( !atLeastReadLocked() ) { 
            lockState().dump();
            dassert(false); // dassert will terminate buildbot
            msgasserted(16102, "expected read lock");
        }
    }

    void locked_W() {
        d.dbMutex._minfo.entered(); // hopefully eliminate one day 
    }
    void unlocking_w() { 
        // we can't commit early in this case; so a bit more to do here.
        dur::releasingWriteLock();
    }
    void unlocking_W() {
        d.dbMutex._minfo.leaving();
        dur::releasingWriteLock();
    }

    MongoMutex::MongoMutex() {
        static int n = 0;
        assert( ++n == 1 );
    }

}
