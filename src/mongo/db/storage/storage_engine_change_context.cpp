/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/storage_engine_change_context.h"

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/recovery_unit_noop.h"
#include "mongo/logv2/log.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {
const ServiceContext::Decoration<StorageEngineChangeContext> getStorageEngineChangeContext =
    ServiceContext::declareDecoration<StorageEngineChangeContext>();
}  // namespace

/* static */
StorageEngineChangeContext* StorageEngineChangeContext::get(ServiceContext* service) {
    return &getStorageEngineChangeContext(service);
}

StorageEngineChangeContext::StorageChangeToken
StorageEngineChangeContext::killOpsForStorageEngineChange(ServiceContext* service) {
    invariant(this == StorageEngineChangeContext::get(service));
    // Prevent new operations from being created.
    stdx::unique_lock storageChangeLk(_storageChangeSpinlock);
    stdx::unique_lock lk(_mutex);
    {
        ServiceContext::LockedClientsCursor clientCursor(service);
        // Interrupt all active operations with a real recovery unit.
        while (auto* client = clientCursor.next()) {
            if (client == Client::getCurrent())
                continue;
            OperationId killedOperationId;
            {
                stdx::lock_guard<Client> lk(*client);
                auto opCtxToKill = client->getOperationContext();
                if (!opCtxToKill || !opCtxToKill->recoveryUnit() ||
                    opCtxToKill->recoveryUnit()->isNoop())
                    continue;
                service->killOperation(lk, opCtxToKill, ErrorCodes::InterruptedDueToStorageChange);
                killedOperationId = opCtxToKill->getOpID();
                _previousStorageOpIds.insert(killedOperationId);
            }
            LOGV2_DEBUG(5781190,
                        1,
                        "Killed OpCtx for storage change",
                        "killedOperationId"_attr = killedOperationId);
        }
    }

    // Wait for active operation contexts to be released.
    _allOldStorageOperationContextsReleased.wait(lk, [&] { return _previousStorageOpIds.empty(); });
    // Free the old storage engine.
    service->clearStorageEngine();
    return storageChangeLk;
}

void StorageEngineChangeContext::changeStorageEngine(ServiceContext* service,
                                                     StorageChangeToken token,
                                                     std::unique_ptr<StorageEngine> engine) {
    invariant(this == StorageEngineChangeContext::get(service));
    service->setStorageEngine(std::move(engine));
    // Token -- which is a lock -- is released at end of scope, allowing OperationContexts to be
    // created again.
}

void StorageEngineChangeContext::onDestroyOperationContext(OperationContext* opCtx) {
    // If we're waiting for opCtxs to be destroyed, check if this is one of them.
    stdx::lock_guard lk(_mutex);
    auto iter = _previousStorageOpIds.find(opCtx->getOpID());
    if (iter != _previousStorageOpIds.end()) {
        // Without this, recovery unit will be released when opCtx is finally destroyed, which
        // happens outside the _mutex and thus may not be synchronized with the removal of the
        // storage engine.
        {
            stdx::lock_guard clientLock(*opCtx->getClient());
            opCtx->setRecoveryUnit(std::make_unique<RecoveryUnitNoop>(),
                                   WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);
        }
        _previousStorageOpIds.erase(iter);
        if (_previousStorageOpIds.empty()) {
            _allOldStorageOperationContextsReleased.notify_one();
        }
    }
}

/**
 * SharedSpinLock routines.
 *
 * The spin lock's lock word is logically divided into a bit (kExclusiveLock) and an unsigned
 * integer value for the rest of the word.  The meanings are as follows:
 *
 * kExclusiveLock not set, rest of word 0: Lock not held nor waited on.
 *
 * kExclusiveLock not set, rest of word non-zero: Lock held in shared mode by the number of holders
 * specified in the rest of the word.
 *
 * kExclusiveLock set, rest of word 0: Lock held in exclusive mode, no shared waiters.
 *
 * kExclusiveLock set, rest of word non-zero: Lock held in exclusive mode, number of waiters for the
 * shared lock specified in the rest of the word.
 *
 * Note that if there are shared waiters when the exclusive lock is released, they will obtain the
 * lock before another exclusive lock can be obtained.  This should be considered an implementation
 * detail and not a guarantee.
 *
 */
void StorageEngineChangeContext::SharedSpinLock::lock() {
    uint32_t expected = 0;
    while (!_lockWord.compareAndSwap(&expected, kExclusiveLock)) {
        expected = 0;
        mongo::sleepmillis(100);
    }
}

void StorageEngineChangeContext::SharedSpinLock::unlock() {
    uint32_t prevLockWord = _lockWord.fetchAndBitAnd(~kExclusiveLock);
    invariant(prevLockWord & kExclusiveLock);
}

void StorageEngineChangeContext::SharedSpinLock::lock_shared() {
    uint32_t prevLockWord = _lockWord.fetchAndAdd(1);
    // If the shared part of the lock word was all-ones, we just overflowed it.  This requires
    // 2^31 threads creating an opCtx at once, which shouldn't happen.
    invariant((prevLockWord & ~kExclusiveLock) != ~kExclusiveLock);
    while (MONGO_unlikely(prevLockWord & kExclusiveLock)) {
        mongo::sleepmillis(kLockPollIntervalMillis);
        prevLockWord = _lockWord.load();
    }
}

void StorageEngineChangeContext::SharedSpinLock::unlock_shared() {
    uint32_t prevLockWord = _lockWord.fetchAndSubtract(1);
    invariant(!(prevLockWord & kExclusiveLock));
}

}  // namespace mongo
