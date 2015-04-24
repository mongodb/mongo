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

#include <string>

#include "mongo/db/service_context.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/server_parameters.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/stacktrace.h"

namespace mongo {
namespace {

    //  SERVER-14668: Remove or invert sense once MMAPv1 CLL can be default
    MONGO_EXPORT_STARTUP_SERVER_PARAMETER(enableCollectionLocking, bool, true);


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

} // namespace


    RWLockRecursive Lock::ParallelBatchWriterMode::_batchLock("special");


    Lock::TempRelease::TempRelease(Locker* lockState)
        : _lockState(lockState),
          _lockSnapshot(),
          _locksReleased(_lockState->saveLockStateAndUnlock(&_lockSnapshot)) {

    }

    Lock::TempRelease::~TempRelease() {
        if (_locksReleased) {
            invariant(!_lockState->isLocked());
            _lockState->restoreLockState(_lockSnapshot);
        }
    }


    void Lock::GlobalLock::_lock(LockMode lockMode, unsigned timeoutMs) {
        if (!_locker->isBatchWriter()) {
            AcquiringParallelWriter a(_locker);
            _pbws_lk.reset(new RWLockRecursive::Shared(ParallelBatchWriterMode::_batchLock));
        }

        _result = _locker->lockGlobalBegin(lockMode);
        if (_result == LOCK_WAITING) {
            _result = _locker->lockGlobalComplete(timeoutMs);
        }

        if (_result != LOCK_OK) {
            _pbws_lk.reset();
        }
    }

    void Lock::GlobalLock::_unlock() {
        if (isLocked()) {
            _locker->unlockAll();
            _pbws_lk.reset();
            _result = LOCK_INVALID;
        }
    }


    Lock::DBLock::DBLock(Locker* locker, StringData db, LockMode mode)
        : _id(RESOURCE_DATABASE, db),
          _locker(locker),
          _mode(mode),
          _globalLock(locker, isSharedLockMode(_mode) ? MODE_IS : MODE_IX, UINT_MAX) {

        massert(28539, "need a valid database name", !db.empty() && nsIsDbOnly(db));

        // Need to acquire the flush lock
        _locker->lockMMAPV1Flush();

        if (supportsDocLocking() || enableCollectionLocking) {
            // The check for the admin db is to ensure direct writes to auth collections
            // are serialized (see SERVER-16092).
            if ((_id == resourceIdAdminDB) && !isSharedLockMode(_mode)) {
                _mode = MODE_X;
            }

            invariant(LOCK_OK == _locker->lock(_id, _mode));
        }
        else {
            invariant(LOCK_OK == _locker->lock(_id, isSharedLockMode(_mode) ? MODE_S : MODE_X));
        }
    }

    Lock::DBLock::~DBLock() {
        _locker->unlock(_id);
    }

    void Lock::DBLock::relockWithMode(LockMode newMode) {
        // 2PL would delay the unlocking
        invariant(!_locker->inAWriteUnitOfWork());

        // Not allowed to change global intent
        invariant(!isSharedLockMode(_mode) || isSharedLockMode(newMode));

        _locker->unlock(_id);
        _mode = newMode;

        if (supportsDocLocking() || enableCollectionLocking) {
            invariant(LOCK_OK == _locker->lock(_id, _mode));
        }
        else {
            invariant(LOCK_OK == _locker->lock(_id, isSharedLockMode(_mode) ? MODE_S : MODE_X));
        }
    }


    Lock::CollectionLock::CollectionLock(Locker* lockState,
                                         StringData ns,
                                         LockMode mode)
        : _id(RESOURCE_COLLECTION, ns),
          _lockState(lockState) {

        massert(28538, "need a non-empty collection name", nsIsFull(ns));

        dassert(_lockState->isDbLockedForMode(nsToDatabaseSubstring(ns),
                                              isSharedLockMode(mode) ? MODE_IS : MODE_IX));
        if (supportsDocLocking()) {
            _lockState->lock(_id, mode);
        }
        else if (enableCollectionLocking) {
            _lockState->lock(_id, isSharedLockMode(mode) ? MODE_S : MODE_X);
        }
    }

    Lock::CollectionLock::~CollectionLock() {
        if (supportsDocLocking() || enableCollectionLocking) {
            _lockState->unlock(_id);
        }
    }

    void Lock::CollectionLock::relockAsDatabaseExclusive(Lock::DBLock& dbLock) {
        if (supportsDocLocking() || enableCollectionLocking) {
            _lockState->unlock(_id);
        }

        dbLock.relockWithMode(MODE_X);

        if (supportsDocLocking() || enableCollectionLocking) {
            // don't need the lock, but need something to unlock in the destructor
            _lockState->lock(_id, MODE_IX);
        }
    }

namespace {
    boost::mutex oplogSerialization; // for OplogIntentWriteLock
} // namespace

    Lock::OplogIntentWriteLock::OplogIntentWriteLock(Locker* lockState)
          : _lockState(lockState),
            _serialized(false) {
        _lockState->lock(resourceIdOplog, MODE_IX);
    }

    Lock::OplogIntentWriteLock::~OplogIntentWriteLock() {
        if (_serialized) {
            oplogSerialization.unlock();
        }
        _lockState->unlock(resourceIdOplog);
    }

    void Lock::OplogIntentWriteLock::serializeIfNeeded() {
        if (!supportsDocLocking() && !_serialized) {
            oplogSerialization.lock();
            _serialized = true;
        }
    }


    void Lock::ResourceLock::lock(LockMode mode) {
        invariant(_result == LOCK_INVALID);
        _result = _locker->lock(_rid, mode);
    }

    void Lock::ResourceLock::unlock() {
        if (_result == LOCK_OK) {
            _locker->unlock(_rid);
            _result = LOCK_INVALID;
        }
    }

} // namespace mongo
