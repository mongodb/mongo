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

        _lockState->lockGlobal(MODE_X);
        resetTime();
    }

    Lock::GlobalWrite::GlobalWrite(Locker* lockState, unsigned timeoutms)
        : ScopedLock(lockState, 'W') {

        LockResult result = _lockState->lockGlobal(MODE_X, timeoutms);
        if (result == LOCK_TIMEOUT) {
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

        LockResult result = _lockState->lockGlobal(MODE_S, timeoutms);
        if (result == LOCK_TIMEOUT) {
            throw DBTryLockTimeoutException();
        }

        resetTime();
    }

    Lock::GlobalRead::~GlobalRead() {
        _lockState->unlockAll();
        recordTime();
    }

    Lock::DBLock::DBLock(Locker* lockState, const StringData& db, const LockMode mode)
        : ScopedLock(lockState, mode == MODE_S || mode == MODE_IS ? 'r' : 'w'),
          _id(RESOURCE_DATABASE, db),
          _mode(mode) {
        dassert(!db.empty());
        dassert(!nsIsFull(db));
        lockDB();
    }

    Lock::DBLock::~DBLock() {
        unlockDB();
    }

    void Lock::DBLock::lockDB() {
        const bool isRead = (_mode == MODE_S || _mode == MODE_IS);

        _lockState->lockGlobal(isRead ? MODE_IS : MODE_IX);
        if (supportsDocLocking()) {
            //  SERVER-14668: Make this branch unconditional when MMAPv1 has coll. locking
            _lockState->lock(_id, _mode);
        }
        else {
            _lockState->lock(_id, isRead ? MODE_S : MODE_X);
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
                                         LockMode mode)
        : _id(RESOURCE_COLLECTION, ns),
          _lockState(lockState) {
        const bool isRead = (mode == MODE_S || mode == MODE_IS);
        dassert(!ns.empty());
        dassert(nsIsFull(ns));
        dassert(_lockState->isLockHeldForMode(ResourceId(RESOURCE_DATABASE,
                                                                nsToDatabaseSubstring(ns)),
                                              isRead ? MODE_IS : MODE_IX));
        if (supportsDocLocking()) {
            _lockState->lock(_id, mode);
            // SERVER-14668: add when MMAPv1 ready for collection-level locking
            // else { _lockState->lock(_id, isRead ? MODE_S : MODE_X); }
            invariant(isRead || !isRead); // artificial use to silence warning.
        }
    }

    Lock::CollectionLock::~CollectionLock() {
        if (supportsDocLocking()) {
            // SERVER-14668: Make unconditional when MMAPv1 has collection-level locking
            _lockState->unlock(_id);
        }
    }

    Lock::DBRead::DBRead(Locker* lockState, const StringData& dbOrNs) :
        DBLock(lockState, nsToDatabaseSubstring(dbOrNs), MODE_S) { }

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
