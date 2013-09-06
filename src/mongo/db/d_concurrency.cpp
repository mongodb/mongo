// @file d_concurrency.cpp 

/**
*    Copyright (C) 2008 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/pch.h"

#include "mongo/db/d_concurrency.h"

#include "mongo/db/client.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/curop.h"
#include "mongo/db/d_globals.h"
#include "mongo/db/dur.h"
#include "mongo/db/lockstat.h"
#include "mongo/db/namespace_string.h"
#include "mongo/server.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/mapsf.h"
#include "mongo/util/concurrency/qlock.h"
#include "mongo/util/concurrency/rwlock.h"
#include "mongo/util/concurrency/threadlocal.h"
#include "mongo/util/stacktrace.h"

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

namespace mongo { 

    static const bool DB_LEVEL_LOCKING_ENABLED = ( ( MONGOD_CONCURRENCY_LEVEL ) >= MONGOD_CONCURRENCY_LEVEL_DB );

    inline LockState& lockState() { 
        return cc().lockState();
    }

    char threadState() { 
        return lockState().threadState();
    }

    class DBTryLockTimeoutException : public std::exception {
    public:
        DBTryLockTimeoutException() {}
        virtual ~DBTryLockTimeoutException() throw() { }
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
    typedef mapsf< StringMap<WrapperForRWLock*> > DBLocksMap;
    static DBLocksMap dblocks;

    /* we don't want to touch dblocks too much as a mutex is involved.  thus party for that, 
       this is here...
    */
    WrapperForRWLock *nestableLocks[] = { 
        0, 
        new WrapperForRWLock("local"),
        new WrapperForRWLock("admin")
    };

    LockStat* Lock::nestableLockStat( Nestable db ) {
        return &nestableLocks[db]->stats;
    }

    static void locked_W();
    static void unlocking_w();
    static void unlocking_W();

    class WrapperForQLock { 
        QLock q;
    public:
        LockStat stats;

        void lock_r() { 
            verify( threadState() == 0 );
            lockState().lockedStart( 'r' );
            q.lock_r(); 
        }
        
        void lock_w() { 
            verify( threadState() == 0 );
            getDur().commitIfNeeded();
            lockState().lockedStart( 'w' );
            q.lock_w(); 
        }
        
        void lock_R() {
            LockState& ls = lockState();
            massert(16103, str::stream() << "can't lock_R, threadState=" << (int) ls.threadState(), ls.threadState() == 0);
            ls.lockedStart( 'R' );
            q.lock_R(); 
        }

        void lock_W() {            
            LockState& ls = lockState();
            if(  ls.threadState() ) {
                log() << "can't lock_W, threadState=" << (int) ls.threadState() << endl;
                fassert(16114,false);
            }
            getDur().commitIfNeeded(); // check before locking - will use an R lock for the commit if need to do one, which is better than W
            ls.lockedStart( 'W' );
            {
                q.lock_W();
            }
            locked_W();
        }

        // how to count try's that fail is an interesting question. we should get rid of try().
        bool lock_R_try(int millis) { 
            verify( threadState() == 0 );
            bool got = q.lock_R_try(millis); 
            if( got ) 
                lockState().lockedStart( 'R' );
            return got;
        }
        
        bool lock_W_try(int millis) { 
            verify( threadState() == 0 );
            bool got = q.lock_W_try(millis); 
            if( got ) {
                lockState().lockedStart( 'W' );
                locked_W();
            }
            return got;
        }

        void unlock_r() {
            wassert( threadState() == 'r' );
            lockState().unlocked();
            q.unlock_r(); 
        }

        void unlock_w() {
            unlocking_w();
            wassert( threadState() == 'w' );
            lockState().unlocked();
            q.unlock_w(); 
        }

        void unlock_R() { _unlock_R(); }

        void unlock_W() {
            wassert( threadState() == 'W' );
            unlocking_W();
            lockState().unlocked();
            q.unlock_W(); 
        }

        // todo timing stats? : 
        void W_to_R()                        { q.W_to_R(); }
        void R_to_W()                        { q.R_to_W(); }
        bool w_to_X() { return q.w_to_X(); }
        void X_to_w() { q.X_to_w(); }

    private:
        void _unlock_R() {
            wassert( threadState() == 'R' );
            lockState().unlocked();
            q.unlock_R(); 
        }
    };

    static WrapperForQLock& qlk = *new WrapperForQLock();
    LockStat* Lock::globalLockStat() {
        return &qlk.stats;
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

    bool Lock::isWriteLocked(const StringData& ns) { 
        LockState &ls = lockState();
        if( ls.threadState() == 'W' ) 
            return true;
        if( ls.threadState() != 'w' ) 
            return false;
        return ls.isLocked( ns );
    }
    bool Lock::atLeastReadLocked(const StringData& ns)
    { 
        LockState &ls = lockState();
        if( ls.threadState() == 'R' || ls.threadState() == 'W' ) 
            return true; // global
        if( ls.threadState() == 0 ) 
            return false;
        return ls.isLocked( ns );
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

    RWLockRecursive &Lock::ParallelBatchWriterMode::_batchLock = *(new RWLockRecursive("special"));
    void Lock::ParallelBatchWriterMode::iAmABatchParticipant() {
        lockState()._batchWriter = true;
    }

    Lock::ParallelBatchWriterSupport::ParallelBatchWriterSupport() {
        relock();
    }

    void Lock::ParallelBatchWriterSupport::tempRelease() {
        _lk.reset( 0 );
    }

    void Lock::ParallelBatchWriterSupport::relock() {
        LockState& ls = lockState();
        if ( ! ls._batchWriter ) {
            AcquiringParallelWriter a(ls);
            _lk.reset( new RWLockRecursive::Shared(ParallelBatchWriterMode::_batchLock) );
        }
    }


    Lock::ScopedLock::ScopedLock( char type ) 
        : _type(type), _stat(0) {
        LockState& ls = lockState();
        ls.enterScopedLock( this );
    }
    Lock::ScopedLock::~ScopedLock() { 
        LockState& ls = lockState();
        int prevCount = ls.recursiveCount();
        Lock::ScopedLock* what = ls.leaveScopedLock();
        fassert( 16171 , prevCount != 1 || what == this );
    }
    
    long long Lock::ScopedLock::acquireFinished( LockStat* stat ) {
        long long acquisitionTime = _timer.micros();
        _timer.reset();
        _stat = stat;
        cc().curop()->lockStat().recordAcquireTimeMicros( _type , acquisitionTime );
        return acquisitionTime;
    }

    void Lock::ScopedLock::tempRelease() {
        long long micros = _timer.micros();
        _tempRelease();
        _pbws_lk.tempRelease();
        _recordTime( micros ); // might as well do after we unlock
    }

    void Lock::ScopedLock::_recordTime( long long micros ) {
        if ( _stat )
            _stat->recordLockTimeMicros( _type , micros );
        cc().curop()->lockStat().recordLockTimeMicros( _type , micros );
    }

    void Lock::ScopedLock::recordTime() {
        _recordTime(_timer.micros());
    }

    void Lock::ScopedLock::resetTime() {
        _timer.reset();
    }
    
    void Lock::ScopedLock::relock() {
        _pbws_lk.relock();
        _relock();
        resetTime();
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

    void Lock::GlobalWrite::_tempRelease() { 
        fassert(16121, !noop);
        char ts = threadState();
        fassert(16122, ts != 'R'); // indicates downgraded; not allowed with temprelease
        fassert(16123, ts == 'W');
        qlk.unlock_W();
    }
    void Lock::GlobalWrite::_relock() { 
        fassert(16125, !noop);
        char ts = threadState();
        fassert(16126, ts == 0);
        Acquiring a(this,lockState());
        qlk.lock_W();
    }

    void Lock::GlobalRead::_tempRelease() { 
        fassert(16127, !noop);
        char ts = threadState();
        fassert(16128, ts == 'R');
        qlk.unlock_R();
    }
    void Lock::GlobalRead::_relock() { 
        fassert(16129, !noop);
        char ts = threadState();
        fassert(16130, ts == 0);
        Acquiring a(this,lockState());
        qlk.lock_R();
    }

    void Lock::DBWrite::_tempRelease() { 
        unlockDB();
    }
    void Lock::DBWrite::_relock() { 
        lockDB(_what);
    }
    void Lock::DBRead::_tempRelease() {
        unlockDB();
    }
    void Lock::DBRead::_relock() { 
        lockDB(_what);
    }

    Lock::GlobalWrite::GlobalWrite(bool sg, int timeoutms)
        : ScopedLock('W') {
        char ts = threadState();
        noop = false;
        if( ts == 'W' ) { 
            noop = true;
            return;
        }
        dassert( ts == 0 );

        Acquiring a(this,lockState());
        
        if ( timeoutms != -1 ) {
            bool success = qlk.lock_W_try( timeoutms );
            if ( !success ) throw DBTryLockTimeoutException(); 
        }
        else {
            qlk.lock_W();
        }
    }
    Lock::GlobalWrite::~GlobalWrite() {
        if( noop ) { 
            return;
        }
        recordTime();  // for lock stats
        if( threadState() == 'R' ) { // we downgraded
            qlk.unlock_R();
        }
        else {
            qlk.unlock_W();
        }
    }
    void Lock::GlobalWrite::downgrade() { 
        verify( !noop );
        verify( threadState() == 'W' );
        qlk.W_to_R();
        lockState().changeLockState( 'R' );
    }

    // you will deadlock if 2 threads doing this
    void Lock::GlobalWrite::upgrade() { 
        verify( !noop );
        verify( threadState() == 'R' );
        qlk.R_to_W();
        lockState().changeLockState( 'W' );
    }

    Lock::GlobalRead::GlobalRead( int timeoutms ) 
        : ScopedLock( 'R' ) {
        LockState& ls = lockState();
        char ts = ls.threadState();
        noop = false;
        if( ts == 'R' || ts == 'W' ) { 
            noop = true;
            return;
        }

        Acquiring a(this,ls);

        if ( timeoutms != -1 ) {
            bool success = qlk.lock_R_try( timeoutms );
            if ( !success ) throw DBTryLockTimeoutException(); 
        }
        else {
            qlk.lock_R(); // we are unlocked in the qlock/top sense.  lock_R will assert if we are in an in compatible state
        }
    }
    Lock::GlobalRead::~GlobalRead() {
        if( !noop ) {
            recordTime();  // for lock stats
            qlk.unlock_R();
        }
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
            fassert(16132,_weLocked==0);
            ls.lockedNestable(db, 1);
            _weLocked = nestableLocks[db];
            _weLocked->lock();
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
            fassert(16133,_weLocked==0);
            _weLocked = nestableLocks[db];
            _weLocked->lock_shared();
        }
    }

    void Lock::DBWrite::lockOther(const StringData& db) {
        fassert( 16252, !db.empty() );
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

        if( db != ls.otherName() )
        {
            DBLocksMap::ref r(dblocks);
            WrapperForRWLock*& lock = r[db];
            if( lock == 0 )
                lock = new WrapperForRWLock(db);
            ls.lockedOther( db , 1 , lock );
        }
        else { 
            DEV OCCASIONALLY { dassert( dblocks.get(db) == ls.otherLock() ); }
            ls.lockedOther(1);
        }
        
        fassert(16134,_weLocked==0);
        ls.otherLock()->lock();
        _weLocked = ls.otherLock();
    }

    static Lock::Nestable n(const StringData& db) { 
        if( db == "local" )
            return Lock::local;
        if( db == "admin" )
            return Lock::admin;
        return Lock::notnestable;
    }

    void Lock::DBWrite::lockDB(const string& ns) {
        fassert( 16253, !ns.empty() );
        LockState& ls = lockState();
        
        Acquiring a(this,ls);
        _locked_W=false;
        _locked_w=false; 
        _weLocked=0;


        massert( 16186 , "can't get a DBWrite while having a read lock" , ! ls.hasAnyReadLock() );
        if( ls.isW() )
            return;

        if (DB_LEVEL_LOCKING_ENABLED) {
            StringData db = nsToDatabaseSubstring( ns );
            Nestable nested = n(db);
            if( nested == admin ) { 
                // we can't nestedly lock both admin and local as implemented. so lock_W.
                qlk.lock_W();
                _locked_W = true;
                return;
            } 
            if( !nested )
                lockOther(db);
            lockTop(ls);
            if( nested )
                lockNestable(nested);
        } 
        else {
            qlk.lock_W();
            _locked_w = true;
        }
    }

    void Lock::DBRead::lockDB(const string& ns) {
        fassert( 16254, !ns.empty() );
        LockState& ls = lockState();
        
        Acquiring a(this,ls);
        _locked_r=false; 
        _weLocked=0; 

        if ( ls.isRW() )
            return;
        if (DB_LEVEL_LOCKING_ENABLED) {
            StringData db = nsToDatabaseSubstring(ns);
            Nestable nested = n(db);
            if( !nested )
                lockOther(db);
            lockTop(ls);
            if( nested )
                lockNestable(nested);
        } 
        else {
            qlk.lock_R();
            _locked_r = true;
        }
    }

    Lock::DBWrite::DBWrite( const StringData& ns )
        : ScopedLock( 'w' ), _what(ns.toString()), _nested(false) {
        lockDB( _what );
    }

    Lock::DBRead::DBRead( const StringData& ns )
        : ScopedLock( 'r' ), _what(ns.toString()), _nested(false) {
        lockDB( _what );
    }

    Lock::DBWrite::~DBWrite() {
        unlockDB();
    }
    Lock::DBRead::~DBRead() {
        unlockDB();
    }

    void Lock::DBWrite::unlockDB() {
        if( _weLocked ) {
            recordTime();  // for lock stats
        
            if ( _nested )
                lockState().unlockedNestable();
            else
                lockState().unlockedOther();
    
            _weLocked->unlock();
        }

        if( _locked_w ) {
            if (DB_LEVEL_LOCKING_ENABLED) {
                qlk.unlock_w();
            } else {
                qlk.unlock_W();
            }
        }
        if( _locked_W ) {
            qlk.unlock_W();
        }
        _weLocked = 0;
        _locked_W = _locked_w = false;
    }
    void Lock::DBRead::unlockDB() {
        if( _weLocked ) {
            recordTime();  // for lock stats
        
            if( _nested )
                lockState().unlockedNestable();
            else
                lockState().unlockedOther();

            _weLocked->unlock_shared();
        }

        if( _locked_r ) {
            if (DB_LEVEL_LOCKING_ENABLED) {
                qlk.unlock_r();
            } else {
                qlk.unlock_R();
            }
        }
        _weLocked = 0;
        _locked_r = false;
    }

    void Lock::DBWrite::lockTop(LockState& ls) { 
        switch( ls.threadState() ) { 
        case 'w':
            break;
        default:
            verify(false);
        case  0  : 
            qlk.lock_w();
            _locked_w = true;
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
            qlk.lock_r();
            _locked_r = true;
        }
    }

    void Lock::DBRead::lockOther(const StringData& db) {
        fassert( 16255, !db.empty() );
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

        if( db != ls.otherName() )
        {
            DBLocksMap::ref r(dblocks);
            WrapperForRWLock*& lock = r[db];
            if( lock == 0 )
                lock = new WrapperForRWLock(db);
            ls.lockedOther( db , -1 , lock );
        }
        else { 
            DEV OCCASIONALLY { dassert( dblocks.get(db) == ls.otherLock() ); }
            ls.lockedOther(-1);
        }
        fassert(16135,_weLocked==0);
        ls.otherLock()->lock_shared();
        _weLocked = ls.otherLock();
    }

    Lock::DBWrite::UpgradeToExclusive::UpgradeToExclusive() {
        fassert( 16187, lockState().threadState() == 'w' );

        // We're about to temporarily drop w, so stop the lock time stopwatch
        lockState().recordLockTime();

        _gotUpgrade = qlk.w_to_X();
        if ( _gotUpgrade ) {
            lockState().changeLockState('W');
            lockState().resetLockTime();
        }
    }

    Lock::DBWrite::UpgradeToExclusive::~UpgradeToExclusive() {
        if ( _gotUpgrade ) {
            fassert( 16188, lockState().threadState() == 'W' );
            lockState().recordLockTime();
            qlk.X_to_w();
            lockState().changeLockState('w');
        }
        else {
            fassert( 16189, lockState().threadState() == 'w' );
        }

        // Start recording lock time again
        lockState().resetLockTime();
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
    }
    void unlocking_w() {
        // we can't commit early in this case; so a bit more to do here.
        dur::releasingWriteLock();
    }
    void unlocking_W() {
        dur::releasingWriteLock();
    }

    class GlobalLockServerStatusSection : public ServerStatusSection {
    public:
        GlobalLockServerStatusSection() : ServerStatusSection( "globalLock" ){
            _started = curTimeMillis64();
        }

        virtual bool includeByDefault() const { return true; }

        virtual BSONObj generateSection( const BSONElement& configElement ) const {
            BSONObjBuilder t;

            t.append( "totalTime" , (long long)(1000 * ( curTimeMillis64() - _started ) ) );
            t.append( "lockTime" , Lock::globalLockStat()->getTimeLocked( 'W' ) );

            {
                BSONObjBuilder ttt( t.subobjStart( "currentQueue" ) );
                int w=0, r=0;
                Client::recommendedYieldMicros( &w , &r, true );
                ttt.append( "total" , w + r );
                ttt.append( "readers" , r );
                ttt.append( "writers" , w );
                ttt.done();
            }

            {
                BSONObjBuilder ttt( t.subobjStart( "activeClients" ) );
                int w=0, r=0;
                Client::getActiveClientCount( w , r );
                ttt.append( "total" , w + r );
                ttt.append( "readers" , r );
                ttt.append( "writers" , w );
                ttt.done();
            }

            return t.obj();
        }

    private:
        unsigned long long _started;

    } globalLockServerStatusSection;

    class LockStatsServerStatusSection : public ServerStatusSection {
    public:
        LockStatsServerStatusSection() : ServerStatusSection( "locks" ){}
        virtual bool includeByDefault() const { return true; }

        BSONObj generateSection( const BSONElement& configElement ) const {
            BSONObjBuilder b;
            b.append(".", qlk.stats.report());
            b.append("admin", nestableLocks[Lock::admin]->stats.report());
            b.append("local", nestableLocks[Lock::local]->stats.report());
            {
                DBLocksMap::ref r(dblocks);
                for( DBLocksMap::const_iterator i = r.r.begin(); i != r.r.end(); ++i ) {
                    b.append(i->first, i->second->stats.report());
                }
            }
            return b.obj();
        }

    } lockStatsServerStatusSection;

}
