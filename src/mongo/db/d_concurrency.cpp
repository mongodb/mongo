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
#include "lockstat.h"

// oplog locking
// no top level read locks
// system.profile writing
// oplog now
// yielding
// commitIfNeeded

#define MONGOD_CONCURRENCY_LEVEL_GLOBAL 0
#define MONGOD_CONCURRENCY_LEVEL_DB 1

#ifndef MONGOD_CONCURRENCY_LEVEL
#define MONGOD_CONCURRENCY_LEVEL MONGOD_CONCURRENCY_LEVEL_DB
#endif

#define DB_LEVEL_LOCKING_ENABLED ( ( MONGOD_CONCURRENCY_LEVEL ) >= MONGOD_CONCURRENCY_LEVEL_DB )

namespace mongo { 
    inline BSONObj LockStat::report() const { 
        BSONObjBuilder x;
        BSONObjBuilder y;
        x.append("R", (long long) timeLocked[0]);
        x.append("W", (long long) timeLocked[1]);
        if( timeLocked[2] || timeLocked[3] ) {
            x.append("r", (long long) timeLocked[2]);
            x.append("w", (long long) timeLocked[3]);
        }
        y.append("R", (long long) timeAcquiring[0]);
        y.append("W", (long long) timeAcquiring[1]);
        if( timeAcquiring[2] || timeAcquiring[3] ) {
            y.append("r", (long long) timeAcquiring[2]);
            y.append("w", (long long) timeAcquiring[3]);
        }
        return BSON(
            "timeLocked" << x.obj() << 
            "timeAcquiring" << y.obj()
        );
    }

    inline unsigned LockStat::mapNo(char type) {
        switch( type ) { 
        case 'R' : return 0;
        case 'W' : return 1;
        case 'r' : return 2;
        case 'w' : return 3;
        default: ;
        }
        fassert(16146,false);
        return 0;
    }

    inline LockStat::Acquiring::Acquiring(LockStat& _ls, char t) : ls(_ls) { 
        type = mapNo(t);
        dassert( type < N );
    }

    // note: we have race conditions on the following += 
    // hmmm....

    inline LockStat::Acquiring::~Acquiring() { 
        ls.timeAcquiring[type] += tmr.micros();
        if( type == 1 ) 
            ls.W_Timer.reset();
    }

    inline void LockStat::unlocking(char tp) { 
        unsigned type = mapNo(tp);
        if( type == 1 ) 
            timeLocked[type] += W_Timer.micros();
    }
}

namespace mongo { 

    class WrapperForRWLock : boost::noncopyable { 
        SimpleRWLock r;
    public:
        string name() const { return r.name; }
        LockStat stats;
        WrapperForRWLock(const char *name) : r(name) { }
        void lock()          { LockStat::Acquiring a(stats,'W'); r.lock();          }
        void lock_shared()   { LockStat::Acquiring a(stats,'R'); r.lock_shared();   }
        void unlock()        { stats.unlocking('W');             r.unlock();        }
        void unlock_shared() { stats.unlocking('R');             r.unlock_shared(); }
    };

    class DBTryLockTimeoutException : public std::exception {
    public:
    	DBTryLockTimeoutException() {}
    	virtual ~DBTryLockTimeoutException() throw() { }
    };

    struct atstartup { 
        atstartup() { 
            cerr << "db level locking enabled: " << ( DB_LEVEL_LOCKING_ENABLED ) << endl;
        }
    } atst;

    Client* curopWaitingForLock( char type );
    void curopGotLock(Client*);
    struct Acquiring { 
        Client* c;
        ~Acquiring() { curopGotLock(c); }
        Acquiring(char type)
        {
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
    static mapsf<string,WrapperForRWLock*> dblocks;

    /* we don't want to touch dblocks too much as a mutex is involved.  thus party for that, 
       this is here...
    */
    WrapperForRWLock *nestableLocks[] = { 
        0, 
        new WrapperForRWLock("local"),
        new WrapperForRWLock("admin")
    };

    static void locked_W();
    static void unlocking_w();
    static void unlocking_W();

    class WrapperForTiming { 
        QLock q;
    public:
        LockStat stats;

        void start_greed()          { q.start_greed(); }

        void lock_r()               { LockStat::Acquiring a(stats,'r'); q.lock_r(); }
        void lock_w()               { LockStat::Acquiring a(stats,'w'); q.lock_w(); }
        void lock_R()               { LockStat::Acquiring a(stats,'R'); q.lock_R(); }
        void lock_W()               { LockStat::Acquiring a(stats,'W'); q.lock_W(); }
        void lock_W_stop_greed()    { LockStat::Acquiring a(stats,'W'); q.lock_W_stop_greed(); }

        // how to count try's that fail is an interesting question. we should get rid of try().
        bool lock_R_try(int millis) { LockStat::Acquiring a(stats,'R'); return q.lock_R_try(millis); }
        bool lock_W_try(int millis) { LockStat::Acquiring a(stats,'W'); return q.lock_W_try(millis); }

        void unlock_r()             { stats.unlocking('r'); q.unlock_r(); }
        void unlock_w()             { stats.unlocking('w'); q.unlock_w(); }
        void unlock_R()             { stats.unlocking('R'); q.unlock_R(); }
        void unlock_W()             { stats.unlocking('W'); q.unlock_W(); }

        // todo timing stats? : 
        void runExclusively(void (*f)(void)) { q.runExclusively(f); }
        void W_to_R()                        { q.W_to_R(); }
        bool R_to_W()                        { return q.R_to_W(); }
    };

    static WrapperForTiming& q = *new WrapperForTiming();

    void reportLockStats(BSONObjBuilder& result) {
        BSONObjBuilder b;
        b.append(".", q.stats.report());
        b.append("admin", nestableLocks[Lock::local]->stats.report());
        b.append("local", nestableLocks[Lock::local]->stats.report());
        {
            mapsf<string,WrapperForRWLock*>::ref r(dblocks);
            for( map<string,WrapperForRWLock*>::const_iterator i = r.r.begin(); i != r.r.end(); i++ ) {
                b.append(i->first, i->second->stats.report());
            }
        }
        result.append("locks", b.obj());
    }

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
        return cc()._ls;
    }

    static string kind(int n) { 
        if( n > 0 )
            return "W";
        if( n < 0 ) 
            return "R";
        return "?";
    }

    
    LockState::LockState() 
        : _recursive(0), 
          _threadState(0),
          _whichNestable( Lock::notnestable ),
          _nestableCount(0), 
          _otherCount(0), 
          _otherLock(NULL),
          _scopedLk(NULL)
    {
    }

    void LockState::locked( char newState ) {
        _threadState = newState;
    }
    void LockState::unlocked() {
        _threadState = 0;
    }

    void LockState::changeLockState( char newState ) {
        fassert( 16169 , _threadState != 0 );
        _threadState = newState;
    }


    /** Note: this is is called by the currentOp command, which is a different 
              thread. So be careful about thread safety here. For example reading 
              this->otherName would not be safe as-is!
    */
    void LockState::reportState(BSONObjBuilder& res) {
        BSONObjBuilder b;
        if( _threadState ) {
            char buf[2];
            buf[0] = _threadState; 
            buf[1] = 0;
            b.append(".", buf);
        }
        if( _nestableCount ) {
            string s = "?";
            if( _whichNestable == Lock::local ) 
                s = ".local";
            else if( _whichNestable == Lock::admin ) 
                s = ".admin";
            b.append(s, kind(_nestableCount));
        }
        if( _otherCount ) { 
            WrapperForRWLock *k = _otherLock;
            if( k ) {
                string s = ".";
                s += k->name();
                b.append(s, kind(_otherCount));
            }
        }
        BSONObj o = b.obj();
        if( !o.isEmpty() ) 
            res.append("locks", o);
    }

    void LockState::Dump() {
        lockState().dump();
    }
    void LockState::dump() {
        char s = _threadState;
        stringstream ss;
        ss << "lock status: ";
        if( s == 0 ){
            ss << "unlocked"; 
        }
        else {
            ss << s;
            if( _recursive ) { 
                ss << " recursive:" << _recursive;
            }
            ss << " otherCount:" << _otherCount;
            if( _otherCount ) {
                ss << " otherdb:" << _otherName;
            }
            if( _nestableCount ) {
                ss << " nestableCount:" << _nestableCount << " which:";
                if( _whichNestable == Lock::local ) 
                    ss << "local";
                else if( _whichNestable == Lock::admin ) 
                    ss << "admin";
                else 
                    ss << (int)_whichNestable;
            }
        }
        log() << ss.str() << endl;
    }

    void LockState::enterScopedLock( Lock::ScopedLock* lock ) {
        _recursive++;
        if ( _recursive == 1 ) {
            fassert(16115, _scopedLk == 0);
            _scopedLk = lock;
        }
    }

    Lock::ScopedLock* LockState::leaveScopedLock() {
        _recursive--;
        dassert( _recursive < 10000 );
        Lock::ScopedLock* temp = _scopedLk;

        if ( _recursive > 0 ) {
            return NULL;
        }
        
        _scopedLk = NULL;
        return temp;
    }

    void LockState::lockedNestable( Lock::Nestable what , int type) {
        verify( type );
        _whichNestable = what;
        _nestableCount += type;
    }

    void LockState::unlockedNestable() {
        _whichNestable = Lock::notnestable;
        _nestableCount = 0;
    }

    void LockState::lockedOther( const string& other , int type , WrapperForRWLock* lock ) {
        fassert( 16170 , _otherCount == 0 );
        _otherName = other;
        _otherCount = type;
        _otherLock = lock;
    }

    void LockState::unlockedOther() {
        _otherName = "";
        _otherCount = 0;
        _otherLock = 0;
    }

    char threadState() { 
        return lockState().threadState();
    }

    static bool lock_R_try(int ms) { 
        verify( threadState() == 0 );
        bool got = q.lock_R_try(ms);
        if( got ) 
            lockState().locked( 'R' );
        return got;
    }
    static bool lock_W_try(int ms) { 
        verify( threadState() == 0 );
        bool got = q.lock_W_try(ms);
        if( got ) {
            lockState().locked( 'W' );
            locked_W();
        }
        return got;
    }
    static void lock_W_stop_greed() { 
        verify( threadState() == 0 );
        lockState().locked( 'W' );
        {
            Acquiring a('W');
            q.lock_W_stop_greed();
        }
        locked_W();
    }
    static void lock_W() { 
        LockState& ls = lockState();
        if(  ls.threadState() ) {
            log() << "can't lock_W, threadState=" << (int) ls.threadState() << endl;
            fassert(16114,false);
        }
        getDur().commitIfNeeded(); // check before locking - will use an R lock for the commit if need to do one, which is better than W
        ls.locked( 'W' );
        {
            Acquiring a('W');
            q.lock_W();
        }
        locked_W();
    }
    static void unlock_W() { 
        wassert( threadState() == 'W' );
        unlocking_W();
        lockState().unlocked();
        q.unlock_W();
    }
    static void lock_R() { 
        LockState& ls = lockState();
        massert(16103, str::stream() << "can't lock_R, threadState=" << (int) ls.threadState(), ls.threadState() == 0);
        ls.locked( 'R' );
        Acquiring a('R');
        q.lock_R();
    }
    static void unlock_R() { 
        wassert( threadState() == 'R' );
        lockState().unlocked();
        q.unlock_R();
    }    
    static void lock_w() { 
        verify( threadState() == 0 );
        getDur().commitIfNeeded();
        lockState().locked( 'w' );
        q.lock_w();
    }
    static void unlock_w() { 
        unlocking_w();
        wassert( threadState() == 'w' );
        lockState().unlocked();
        q.unlock_w();
    }
    static void lock_r() {
        verify( threadState() == 0 );
        lockState().locked( 'r' );
        q.lock_r();
    }
    static void unlock_r() { 
        wassert( threadState() == 'r' );
        lockState().unlocked();
        q.unlock_r();
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
        // note this doesn't tell us much actually, it tells us if we are nesting locks but 
        // they could be the a global lock twice or a global and a specific or two specifics 
        // (such as including local) 
        return lockState().recursiveCount() > 1;
    }
    static bool weLocked(const LockState &ls, const StringData& ns) { 
        char db[MaxDatabaseNameLen];
        nsToDatabase(ns.data(), db);
        if( str::equals(db,"local") ) {
            if( ls.whichNestable() == Lock::local ) 
                return ls.nestableCount();
            return false;
        }
        if( str::equals(db,"admin") ) {
            if( ls.whichNestable() == Lock::admin ) 
                return ls.nestableCount();
            return false;
        }
        return db == ls.otherName() && ls.otherCount();
    }
    bool Lock::isWriteLocked(const StringData& ns) { 
        LockState &ls = lockState();
        if( ls.threadState() == 'W' ) 
            return true;
        if( ls.threadState() != 'w' ) 
            return false;
        return weLocked(ls,ns);
    }
    bool Lock::atLeastReadLocked(const StringData& ns)
    { 
        LockState &ls = lockState();
        if( ls.threadState() == 'R' || ls.threadState() == 'W' ) 
            return true; // global
        if( ls.threadState() == 0 ) 
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
        ls.enterScopedLock( this );
    }
    Lock::ScopedLock::~ScopedLock() { 
        LockState& ls = lockState();
        int prevCount = ls.recursiveCount();
        Lock::ScopedLock* what = ls.leaveScopedLock();
        fassert( 16171 , prevCount != 1 || what == this );
    }

    Lock::TempRelease::TempRelease() : cant( Lock::nested() )
    {
        if( cant )
            return;

        LockState& ls = lockState();
        
        fassert( 16116, ls.recursiveCount() == 1 );
        fassert( 16117, ls.threadState() != 0 );    
        
        scopedLk = ls.leaveScopedLock();
        fassert( 16118, scopedLk );
        scopedLk->tempRelease();
    }
    Lock::TempRelease::~TempRelease()
    {
        if( cant )
            return;


        
        LockState& ls = lockState();

        fassert( 16119, scopedLk );
        fassert( 16120 , ls.threadState() == 0 );

        ls.enterScopedLock( scopedLk );
        scopedLk->relock();
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
        lockState().changeLockState( 'R' );
    }
    // you will deadlock if 2 threads doing this
    bool Lock::GlobalWrite::upgrade() { 
        verify( !noop );
        verify( threadState() == 'R' );
        if( stoppedGreed ) { 
            // we undo stopgreed here if it were set earlier, as we now want a W lock
            stoppedGreed = false;
            q.start_greed();
        }
        if( q.R_to_W() ) {
            lockState().changeLockState( 'W' );
            return true;
        }
        return false;
    }

    Lock::GlobalRead::GlobalRead( int timeoutms ) {
        LockState& ls = lockState();
        char ts = ls.threadState();
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
        switch( ls.threadState() ) { 
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
        _nested = true;
        LockState& ls = lockState();
        if( ls.nestableCount() ) { 
            if( db != ls.whichNestable() ) { 
                error() << "can't lock local and admin db at the same time " << (int) db << ' ' << (int) ls.whichNestable() << endl;
                fassert(16131,false);
            }
            verify( ls.nestableCount() > 0 );
        }
        else {
            fassert(16132,weLocked==0);
            ls.lockedNestable(db, 1);
            weLocked = nestableLocks[db];
            weLocked->lock();
        }
    }
    void Lock::DBRead::lockNestable(Nestable db) { 
        _nested = true;
        LockState& ls = lockState();
        if( ls.nestableCount() ) { 
            // we are nested in our locking of local.  previous lock could be read OR write lock on local.
        }
        else {
            ls.lockedNestable(db,-1);
            fassert(16133,weLocked==0);
            weLocked = nestableLocks[db];
            weLocked->lock_shared();
        }
    }

    void Lock::DBWrite::lockOther(const string& db) {
        LockState& ls = lockState();

        // we do checks first, as on assert destructor won't be called so don't want to be half finished with our work.
        if( ls.otherCount() ) { 
            // nested. if/when we do temprelease with DBWrite we will need to increment here
            // (so we can not release or assert if nested).
            massert(16106, str::stream() << "internal error tried to lock two databases at the same time. old:" << ls.otherName() << " new:" << db , db == ls.otherName() );
            return;
        }

        // first lock for this db. check consistent order with local db lock so we never deadlock. local always comes last
        massert(16098, str::stream() << "can't dblock:" << db << " when local or admin is already locked", ls.nestableCount() == 0);

        {
            mapsf<string,WrapperForRWLock*>::ref r(dblocks);
            WrapperForRWLock*& lock = r[db];
            if( lock == 0 )
                lock = new WrapperForRWLock(db.c_str());
            ls.lockedOther( db , 1 , lock );
        }
        
        fassert(16134,weLocked==0);
        ls.otherLock()->lock();
        weLocked = ls.otherLock();
    }


    static Lock::Nestable n(const char *db) { 
        if( str::equals(db, "local") )
            return Lock::local;
        if( str::equals(db, "admin") )
            return Lock::admin;
        return Lock::notnestable;
    }

    void Lock::DBWrite::lockDB(const string& ns) {
        verify( ns.size() );
        Acquiring a( 'w' );
        locked_W=false;
        locked_w=false; 
        weLocked=0;
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
                lockOther(db);
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
        verify( ns.size() );
        Acquiring a( 'r' );
        locked_r=false; 
        weLocked=0; 
        LockState& ls = lockState();
        if( isRW(ls) )
            return;
        if (DB_LEVEL_LOCKING_ENABLED) {
            char db[MaxDatabaseNameLen];
            nsToDatabase(ns.data(), db);
            Nestable nested = n(db);
            if( !nested )
                lockOther(db);
            lockTop(ls);
            if( nested )
                lockNestable(nested);
        } 
        else {
            lock_R();
            locked_r = true;
        }
    }

    Lock::DBWrite::DBWrite( const StringData& ns ) : what(ns.data()), _nested(false) {
        lockDB( what );
    }

    Lock::DBRead::DBRead( const StringData& ns )   : what(ns.data()), _nested(false) {
        lockDB( what );
    }

    Lock::DBWrite::~DBWrite() {
        unlockDB();
    }
    Lock::DBRead::~DBRead() {
        unlockDB();
    }

    void Lock::DBWrite::unlockDB() {
        if( weLocked ) {
            if ( _nested )
                lockState().unlockedNestable();
            else
                lockState().unlockedOther();
    
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
        weLocked = 0;
        locked_W = locked_w = false;
    }
    void Lock::DBRead::unlockDB() {
        if( weLocked ) {
            if( _nested )
                lockState().unlockedNestable();
            else
                lockState().unlockedOther();

            weLocked->unlock_shared();
        }

        if( locked_r ) {
            if (DB_LEVEL_LOCKING_ENABLED) {
                unlock_r();
            } else {
                unlock_R();
            }
        }
        weLocked = 0;
        locked_r = false;
    }

    bool Lock::DBRead::isRW(LockState& ls) const { 
        switch( ls.threadState() ) { 
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
        switch( ls.threadState() ) { 
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
        switch( ls.threadState() ) { 
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

    void Lock::DBRead::lockOther(const string& db) {
        LockState& ls = lockState();

        // we do checks first, as on assert destructor won't be called so don't want to be half finished with our work.
        if( ls.otherCount() ) { 
            // nested. prev could be read or write. if/when we do temprelease with DBRead/DBWrite we will need to increment/decrement here
            // (so we can not release or assert if nested).  temprelease we should avoid if we can though, it's a bit of an anti-pattern.
            massert(16099, str::stream() << "internal error tried to lock two databases at the same time. old:" << ls.otherName() << " new:" << db, db == ls.otherName() );
            return;
        }

        // first lock for this db. check consistent order with local db lock so we never deadlock. local always comes last
        massert(16100, str::stream() << "can't dblock:" << db << " when local or admin is already locked", ls.nestableCount() == 0);

        {
            mapsf<string,WrapperForRWLock*>::ref r(dblocks);
            WrapperForRWLock*& lock = r[db];
            if( lock == 0 )
                lock = new WrapperForRWLock(db.c_str());
            ls.lockedOther( db , -1 , lock );
        }
        fassert(16135,weLocked==0);
        ls.otherLock()->lock_shared();
        weLocked = ls.otherLock();
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
