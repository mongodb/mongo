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

#include <map>
#include <memory>

#include "mongo/bson/oid.h"
#include "mongo/client/connection_string.h"
#include "mongo/s/catalog/dist_lock_manager.h"
#include "mongo/s/catalog/legacy/distlock.h"
#include "mongo/s/catalog/legacy/legacy_dist_lock_pinger.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/thread.h"

namespace mongo {

class DistributedLock;

class LegacyDistLockManager : public DistLockManager {
public:
    explicit LegacyDistLockManager(ConnectionString configServer, const std::string& processId);

    virtual ~LegacyDistLockManager() = default;

    virtual void startUp() override;
    virtual void shutDown(OperationContext* txn, bool allowNetworking) override;

    virtual std::string getProcessID() override;

    virtual StatusWith<DistLockManager::ScopedDistLock> lock(
        OperationContext* txn,
        StringData name,
        StringData whyMessage,
        stdx::chrono::milliseconds waitFor,
        stdx::chrono::milliseconds lockTryInterval) override;

    virtual void unlockAll(OperationContext* txn, const std::string& processID) override;

    // For testing only.  Must be called before any calls to startUp().
    static void disablePinger() {
        _pingerEnabled = false;
    }

protected:
    virtual void unlock(OperationContext* txn,
                        const DistLockHandle& lockHandle) BOOST_NOEXCEPT override;

    virtual Status checkStatus(OperationContext* txn, const DistLockHandle& lockHandle) override;

private:
    const ConnectionString _configServer;

    // Identifier of this process for determining ownership of distributed locks.
    std::string _processId;

    stdx::mutex _mutex;
    stdx::condition_variable _noLocksCV;
    std::map<DistLockHandle, std::unique_ptr<DistributedLock>> _lockMap;

    std::unique_ptr<LegacyDistLockPinger> _pinger;

    bool _isStopped;
    static bool _pingerEnabled;
};
}
