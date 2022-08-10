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


#include "mongo/db/s/ddl_lock_manager.h"

#include "mongo/db/operation_context.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

// TODO SERVER-68551: Remove once 7.0 becomes last-lts
MONGO_FAIL_POINT_DEFINE(disableReplSetDistLockManager);

const auto ddlLockManagerDecorator = ServiceContext::declareDecoration<DDLLockManager>();

}  // namespace

const Minutes DDLLockManager::kDefaultLockTimeout(5);
const Milliseconds DDLLockManager::kSingleLockAttemptTimeout(0);

DDLLockManager* DDLLockManager::get(ServiceContext* service) {
    return &ddlLockManagerDecorator(service);
}

DDLLockManager* DDLLockManager::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

DDLLockManager::ScopedLock DDLLockManager::lock(OperationContext* opCtx,
                                                StringData ns,
                                                StringData reason,
                                                Milliseconds timeout) {
    stdx::unique_lock<Latch> lock(_mutex);
    auto iter = _inProgressMap.find(ns);

    if (iter == _inProgressMap.end()) {
        _inProgressMap.try_emplace(ns, std::make_shared<NSLock>(reason));
    } else {
        auto nsLock = iter->second;
        nsLock->numWaiting++;
        ScopeGuard guard([&] { nsLock->numWaiting--; });
        if (!opCtx->waitForConditionOrInterruptFor(
                nsLock->cvLocked, lock, timeout, [nsLock]() { return !nsLock->isInProgress; })) {
            using namespace fmt::literals;
            uasserted(
                ErrorCodes::LockBusy,
                "Failed to acquire DDL lock for namespace '{}' after {} that is currently locked with reason '{}'"_format(
                    ns, timeout.toString(), reason));
        }
        guard.dismiss();
        nsLock->reason = reason.toString();
        nsLock->isInProgress = true;
    }

    LOGV2(6855301, "Acquired DDL lock", "resource"_attr = ns, "reason"_attr = reason);
    return {ns, reason, this};
}

DDLLockManager::ScopedLock::ScopedLock(StringData ns,
                                       StringData reason,
                                       DDLLockManager* lockManager)
    : _ns(ns.toString()), _reason(reason.toString()), _lockManager(lockManager) {}

DDLLockManager::ScopedLock::ScopedLock(ScopedLock&& other)
    : _ns(std::move(other._ns)),
      _reason(std::move(other._reason)),
      _lockManager(other._lockManager) {
    other._lockManager = nullptr;
}

DDLLockManager::ScopedLock::~ScopedLock() {
    if (_lockManager) {
        stdx::unique_lock<Latch> lock(_lockManager->_mutex);
        auto iter = _lockManager->_inProgressMap.find(_ns);

        iter->second->numWaiting--;
        iter->second->reason.clear();
        iter->second->isInProgress = false;
        iter->second->cvLocked.notify_one();

        if (iter->second->numWaiting == 0) {
            _lockManager->_inProgressMap.erase(_ns);
        }
        LOGV2(6855302, "Released DDL lock", "resource"_attr = _ns, "reason"_attr = _reason);
    }
}

}  // namespace mongo
