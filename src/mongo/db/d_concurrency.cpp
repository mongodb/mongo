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
#include "dur.h"

// oplog locking
// no top level read locks
// system.profile writing
// oplog now
// yielding
// commitIfNeeded

#define MONGOD_CONCURRENCY_LEVEL_GLOBAL 0
#define MONGOD_CONCURRENCY_LEVEL_DB 1

//#define MONGOD_CONCURRENCY_LEVEL 1

#ifndef MONGOD_CONCURRENCY_LEVEL
#define MONGOD_CONCURRENCY_LEVEL MONGOD_CONCURRENCY_LEVEL_GLOBAL
#endif

#define DB_LEVEL_LOCKING_ENABLED ( ( MONGOD_CONCURRENCY_LEVEL ) >= MONGOD_CONCURRENCY_LEVEL_DB )

namespace mongo { 

    class DBTryLockTimeoutException : public std::exception {
    public:
    	DBTryLockTimeoutException() {}
    	virtual ~DBTryLockTimeoutException() throw() { }
    };

    struct atstartup { 
        atstartup() { 
            cout << "db level locking enabled: " << ( DB_LEVEL_LOCKING_ENABLED ) << endl;
        }
    } atst;

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

    /* we don't want to touch dblocks too much as a mutex is involved.  thus party for that, 
       this is here...
    */
    SimpleRWLock *nestableLocks[] = { 
        0, 
        new SimpleRWLock("localDBLock"),
        new SimpleRWLock("adminDBLock")
    };

    static void locked_W();
    static void unlocking_w();
    static void unlocking_W();
    static QLock& q = *new QLock();
    TSP_DECLARE(LockState,ls);
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
        boost::mutex::scoped_lock lk( m );
        verify( w.n > 0 );
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
    LockState::LockState() : recursive(0), 
                             threadState(0), 
                             nestableCount(0), 
                             otherCount(0), 
                             otherLock(NULL),
                             scopedLk(NULL)
    {
        whichNestable = Lock::notnestable;
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
            ss << " otherCount:" << otherCount;
            if( otherCount ) {
                ss << " otherdb:" << otherName;
            }
            if( nestableCount ) {
                ss << " nestableCount:" << nestableCount << " which:";
                if( whichNestable == Lock::local ) 
                    ss << "local";
                else if( whichNestable == Lock::admin ) 
                    ss << "admin";
                else 
                    ss << (int) whichNestable;
            }
        }
        log() << ss.str() << endl;
    }
    inline char& threadState() { 
        return lockState().threadState;
    }

    // note this doesn't tell us much actually, it tells us if we are nesting locks but 
    // they could be the a global lock twice or a global and a specific or two specifics 
    // (such as including local) 
    inline unsigned& recursive() {
        return lockState().recursive;
    }

    static bool lock_R_try(int ms) { 
        verify( threadState() == 0 );
        bool got = q.lock_R_try(ms);
        if( got ) 
            threadState() = 'R';
        return got;
    }
    static bool lock_W_try(int ms) { 
        verify( threadState() == 0 );
        bool got = q.lock_W_try(ms);
        if( got ) {
            threadState() = 'W';
            locked_W();
        }
        return got;
    }
    static void lock_W_stop_greed() { 
        verify( threadState() == 0 );
        threadState() = 'W';
        {
            Acquiring a('W');
            q.lock_W_stop_greed();
        }
        locked_W();
    }
    static void lock_W() { 
        LockState& ls = lockState();
        if(  ls.threadState ) {
            log() << "can't lock_W, threadState=" << (int) ls.threadState << endl;
            fassert(16114,false);
        }
        getDur().commitIfNeeded(); // check before locking - will use an R lock for the commit if need to do one, which is better than W
        threadState() = 'W';
        {
            Acquiring a('W');
            q.lock_W();
        }
        locked_W();
    }
    static void unlock_W() { 
        wassert( threadState() == 'W' );
        unlocking_W();
        threadState() = 0;
        q.unlock_W();
    }
    static void lock_R() { 
        LockState& ls = lockState();
        massert(16103, str::stream() << "can't lock_R, threadState=" << (int) ls.threadState, ls.threadState == 0);
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
        verify( ts == 0 );
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
        verify( ts == 0 );
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
        verify( threadState() == 0 ); // as this spans threads the tls wouldn't make sense
        lock_W_stop_greed();
    }
    void Lock::ThreadSpanningOp::W_to_R() { 
        verify( threadState() == 'W' );
        dur::assertNothingSpooled();
        q.W_to_R();
        threadState() = 'R';
    }
    void Lock::ThreadSpanningOp::unsetW() { // note there is no unlocking_W() call here
        verify( threadState() == 'W' );
        q.unlock_W();
        q.start_greed();
        threadState() = 0;
    }
    void Lock::ThreadSpanningOp::unsetR() {
        verify( threadState() == 'R' || threadState() == 0 ); 
        q.unlock_R();
        q.start_greed();
        threadState() = 0;
    }
    void Lock::ThreadSpanningOp::handoffR() {
        threadState() = 0;
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
        return recursive() > 1;
    }
    static bool weLocked(const LockState &ls, const StringData& ns) { 
        char db[MaxDatabaseNameLen];
        nsToDatabase(ns.data(), db);
        if( str::equals(db,"local") ) {
            if( ls.whichNestable == Lock::local ) 
                return ls.nestableCount;
            return false;
        }
        if( str::equals(db,"admin") ) {
            if( ls.whichNestable == Lock::admin ) 
                return ls.nestableCount;
            return false;
        }
        return db == ls.otherName && ls.otherCount;
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
            msgasserted(16104, str::stream() << "expected to be read locked for " << ns);
        }
    }
    void Lock::assertWriteLocked(const StringData& ns) { 
        if( !Lock::isWriteLocked(ns) ) { 
            lockState().dump();
            msgasserted(16105, str::stream() << "expected to be write locked for " << ns);
        }
    }
    bool Lock::dbLevelLockingEnabled() {
        return DB_LEVEL_LOCKING_ENABLED;
    }
    
    Lock::ScopedLock::ScopedLock() {
        LockState& ls = lockState();
        ls.recursive++;
        if( ls.recursive == 1 ) { 
            fassert(16115, ls.scopedLk == 0);
            ls.scopedLk = this;
        }
    }
    Lock::ScopedLock::~ScopedLock() { 
        LockState& ls = lockState();
        ls.recursive--;
        dassert( ls.recursive < 10000 );
        if( ls.recursive == 0 ) { 
            wassert( ls.scopedLk == this );
            ls.scopedLk = NULL;
        }
        else { 
            wassert( ls.scopedLk != this );
        }
    }

    Lock::TempRelease::TempRelease() : cant( Lock::nested() )
    {
        if( cant )
            return;
        LockState& ls = lockState();
        fassert( 16116, ls.recursive == 1 );
        fassert( 16117, ls.threadState );    
        fassert( 16118, ls.scopedLk );
        ls.recursive--;
        ls.scopedLk->tempRelease();
        scopedLk = ls.scopedLk;
        ls.scopedLk = NULL;  // this must be cleared out for further ScopedLock's to work
    }
    Lock::TempRelease::~TempRelease()
    {
        if( cant )
            return;
        LockState& ls = lockState();
        ls.recursive++;
        fassert( 16119, scopedLk );
        fassert( 16120, ls.scopedLk==NULL );
        ls.scopedLk = scopedLk;
        ls.scopedLk->relock();
    }

    void Lock::GlobalWrite::tempRelease() { 
        fassert(16121, !noop);
        char ts = threadState();
        fassert(16122, ts != 'R'); // indicates downgraded; not allowed with temprelease
        fassert(16123, ts == 'W');
        fassert(16124, !stoppedGreed); // not allowed with temprelease
        unlock_W();
    }
    void Lock::GlobalWrite::relock() { 
        fassert(16125, !noop);
        char ts = threadState();
        fassert(16126, ts == 0);
        lock_W();
    }

    void Lock::GlobalRead::tempRelease() { 
        fassert(16127, !noop);
        char ts = threadState();
        fassert(16128, ts == 'R');
        unlock_R();
    }
    void Lock::GlobalRead::relock() { 
        fassert(16129, !noop);
        char ts = threadState();
        fassert(16130, ts == 0);
        lock_R();
    }

    void Lock::DBWrite::tempRelease() { 
        unlockDB();
    }
    void Lock::DBWrite::relock() { 
        lockDB(what);
    }
    void Lock::DBRead::tempRelease() {
        unlockDB();
    }
    void Lock::DBRead::relock() { 
        lockDB(what);
    }

    Lock::GlobalWrite::GlobalWrite(bool sg, int timeoutms) : 
        stoppedGreed(sg)
    {
        char ts = threadState();
        noop = false;
        if( ts == 'W' ) { 
            noop = true;
            DEV if( sg ) { 
                log() << "info Lock::GlobalWrite does not stop greed on recursive invocation" << endl;
            }
            return;
        }
        dassert( ts == 0 );
        if( sg ) {
            lock_W_stop_greed();
        } 
        else if ( timeoutms != -1 ) {
            bool success = lock_W_try( timeoutms );
            if ( !success ) throw DBTryLockTimeoutException(); 
        }
        else {
            lock_W();
        }
    }
    Lock::GlobalWrite::~GlobalWrite() {
        if( noop ) { 
            return;
        }
        if( threadState() == 'R' ) { // we downgraded
            unlock_R();
        }
        else {
            unlock_W();
        }
        if( stoppedGreed ) {
            q.start_greed();
        }
    }
    void Lock::GlobalWrite::downgrade() { 
        verify( !noop );
        verify( threadState() == 'W' );
        q.W_to_R();
        threadState() = 'R';
    }
    // you will deadlock if 2 threads doing this
    bool Lock::GlobalWrite::upgrade() { 
        verify( !noop );
        verify( threadState() == 'R' );
        if( q.R_to_W() ) {
            threadState() = 'W';
            return true;
        }
        return false;
    }

    Lock::GlobalRead::GlobalRead( int timeoutms ) {
        LockState& ls = lockState();
        char ts = ls.threadState;
        noop = false;
        if( ts == 'R' || ts == 'W' ) { 
            noop = true;
            return;
        }
        if ( timeoutms != -1 ) {
            bool success = lock_R_try( timeoutms );
            if ( !success ) throw DBTryLockTimeoutException(); 
        }
        else {
            lock_R(); // we are unlocked in the qlock/top sense.  lock_R will assert if we are in an in compatible state
        }
    }
    Lock::GlobalRead::~GlobalRead() {
        if( !noop ) {
            unlock_R();
        }
    }

    bool Lock::DBWrite::isW(LockState& ls) const { 
        switch( ls.threadState ) { 
        case 'R' : 
            {
                error() << "trying to get a w lock after already getting an R lock is not allowed" << endl;
                verify(false);
            }
        case 'r' : 
            {
                error() << "trying to get a w lock after already getting an r lock is not allowed" << endl;
                verify(false);
            }
            return false;
        case 'W' :
            return true; // lock nothing further
        default:
            verify(false);
        case 'w' :
        case  0  : 
            break;
        }
        return false;
    }
    void Lock::DBWrite::lockNestable(Nestable db) { 
        LockState& ls = lockState();
        if( ls.nestableCount ) { 
            if( db != ls.whichNestable ) { 
                error() << "can't lock local and admin db at the same time " << (int) db << ' ' << (int) ls.whichNestable << endl;
                fassert(16131,false);
            }
            verify( ls.nestableCount > 0 );
        }
        else {
            ls.whichNestable = db;
            ourCounter = &ls.nestableCount;
            ls.nestableCount++;
            fassert(16132,weLocked==0);
            weLocked = nestableLocks[db];
            weLocked->lock();
        }
    }
    void Lock::DBRead::lockNestable(Nestable db) { 
        LockState& ls = lockState();
        if( ls.nestableCount ) { 
            // we are nested in our locking of local.  previous lock could be read OR write lock on local.
        }
        else {
            ls.whichNestable = db;
            ourCounter = &ls.nestableCount;
            ls.nestableCount--;
            fassert(16133,weLocked==0);
            weLocked = nestableLocks[db];
            weLocked->lock_shared();
        }
    }

    void Lock::DBWrite::lock(const string& db) {
        LockState& ls = lockState();

        // we do checks first, as on assert destructor won't be called so don't want to be half finished with our work.
        bool same = (db == ls.otherName);
        if( ls.otherCount ) { 
            // nested. if/when we do temprelease with DBWrite we will need to increment here
            // (so we can not release or assert if nested).
            massert(16106, str::stream() << "internal error tried to lock two databases at the same time. old:" << ls.otherName << " new:" << db,same);
            return;
        }

        // first lock for this db. check consistent order with local db lock so we never deadlock. local always comes last
        massert(16098, str::stream() << "can't dblock:" << db << " when local or admin is already locked", ls.nestableCount == 0);

        ourCounter = &ls.otherCount;
        dassert( ls.otherCount == 0 );
        ls.otherCount++;
        if( !same ) {
            ls.otherName = db;
            mapsf<string,SimpleRWLock*>::ref r(dblocks);
            SimpleRWLock*& lock = r[db];
            if( lock == 0 )
                lock = new SimpleRWLock();
            ls.otherLock = lock;
        }
        fassert(16134,weLocked==0);
        ls.otherLock->lock();
        weLocked = ls.otherLock;
    }


    static Lock::Nestable n(const char *db) { 
        if( str::equals(db, "local") )
            return Lock::local;
        if( str::equals(db, "admin") )
            return Lock::admin;
        return Lock::notnestable;
    }

    void Lock::DBWrite::lockDB(const string& ns) {
        locked_W=false;
        locked_w=false; weLocked=0; ourCounter = 0;
        LockState& ls = lockState();
        if( isW(ls) )
            return;
        if (DB_LEVEL_LOCKING_ENABLED) {
            char db[MaxDatabaseNameLen];
            nsToDatabase(ns.data(), db);
            Nestable nested = n(db);
            if( nested == admin ) { 
                // we can't nestedly lock both admin and local as implemented. so lock_W.
                lock_W();
                locked_W = true;
                return;
            } 
            if( !nested )
                lock(db);
            lockTop(ls);
            if( nested )
                lockNestable(nested);
        } 
        else {
            lock_W();
            locked_w = true;
        }
    }
    void Lock::DBRead::lockDB(const string& ns) {
        locked_r=false; weLocked=0; ourCounter = 0;
        LockState& ls = lockState();
        if( isRW(ls) )
            return;
        if (DB_LEVEL_LOCKING_ENABLED) {
            char db[MaxDatabaseNameLen];
            nsToDatabase(ns.data(), db);
            Nestable nested = n(db);
            if( !nested )
                lock(db);
            lockTop(ls);
            if( nested )
                lockNestable(nested);
        } 
        else {
            lock_R();
            locked_r = true;
        }
    }

    Lock::DBWrite::DBWrite( const StringData& ns ) : what(ns.data()) {
        lockDB( what );
    }

    Lock::DBRead::DBRead( const StringData& ns )   : what(ns.data()) {
        lockDB( what );
    }

    Lock::DBWrite::~DBWrite() {
        unlockDB();
    }
    Lock::DBRead::~DBRead() {
        unlockDB();
    }

    void Lock::DBWrite::unlockDB() {
        if( ourCounter ) {
            (*ourCounter)--;
            wassert( *ourCounter >= 0 );
        }
        if( weLocked ) {
            wassert( ourCounter && *ourCounter == 0 );
            weLocked->unlock();
        }
        if( locked_w ) {
            if (DB_LEVEL_LOCKING_ENABLED) {
                unlock_w();
            } else {
                unlock_W();
            }
        }
        if( locked_W ) {
            unlock_W();
        }
        ourCounter = 0;
        weLocked = 0;
        locked_W = locked_w = false;
    }
    void Lock::DBRead::unlockDB() {
        if( ourCounter ) {
            (*ourCounter)++;
            wassert( *ourCounter <= 0 );
        }
        if( weLocked ) {
            wassert( ourCounter && *ourCounter == 0 );
            weLocked->unlock_shared();
        }
        if( locked_r ) {
            if (DB_LEVEL_LOCKING_ENABLED) {
                unlock_r();
            } else {
                unlock_R();
            }
        }
        ourCounter = 0;
        weLocked = 0;
        locked_r = false;
    }

    bool Lock::DBRead::isRW(LockState& ls) const { 
        switch( ls.threadState ) { 
        case 'W' :
        case 'R' : 
            return true;
        case 'r' :
        case 'w' :
            return false;
        default:
            verify(false);
        case  0  : 
            ;
        }
        return false;
    }

    void Lock::DBWrite::lockTop(LockState& ls) { 
        switch( ls.threadState ) { 
        case 'w':
            break;
        default:
            verify(false);
        case  0  : 
            lock_w();
            locked_w = true;
        }
    }
    void Lock::DBRead::lockTop(LockState& ls) { 
        switch( ls.threadState ) { 
        case 'r':
        case 'w':
            break;
        default:
            verify(false);
        case  0  : 
            lock_r();
            locked_r = true;
        }
    }

    void Lock::DBRead::lock(const string& db) {
        LockState& ls = lockState();

        // we do checks first, as on assert destructor won't be called so don't want to be half finished with our work.
        bool same = (db == ls.otherName);
        if( ls.otherCount ) { 
            // nested. prev could be read or write. if/when we do temprelease with DBRead/DBWrite we will need to increment/decrement here
            // (so we can not release or assert if nested).  temprelease we should avoid if we can though, it's a bit of an anti-pattern.
            massert(16099, str::stream() << "internal error tried to lock two databases at the same time. old:" << ls.otherName << " new:" << db,same);
            return;
        }

        // first lock for this db. check consistent order with local db lock so we never deadlock. local always comes last
        massert(16100, str::stream() << "can't dblock:" << db << " when local or admin is already locked", ls.nestableCount == 0);

        ourCounter = &ls.otherCount;
        dassert( ls.otherCount == 0 );
        ls.otherCount--;
        if( !same ) {
            ls.otherName = db;
            mapsf<string,SimpleRWLock*>::ref r(dblocks);
            SimpleRWLock*& lock = r[db];
            if( lock == 0 )
                lock = new SimpleRWLock();
            ls.otherLock = lock;
        }
        fassert(16135,weLocked==0);
        ls.otherLock->lock_shared();
        weLocked = ls.otherLock;
    }
}

// legacy hooks and glue
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
        _got( false ),
        _dbwlock( NULL )
    { 
        try { 
            _dbwlock.reset(new Lock::GlobalWrite( false, tryms ));
        }
        catch ( DBTryLockTimeoutException & ) {
            return;
        }
        _got = true;
    }
    writelocktry::~writelocktry() { 
    }

    // note: the 'already' concept here might be a bad idea as a temprelease wouldn't notice it is nested then
    readlocktry::readlocktry( int tryms ) :
        _got( false ),
        _dbrlock( NULL )
    {
        try { 
            _dbrlock.reset(new Lock::GlobalRead( tryms ));
        }
        catch ( DBTryLockTimeoutException & ) {
            return;
        }
        _got = true;
    }
    readlocktry::~readlocktry() { 
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
        verify( ++n == 1 );
    }
}
