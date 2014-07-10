// @file d_concurrency.cpp 

/**
*    Copyright (C) 2008-2014 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/d_concurrency.h"

#include "mongo/db/client.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/curop.h"
#include "mongo/db/d_globals.h"
#include "mongo/db/global_environment_experiment.h"
#include "mongo/db/lockstat.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/operation_context.h"
#include "mongo/server.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/mapsf.h"
#include "mongo/util/concurrency/qlock.h"
#include "mongo/util/concurrency/rwlock.h"
#include "mongo/util/concurrency/threadlocal.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/stacktrace.h"

// oplog locking
// no top level read locks
// system.profile writing
// oplog now
// yielding

namespace mongo {

    class DBTryLockTimeoutException : public std::exception {
    public:
        DBTryLockTimeoutException() {}
        virtual ~DBTryLockTimeoutException() throw() { }
    };

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
        return &nestableLocks[db]->getStats();
    }

    class WrapperForQLock { 
    public:
        QLock q;
        LockStat stats;

        void lock_R(LockState* lockState) {
            massert(16103,
                    str::stream() << "can't lock_R, threadState=" 
                                  << (int)lockState->threadState(),
                    lockState->threadState() == 0);
            lockState->lockedStart('R');
            q.lock_R(); 
        }

        void lock_W(LockState* lockState) {
            if (lockState->threadState()) {
                log() << "can't lock_W, threadState=" << (int)lockState->threadState() << endl;
                fassert(16114,false);
            }

            lockState->lockedStart('W');
            q.lock_W();
        }

        // how to count try's that fail is an interesting question. we should get rid of try().
        bool lock_R_try(LockState* lockState, int millis) {
            verify(lockState->threadState() == 0);
            bool got = q.lock_R_try(millis); 
            if (got) {
                lockState->lockedStart('R');
            }
            return got;
        }
        
        bool lock_W_try(LockState* lockState, int millis) {
            verify(lockState->threadState() == 0);
            bool got = q.lock_W_try(millis); 
            if( got ) {
                lockState->lockedStart('W');
            }
            return got;
        }

        void unlock_R(LockState* lockState) {
            wassert(lockState->threadState() == 'R');
            lockState->unlocked();
            q.unlock_R();
        }

        void unlock_W(LockState* lockState) {
            wassert(lockState->threadState() == 'W');
            lockState->unlocked();
            q.unlock_W(); 
        }

        // todo timing stats? : 
        void W_to_R()                        { q.W_to_R(); }
        void R_to_W()                        { q.R_to_W(); }
        bool w_to_X() { return q.w_to_X(); }
        void X_to_w() { q.X_to_w(); }
    };

    static WrapperForQLock& qlk = *new WrapperForQLock();
    LockStat* Lock::globalLockStat() {
        return &qlk.stats;
    }


    RWLockRecursive &Lock::ParallelBatchWriterMode::_batchLock = *(new RWLockRecursive("special"));
    void Lock::ParallelBatchWriterMode::iAmABatchParticipant(LockState* lockState) {
        lockState->_batchWriter = true;
    }

    Lock::ScopedLock::ParallelBatchWriterSupport::ParallelBatchWriterSupport(LockState* lockState)
        : _lockState(lockState) {
        relock();
    }

    void Lock::ScopedLock::ParallelBatchWriterSupport::tempRelease() {
        _lk.reset( 0 );
    }

    void Lock::ScopedLock::ParallelBatchWriterSupport::relock() {
        if (!_lockState->_batchWriter) {
            AcquiringParallelWriter a(*_lockState);
            _lk.reset( new RWLockRecursive::Shared(ParallelBatchWriterMode::_batchLock) );
        }
    }


    Lock::ScopedLock::ScopedLock(LockState* lockState, char type)
        : _lockState(lockState), _pbws_lk(lockState), _type(type), _stat(0) {

        _lockState->enterScopedLock(this);
    }

    Lock::ScopedLock::~ScopedLock() { 
        int prevCount = _lockState->recursiveCount();
        Lock::ScopedLock* what = _lockState->leaveScopedLock();
        fassert( 16171 , prevCount != 1 || what == this );
    }
    
    long long Lock::ScopedLock::acquireFinished( LockStat* stat ) {
        long long acquisitionTime = _timer.micros();
        _timer.reset();
        _stat = stat;

        // increment the operation level statistics
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
        resetTime();
        _relock();
    }

    Lock::TempRelease::TempRelease(LockState* lockState)
        : cant(lockState->isRecursive()), _lockState(lockState) {

        if( cant )
            return;

        fassert(16116, _lockState->recursiveCount() == 1);
        fassert(16117, _lockState->threadState() != 0);
        
        scopedLk = _lockState->leaveScopedLock();
        fassert(16118, scopedLk);

        invariant(_lockState == scopedLk->_lockState);

        scopedLk->tempRelease();
    }
    Lock::TempRelease::~TempRelease()
    {
        if( cant )
            return;
        
        fassert(16119, scopedLk);
        fassert(16120, _lockState->threadState() == 0);

        _lockState->enterScopedLock(scopedLk);
        scopedLk->relock();
    }

    void Lock::GlobalWrite::_tempRelease() { 
        fassert(16121, !noop);
        char ts = _lockState->threadState();
        fassert(16122, ts != 'R'); // indicates downgraded; not allowed with temprelease
        fassert(16123, ts == 'W');
        qlk.unlock_W(_lockState);
    }
    void Lock::GlobalWrite::_relock() { 
        fassert(16125, !noop);
        char ts = _lockState->threadState();
        fassert(16126, ts == 0);
        Acquiring a(this, *_lockState);
        qlk.lock_W(_lockState);
    }

    void Lock::GlobalRead::_tempRelease() { 
        fassert(16127, !noop);
        char ts = _lockState->threadState();
        fassert(16128, ts == 'R');
        qlk.unlock_R(_lockState);
    }
    void Lock::GlobalRead::_relock() { 
        fassert(16129, !noop);
        char ts = _lockState->threadState();
        fassert(16130, ts == 0);
        Acquiring a(this, *_lockState);
        qlk.lock_R(_lockState);
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

    Lock::GlobalWrite::GlobalWrite(LockState* lockState, int timeoutms)
        : ScopedLock(lockState, 'W') {

        char ts = _lockState->threadState();
        noop = false;
        if( ts == 'W' ) { 
            noop = true;
            return;
        }
        dassert( ts == 0 );

        Acquiring a(this, *_lockState);
        
        if ( timeoutms != -1 ) {
            bool success = qlk.lock_W_try(_lockState, timeoutms);
            if ( !success ) throw DBTryLockTimeoutException(); 
        }
        else {
            qlk.lock_W(_lockState);
        }
    }
    Lock::GlobalWrite::~GlobalWrite() {
        if( noop ) { 
            return;
        }
        recordTime();  // for lock stats
        if (_lockState->threadState() == 'R') { // we downgraded
            qlk.unlock_R(_lockState);
        }
        else {
            qlk.unlock_W(_lockState);
        }
    }
    void Lock::GlobalWrite::downgrade() { 
        verify( !noop );
        verify(_lockState->threadState() == 'W');

        qlk.W_to_R();
        _lockState->changeLockState('R');
    }

    // you will deadlock if 2 threads doing this
    void Lock::GlobalWrite::upgrade() { 
        verify( !noop );
        verify(_lockState->threadState() == 'R');

        qlk.R_to_W();
        _lockState->changeLockState('W');
    }

    Lock::GlobalRead::GlobalRead(LockState* lockState, int timeoutms)
        : ScopedLock(lockState, 'R') {

        char ts = _lockState->threadState();
        noop = false;
        if( ts == 'R' || ts == 'W' ) { 
            noop = true;
            return;
        }

        Acquiring a(this, *_lockState);

        if ( timeoutms != -1 ) {
            bool success = qlk.lock_R_try(_lockState, timeoutms);
            if ( !success ) throw DBTryLockTimeoutException(); 
        }
        else {
            // we are unlocked in the qlock/top sense.  lock_R will assert if we are in an in compatible state
            qlk.lock_R(_lockState); 
        }
    }
    Lock::GlobalRead::~GlobalRead() {
        if( !noop ) {
            recordTime();  // for lock stats
            qlk.unlock_R(_lockState);
        }
    }

    void Lock::DBWrite::lockNestable(Nestable db) { 
        _nested = true;

        if (_lockState->nestableCount()) {
            if( db != _lockState->whichNestable() ) { 
                error() << "can't lock local and admin db at the same time " << (int) db << ' ' << (int) _lockState->whichNestable() << endl;
                fassert(16131,false);
            }
            verify( _lockState->nestableCount() > 0 );
        }
        else {
            fassert(16132,_weLocked==0);
            _lockState->lockedNestable(db, 1);
            _weLocked = nestableLocks[db];
            _weLocked->lock();
        }
    }
    void Lock::DBRead::lockNestable(Nestable db) { 
        _nested = true;

        if (_lockState->nestableCount()) {
            // we are nested in our locking of local.  previous lock could be read OR write lock on local.
        }
        else {
            _lockState->lockedNestable(db, -1);
            fassert(16133,_weLocked==0);
            _weLocked = nestableLocks[db];
            _weLocked->lock_shared();
        }
    }

    void Lock::DBWrite::lockOtherRead(const StringData& db) {
        fassert(18517, !db.empty());

        // we do checks first, as on assert destructor won't be called so don't want to be half finished with our work.
        if( _lockState->otherCount() ) { 
            // nested. prev could be read or write. if/when we do temprelease with DBRead/DBWrite we will need to increment/decrement here
            // (so we can not release or assert if nested).  temprelease we should avoid if we can though, it's a bit of an anti-pattern.
            massert(18513,
                    str::stream() << "internal error tried to lock two databases at the same time. old:" 
                                  << _lockState->otherName() << " new:" << db,
                    db == _lockState->otherName());
            return;
        }

        // first lock for this db. check consistent order with local db lock so we never deadlock. local always comes last
        massert(18514,
                str::stream() << "can't dblock:" << db 
                              << " when local or admin is already locked",
                _lockState->nestableCount() == 0);

        if (db != _lockState->otherName()) {
            DBLocksMap::ref r(dblocks);
            WrapperForRWLock*& lock = r[db];
            if (lock == NULL) {
                lock = new WrapperForRWLock(db);
            }

            _lockState->lockedOther(db, -1, lock);
        }
        else { 
            DEV OCCASIONALLY{ dassert(dblocks.get(db) == _lockState->otherLock()); }
            _lockState->lockedOther(-1);
        }

        fassert(18515, _weLocked == 0);
        _lockState->otherLock()->lock_shared();
        _weLocked = _lockState->otherLock();
    }

    void Lock::DBWrite::lockOtherWrite(const StringData& db) {
        fassert(16252, !db.empty());

        // we do checks first, as on assert destructor won't be called so don't want to be half finished with our work.
        if (_lockState->otherCount()) {
            // nested. if/when we do temprelease with DBWrite we will need to increment here
            // (so we can not release or assert if nested).
            massert(16106,
                    str::stream() << "internal error tried to lock two databases at the same "
                                  << "time. old:" << _lockState->otherName() << " new:" << db,
                    db == _lockState->otherName());
            return;
        }

        // first lock for this db. check consistent order with local db lock so we never deadlock. local always comes last
        massert(16098,
                str::stream() << "can't dblock:" << db 
                              << " when local or admin is already locked",
                _lockState->nestableCount() == 0);

        if (db != _lockState->otherName()) {
            DBLocksMap::ref r(dblocks);
            WrapperForRWLock*& lock = r[db];
            if (lock == NULL) {
                lock = new WrapperForRWLock(db);
            }

            _lockState->lockedOther(db, 1, lock);
        }
        else { 
            DEV OCCASIONALLY{ dassert(dblocks.get(db) == _lockState->otherLock()); }
            _lockState->lockedOther(1);
        }
        
        fassert(16134,_weLocked==0);

        _lockState->otherLock()->lock();
        _weLocked = _lockState->otherLock();
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

        Acquiring a(this, *_lockState);
        _locked_W=false;
        _locked_w=false; 
        _weLocked=0;

        massert(16186, "can't get a DBWrite while having a read lock", !_lockState->hasAnyReadLock());
        if (_lockState->isW())
            return;

        StringData db = nsToDatabaseSubstring( ns );
        Nestable nested = n(db);
        if( nested == admin ) { 
            // we can't nestedly lock both admin and local as implemented. so lock_W.
            qlk.lock_W(_lockState);
            _locked_W = true;
            return;
        } 

        if (!nested) {
            if (_isIntentWrite) {
                lockOtherRead(db);
            }
            else {
                lockOtherWrite(db);
            }
        }

        lockTop();
        if( nested )
            lockNestable(nested);
    }

    void Lock::DBRead::lockDB(const string& ns) {
        fassert( 16254, !ns.empty() );

        Acquiring a(this, *_lockState);
        _locked_r=false; 
        _weLocked=0; 

        if (_lockState->isRW())
            return;

        StringData db = nsToDatabaseSubstring(ns);
        Nestable nested = n(db);
        if( !nested )
            lockOther(db);
        lockTop();
        if( nested )
            lockNestable(nested);
    }

    Lock::DBWrite::DBWrite(LockState* lockState, const StringData& ns, bool intentWrite)
        : ScopedLock(lockState, 'w'),
          _isIntentWrite(intentWrite),
          _what(ns.toString()),
          _nested(false) {
        lockDB(_what);
    }

    Lock::DBRead::DBRead(LockState* lockState, const StringData& ns)
        : ScopedLock(lockState, 'r' ), _what(ns.toString()), _nested(false) {
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
                _lockState->unlockedNestable();
            else
                _lockState->unlockedOther();
    
            _weLocked->unlock();
        }

        if( _locked_w ) {
            wassert(_lockState->threadState() == 'w');
            _lockState->unlocked();
            qlk.q.unlock_w();
        }

        if( _locked_W ) {
            qlk.unlock_W(_lockState);
        }

        _weLocked = 0;
        _locked_W = _locked_w = false;
    }
    void Lock::DBRead::unlockDB() {
        if( _weLocked ) {
            recordTime();  // for lock stats
        
            if( _nested )
                _lockState->unlockedNestable();
            else
                _lockState->unlockedOther();

            _weLocked->unlock_shared();
        }

        if( _locked_r ) {
            wassert(_lockState->threadState() == 'r');
            _lockState->unlocked();
            qlk.q.unlock_r();
        }
        _weLocked = 0;
        _locked_r = false;
    }

    void Lock::DBWrite::lockTop() { 
        switch (_lockState->threadState()) {
        case 'w':
            break;
        default:
            verify(false);
        case  0  : 
            verify(_lockState->threadState() == 0);
            _lockState->lockedStart('w');
            qlk.q.lock_w();
            _locked_w = true;
        }
    }
    void Lock::DBRead::lockTop() { 
        switch (_lockState->threadState()) {
        case 'r':
        case 'w':
            break;
        default:
            verify(false);
        case  0  : 
            verify(_lockState->threadState() == 0);
            _lockState->lockedStart('r');
            qlk.q.lock_r();
            _locked_r = true;
        }
    }

    void Lock::DBRead::lockOther(const StringData& db) {
        fassert( 16255, !db.empty() );

        // we do checks first, as on assert destructor won't be called so don't want to be half finished with our work.
        if( _lockState->otherCount() ) { 
            // nested. prev could be read or write. if/when we do temprelease with DBRead/DBWrite we will need to increment/decrement here
            // (so we can not release or assert if nested).  temprelease we should avoid if we can though, it's a bit of an anti-pattern.
            massert(16099,
                    str::stream() << "internal error tried to lock two databases at the same time. old:" 
                                  << _lockState->otherName() << " new:" << db,
                    db == _lockState->otherName());
            return;
        }

        // first lock for this db. check consistent order with local db lock so we never deadlock. local always comes last
        massert(16100, 
                str::stream() << "can't dblock:" << db 
                              << " when local or admin is already locked",
                _lockState->nestableCount() == 0);

        if (db != _lockState->otherName()) {
            DBLocksMap::ref r(dblocks);
            WrapperForRWLock*& lock = r[db];
            if (lock == NULL) {
                lock = new WrapperForRWLock(db);
            }

            _lockState->lockedOther(db, -1, lock);
        }
        else { 
            DEV OCCASIONALLY{ dassert(dblocks.get(db) == _lockState->otherLock()); }
            _lockState->lockedOther(-1);
        }

        fassert(16135,_weLocked==0);
        _lockState->otherLock()->lock_shared();
        _weLocked = _lockState->otherLock();
    }

    Lock::UpgradeGlobalLockToExclusive::UpgradeGlobalLockToExclusive(LockState* lockState)
            : _lockState(lockState) {
        fassert( 16187, _lockState->threadState() == 'w' );

        // We're about to temporarily drop w, so stop the lock time stopwatch
        _lockState->recordLockTime();

        _gotUpgrade = qlk.w_to_X();
        if ( _gotUpgrade ) {
            _lockState->changeLockState('W');
            _lockState->resetLockTime();
        }
    }

    Lock::UpgradeGlobalLockToExclusive::~UpgradeGlobalLockToExclusive() {
        if ( _gotUpgrade ) {
            fassert(16188, _lockState->threadState() == 'W');
            _lockState->recordLockTime();
            qlk.X_to_w();
            _lockState->changeLockState('w');
        }
        else {
            fassert(16189, _lockState->threadState() == 'w');
        }

        // Start recording lock time again
        _lockState->resetLockTime();
    }

    writelocktry::writelocktry(LockState* lockState, int tryms) :
        _got( false ),
        _dbwlock( NULL )
    { 
        try { 
            _dbwlock.reset(new Lock::GlobalWrite(lockState, tryms));
        }
        catch ( DBTryLockTimeoutException & ) {
            return;
        }
        _got = true;
    }

    writelocktry::~writelocktry() {

    }

    // note: the 'already' concept here might be a bad idea as a temprelease wouldn't notice it is nested then
    readlocktry::readlocktry(LockState* lockState, int tryms) :
        _got( false ),
        _dbrlock( NULL )
    {
        try { 
            _dbrlock.reset(new Lock::GlobalRead(lockState, tryms));
        }
        catch ( DBTryLockTimeoutException & ) {
            return;
        }
        _got = true;
    }

    readlocktry::~readlocktry() {

    }

    /**
     * This is passed to the iterator for global environments and aggregates information about the
     * locks which are currently being held or waited on.
     */
    class LockStateAggregator : public GlobalEnvironmentExperiment::ProcessOperationContext {
    public:
        LockStateAggregator(bool blockedOnly) 
            : numWriteLocked(0),
              numReadLocked(0),
              _blockedOnly(blockedOnly) {

        }

        virtual void processOpContext(OperationContext* txn) {
            if (_blockedOnly && !txn->lockState()->hasLockPending()) {
                return;
            }

            if (txn->lockState()->isWriteLocked()) {
                numWriteLocked++;
            }
            else {
                numReadLocked++;
            }
        }

        int numWriteLocked;
        int numReadLocked;

    private:
        const bool _blockedOnly;
    };


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

            // This returns the blocked lock states
            {
                BSONObjBuilder ttt( t.subobjStart( "currentQueue" ) );

                LockStateAggregator blocked(true);
                getGlobalEnvironment()->forEachOperationContext(&blocked);

                ttt.append("total", blocked.numReadLocked + blocked.numWriteLocked);
                ttt.append("readers", blocked.numReadLocked);
                ttt.append("writers", blocked.numWriteLocked);
                ttt.done();
            }

            // This returns all the active clients (including those holding locks)
            {
                BSONObjBuilder ttt( t.subobjStart( "activeClients" ) );

                LockStateAggregator active(false);
                getGlobalEnvironment()->forEachOperationContext(&active);

                ttt.append("total", active.numReadLocked + active.numWriteLocked);
                ttt.append("readers", active.numReadLocked);
                ttt.append("writers", active.numWriteLocked);
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
            b.append("admin", nestableLocks[Lock::admin]->getStats().report());
            b.append("local", nestableLocks[Lock::local]->getStats().report());
            {
                DBLocksMap::ref r(dblocks);
                for( DBLocksMap::const_iterator i = r.r.begin(); i != r.r.end(); ++i ) {
                    b.append(i->first, i->second->getStats().report());
                }
            }
            return b.obj();
        }

    } lockStatsServerStatusSection;
}
