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

#include "mongo/db/concurrency/d_concurrency.h"

#include "mongo/db/client.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/curop.h"
#include "mongo/db/global_environment_experiment.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/concurrency/lock_stat.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/qlock.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/stacktrace.h"

// oplog locking
// no top level read locks
// system.profile writing
// oplog now
// yielding

namespace mongo {

    DBTryLockTimeoutException::DBTryLockTimeoutException() {}
    DBTryLockTimeoutException::~DBTryLockTimeoutException() throw() { }

    class WrapperForQLock { 
    public:
        QLock q;
        LockStat stats;

        void lock_R(Locker* lockState) {
            invariant(lockState->threadState() == 0);
            lockState->lockedStart('R');
            q.lock_R(); 
        }

        void lock_W(Locker* lockState) {
            if (lockState->threadState()) {
                log() << "can't lock_W, threadState=" << (int)lockState->threadState() << endl;
                fassert(16114,false);
            }

            lockState->lockedStart('W');
            q.lock_W();
        }

        // how to count try's that fail is an interesting question. we should get rid of try().
        bool lock_R_try(Locker* lockState, int millis) {
            verify(lockState->threadState() == 0);
            bool got = q.lock_R_try(millis); 
            if (got) {
                lockState->lockedStart('R');
            }
            return got;
        }
        
        bool lock_W_try(Locker* lockState, int millis) {
            verify(lockState->threadState() == 0);
            bool got = q.lock_W_try(millis); 
            if( got ) {
                lockState->lockedStart('W');
            }
            return got;
        }

        void unlock_R(Locker* lockState) {
            wassert(lockState->threadState() == 'R');
            lockState->unlocked();
            q.unlock_R();
        }

        void unlock_W(Locker* lockState) {
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

    /**
     * SERVER-14978: This class is temporary and is used to aggregate the per-operation lock
     * acquisition times. It will go away once we have figured out the lock stats reporting story.
     */
    class TrackLockAcquireTime {
        MONGO_DISALLOW_COPYING(TrackLockAcquireTime);
    public:
        TrackLockAcquireTime(char type) : _type(type) {

        }

        ~TrackLockAcquireTime() {
            if (haveClient()) {
                cc().curop()->lockStat().recordAcquireTimeMicros(_type, _timer.micros());
            }
        }

    private:
        char _type;
        Timer _timer;
    };


    class AcquiringParallelWriter {
    public:

        AcquiringParallelWriter(Locker* ls)
            : _ls(ls) {

            _ls->setLockPendingParallelWriter(true);
        }

        ~AcquiringParallelWriter() {
            _ls->setLockPendingParallelWriter(false);
        }

    private:
        Locker* const _ls;
    };


    RWLockRecursive &Lock::ParallelBatchWriterMode::_batchLock = *(new RWLockRecursive("special"));
    void Lock::ParallelBatchWriterMode::iAmABatchParticipant(Locker* lockState) {
        lockState->setIsBatchWriter(true);
    }

    Lock::ScopedLock::ParallelBatchWriterSupport::ParallelBatchWriterSupport(Locker* lockState)
        : _lockState(lockState) {
        relock();
    }

    void Lock::ScopedLock::ParallelBatchWriterSupport::tempRelease() {
        _lk.reset( 0 );
    }

    void Lock::ScopedLock::ParallelBatchWriterSupport::relock() {
        if (!_lockState->isBatchWriter()) {
            AcquiringParallelWriter a(_lockState);
            _lk.reset( new RWLockRecursive::Shared(ParallelBatchWriterMode::_batchLock) );
        }
    }


    Lock::ScopedLock::ScopedLock(Locker* lockState, char type)
        : _lockState(lockState), _pbws_lk(lockState), _type(type) {

        _lockState->enterScopedLock(this);
    }

    Lock::ScopedLock::~ScopedLock() { 
        _lockState->leaveScopedLock(this);
    }

    void Lock::ScopedLock::tempRelease() {
        _tempRelease();
        _pbws_lk.tempRelease();
    }

    void Lock::ScopedLock::relock() {
        _pbws_lk.relock();
        _relock();
    }

    void Lock::ScopedLock::resetTime() {
        _timer.reset();
    }

    void Lock::ScopedLock::recordTime() {
        if (haveClient()) {
            cc().curop()->lockStat().recordLockTimeMicros(_type, _timer.micros());
        }
    }

    Lock::TempRelease::TempRelease(Locker* lockState)
        : cant(lockState->isRecursive()), _lockState(lockState) {

        if (cant) {
            return;
        }

        fassert(16116, _lockState->recursiveCount() == 1);
        fassert(16117, _lockState->threadState() != 0);
        
        scopedLk = _lockState->getCurrentScopedLock();
        fassert(16118, scopedLk);

        invariant(_lockState == scopedLk->_lockState);

        scopedLk->tempRelease();
        _lockState->leaveScopedLock(scopedLk);
    }

    Lock::TempRelease::~TempRelease() {
        if (cant) {
            return;
        }
        
        fassert(16119, scopedLk);
        fassert(16120, _lockState->threadState() == 0);

        _lockState->enterScopedLock(scopedLk);
        scopedLk->relock();
    }

    void Lock::GlobalWrite::_tempRelease() { 
        fassert(16121, !noop);
        char ts = _lockState->threadState();
        fassert(16123, ts == 'W');
        qlk.unlock_W(_lockState);
        recordTime();
    }
    void Lock::GlobalWrite::_relock() { 
        fassert(16125, !noop);
        char ts = _lockState->threadState();
        fassert(16126, ts == 0);

        TrackLockAcquireTime a('W');
        qlk.lock_W(_lockState);
        resetTime();
    }

    void Lock::GlobalRead::_tempRelease() { 
        fassert(16127, !noop);
        char ts = _lockState->threadState();
        fassert(16128, ts == 'R');
        qlk.unlock_R(_lockState);
        recordTime();
    }

    void Lock::GlobalRead::_relock() { 
        fassert(16129, !noop);
        char ts = _lockState->threadState();
        fassert(16130, ts == 0);

        TrackLockAcquireTime a('R');
        qlk.lock_R(_lockState);
        resetTime();
    }

    void Lock::DBWrite::_tempRelease() { 
        unlockDB();
    }
    void Lock::DBWrite::_relock() { 
        lockDB();
    }
    void Lock::DBRead::_tempRelease() {
        unlockDB();
    }
    void Lock::DBRead::_relock() { 
        lockDB();
    }

    Lock::GlobalWrite::GlobalWrite(Locker* lockState, int timeoutms)
        : ScopedLock(lockState, 'W') {

        char ts = _lockState->threadState();
        noop = false;
        if( ts == 'W' ) { 
            noop = true;
            return;
        }
        dassert( ts == 0 );

        TrackLockAcquireTime a('W');
        
        if ( timeoutms != -1 ) {
            bool success = qlk.lock_W_try(_lockState, timeoutms);
            if ( !success ) throw DBTryLockTimeoutException(); 
        }
        else {
            qlk.lock_W(_lockState);
        }

        resetTime();
    }
    Lock::GlobalWrite::~GlobalWrite() {
        if( noop ) { 
            return;
        }

        if (_lockState->threadState() == 'R') { // we downgraded
            qlk.unlock_R(_lockState);
        }
        else {
            qlk.unlock_W(_lockState);
        }

        recordTime();
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

    Lock::GlobalRead::GlobalRead(Locker* lockState, int timeoutms)
        : ScopedLock(lockState, 'R') {

        char ts = _lockState->threadState();
        noop = false;
        if( ts == 'R' || ts == 'W' ) { 
            noop = true;
            return;
        }

        TrackLockAcquireTime a('R');

        if ( timeoutms != -1 ) {
            bool success = qlk.lock_R_try(_lockState, timeoutms);
            if ( !success ) throw DBTryLockTimeoutException(); 
        }
        else {
            // we are unlocked in the qlock/top sense.  lock_R will assert if we are in an in compatible state
            qlk.lock_R(_lockState); 
        }

        resetTime();
    }

    Lock::GlobalRead::~GlobalRead() {
        if( !noop ) {
            qlk.unlock_R(_lockState);
            recordTime();
        }
    }

    static bool isLocalOrAdmin(const string& dbName) {
        return (dbName == "local" || dbName == "admin");
    }


    Lock::DBWrite::DBWrite(Locker* lockState, const StringData& dbOrNs)
        : ScopedLock(lockState, 'w'),
          _lockAcquired(false),
          _ns(dbOrNs.toString()) {

        fassert(16253, !_ns.empty());
        lockDB();
    }

    Lock::DBWrite::~DBWrite() {
        unlockDB();
    }

    void Lock::DBWrite::lockTop() {
        switch (_lockState->threadState()) {
        case 0:
            _lockState->lockedStart('w');
            qlk.q.lock_w();
            break;
        case 'w':
            break;
        default:
            invariant(!"Invalid thread state");
        }
    }

    void Lock::DBWrite::lockDB() {
        if (_lockState->isW()) {
            return;
        }

        TrackLockAcquireTime a('w');

        const StringData db = nsToDatabaseSubstring(_ns);
        const newlm::ResourceId resIdDb(newlm::RESOURCE_DATABASE, db);

        // As weird as it looks, currently the top (global 'w' lock) is acquired after the DB lock
        // has been acquired in order to avoid deadlock with UpgradeGlobalLockToExclusive, which is
        // called while holding some form of write lock on the database.
        //
        // However, for the local/admin database, since its lock is acquired usually after another
        // DB lock is already held, the nested database's lock (local or admin) needs to be
        // acquired after the global lock. Consider the following sequence:
        // 
        // T1: Acquires DBWrite on 'other' DB, then 'w'
        // T2: Wants to flush so it queues behind T1 for 'R' lock
        // T3: Acquires DBRead on 'local' DB, then blocks on 'r' (T2 has precedence, due to 'R')
        // T1: Does whatever writes it does and tries to acquire DBWrite on 'local' in order to
        //     insert in the OpLog, and blocks because of T3's 'r' lock on local.
        //
        // This is a deadlock T1 -> T3 -> T2 -> T1.
        //
        // Moving the acquisition of local or admin's lock to be after the global lock solves this
        // problem, which would otherwise break replication.
        //
        // TODO: This whole lock ordering mess will go away once we make flush be it's own lock
        // instead of doing upgrades on the global lock.
        if (isLocalOrAdmin(db.toString())) {
            lockTop();
        }

        _lockState->lock(resIdDb, newlm::MODE_X);

        if (!isLocalOrAdmin(db.toString())) {
            lockTop();
        }

        _lockAcquired = true;

        resetTime();
    }

    void Lock::DBWrite::unlockDB() {
        if (_lockAcquired) {
            const StringData db = nsToDatabaseSubstring(_ns);
            const newlm::ResourceId resIdDb(newlm::RESOURCE_DATABASE, db);

            _lockState->unlock(resIdDb);

            // The last one frees the Global lock
            if (_lockState->recursiveCount() == 1) {
                invariant(_lockState->threadState() == 'w');
                qlk.q.unlock_w();
                _lockState->unlocked();
                recordTime();
            }

            _lockAcquired = false;
        }
    }


    Lock::DBRead::DBRead(Locker* lockState, const StringData& dbOrNs)
        : ScopedLock(lockState, 'r'),
          _lockAcquired(false),
          _ns(dbOrNs.toString()) {

        fassert(16254, !_ns.empty());
        lockDB();
    }

    Lock::DBRead::~DBRead() {
        unlockDB();
    }

    void Lock::DBRead::lockTop() {
        switch (_lockState->threadState()) {
        case 0:
            _lockState->lockedStart('r');
            qlk.q.lock_r();
            break;
        case 'r':
        case 'w':
            break;
        default:
            invariant(false);
        }
    }

    void Lock::DBRead::lockDB() {
        if (_lockState->isRW()) {
            return;
        }

        TrackLockAcquireTime a('r');

        const StringData db = nsToDatabaseSubstring(_ns);
        const newlm::ResourceId resIdDb(newlm::RESOURCE_DATABASE, db);

        // Keep the order of acquisition of the 'r' lock the same as it is in DBWrite in order to
        // avoid deadlocks. See the comment inside DBWrite::lockDB for more information on the
        // reason for the weird ordering of locks.
        if (isLocalOrAdmin(db.toString())) {
            lockTop();
        }

        _lockState->lock(resIdDb, newlm::MODE_S);

        if (!isLocalOrAdmin(db.toString())) {
            lockTop();
        }

        _lockAcquired = true;
        resetTime();
    }

    void Lock::DBRead::unlockDB() {
        if (_lockAcquired) {
            const StringData db = nsToDatabaseSubstring(_ns);
            const newlm::ResourceId resIdDb(newlm::RESOURCE_DATABASE, db);

            _lockState->unlock(resIdDb);
            
            // The last one frees the Global lock
            if (_lockState->recursiveCount() == 1) {
                invariant(_lockState->threadState() == 'r');
                qlk.q.unlock_r();
                _lockState->unlocked();
                recordTime();
            }

            _lockAcquired = false;
        }
    }


    Lock::UpgradeGlobalLockToExclusive::UpgradeGlobalLockToExclusive(Locker* lockState)
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

    writelocktry::writelocktry(Locker* lockState, int tryms) :
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
    readlocktry::readlocktry(Locker* lockState, int tryms) :
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
    class LockerAggregator : public GlobalEnvironmentExperiment::ProcessOperationContext {
    public:
        LockerAggregator(bool blockedOnly) 
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
            t.append( "lockTime" , qlk.stats.getTimeLocked( 'W' ) );

            // This returns the blocked lock states
            {
                BSONObjBuilder ttt( t.subobjStart( "currentQueue" ) );

                LockerAggregator blocked(true);
                getGlobalEnvironment()->forEachOperationContext(&blocked);

                ttt.append("total", blocked.numReadLocked + blocked.numWriteLocked);
                ttt.append("readers", blocked.numReadLocked);
                ttt.append("writers", blocked.numWriteLocked);
                ttt.done();
            }

            // This returns all the active clients (including those holding locks)
            {
                BSONObjBuilder ttt( t.subobjStart( "activeClients" ) );

                LockerAggregator active(false);
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

            // TODO: Add per-db lock information here

            return b.obj();
        }

    } lockStatsServerStatusSection;
}
