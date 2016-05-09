/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/catalog/dist_lock_manager.h"

#include <memory>

namespace mongo {

const Seconds DistLockManager::kDefaultLockTimeout(20);
const Milliseconds DistLockManager::kSingleLockAttemptTimeout(0);
const Milliseconds DistLockManager::kDefaultLockRetryInterval(500);

DistLockManager::ScopedDistLock::ScopedDistLock(OperationContext* txn,
                                                DistLockHandle lockHandle,
                                                DistLockManager* lockManager)
    : _txn(txn), _lockID(std::move(lockHandle)), _lockManager(lockManager) {}

DistLockManager::ScopedDistLock::~ScopedDistLock() {
    if (_lockManager) {
        _lockManager->unlock(_txn, _lockID);
    }
}

DistLockManager::ScopedDistLock::ScopedDistLock(ScopedDistLock&& other)
    : _txn(nullptr), _lockManager(nullptr) {
    *this = std::move(other);
}

DistLockManager::ScopedDistLock& DistLockManager::ScopedDistLock::operator=(
    ScopedDistLock&& other) {
    if (this != &other) {
        invariant(_lockManager == nullptr);
        invariant(_txn == nullptr);

        _txn = other._txn;
        _lockID = std::move(other._lockID);
        _lockManager = other._lockManager;
        other._lockManager = nullptr;
    }

    return *this;
}

Status DistLockManager::ScopedDistLock::checkStatus() {
    if (!_lockManager) {
        return Status(ErrorCodes::IllegalOperation, "no lock manager, lock was not acquired");
    }

    return _lockManager->checkStatus(_txn, _lockID);
}

}  // namespace mongo
