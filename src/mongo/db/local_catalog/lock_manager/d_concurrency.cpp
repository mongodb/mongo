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

#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/intent_registry.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {

Lock::ResourceLock::ResourceLock(ResourceLock&& other)
    : _opCtx(other._opCtx), _rid(std::move(other._rid)), _result(other._result) {
    other._opCtx = nullptr;
    other._result = LOCK_INVALID;
}

void Lock::ResourceLock::_lock(LockMode mode, Date_t deadline) {
    invariant(_result == LOCK_INVALID);
    shard_role_details::getLocker(_opCtx)->lock(_opCtx, _rid, mode, deadline);
    _result = LOCK_OK;
}

void Lock::ResourceLock::_unlock() {
    if (_isLocked()) {
        shard_role_details::getLocker(_opCtx)->unlock(_rid);
        _result = LOCK_INVALID;
    }
}

void Lock::ExclusiveLock::lock() {
    // The contract of the condition_variable-like utilities is that that the lock is returned in
    // the locked state so the acquisition below must be guaranteed to always succeed.
    invariant(_opCtx);
    UninterruptibleLockGuard ulg(_opCtx);  // NOLINT.
    _lock(MODE_X);
}

Lock::GlobalLock::GlobalLock(OperationContext* opCtx,
                             LockMode lockMode,
                             Date_t deadline,
                             InterruptBehavior behavior)
    : GlobalLock(opCtx, lockMode, deadline, behavior, GlobalLockOptions{}) {}

Lock::GlobalLock::GlobalLock(OperationContext* opCtx,
                             LockMode lockMode,
                             Date_t deadline,
                             InterruptBehavior behavior,
                             GlobalLockOptions options)
    : _opCtx(opCtx), _interruptBehavior(behavior), _skipRSTLLock(options.skipRSTLLock) {
    if (!options.skipFlowControlTicket) {
        shard_role_details::getLocker(_opCtx)->getFlowControlTicket(_opCtx, lockMode);
    }

    try {
        const bool acquireMultiDocumentTxnBarrier = [&] {
            if (opCtx->inMultiDocumentTransaction()) {
                invariant(lockMode == MODE_IS || lockMode == MODE_IX);
                return true;
            }
            return lockMode == MODE_S || lockMode == MODE_X;
        }();
        if (acquireMultiDocumentTxnBarrier) {
            _multiDocTxnBarrier.emplace(
                _opCtx, resourceIdMultiDocumentTransactionsBarrier, lockMode, deadline);
        }
        ScopeGuard unlockMultiDocTxnBarrier([this] { _multiDocTxnBarrier.reset(); });

        _result = LOCK_INVALID;
        if (gFeatureFlagIntentRegistration.isEnabled() || options.skipRSTLLock) {
            _declareIntent(lockMode, options.explicitIntent);
            _takeGlobalLockOnly(lockMode, deadline);
        } else {
            _takeGlobalAndRSTLLocks(lockMode, deadline, options.explicitIntent);
        }
        _result = LOCK_OK;

        unlockMultiDocTxnBarrier.dismiss();
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
    auto acquiredLockMode = shard_role_details::getLocker(_opCtx)->getLockMode(resourceIdGlobal);
    shard_role_details::getLocker(_opCtx)->setGlobalLockTakenInMode(acquiredLockMode);
}

void Lock::GlobalLock::_takeGlobalLockOnly(LockMode lockMode, Date_t deadline) {
    shard_role_details::getLocker(_opCtx)->lockGlobal(_opCtx, lockMode, deadline);
}

void Lock::GlobalLock::_takeGlobalAndRSTLLocks(
    LockMode lockMode,
    Date_t deadline,
    boost::optional<rss::consensus::IntentRegistry::Intent> explicitIntent) {
    shard_role_details::getLocker(_opCtx)->lock(
        _opCtx, resourceIdReplicationStateTransitionLock, MODE_IX, deadline);
    ScopeGuard unlockRSTL([this] {
        shard_role_details::getLocker(_opCtx)->unlock(resourceIdReplicationStateTransitionLock);
    });

    _declareIntent(lockMode, explicitIntent);

    shard_role_details::getLocker(_opCtx)->lockGlobal(_opCtx, lockMode, deadline);

    unlockRSTL.dismiss();
}

void Lock::GlobalLock::_declareIntent(
    LockMode lockMode, boost::optional<rss::consensus::IntentRegistry::Intent> explicitIntent) {
    if (gFeatureFlagIntentRegistration.isEnabled()) {
        if (explicitIntent) {
            _guard.emplace(explicitIntent.get(), _opCtx);
            if (isSharedLockMode(lockMode)) {
                // Only read intent is allowed with MODE_IS global lock acquisitions.
                invariant(explicitIntent.get() == rss::consensus::IntentRegistry::Intent::Read);
            }
        } else if (!isSharedLockMode(lockMode)) {
            _guard.emplace(rss::consensus::IntentRegistry::Intent::Write, _opCtx);
        } else if (isSharedLockMode(lockMode)) {
            _guard.emplace(rss::consensus::IntentRegistry::Intent::Read, _opCtx);
        } else {
            MONGO_UNREACHABLE;
        }
    }
}

Lock::GlobalLock::GlobalLock(GlobalLock&& otherLock)
    : _opCtx(otherLock._opCtx),
      _result(otherLock._result),
      _multiDocTxnBarrier(std::move(otherLock._multiDocTxnBarrier)),
      _interruptBehavior(otherLock._interruptBehavior),
      _skipRSTLLock(otherLock._skipRSTLLock),
      _guard(std::move(otherLock._guard)) {
    // Mark as moved so the destructor doesn't invalidate the newly-constructed lock.
    otherLock._result = LOCK_INVALID;
    otherLock._guard = boost::none;
}

Lock::GlobalLock::~GlobalLock() {
    // Preserve the original lock result which will be overridden by unlock().
    auto lockResult = _result;
    auto* locker = shard_role_details::getLocker(_opCtx);

    if (isLocked()) {
        // Abandon our snapshot if destruction of the GlobalLock object results in actually
        // unlocking the global lock. Recursive locking and the two-phase locking protocol may
        // prevent lock release.
        const bool willReleaseLock =
            !locker->isGlobalLockedRecursively() && !locker->inAWriteUnitOfWork();
        if (willReleaseLock) {
            shard_role_details::getRecoveryUnit(_opCtx)->abandonSnapshot();
        }
        _unlock();
    }

    if (!_skipRSTLLock && (lockResult == LOCK_OK || lockResult == LOCK_WAITING)) {
        locker->unlock(resourceIdReplicationStateTransitionLock);
    }
}

void Lock::GlobalLock::_unlock() {
    shard_role_details::getLocker(_opCtx)->unlockGlobal();
    _result = LOCK_INVALID;
}

Lock::TenantLock::TenantLock(OperationContext* opCtx,
                             const TenantId& tenantId,
                             LockMode mode,
                             Date_t deadline)
    : _id{RESOURCE_TENANT, tenantId}, _opCtx{opCtx} {
    dassert(shard_role_details::getLocker(_opCtx)->isLockHeldForMode(
        resourceIdGlobal, isSharedLockMode(mode) ? MODE_IS : MODE_IX));
    shard_role_details::getLocker(_opCtx)->lock(_opCtx, _id, mode, deadline);
}

Lock::TenantLock::TenantLock(TenantLock&& otherLock)
    : _id(otherLock._id), _opCtx(otherLock._opCtx) {
    otherLock._opCtx = nullptr;
}

Lock::TenantLock::~TenantLock() {
    if (_opCtx) {
        shard_role_details::getLocker(_opCtx)->unlock(_id);
    }
}

Lock::DBLock::DBLock(OperationContext* opCtx,
                     const DatabaseName& dbName,
                     LockMode mode,
                     Date_t deadline,
                     boost::optional<LockMode> tenantLockMode)
    : DBLock(opCtx, dbName, mode, deadline, DBLockSkipOptions{}, tenantLockMode) {}

Lock::DBLock::DBLock(OperationContext* opCtx,
                     const DatabaseName& dbName,
                     LockMode mode,
                     Date_t deadline,
                     DBLockSkipOptions options,
                     boost::optional<LockMode> tenantLockMode)
    : _id(RESOURCE_DATABASE, dbName),
      _opCtx(opCtx),
      _result(LOCK_INVALID),
      _mode(mode),
      _oldBlockingAllowed(shard_role_details::getRecoveryUnit((_opCtx))->getBlockingAllowed()) {

    if ((dbName.isLocalDB() || !opCtx->writesAreReplicated()) && !isSharedLockMode(mode)) {
        options.explicitIntent = rss::consensus::IntentRegistry::Intent::LocalWrite;
    }
    _globalLock.emplace(opCtx,
                        isSharedLockMode(_mode) ? MODE_IS : MODE_IX,
                        deadline,
                        InterruptBehavior::kThrow,
                        std::move(options));

    massert(28539, "need a valid database name", !dbName.isEmpty());

    tassert(6671501,
            str::stream() << "Tenant lock mode " << modeName(*tenantLockMode)
                          << " specified for database " << dbName.toStringForErrorMsg()
                          << " that does not belong to a tenant",
            !tenantLockMode || dbName.tenantId());

    // Acquire the tenant lock.
    if (dbName.tenantId()) {
        const auto effectiveTenantLockMode = [&]() {
            const auto defaultTenantLockMode = isSharedLockMode(_mode) ? MODE_IS : MODE_IX;
            if (tenantLockMode) {
                tassert(6671505,
                        str::stream() << "Requested tenant lock mode " << modeName(*tenantLockMode)
                                      << " that is weaker than the default one  "
                                      << modeName(defaultTenantLockMode) << " for database "
                                      << dbName.toStringForErrorMsg() << " of tenant  "
                                      << dbName.tenantId()->toString(),
                        isModeCovered(defaultTenantLockMode, *tenantLockMode));
                return *tenantLockMode;
            } else {
                return defaultTenantLockMode;
            }
        }();
        _tenantLock.emplace(opCtx, *dbName.tenantId(), effectiveTenantLockMode, deadline);
    }

    shard_role_details::getLocker(_opCtx)->lock(_opCtx, _id, _mode, deadline);

    // If a user operation on secondaries acquires a lock in MODE_S and then blocks on a prepare
    // conflict with a prepared transaction, a deadlock will occur at the commit time of the
    // prepared transaction when it attempts to reacquire (since locks were yielded on
    // secondaries) an IX lock that conflicts with the MODE_S lock held by the user operation.
    // User operations that acquire MODE_X locks and block on prepare conflicts could lead to
    // the same problem. However, user operations on secondaries should never hold MODE_X locks, but
    // we add it here for completeness. Since prepared transactions reacquire database locks at
    // commit time, instruct the storage engine that it may not block on prepared transactions.
    if (mode == MODE_X || mode == MODE_S) {
        shard_role_details::getRecoveryUnit((_opCtx))->setBlockingAllowed(false);
    }

    _result = LOCK_OK;
}

Lock::DBLock::DBLock(DBLock&& otherLock)
    : _id(otherLock._id),
      _opCtx(otherLock._opCtx),
      _result(otherLock._result),
      _mode(otherLock._mode),
      _globalLock(std::move(otherLock._globalLock)),
      _tenantLock(std::move(otherLock._tenantLock)),
      _oldBlockingAllowed(otherLock._oldBlockingAllowed) {
    // Mark as moved so the destructor doesn't invalidate the newly-constructed lock.
    otherLock._result = LOCK_INVALID;
}

Lock::DBLock::~DBLock() {
    if (isLocked()) {
        shard_role_details::getLocker(_opCtx)->unlock(_id);
        shard_role_details::getRecoveryUnit((_opCtx))->setBlockingAllowed(_oldBlockingAllowed);
    }
}

Lock::CollectionLock::CollectionLock(OperationContext* opCtx,
                                     const NamespaceString& ns,
                                     LockMode mode,
                                     Date_t deadline)
    : _id(RESOURCE_COLLECTION, ns),
      _opCtx(opCtx),
      _oldBlockingAllowed(shard_role_details::getRecoveryUnit((_opCtx))->getBlockingAllowed()) {
    invariant(!ns.coll().empty());
    dassert(shard_role_details::getLocker(_opCtx)->isDbLockedForMode(
        ns.dbName(), isSharedLockMode(mode) ? MODE_IS : MODE_IX));

    shard_role_details::getLocker(_opCtx)->lock(_opCtx, _id, mode, deadline);

    // If a user operation on secondaries acquires a lock in MODE_S and then blocks on a prepare
    // conflict with a prepared transaction, a deadlock will occur at the commit time of the
    // prepared transaction when it attempts to reacquire (since locks were yielded on
    // secondaries) an IX lock that conflicts with the MODE_S lock held by the user operation.
    // User operations that acquire MODE_X locks and block on prepare conflicts could lead to
    // the same problem. However, user operations on secondaries should never hold MODE_X locks, but
    // we add it here for completeness. Since prepared transactions reacquire collection locks at
    // commit time, instruct the storage engine that it may not block on prepared transactions.
    if (mode == MODE_X || mode == MODE_S) {
        shard_role_details::getRecoveryUnit((_opCtx))->setBlockingAllowed(false);
    }
}

Lock::CollectionLock::CollectionLock(CollectionLock&& otherLock)
    : _id(otherLock._id),
      _opCtx(otherLock._opCtx),
      _oldBlockingAllowed(otherLock._oldBlockingAllowed) {
    otherLock._opCtx = nullptr;
}

Lock::CollectionLock& Lock::CollectionLock::operator=(CollectionLock&& other) {
    _id = other._id;
    _opCtx = other._opCtx;
    _oldBlockingAllowed = other._oldBlockingAllowed;
    other._opCtx = nullptr;
    return *this;
}

Lock::CollectionLock::~CollectionLock() {
    if (_opCtx) {
        shard_role_details::getLocker(_opCtx)->unlock(_id);
        shard_role_details::getRecoveryUnit((_opCtx))->setBlockingAllowed(_oldBlockingAllowed);
    }
}

}  // namespace mongo
