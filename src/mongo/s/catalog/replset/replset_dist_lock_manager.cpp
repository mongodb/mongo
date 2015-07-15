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

#include "mongo/s/catalog/replset/replset_dist_lock_manager.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/service_context.h"
#include "mongo/s/catalog/dist_lock_catalog.h"
#include "mongo/s/catalog/type_lockpings.h"
#include "mongo/s/catalog/type_locks.h"
#include "mongo/stdx/chrono.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/timer.h"

namespace mongo {

using std::string;
using std::unique_ptr;
using stdx::chrono::milliseconds;
using stdx::chrono::duration_cast;

const stdx::chrono::seconds ReplSetDistLockManager::kDistLockWriteConcernTimeout{5};
const stdx::chrono::seconds ReplSetDistLockManager::kDistLockPingInterval{30};
const stdx::chrono::minutes ReplSetDistLockManager::kDistLockExpirationTime{15};

ReplSetDistLockManager::ReplSetDistLockManager(ServiceContext* globalContext,
                                               StringData processID,
                                               unique_ptr<DistLockCatalog> catalog,
                                               milliseconds pingInterval,
                                               milliseconds lockExpiration)
    : _serviceContext(globalContext),
      _processID(processID.toString()),
      _catalog(std::move(catalog)),
      _pingInterval(pingInterval),
      _lockExpiration(lockExpiration) {}

ReplSetDistLockManager::~ReplSetDistLockManager() = default;

void ReplSetDistLockManager::startUp() {
    _execThread = stdx::make_unique<stdx::thread>(&ReplSetDistLockManager::doTask, this);
}

void ReplSetDistLockManager::shutDown() {
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _isShutDown = true;
        _shutDownCV.notify_all();
    }

    // Don't grab _mutex, otherwise will deadlock trying to join. Safe to read
    // _execThread since it is modified only at statrUp().
    if (_execThread && _execThread->joinable()) {
        _execThread->join();
        _execThread.reset();
    }

    auto status = _catalog->stopPing(_processID);
    if (!status.isOK()) {
        warning() << "error encountered while cleaning up distributed ping entry for " << _processID
                  << causedBy(status);
    }
}

bool ReplSetDistLockManager::isShutDown() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _isShutDown;
}

void ReplSetDistLockManager::doTask() {
    LOG(0) << "creating distributed lock ping thread for process " << _processID
           << " (sleeping for " << duration_cast<milliseconds>(_pingInterval).count() << " ms)";

    Timer elapsedSincelastPing(_serviceContext->getTickSource());
    while (!isShutDown()) {
        auto pingStatus = _catalog->ping(_processID, Date_t::now());

        if (!pingStatus.isOK()) {
            warning() << "pinging failed for distributed lock pinger" << causedBy(pingStatus);
        }

        const milliseconds elapsed(elapsedSincelastPing.millis());
        if (elapsed > 10 * _pingInterval) {
            warning() << "Lock pinger for proc: " << _processID << " was inactive for " << elapsed
                      << " ms";
        }
        elapsedSincelastPing.reset();

        std::deque<DistLockHandle> toUnlockBatch;
        {
            stdx::unique_lock<stdx::mutex> lk(_mutex);
            toUnlockBatch.swap(_unlockList);
        }

        for (const auto& toUnlock : toUnlockBatch) {
            auto unlockStatus = _catalog->unlock(toUnlock);

            if (!unlockStatus.isOK()) {
                warning() << "Failed to unlock lock with " << LocksType::lockID() << ": "
                          << toUnlock << causedBy(unlockStatus);
                queueUnlock(toUnlock);
            } else {
                LOG(0) << "distributed lock with " << LocksType::lockID() << ": " << toUnlock
                       << "' unlocked.";
            }

            if (isShutDown()) {
                return;
            }
        }

        stdx::unique_lock<stdx::mutex> lk(_mutex);
        _shutDownCV.wait_for(lk, _pingInterval);
    }
}

StatusWith<bool> ReplSetDistLockManager::canOvertakeLock(LocksType lockDoc) {
    const auto& processID = lockDoc.getProcess();
    auto pingStatus = _catalog->getPing(processID);
    if (!pingStatus.isOK()) {
        return pingStatus.getStatus();
    }

    const auto& pingDoc = pingStatus.getValue();
    Status pingDocValidationStatus = pingDoc.validate();
    if (!pingDocValidationStatus.isOK()) {
        return {ErrorCodes::UnsupportedFormat,
                str::stream() << "invalid ping document for " << processID << ": "
                              << pingDocValidationStatus.toString()};
    }

    Timer timer(_serviceContext->getTickSource());
    auto serverInfoStatus = _catalog->getServerInfo();
    if (!serverInfoStatus.isOK()) {
        return serverInfoStatus.getStatus();
    }

    // Be conservative when determining that lock expiration has elapsed by
    // taking into account the roundtrip delay of trying to get the local
    // time from the config server.
    milliseconds delay(timer.millis() / 2);  // Assuming symmetrical delay.

    Date_t pingValue = pingDoc.getPing();
    const auto& serverInfo = serverInfoStatus.getValue();

    stdx::lock_guard<stdx::mutex> lk(_mutex);
    auto pingIter = _pingHistory.find(lockDoc.getName());

    if (pingIter == _pingHistory.end()) {
        // We haven't seen this lock before so we don't have any point of reference
        // to compare and determine the elapsed time. Save the current ping info
        // for this lock.
        _pingHistory.emplace(std::piecewise_construct,
                             std::forward_as_tuple(lockDoc.getName()),
                             std::forward_as_tuple(processID,
                                                   pingValue,
                                                   serverInfo.serverTime,
                                                   lockDoc.getLockID(),
                                                   serverInfo.electionId));
        return false;
    }

    auto configServerLocalTime = serverInfo.serverTime - delay;

    auto* pingInfo = &pingIter->second;

    LOG(1) << "checking last ping for lock '" << lockDoc.getName() << "' against last seen process "
           << pingInfo->processId << " and ping " << pingInfo->lastPing;

    if (pingInfo->lastPing != pingValue ||  // ping is active

        // Owner of this lock is now different from last time so we can't
        // use the ping data.
        pingInfo->lockSessionId != lockDoc.getLockID() ||

        // Primary changed, we can't trust that clocks are synchronized so
        // treat as if this is a new entry.
        pingInfo->electionId != serverInfo.electionId) {
        pingInfo->lastPing = pingValue;
        pingInfo->electionId = serverInfo.electionId;
        pingInfo->configLocalTime = configServerLocalTime;
        pingInfo->lockSessionId = lockDoc.getLockID();
        return false;
    }

    if (configServerLocalTime < pingInfo->configLocalTime) {
        warning() << "config server local time went backwards, from last seen: "
                  << pingInfo->configLocalTime << " to " << configServerLocalTime;
        return false;
    }

    milliseconds elapsedSinceLastPing(configServerLocalTime - pingInfo->configLocalTime);
    if (elapsedSinceLastPing >= _lockExpiration) {
        LOG(0) << "forcing lock '" << lockDoc.getName() << "' because elapsed time "
               << duration_cast<milliseconds>(elapsedSinceLastPing).count()
               << " ms >= takeover time " << duration_cast<milliseconds>(_lockExpiration).count()
               << " ms";
        return true;
    }

    LOG(1) << "could not force lock '" << lockDoc.getName() << "' because elapsed time "
           << duration_cast<milliseconds>(elapsedSinceLastPing).count() << " ms < takeover time "
           << duration_cast<milliseconds>(_lockExpiration).count() << " ms";
    return false;
}

StatusWith<DistLockManager::ScopedDistLock> ReplSetDistLockManager::lock(
    StringData name, StringData whyMessage, milliseconds waitFor, milliseconds lockTryInterval) {
    Timer timer(_serviceContext->getTickSource());
    Timer msgTimer(_serviceContext->getTickSource());

    while (waitFor <= milliseconds::zero() || milliseconds(timer.millis()) < waitFor) {
        OID lockSessionID = OID::gen();
        string who = str::stream() << _processID << ":" << getThreadName();

        LOG(1) << "trying to acquire new distributed lock for " << name
               << " ( lock timeout : " << duration_cast<milliseconds>(_lockExpiration).count()
               << " ms, ping interval : " << duration_cast<milliseconds>(_pingInterval).count()
               << " ms, process : " << _processID << " )"
               << " with lockSessionID: " << lockSessionID << ", why: " << whyMessage;

        auto lockResult =
            _catalog->grabLock(name, lockSessionID, who, _processID, Date_t::now(), whyMessage);

        auto status = lockResult.getStatus();

        if (status.isOK()) {
            // Lock is acquired since findAndModify was able to successfully modify
            // the lock document.
            LOG(0) << "distributed lock '" << name << "' acquired, ts : " << lockSessionID;
            return ScopedDistLock(lockSessionID, this);
        }

        if (status != ErrorCodes::LockStateChangeFailed) {
            // An error occurred but the write might have actually been applied on the
            // other side. Schedule an unlock to clean it up just in case.
            queueUnlock(lockSessionID);
            return status;
        }

        // Get info from current lock and check if we can overtake it.
        auto getLockStatusResult = _catalog->getLockByName(name);
        const auto& getLockStatus = getLockStatusResult.getStatus();

        if (!getLockStatusResult.isOK() && getLockStatus != ErrorCodes::LockNotFound) {
            return getLockStatus;
        }

        // Note: Only attempt to overtake locks that actually exists. If lock was not
        // found, use the normal grab lock path to acquire it.
        if (getLockStatusResult.isOK()) {
            auto currentLock = getLockStatusResult.getValue();
            auto canOvertakeResult = canOvertakeLock(currentLock);

            if (!canOvertakeResult.isOK()) {
                return canOvertakeResult.getStatus();
            }

            if (canOvertakeResult.getValue()) {
                auto overtakeResult = _catalog->overtakeLock(name,
                                                             lockSessionID,
                                                             currentLock.getLockID(),
                                                             who,
                                                             _processID,
                                                             Date_t::now(),
                                                             whyMessage);

                const auto& overtakeStatus = overtakeResult.getStatus();

                if (overtakeResult.isOK()) {
                    // Lock is acquired since findAndModify was able to successfully modify
                    // the lock document.

                    LOG(0) << "lock '" << name << "' successfully forced";
                    LOG(0) << "distributed lock '" << name << "' acquired, ts : " << lockSessionID;
                    return ScopedDistLock(lockSessionID, this);
                }

                if (overtakeStatus != ErrorCodes::LockStateChangeFailed) {
                    // An error occurred but the write might have actually been applied on the
                    // other side. Schedule an unlock to clean it up just in case.
                    queueUnlock(lockSessionID);
                    return overtakeStatus;
                }
            }
        }

        LOG(1) << "distributed lock '" << name << "' was not acquired.";

        if (waitFor == milliseconds::zero()) {
            break;
        }

        // Periodically message for debugging reasons
        if (msgTimer.seconds() > 10) {
            LOG(0) << "waited " << timer.seconds() << "s for distributed lock " << name << " for "
                   << whyMessage;

            msgTimer.reset();
        }

        milliseconds timeRemaining =
            std::max(milliseconds::zero(), waitFor - milliseconds(timer.millis()));
        sleepFor(std::min(lockTryInterval, timeRemaining));
    }

    return {ErrorCodes::LockBusy, str::stream() << "timed out waiting for " << name};
}

void ReplSetDistLockManager::unlock(const DistLockHandle& lockSessionID) {
    auto unlockStatus = _catalog->unlock(lockSessionID);

    if (!unlockStatus.isOK()) {
        queueUnlock(lockSessionID);
    } else {
        LOG(0) << "distributed lock with " << LocksType::lockID() << ": " << lockSessionID
               << "' unlocked.";
    }
}

Status ReplSetDistLockManager::checkStatus(const DistLockHandle& lockHandle) {
    return _catalog->getLockByTS(lockHandle).getStatus();
}

void ReplSetDistLockManager::queueUnlock(const DistLockHandle& lockSessionID) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _unlockList.push_back(lockSessionID);
}
}
