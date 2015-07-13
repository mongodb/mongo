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

#include "mongo/s/catalog/legacy/legacy_dist_lock_manager.h"

#include "mongo/s/catalog/type_locks.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/timer.h"

namespace mongo {

using std::string;
using std::unique_ptr;
using stdx::chrono::milliseconds;


namespace {
const stdx::chrono::seconds kDefaultSocketTimeout(30);
const milliseconds kDefaultPingInterval(30 * 1000);
}  // unnamed namespace

LegacyDistLockManager::LegacyDistLockManager(ConnectionString configServer)
    : _configServer(std::move(configServer)), _isStopped(false), _pingerEnabled(true) {}

void LegacyDistLockManager::startUp() {
    stdx::lock_guard<stdx::mutex> sl(_mutex);
    invariant(!_pinger);
    _pinger = stdx::make_unique<LegacyDistLockPinger>();
}

void LegacyDistLockManager::shutDown() {
    stdx::unique_lock<stdx::mutex> sl(_mutex);
    _isStopped = true;

    while (!_lockMap.empty()) {
        _noLocksCV.wait(sl);
    }

    if (_pinger) {
        _pinger->shutdown();
    }
}

StatusWith<DistLockManager::ScopedDistLock> LegacyDistLockManager::lock(
    StringData name, StringData whyMessage, milliseconds waitFor, milliseconds lockTryInterval) {
    auto distLock = stdx::make_unique<DistributedLock>(_configServer, name.toString());

    {
        stdx::lock_guard<stdx::mutex> sl(_mutex);

        if (_isStopped) {
            return Status(ErrorCodes::LockBusy, "legacy distlock manager is stopped");
        }

        if (_pingerEnabled) {
            auto pingStatus = _pinger->startPing(*(distLock.get()), kDefaultPingInterval);
            if (!pingStatus.isOK()) {
                return pingStatus;
            }
        }
    }

    auto lastStatus =
        Status(ErrorCodes::LockBusy, str::stream() << "timed out waiting for " << name);

    Timer timer;
    Timer msgTimer;
    while (waitFor <= milliseconds::zero() || milliseconds(timer.millis()) < waitFor) {
        bool acquired = false;
        BSONObj lockDoc;
        try {
            acquired =
                distLock->lock_try(whyMessage.toString(), &lockDoc, kDefaultSocketTimeout.count());

            if (!acquired) {
                lastStatus = Status(ErrorCodes::LockBusy,
                                    str::stream() << "Lock for " << whyMessage << " is taken.");
            }
        } catch (const LockException& lockExcep) {
            OID needUnlockID(lockExcep.getMustUnlockID());
            if (needUnlockID.isSet()) {
                _pinger->addUnlockOID(needUnlockID);
            }

            lastStatus = lockExcep.toStatus();
        } catch (...) {
            lastStatus = exceptionToStatus();
        }

        if (acquired) {
            verify(!lockDoc.isEmpty());

            auto locksTypeResult = LocksType::fromBSON(lockDoc);
            if (!locksTypeResult.isOK()) {
                return StatusWith<ScopedDistLock>(
                    ErrorCodes::UnsupportedFormat,
                    str::stream() << "error while parsing lock document: " << lockDoc << " : "
                                  << locksTypeResult.getStatus().toString());
            }
            const LocksType& lock = locksTypeResult.getValue();
            dassert(lock.isLockIDSet());

            {
                stdx::lock_guard<stdx::mutex> sl(_mutex);
                _lockMap.insert(std::make_pair(lock.getLockID(), std::move(distLock)));
            }

            return ScopedDistLock(lock.getLockID(), this);
        }

        if (waitFor == milliseconds::zero())
            break;

        if (lastStatus != ErrorCodes::LockBusy) {
            return lastStatus;
        }

        // Periodically message for debugging reasons
        if (msgTimer.seconds() > 10) {
            log() << "waited " << timer.seconds() << "s for distributed lock " << name << " for "
                  << whyMessage << ": " << lastStatus.toString();

            msgTimer.reset();
        }

        milliseconds timeRemaining =
            std::max(milliseconds::zero(), waitFor - milliseconds(timer.millis()));
        sleepFor(std::min(lockTryInterval, timeRemaining));
    }

    return lastStatus;
}

void LegacyDistLockManager::unlock(const DistLockHandle& lockHandle) BOOST_NOEXCEPT {
    unique_ptr<DistributedLock> distLock;

    {
        stdx::lock_guard<stdx::mutex> sl(_mutex);
        auto iter = _lockMap.find(lockHandle);
        invariant(iter != _lockMap.end());

        distLock = std::move(iter->second);
        _lockMap.erase(iter);
    }

    if (!distLock->unlock(lockHandle)) {
        _pinger->addUnlockOID(lockHandle);
    }

    {
        stdx::lock_guard<stdx::mutex> sl(_mutex);
        if (_lockMap.empty()) {
            _noLocksCV.notify_all();
        }
    }
}

Status LegacyDistLockManager::checkStatus(const DistLockHandle& lockHandle) {
    // Note: this should not happen when locks are managed through ScopedDistLock.
    if (_pinger->willUnlockOID(lockHandle)) {
        return Status(ErrorCodes::LockFailed,
                      str::stream() << "lock " << lockHandle << " is not held and "
                                    << "is currently being scheduled for lazy unlock");
    }

    DistributedLock* distLock = nullptr;

    {
        // Assumption: lockHandles are never shared across threads.
        stdx::lock_guard<stdx::mutex> sl(_mutex);
        auto iter = _lockMap.find(lockHandle);
        invariant(iter != _lockMap.end());

        distLock = iter->second.get();
    }

    return distLock->checkStatus(kDefaultSocketTimeout.count());
}

void LegacyDistLockManager::enablePinger(bool enable) {
    stdx::lock_guard<stdx::mutex> sl(_mutex);
    _pingerEnabled = enable;
}
}
