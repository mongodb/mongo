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
#include "mongo/db/curop.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/concurrency/lock_stat.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/global_environment_experiment.h"
#include "mongo/db/namespace_string.h"
#include "mongo/util/assert_util.h"
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

    void Lock::ScopedLock::_tempRelease() {
        // TempRelease is only used for global locks
        invariant(false);
    }

    void Lock::ScopedLock::_relock() {
        // TempRelease is only used for global locks
        invariant(false);
    }

    Lock::TempRelease::TempRelease(Locker* lockState)
        : cant(lockState->isRecursive()), _lockState(lockState) {

        if (cant) {
            return;
        }

        fassert(16116, _lockState->recursiveCount() == 1);
        fassert(16117, _lockState->isLocked());
        
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
        fassert(16120, !_lockState->isLocked());

        _lockState->enterScopedLock(scopedLk);
        scopedLk->relock();
    }

    void Lock::GlobalWrite::_tempRelease() { 
        invariant(_lockState->isW());

        invariant(_lockState->unlockAll());
        recordTime();
    }
    void Lock::GlobalWrite::_relock() { 
        invariant(!_lockState->isLocked());

        TrackLockAcquireTime a('W');
        _lockState->lockGlobal(newlm::MODE_X);
        resetTime();
    }

    Lock::GlobalWrite::GlobalWrite(Locker* lockState, unsigned timeoutms)
        : ScopedLock(lockState, 'W') {

        TrackLockAcquireTime a('W');

        newlm::LockResult result = _lockState->lockGlobal(newlm::MODE_X, timeoutms);
        if (result == newlm::LOCK_TIMEOUT) {
            throw DBTryLockTimeoutException();
        }

        resetTime();
    }

    Lock::GlobalWrite::~GlobalWrite() {
        // If the lock state is R, this means downgrade happened and this is only for fsyncLock.
        invariant(_lockState->isW() || _lockState->isR());

        _lockState->unlockAll();
        recordTime();
    }

    Lock::GlobalRead::GlobalRead(Locker* lockState, unsigned timeoutms)
        : ScopedLock(lockState, 'R') {

        TrackLockAcquireTime a('R');

        newlm::LockResult result = _lockState->lockGlobal(newlm::MODE_S, timeoutms);
        if (result == newlm::LOCK_TIMEOUT) {
            throw DBTryLockTimeoutException();
        }

        resetTime();
    }

    Lock::GlobalRead::~GlobalRead() {
        _lockState->unlockAll();
        recordTime();
    }

    Lock::DBLock::DBLock(Locker* lockState, const StringData& db, const newlm::LockMode mode)
        : ScopedLock(lockState, mode == newlm::MODE_S || mode == newlm::MODE_IS ? 'r' : 'w'),
          _id(newlm::RESOURCE_DATABASE, db),
          _mode(mode) {
        dassert(!db.empty());
        dassert(!nsIsFull(db));
        lockDB();
    }

    Lock::DBLock::~DBLock() {
        unlockDB();
    }

    void Lock::DBLock::lockDB() {
        const bool isRead = (_mode == newlm::MODE_S || _mode == newlm::MODE_IS);
        TrackLockAcquireTime a(isRead ? 'r' : 'w');

        _lockState->lockGlobal(isRead ? newlm::MODE_IS : newlm::MODE_IX);

        if (supportsDocLocking() || isRead) {
            _lockState->lock(_id, _mode);
        }
        else {
            _lockState->lock(_id, newlm::MODE_X);
        }

        resetTime();
    }

    void Lock::DBLock::unlockDB() {
        _lockState->unlock(_id);

        // The last release reports time the lock was held
        if (_lockState->unlockAll()) {
            recordTime();
        }
    }

    Lock::CollectionLock::CollectionLock(Locker* lockState,
                                         const StringData& ns,
                                         newlm::LockMode mode)
        : _id(newlm::RESOURCE_COLLECTION, ns),
          _lockState(lockState) {
        const bool isRead = (mode == newlm::MODE_S || mode == newlm::MODE_IS);
        dassert(!ns.empty());
        dassert(nsIsFull(ns));
        dassert(_lockState->isLockHeldForMode(newlm::ResourceId(newlm::RESOURCE_DATABASE,
                                                                nsToDatabaseSubstring(ns)),
                                              isRead ? newlm::MODE_IS : newlm::MODE_IX));
        if (supportsDocLocking()) {
            _lockState->lock(_id, mode);
        }
        else {
            _lockState->lock(_id, isRead ? newlm::MODE_S : newlm::MODE_X);
        }
    }

    Lock::CollectionLock::~CollectionLock() {
        _lockState->unlock(_id);
    }

    Lock::DBWrite::DBWrite(Locker* lockState, const StringData& dbOrNs) :
        DBLock(lockState, nsToDatabaseSubstring(dbOrNs), newlm::MODE_X) { }

    Lock::DBRead::DBRead(Locker* lockState, const StringData& dbOrNs) :
        DBLock(lockState, nsToDatabaseSubstring(dbOrNs), newlm::MODE_S) { }

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

}
