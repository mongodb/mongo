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

#pragma once

#include <string>
#include <vector>

#include "mongo/s/catalog/dist_lock_manager.h"
#include "mongo/stdx/functional.h"

namespace mongo {

class DistLockManagerMock : public DistLockManager {
public:
    DistLockManagerMock();

    virtual ~DistLockManagerMock() = default;

    virtual void startUp() override;
    virtual void shutDown(OperationContext* txn) override;

    virtual std::string getProcessID() override;

    virtual StatusWith<DistLockManager::ScopedDistLock> lock(OperationContext* txn,
                                                             StringData name,
                                                             StringData whyMessage,
                                                             Milliseconds waitFor,
                                                             Milliseconds lockTryInterval) override;

    virtual StatusWith<DistLockManager::ScopedDistLock> lockWithSessionID(
        OperationContext* txn,
        StringData name,
        StringData whyMessage,
        const OID lockSessionID,
        Milliseconds waitFor,
        Milliseconds lockTryInterval) override;

    virtual void unlockAll(OperationContext* txn, const std::string& processID) override;

    using LockFunc = stdx::function<void(StringData name,
                                         StringData whyMessage,
                                         Milliseconds waitFor,
                                         Milliseconds lockTryInterval)>;

    void expectLock(LockFunc checkerFunc, Status lockStatus);

protected:
    virtual void unlock(OperationContext* txn, const DistLockHandle& lockHandle) override;

    virtual Status checkStatus(OperationContext* txn, const DistLockHandle& lockHandle) override;

private:
    struct LockInfo {
        DistLockHandle lockID;
        std::string name;
    };

    std::vector<LockInfo> _locks;
    Status _lockReturnStatus;
    LockFunc _lockChecker;
};
}
