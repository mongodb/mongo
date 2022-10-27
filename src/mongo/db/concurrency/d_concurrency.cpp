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

#include "mongo/db/concurrency/d_concurrency.h"

#include <memory>
#include <string>
#include <vector>

#include "mongo/db/concurrency/flow_control_ticketholder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
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
        stdx::lock_guard<Latch> lk(resourceIdFactory->labelsMutex);
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
        stdx::lock_guard<Latch> lk(labelsMutex);
        invariant(nextId == labels.size());
        labels.push_back(std::move(resourceLabel));

        return ResourceId(RESOURCE_MUTEX, nextId++);
    }

    static ResourceIdFactory* resourceIdFactory;

    std::uint64_t nextId = 0;
    std::vector<std::string> labels;
    Mutex labelsMutex = MONGO_MAKE_LATCH("ResourceIdFactory::labelsMutex");
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
    : GlobalLock(opCtx, lockMode, deadline, behavior, GlobalLockSkipOptions{}) {}

Lock::GlobalLock::GlobalLock(OperationContext* opCtx,
                             LockMode lockMode,
                             Date_t deadline,
                             InterruptBehavior behavior,
                             GlobalLockSkipOptions options)
    : _opCtx(opCtx),
      _interruptBehavior(behavior),
      _skipRSTLLock(options.skipRSTLLock),
      _isOutermostLock(!opCtx->lockState()->isLocked()) {
    if (!options.skipFlowControlTicket) {
        _opCtx->lockState()->getFlowControlTicket(_opCtx, lockMode);
    }

    try {
        if (_opCtx->lockState()->shouldConflictWithSecondaryBatchApplication()) {
            _pbwm.emplace(opCtx, resourceIdParallelBatchWriterMode, MODE_IS, deadline);
        }
        ScopeGuard unlockPBWM([this] {
            if (_opCtx->lockState()->shouldConflictWithSecondaryBatchApplication()) {
                _pbwm.reset();
            }
        });

        if (_opCtx->lockState()->shouldConflictWithSetFeatureCompatibilityVersion()) {
            _fcvLock.emplace(_opCtx,
                             resourceIdFeatureCompatibilityVersion,
                             isSharedLockMode(lockMode) ? MODE_IS : MODE_IX,
                             deadline);
        }
        ScopeGuard unlockFCVLock([this] {
            if (_opCtx->lockState()->shouldConflictWithSetFeatureCompatibilityVersion()) {
                _fcvLock.reset();
            }
        });

        _result = LOCK_INVALID;
        if (options.skipRSTLLock) {
            _takeGlobalLockOnly(lockMode, deadline);
        } else {
            _takeGlobalAndRSTLLocks(lockMode, deadline);
        }
        _result = LOCK_OK;

        unlockFCVLock.dismiss();
        unlockPBWM.dismiss();
    } catch (const DBException& ex) {
        // If our opCtx is interrupted or we got a LockTimeout or MaxTimeMSExpired, either throw or
        // suppress the exception depending on the specified interrupt behavior. For any other
        // exception, always throw.
        if ((opCtx->checkForInterruptNoAssert().isOK() && ex.code() != ErrorCodes::LockTimeout &&
             ex.code() != ErrorCodes::MaxTimeMSExpired) ||
            _interruptBehavior == InterruptBehavior::kThrow) {
            throw;
        }
    }
    auto acquiredLockMode = _opCtx->lockState()->getLockMode(resourceIdGlobal);
    _opCtx->lockState()->setGlobalLockTakenInMode(acquiredLockMode);
}

void Lock::GlobalLock::_takeGlobalLockOnly(LockMode lockMode, Date_t deadline) {
    _opCtx->lockState()->lockGlobal(_opCtx, lockMode, deadline);
}

void Lock::GlobalLock::_takeGlobalAndRSTLLocks(LockMode lockMode, Date_t deadline) {
    _opCtx->lockState()->lock(_opCtx, resourceIdReplicationStateTransitionLock, MODE_IX, deadline);
    ScopeGuard unlockRSTL(
        [this] { _opCtx->lockState()->unlock(resourceIdReplicationStateTransitionLock); });

    _opCtx->lockState()->lockGlobal(_opCtx, lockMode, deadline);

    unlockRSTL.dismiss();
}

Lock::GlobalLock::GlobalLock(GlobalLock&& otherLock)
    : _opCtx(otherLock._opCtx),
      _result(otherLock._result),
      _pbwm(std::move(otherLock._pbwm)),
      _fcvLock(std::move(otherLock._fcvLock)),
      _interruptBehavior(otherLock._interruptBehavior),
      _skipRSTLLock(otherLock._skipRSTLLock),
      _isOutermostLock(otherLock._isOutermostLock) {
    // Mark as moved so the destructor doesn't invalidate the newly-constructed lock.
    otherLock._result = LOCK_INVALID;
}

void Lock::GlobalLock::_unlock() {
    _opCtx->lockState()->unlockGlobal();
    _result = LOCK_INVALID;
}

Lock::DBLock::DBLock(OperationContext* opCtx,
                     const DatabaseName& dbName,
                     LockMode mode,
                     Date_t deadline)
    : DBLock(opCtx, dbName, mode, deadline, DBLockSkipOptions{}) {}

Lock::DBLock::DBLock(OperationContext* opCtx,
                     const DatabaseName& dbName,
                     LockMode mode,
                     Date_t deadline,
                     DBLockSkipOptions options)
    : _id(RESOURCE_DATABASE, dbName), _opCtx(opCtx), _result(LOCK_INVALID), _mode(mode) {

    _globalLock.emplace(opCtx,
                        isSharedLockMode(_mode) ? MODE_IS : MODE_IX,
                        deadline,
                        InterruptBehavior::kThrow,
                        std::move(options));

    massert(28539, "need a valid database name", !dbName.db().empty());

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

Lock::CollectionLock::CollectionLock(OperationContext* opCtx,
                                     const NamespaceString& ns,
                                     LockMode mode,
                                     Date_t deadline)
    : _id(RESOURCE_COLLECTION, ns), _opCtx(opCtx) {
    invariant(!ns.coll().empty());
    dassert(_opCtx->lockState()->isDbLockedForMode(ns.dbName(),
                                                   isSharedLockMode(mode) ? MODE_IS : MODE_IX));

    _opCtx->lockState()->lock(_opCtx, _id, mode, deadline);
}

Lock::CollectionLock::CollectionLock(CollectionLock&& otherLock)
    : _id(otherLock._id), _opCtx(otherLock._opCtx) {
    otherLock._opCtx = nullptr;
}

Lock::CollectionLock::~CollectionLock() {
    if (_opCtx)
        _opCtx->lockState()->unlock(_id);
}

Lock::ParallelBatchWriterMode::ParallelBatchWriterMode(OperationContext* opCtx)
    : _pbwm(opCtx, resourceIdParallelBatchWriterMode, MODE_X),
      _shouldNotConflictBlock(opCtx->lockState()) {}

void Lock::ResourceLock::_lock(LockMode mode, Date_t deadline) {
    invariant(_result == LOCK_INVALID);
    if (_opCtx)
        _opCtx->lockState()->lock(_opCtx, _rid, mode, deadline);
    else
        _locker->lock(_rid, mode, deadline);

    _result = LOCK_OK;
}

void Lock::ResourceLock::_unlock() {
    if (_isLocked()) {
        if (_opCtx)
            _opCtx->lockState()->unlock(_rid);
        else
            _locker->unlock(_rid);

        _result = LOCK_INVALID;
    }
}

}  // namespace mongo
