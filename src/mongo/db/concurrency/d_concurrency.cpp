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

#include "mongo/db/namespace_string.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/stacktrace.h"

namespace mongo {

Lock::TempRelease::TempRelease(Locker* lockState)
    : _lockState(lockState),
      _lockSnapshot(),
      _locksReleased(_lockState->saveLockStateAndUnlock(&_lockSnapshot)) {}

Lock::TempRelease::~TempRelease() {
    if (_locksReleased) {
        invariant(!_lockState->isLocked());
        _lockState->restoreLockState(_lockSnapshot);
    }
}

namespace {
AtomicWord<uint64_t> lastResourceMutexHash{0};
}  // namespace

Lock::ResourceMutex::ResourceMutex() : _rid(RESOURCE_MUTEX, lastResourceMutexHash.fetchAndAdd(1)) {}

Lock::GlobalLock::GlobalLock(Locker* locker)
    : _locker(locker), _result(LOCK_INVALID), _pbwm(locker, resourceIdParallelBatchWriterMode) {}

Lock::GlobalLock::GlobalLock(Locker* locker, LockMode lockMode, unsigned timeoutMs)
    : GlobalLock(locker, lockMode, EnqueueOnly()) {
    waitForLock(timeoutMs);
}

Lock::GlobalLock::GlobalLock(Locker* locker, LockMode lockMode, EnqueueOnly enqueueOnly)
    : _locker(locker), _result(LOCK_INVALID), _pbwm(locker, resourceIdParallelBatchWriterMode) {
    _enqueue(lockMode);
}

void Lock::GlobalLock::_enqueue(LockMode lockMode) {
    if (!_locker->isBatchWriter()) {
        _pbwm.lock(MODE_IS);
    }

    _result = _locker->lockGlobalBegin(lockMode);
}

void Lock::GlobalLock::waitForLock(unsigned timeoutMs) {
    if (_result == LOCK_WAITING) {
        _result = _locker->lockGlobalComplete(timeoutMs);
    }

    if (_result != LOCK_OK && !_locker->isBatchWriter()) {
        _pbwm.unlock();
    }
}

void Lock::GlobalLock::_unlock() {
    if (isLocked()) {
        _locker->unlockGlobal();
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

    // The check for the admin db is to ensure direct writes to auth collections
    // are serialized (see SERVER-16092).
    if ((_id == resourceIdAdminDB) && !isSharedLockMode(_mode)) {
        _mode = MODE_X;
    }

    invariant(LOCK_OK == _locker->lock(_id, _mode));
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

    invariant(LOCK_OK == _locker->lock(_id, _mode));
}


Lock::CollectionLock::CollectionLock(Locker* lockState, StringData ns, LockMode mode)
    : _id(RESOURCE_COLLECTION, ns), _lockState(lockState) {
    massert(28538, "need a non-empty collection name", nsIsFull(ns));

    dassert(_lockState->isDbLockedForMode(nsToDatabaseSubstring(ns),
                                          isSharedLockMode(mode) ? MODE_IS : MODE_IX));
    if (supportsDocLocking()) {
        _lockState->lock(_id, mode);
    } else {
        _lockState->lock(_id, isSharedLockMode(mode) ? MODE_S : MODE_X);
    }
}

Lock::CollectionLock::~CollectionLock() {
    _lockState->unlock(_id);
}

void Lock::CollectionLock::relockAsDatabaseExclusive(Lock::DBLock& dbLock) {
    _lockState->unlock(_id);

    dbLock.relockWithMode(MODE_X);

    // don't need the lock, but need something to unlock in the destructor
    _lockState->lock(_id, MODE_IX);
}

namespace {
stdx::mutex oplogSerialization;  // for OplogIntentWriteLock
}  // namespace

Lock::OplogIntentWriteLock::OplogIntentWriteLock(Locker* lockState)
    : _lockState(lockState), _serialized(false) {
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

Lock::ParallelBatchWriterMode::ParallelBatchWriterMode(Locker* lockState)
    : _pbwm(lockState, resourceIdParallelBatchWriterMode, MODE_X), _lockState(lockState) {
    invariant(!_lockState->isBatchWriter());  // Otherwise we couldn't clear in destructor.
    _lockState->setIsBatchWriter(true);
}

Lock::ParallelBatchWriterMode::~ParallelBatchWriterMode() {
    _lockState->setIsBatchWriter(false);
}

void Lock::ResourceLock::lock(LockMode mode) {
    invariant(_result == LOCK_INVALID);
    _result = _locker->lock(_rid, mode);
    invariant(_result == LOCK_OK);
}

void Lock::ResourceLock::unlock() {
    if (_result == LOCK_OK) {
        _locker->unlock(_rid);
        _result = LOCK_INVALID;
    }
}

void synchronizeOnCappedInFlightResource(Locker* lockState, const NamespaceString& cappedNs) {
    dassert(lockState->inAWriteUnitOfWork());
    const ResourceId resource = cappedNs.db() == "local" ? resourceCappedInFlightForLocalDb
                                                         : resourceCappedInFlightForOtherDb;

    // It is illegal to acquire the capped in-flight lock for non-local dbs while holding the
    // capped in-flight lock for the local db. (Unless we already hold the otherDb lock since
    // reacquiring a lock in the same mode never blocks.)
    if (resource == resourceCappedInFlightForOtherDb) {
        dassert(!lockState->isLockHeldForMode(resourceCappedInFlightForLocalDb, MODE_IX) ||
                lockState->isLockHeldForMode(resourceCappedInFlightForOtherDb, MODE_IX));
    }

    Lock::ResourceLock{lockState, resource, MODE_IX};  // held until end of WUOW.
}

}  // namespace mongo
