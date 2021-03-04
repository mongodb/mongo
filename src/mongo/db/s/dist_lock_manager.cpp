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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/dist_lock_manager.h"

#include "mongo/db/operation_context.h"

namespace mongo {
namespace {

const auto getDistLockManager =
    ServiceContext::declareDecoration<std::unique_ptr<DistLockManager>>();

}  // namespace

const Minutes DistLockManager::kDefaultLockTimeout(5);
const Milliseconds DistLockManager::kSingleLockAttemptTimeout(0);

DistLockManager::ScopedDistLock::ScopedDistLock(OperationContext* opCtx,
                                                StringData lockName,
                                                ScopedLock&& scopedLock,
                                                DistLockManager* lockManager)
    : _opCtx(opCtx),
      _lockName(lockName.toString()),
      _scopedLock(std::move(scopedLock)),
      _lockManager(lockManager) {}

DistLockManager::ScopedDistLock::~ScopedDistLock() {
    if (_lockManager) {
        _lockManager->unlock(_opCtx, _lockName);
    }
}

DistLockManager::ScopedDistLock::ScopedDistLock(ScopedDistLock&& other)
    : ScopedDistLock(other._opCtx,
                     std::move(other._lockName),
                     std::move(other._scopedLock),
                     other._lockManager) {
    other._opCtx = nullptr;
    other._lockManager = nullptr;
}

DistLockManager::ScopedDistLock DistLockManager::ScopedDistLock::moveToAnotherThread() {
    auto unownedScopedDistLock(std::move(*this));
    unownedScopedDistLock._opCtx = nullptr;
    return unownedScopedDistLock;
}

void DistLockManager::ScopedDistLock::assignNewOpCtx(OperationContext* opCtx) {
    invariant(!_opCtx);
    _opCtx = opCtx;
}

DistLockManager::DistLockManager(OID lockSessionID) : _lockSessionID(std::move(lockSessionID)) {}

DistLockManager* DistLockManager::get(ServiceContext* service) {
    return getDistLockManager(service).get();
}

DistLockManager* DistLockManager::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

void DistLockManager::create(ServiceContext* service,
                             std::unique_ptr<DistLockManager> distLockManager) {
    invariant(!getDistLockManager(service));
    getDistLockManager(service) = std::move(distLockManager);
}

StatusWith<DistLockManager::ScopedDistLock> DistLockManager::lock(OperationContext* opCtx,
                                                                  StringData name,
                                                                  StringData whyMessage,
                                                                  Milliseconds waitFor) {
    boost::optional<ScopedLock> scopedLock;
    try {
        scopedLock.emplace(lockDirectLocally(opCtx, name, waitFor));
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    auto status = lockDirect(opCtx, name, whyMessage, waitFor);
    if (!status.isOK()) {
        return status;
    }

    return DistLockManager::ScopedDistLock(opCtx, name, std::move(*scopedLock), this);
}

DistLockManager::ScopedLock DistLockManager::lockDirectLocally(OperationContext* opCtx,
                                                               StringData ns,
                                                               Milliseconds waitFor) {
    stdx::unique_lock<Latch> lock(_mutex);
    auto iter = _inProgressMap.find(ns);

    if (iter == _inProgressMap.end()) {
        _inProgressMap.try_emplace(ns, std::make_shared<NSLock>());
    } else {
        auto nsLock = iter->second;
        nsLock->numWaiting++;
        auto guard = makeGuard([&] { nsLock->numWaiting--; });
        if (!opCtx->waitForConditionOrInterruptFor(
                nsLock->cvLocked, lock, waitFor, [nsLock]() { return !nsLock->isInProgress; })) {
            uasserted(ErrorCodes::LockBusy,
                      str::stream() << "Failed to acquire dist lock " << ns << " locally");
        }
        guard.dismiss();
        nsLock->isInProgress = true;
    }

    return ScopedLock(ns, this);
}

DistLockManager::ScopedLock::ScopedLock(StringData ns, DistLockManager* distLockManager)
    : _ns(ns.toString()), _lockManager(distLockManager) {}

DistLockManager::ScopedLock::ScopedLock(ScopedLock&& other)
    : _ns(std::move(other._ns)), _lockManager(other._lockManager) {
    other._lockManager = nullptr;
}

DistLockManager::ScopedLock::~ScopedLock() {
    if (_lockManager) {
        stdx::unique_lock<Latch> lock(_lockManager->_mutex);
        auto iter = _lockManager->_inProgressMap.find(_ns);

        iter->second->numWaiting--;
        iter->second->isInProgress = false;
        iter->second->cvLocked.notify_one();

        if (iter->second->numWaiting == 0) {
            _lockManager->_inProgressMap.erase(_ns);
        }
    }
}

}  // namespace mongo
