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


#include "mongo/platform/basic.h"

#include "mongo/db/concurrency/d_concurrency.h"

#include <memory>
#include <string>
#include <vector>

#include "mongo/db/catalog/collection_catalog.h"
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
                             InterruptBehavior behavior,
                             bool skipRSTLLock)
    : _opCtx(opCtx),
      _result(LOCK_INVALID),
      _pbwm(opCtx->lockState(), resourceIdParallelBatchWriterMode),
      _fcvLock(opCtx->lockState(), resourceIdFeatureCompatibilityVersion),
      _interruptBehavior(behavior),
      _skipRSTLLock(skipRSTLLock),
      _isOutermostLock(!opCtx->lockState()->isLocked()) {
    _opCtx->lockState()->getFlowControlTicket(_opCtx, lockMode);

    try {
        if (_opCtx->lockState()->shouldConflictWithSecondaryBatchApplication()) {
            _pbwm.lock(opCtx, MODE_IS, deadline);
        }
        ScopeGuard unlockPBWM([this] {
            if (_opCtx->lockState()->shouldConflictWithSecondaryBatchApplication()) {
                _pbwm.unlock();
            }
        });

        if (_opCtx->lockState()->shouldConflictWithSetFeatureCompatibilityVersion()) {
            _fcvLock.lock(_opCtx, isSharedLockMode(lockMode) ? MODE_IS : MODE_IX, deadline);
        }
        ScopeGuard unlockFCVLock([this] {
            if (_opCtx->lockState()->shouldConflictWithSetFeatureCompatibilityVersion()) {
                _fcvLock.unlock();
            }
        });

        _result = LOCK_INVALID;
        if (skipRSTLLock) {
            _takeGlobalLockOnly(lockMode, deadline);
        } else {
            _takeGlobalAndRSTLLocks(lockMode, deadline);
        }
        _result = LOCK_OK;

        unlockFCVLock.dismiss();
        unlockPBWM.dismiss();
    } catch (const DBException& ex) {
        // If our opCtx was killed or we got a LockTimeout or MaxTimeMSExpired, either throw or
        // suppress the exception depending on the specified interrupt behavior. For any other
        // exception, always throw.
        if ((!opCtx->isKillPending() && ex.code() != ErrorCodes::LockTimeout &&
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
                     Date_t deadline,
                     bool skipGlobalAndRSTLLocks)
    : _id(RESOURCE_DATABASE, dbName), _opCtx(opCtx), _result(LOCK_INVALID), _mode(mode) {

    if (!skipGlobalAndRSTLLocks) {
        _globalLock.emplace(opCtx,
                            isSharedLockMode(_mode) ? MODE_IS : MODE_IX,
                            deadline,
                            InterruptBehavior::kThrow);
    }
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

void Lock::DBLock::relockWithMode(LockMode newMode) {
    // 2PL would delay the unlocking
    invariant(!_opCtx->lockState()->inAWriteUnitOfWork());

    // Not allowed to change global intent, so check when going from shared to exclusive.
    if (isSharedLockMode(_mode) && !isSharedLockMode(newMode))
        invariant(_opCtx->lockState()->isWriteLocked());

    _opCtx->lockState()->unlock(_id);
    _mode = newMode;

    // Verify we still have at least the Global resource locked.
    invariant(_opCtx->lockState()->isLocked());

    _opCtx->lockState()->lock(_opCtx, _id, _mode);
    _result = LOCK_OK;
}

Lock::CollectionLock::CollectionLock(OperationContext* opCtx,
                                     const NamespaceStringOrUUID& nssOrUUID,
                                     LockMode mode,
                                     Date_t deadline)
    : _opCtx(opCtx) {
    if (nssOrUUID.nss()) {
        auto& nss = *nssOrUUID.nss();
        _id = {RESOURCE_COLLECTION, nss};

        invariant(nss.coll().size(), str::stream() << "expected non-empty collection name:" << nss);
        dassert(_opCtx->lockState()->isDbLockedForMode(nss.dbName(),
                                                       isSharedLockMode(mode) ? MODE_IS : MODE_IX));

        _opCtx->lockState()->lock(_opCtx, _id, mode, deadline);
        return;
    }

    // 'nsOrUUID' must be a UUID and dbName.

    auto nss = CollectionCatalog::get(opCtx)->resolveNamespaceStringOrUUID(opCtx, nssOrUUID);

    // The UUID cannot move between databases so this one dassert is sufficient.
    dassert(_opCtx->lockState()->isDbLockedForMode(nss.dbName(),
                                                   isSharedLockMode(mode) ? MODE_IS : MODE_IX));

    // We cannot be sure that the namespace we lock matches the UUID given because we resolve the
    // namespace from the UUID without the safety of a lock. Therefore, we will continue to re-lock
    // until the namespace we resolve from the UUID before and after taking the lock is the same.
    bool locked = false;
    NamespaceString prevResolvedNss;
    do {
        if (locked) {
            _opCtx->lockState()->unlock(_id);
        }

        _id = ResourceId(RESOURCE_COLLECTION, nss);
        _opCtx->lockState()->lock(_opCtx, _id, mode, deadline);
        locked = true;

        // We looked up UUID without a collection lock so it's possible that the
        // collection name changed now. Look it up again.
        prevResolvedNss = nss;
        nss = CollectionCatalog::get(opCtx)->resolveNamespaceStringOrUUID(opCtx, nssOrUUID);
    } while (nss != prevResolvedNss);
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

void Lock::ResourceLock::lock(OperationContext* opCtx, LockMode mode, Date_t deadline) {
    invariant(_result == LOCK_INVALID);
    _locker->lock(opCtx, _rid, mode, deadline);
    _result = LOCK_OK;
}

void Lock::ResourceLock::unlock() {
    if (_result == LOCK_OK) {
        _locker->unlock(_rid);
        _result = LOCK_INVALID;
    }
}

}  // namespace mongo
