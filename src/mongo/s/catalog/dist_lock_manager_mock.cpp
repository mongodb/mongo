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

#include "mongo/s/catalog/dist_lock_manager_mock.h"

#include <algorithm>

#include "mongo/unittest/unittest.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/time_support.h"

namespace mongo {

namespace {

void NoLockFuncSet(StringData name,
                   StringData whyMessage,
                   Milliseconds waitFor,
                   Milliseconds lockTryInterval) {
    FAIL(str::stream() << "Lock not expected to be called. "
                       << "Name: "
                       << name
                       << ", whyMessage: "
                       << whyMessage
                       << ", waitFor: "
                       << waitFor
                       << ", lockTryInterval: "
                       << lockTryInterval);
}

}  // namespace

DistLockManagerMock::DistLockManagerMock()
    : _lockReturnStatus{Status::OK()}, _lockChecker{NoLockFuncSet} {}

void DistLockManagerMock::startUp() {}

void DistLockManagerMock::shutDown(OperationContext* txn) {
    uassert(28659, "DistLockManagerMock shut down with outstanding locks present", _locks.empty());
}

std::string DistLockManagerMock::getProcessID() {
    return "Mock dist lock manager process id";
}

StatusWith<DistLockManager::ScopedDistLock> DistLockManagerMock::lock(
    OperationContext* txn,
    StringData name,
    StringData whyMessage,
    Milliseconds waitFor,
    Milliseconds lockTryInterval) {
    return lockWithSessionID(
        txn, name, whyMessage, DistLockHandle::gen(), waitFor, lockTryInterval);
}

StatusWith<DistLockManager::ScopedDistLock> DistLockManagerMock::lockWithSessionID(
    OperationContext* txn,
    StringData name,
    StringData whyMessage,
    const OID lockSessionID,
    Milliseconds waitFor,
    Milliseconds lockTryInterval) {
    _lockChecker(name, whyMessage, waitFor, lockTryInterval);
    _lockChecker = NoLockFuncSet;

    if (!_lockReturnStatus.isOK()) {
        return _lockReturnStatus;
    }

    if (_locks.end() != std::find_if(_locks.begin(), _locks.end(), [name](LockInfo info) -> bool {
            return info.name == name;
        })) {
        return Status(ErrorCodes::LockBusy,
                      str::stream() << "Lock \"" << name << "\" is already taken");
    }

    LockInfo info;
    info.name = name.toString();
    info.lockID = lockSessionID;
    _locks.push_back(info);

    return DistLockManager::ScopedDistLock(nullptr, info.lockID, this);
}

void DistLockManagerMock::unlockAll(OperationContext* txn, const std::string& processID) {
    fassertFailed(34366);  // Not implemented for the mock
}

void DistLockManagerMock::unlock(OperationContext* txn, const DistLockHandle& lockHandle) {
    std::vector<LockInfo>::iterator it =
        std::find_if(_locks.begin(), _locks.end(), [&lockHandle](LockInfo info) -> bool {
            return info.lockID == lockHandle;
        });
    if (it == _locks.end()) {
        return;
    }
    _locks.erase(it);
}

Status DistLockManagerMock::checkStatus(OperationContext* txn, const DistLockHandle& lockHandle) {
    return Status::OK();
}

void DistLockManagerMock::expectLock(LockFunc checker, Status status) {
    _lockReturnStatus = std::move(status);
    _lockChecker = checker;
}
}
