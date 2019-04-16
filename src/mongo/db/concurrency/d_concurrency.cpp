/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
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

#include "mongo/db/concurrency/flow_control_ticketholder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/str.h"

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

Lock::GlobalLock::GlobalLock(OperationContext* opCtx,
                             LockMode lockMode,
                             Date_t deadline,
                             InterruptBehavior behavior)
    : GlobalLock(opCtx, lockMode, deadline, behavior, EnqueueOnly()) {
    waitForLockUntil(deadline);
}

Lock::GlobalLock::GlobalLock(OperationContext* opCtx,
                             LockMode lockMode,
                             Date_t deadline,
                             InterruptBehavior behavior,
                             EnqueueOnly enqueueOnly)
    : _opCtx(opCtx),
      _result(LOCK_INVALID),
      _pbwm(opCtx->lockState(), resourceIdParallelBatchWriterMode),
      _interruptBehavior(behavior),
      _isOutermostLock(!opCtx->lockState()->isLocked()) {
    _enqueue(lockMode, deadline);
}

Lock::GlobalLock::GlobalLock(GlobalLock&& otherLock)
    : _opCtx(otherLock._opCtx),
      _result(otherLock._result),
      _pbwm(std::move(otherLock._pbwm)),
      _interruptBehavior(otherLock._interruptBehavior),
      _isOutermostLock(otherLock._isOutermostLock) {
    // Mark as moved so the destructor doesn't invalidate the newly-constructed lock.
    otherLock._result = LOCK_INVALID;
}

void Lock::GlobalLock::_enqueue(LockMode lockMode, Date_t deadline) {
    _opCtx->lockState()->getFlowControlTicket(_opCtx, lockMode);

    try {
        if (_opCtx->lockState()->shouldConflictWithSecondaryBatchApplication()) {
            _pbwm.lock(MODE_IS);
        }
        auto unlockPBWM = makeGuard([this] {
            if (_opCtx->lockState()->shouldConflictWithSecondaryBatchApplication()) {
                _pbwm.unlock();
            }
        });

        _opCtx->lockState()->lock(
            _opCtx, resourceIdReplicationStateTransitionLock, MODE_IX, deadline);

        auto unlockRSTL = makeGuard(
            [this] { _opCtx->lockState()->unlock(resourceIdReplicationStateTransitionLock); });

        _result = LOCK_INVALID;
        _result = _opCtx->lockState()->lockGlobalBegin(_opCtx, lockMode, deadline);

        unlockRSTL.dismiss();
        unlockPBWM.dismiss();
    } catch (const ExceptionForCat<ErrorCategory::Interruption>&) {
        // The kLeaveUnlocked behavior suppresses this exception.
        if (_interruptBehavior == InterruptBehavior::kThrow)
            throw;
    }
}

void Lock::GlobalLock::waitForLockUntil(Date_t deadline) {
    try {
        if (_result == LOCK_WAITING) {
            _result = LOCK_INVALID;
            _opCtx->lockState()->lockGlobalComplete(_opCtx, deadline);
            _result = LOCK_OK;
        }
    } catch (const ExceptionForCat<ErrorCategory::Interruption>&) {
        _opCtx->lockState()->unlock(resourceIdReplicationStateTransitionLock);
        if (_opCtx->lockState()->shouldConflictWithSecondaryBatchApplication()) {
            _pbwm.unlock();
        }
        // The kLeaveUnlocked behavior suppresses this exception.
        if (_interruptBehavior == InterruptBehavior::kThrow)
            throw;
    }

    const ResourceId globalResId(RESOURCE_GLOBAL, ResourceId::SINGLETON_GLOBAL);
    auto lockMode = _opCtx->lockState()->getLockMode(globalResId);
    _opCtx->lockState()->setGlobalLockTakenInMode(lockMode);
}

void Lock::GlobalLock::_unlock() {
    _opCtx->lockState()->unlockGlobal();
    _result = LOCK_INVALID;
}

Lock::DBLock::DBLock(OperationContext* opCtx, StringData db, LockMode mode, Date_t deadline)
    : _id(RESOURCE_DATABASE, db),
      _opCtx(opCtx),
      _result(LOCK_INVALID),
      _mode(mode),
      _globalLock(
          opCtx, isSharedLockMode(_mode) ? MODE_IS : MODE_IX, deadline, InterruptBehavior::kThrow) {
    massert(28539, "need a valid database name", !db.empty() && nsIsDbOnly(db));

    // The check for the admin db is to ensure direct writes to auth collections
    // are serialized (see SERVER-16092).
    if ((_id == resourceIdAdminDB) && !isSharedLockMode(_mode)) {
        _mode = MODE_X;
    }

    _opCtx->lockState()->lock(_opCtx, _id, _mode, deadline);
    _result = LOCK_OK;
}

Lock::DBLock::DBLock(DBLock&& otherLock)
    : _id(otherLock._id),
      _opCtx(otherLock._opCtx),
      _result(otherLock._result),
      _mode(otherLock._mode),
      _globalLock(std::move(otherLock._globalLock)) {
    // Mark as moved so the destructor doesn't invalidate the newly-constructed lock.
    otherLock._result = LOCK_INVALID;
}

Lock::DBLock::~DBLock() {
    if (isLocked()) {
        _opCtx->lockState()->unlock(_id);
    }
}

void Lock::DBLock::relockWithMode(LockMode newMode) {
    // 2PL would delay the unlocking
    invariant(!_opCtx->lockState()->inAWriteUnitOfWork());

    // Not allowed to change global intent, so check when going from shared to exclusive.
    if (isSharedLockMode(_mode) && !isSharedLockMode(newMode))
        invariant(_opCtx->lockState()->isWriteLocked());

    _opCtx->lockState()->unlock(_id);
    _mode = newMode;

    _opCtx->lockState()->lock(_opCtx, _id, _mode);
    _result = LOCK_OK;
}


Lock::CollectionLock::CollectionLock(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     LockMode mode,
                                     Date_t deadline)
    : _id(RESOURCE_COLLECTION, nss.ns()), _opCtx(opCtx) {
    invariant(nss.coll().size(), str::stream() << "expected non-empty collection name:" << nss);
    dassert(opCtx->lockState()->isDbLockedForMode(nss.db(),
                                                  isSharedLockMode(mode) ? MODE_IS : MODE_IX));
    LockMode actualLockMode = mode;
    if (!supportsDocLocking()) {
        actualLockMode = isSharedLockMode(mode) ? MODE_S : MODE_X;
    }
    _opCtx->lockState()->lock(_opCtx, _id, actualLockMode, deadline);
}

Lock::CollectionLock::CollectionLock(CollectionLock&& otherLock)
    : _id(otherLock._id), _opCtx(otherLock._opCtx) {
    otherLock._opCtx = nullptr;
}

Lock::CollectionLock::~CollectionLock() {
    if (_opCtx)
        _opCtx->lockState()->unlock(_id);
}

Lock::ParallelBatchWriterMode::ParallelBatchWriterMode(Locker* lockState)
    : _pbwm(lockState, resourceIdParallelBatchWriterMode, MODE_X),
      _shouldNotConflictBlock(lockState) {}

void Lock::ResourceLock::lock(LockMode mode) {
    invariant(_result == LOCK_INVALID);
    _locker->lock(_rid, mode);
    _result = LOCK_OK;
}

void Lock::ResourceLock::unlock() {
    if (_result == LOCK_OK) {
        _locker->unlock(_rid);
        _result = LOCK_INVALID;
    }
}

}  // namespace mongo
