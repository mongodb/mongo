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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/concurrency/d_concurrency.h"

#include <string>
#include <vector>

#include "mongo/db/namespace_string.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
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

/**
 * ResourceMutexes can be constructed during initialization, thus the code must ensure the vector
 * of labels is constructed before items are added to it. This factory encapsulates all members
 * that need to be initialized before first use. A pointer is allocated to an instance of this
 * factory and the first call will construct an instance.
 */
class ResourceIdFactory {
public:
    static ResourceId newResourceIdForMutex(std::string resourceLabel) {
        ensureInitialized();
        return resourceIdFactory->_newResourceIdForMutex(std::move(resourceLabel));
    }

    static std::string nameForId(ResourceId resourceId) {
        stdx::lock_guard<stdx::mutex> lk(resourceIdFactory->labelsMutex);
        return resourceIdFactory->labels.at(resourceId.getHashId());
    }

    /**
     * Must be called in a single-threaded context (e.g: program initialization) before the factory
     * is safe to use in a multi-threaded context.
     */
    static void ensureInitialized() {
        if (!resourceIdFactory) {
            resourceIdFactory = new ResourceIdFactory();
        }
    }

private:
    ResourceId _newResourceIdForMutex(std::string resourceLabel) {
        stdx::lock_guard<stdx::mutex> lk(labelsMutex);
        invariant(nextId == labels.size());
        labels.push_back(std::move(resourceLabel));

        return ResourceId(RESOURCE_MUTEX, nextId++);
    }

    static ResourceIdFactory* resourceIdFactory;

    std::uint64_t nextId = 0;
    std::vector<std::string> labels;
    stdx::mutex labelsMutex;
};

ResourceIdFactory* ResourceIdFactory::resourceIdFactory;

/**
 * Guarantees `ResourceIdFactory::ensureInitialized` is called at least once during initialization.
 */
struct ResourceIdFactoryInitializer {
    ResourceIdFactoryInitializer() {
        ResourceIdFactory::ensureInitialized();
    }
} resourceIdFactoryInitializer;

}  // namespace


Lock::ResourceMutex::ResourceMutex(std::string resourceLabel)
    : _rid(ResourceIdFactory::newResourceIdForMutex(std::move(resourceLabel))) {}

std::string Lock::ResourceMutex::getName(ResourceId resourceId) {
    invariant(resourceId.getType() == RESOURCE_MUTEX);
    return ResourceIdFactory::nameForId(resourceId);
}

bool Lock::ResourceMutex::isExclusivelyLocked(Locker* locker) {
    return locker->isLockHeldForMode(_rid, MODE_X);
}

bool Lock::ResourceMutex::isAtLeastReadLocked(Locker* locker) {
    return locker->isLockHeldForMode(_rid, MODE_IS);
}

Lock::GlobalLock::GlobalLock(OperationContext* opCtx, LockMode lockMode, unsigned timeoutMs)
    : GlobalLock(opCtx, lockMode, timeoutMs, EnqueueOnly()) {
    waitForLock(timeoutMs);
}

Lock::GlobalLock::GlobalLock(OperationContext* opCtx,
                             LockMode lockMode,
                             unsigned timeoutMs,
                             EnqueueOnly enqueueOnly)
    : _opCtx(opCtx),
      _result(LOCK_INVALID),
      _pbwm(opCtx->lockState(), resourceIdParallelBatchWriterMode) {
    _enqueue(lockMode, timeoutMs);
}

void Lock::GlobalLock::_enqueue(LockMode lockMode, unsigned timeoutMs) {
    if (_opCtx->lockState()->shouldConflictWithSecondaryBatchApplication()) {
        _pbwm.lock(MODE_IS);
    }

    _result = _opCtx->lockState()->lockGlobalBegin(lockMode, Milliseconds(timeoutMs));
}

void Lock::GlobalLock::waitForLock(unsigned timeoutMs) {
    if (_result == LOCK_WAITING) {
        _result = _opCtx->lockState()->lockGlobalComplete(Milliseconds(timeoutMs));
    }

    if (_result != LOCK_OK && _opCtx->lockState()->shouldConflictWithSecondaryBatchApplication()) {
        _pbwm.unlock();
    }
}

void Lock::GlobalLock::_unlock() {
    if (isLocked()) {
        _opCtx->lockState()->unlockGlobal();
        _result = LOCK_INVALID;
    }
}


Lock::DBLock::DBLock(OperationContext* opCtx, StringData db, LockMode mode)
    : _id(RESOURCE_DATABASE, db),
      _opCtx(opCtx),
      _mode(mode),
      _globalLock(opCtx, isSharedLockMode(_mode) ? MODE_IS : MODE_IX, UINT_MAX) {
    massert(28539, "need a valid database name", !db.empty() && nsIsDbOnly(db));

    // Need to acquire the flush lock
    _opCtx->lockState()->lockMMAPV1Flush();

    // The check for the admin db is to ensure direct writes to auth collections
    // are serialized (see SERVER-16092).
    if ((_id == resourceIdAdminDB) && !isSharedLockMode(_mode)) {
        _mode = MODE_X;
    }

    invariant(LOCK_OK == _opCtx->lockState()->lock(_id, _mode));
}

Lock::DBLock::~DBLock() {
    _opCtx->lockState()->unlock(_id);
}

void Lock::DBLock::relockWithMode(LockMode newMode) {
    // 2PL would delay the unlocking
    invariant(!_opCtx->lockState()->inAWriteUnitOfWork());

    // Not allowed to change global intent
    invariant(!isSharedLockMode(_mode) || isSharedLockMode(newMode));

    _opCtx->lockState()->unlock(_id);
    _mode = newMode;

    invariant(LOCK_OK == _opCtx->lockState()->lock(_id, _mode));
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
    : _pbwm(lockState, resourceIdParallelBatchWriterMode, MODE_X),
      _lockState(lockState),
      _orginalShouldConflict(_lockState->shouldConflictWithSecondaryBatchApplication()) {
    _lockState->setShouldConflictWithSecondaryBatchApplication(false);
}

Lock::ParallelBatchWriterMode::~ParallelBatchWriterMode() {
    _lockState->setShouldConflictWithSecondaryBatchApplication(_orginalShouldConflict);
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

    Lock::ResourceLock heldUntilEndOfWUOW{lockState, resource, MODE_IX};
}

}  // namespace mongo
