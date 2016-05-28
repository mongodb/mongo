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
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/service_context.h"
#include "mongo/s/catalog/dist_lock_catalog.h"
#include "mongo/s/catalog/type_lockpings.h"
#include "mongo/s/catalog/type_locks.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/chrono.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

namespace mongo {

MONGO_FP_DECLARE(setDistLockTimeout);

using std::string;
using std::unique_ptr;

namespace {

// How many times to retry acquiring the lock after the first attempt fails
const int kMaxNumLockAcquireRetries = 2;

}  // namespace

const Seconds ReplSetDistLockManager::kDistLockPingInterval{30};
const Minutes ReplSetDistLockManager::kDistLockExpirationTime{15};

ReplSetDistLockManager::ReplSetDistLockManager(ServiceContext* globalContext,
                                               StringData processID,
                                               unique_ptr<DistLockCatalog> catalog,
                                               Milliseconds pingInterval,
                                               Milliseconds lockExpiration)
    : _serviceContext(globalContext),
      _processID(processID.toString()),
      _catalog(std::move(catalog)),
      _pingInterval(pingInterval),
      _lockExpiration(lockExpiration) {}

ReplSetDistLockManager::~ReplSetDistLockManager() = default;

void ReplSetDistLockManager::startUp() {
    if (!_execThread) {
        _execThread = stdx::make_unique<stdx::thread>(&ReplSetDistLockManager::doTask, this);
    }
}

void ReplSetDistLockManager::shutDown(OperationContext* txn) {
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

    auto status = _catalog->stopPing(txn, _processID);
    if (!status.isOK()) {
        warning() << "error encountered while cleaning up distributed ping entry for " << _processID
                  << causedBy(status);
    }
}

std::string ReplSetDistLockManager::getProcessID() {
    return _processID;
}

bool ReplSetDistLockManager::isShutDown() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _isShutDown;
}

void ReplSetDistLockManager::doTask() {
    LOG(0) << "creating distributed lock ping thread for process " << _processID
           << " (sleeping for " << _pingInterval << ")";

    Timer elapsedSincelastPing(_serviceContext->getTickSource());
    Client::initThread("replSetDistLockPinger");

    while (!isShutDown()) {
        {
            auto txn = cc().makeOperationContext();
            auto pingStatus = _catalog->ping(txn.get(), _processID, Date_t::now());

            if (!pingStatus.isOK()) {
                warning() << "pinging failed for distributed lock pinger" << causedBy(pingStatus);
            }

            const Milliseconds elapsed(elapsedSincelastPing.millis());
            if (elapsed > 10 * _pingInterval) {
                warning() << "Lock pinger for proc: " << _processID << " was inactive for "
                          << elapsed << " ms";
            }
            elapsedSincelastPing.reset();

            std::deque<DistLockHandle> toUnlockBatch;
            {
                stdx::unique_lock<stdx::mutex> lk(_mutex);
                toUnlockBatch.swap(_unlockList);
            }

            for (const auto& toUnlock : toUnlockBatch) {
                auto unlockStatus = _catalog->unlock(txn.get(), toUnlock);

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
        }

        stdx::unique_lock<stdx::mutex> lk(_mutex);
        _shutDownCV.wait_for(lk, _pingInterval.toSystemDuration());
    }
}

StatusWith<bool> ReplSetDistLockManager::isLockExpired(OperationContext* txn,
                                                       LocksType lockDoc,
                                                       const Milliseconds& lockExpiration) {
    const auto& processID = lockDoc.getProcess();
    auto pingStatus = _catalog->getPing(txn, processID);

    Date_t pingValue;
    if (pingStatus.isOK()) {
        const auto& pingDoc = pingStatus.getValue();
        Status pingDocValidationStatus = pingDoc.validate();
        if (!pingDocValidationStatus.isOK()) {
            return {ErrorCodes::UnsupportedFormat,
                    str::stream() << "invalid ping document for " << processID << ": "
                                  << pingDocValidationStatus.toString()};
        }

        pingValue = pingDoc.getPing();
    } else if (pingStatus.getStatus() != ErrorCodes::NoMatchingDocument) {
        return pingStatus.getStatus();
    }  // else use default pingValue if ping document does not exist.

    Timer timer(_serviceContext->getTickSource());
    auto serverInfoStatus = _catalog->getServerInfo(txn);
    if (!serverInfoStatus.isOK()) {
        return serverInfoStatus.getStatus();
    }

    // Be conservative when determining that lock expiration has elapsed by
    // taking into account the roundtrip delay of trying to get the local
    // time from the config server.
    Milliseconds delay(timer.millis() / 2);  // Assuming symmetrical delay.

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

    Milliseconds elapsedSinceLastPing(configServerLocalTime - pingInfo->configLocalTime);
    if (elapsedSinceLastPing >= lockExpiration) {
        LOG(0) << "forcing lock '" << lockDoc.getName() << "' because elapsed time "
               << elapsedSinceLastPing << " >= takeover time " << lockExpiration;
        return true;
    }

    LOG(1) << "could not force lock '" << lockDoc.getName() << "' because elapsed time "
           << durationCount<Milliseconds>(elapsedSinceLastPing) << " < takeover time "
           << durationCount<Milliseconds>(lockExpiration) << " ms";
    return false;
}

StatusWith<DistLockManager::ScopedDistLock> ReplSetDistLockManager::lock(
    OperationContext* txn,
    StringData name,
    StringData whyMessage,
    Milliseconds waitFor,
    Milliseconds lockTryInterval) {
    return lockWithSessionID(txn, name, whyMessage, OID::gen(), waitFor, lockTryInterval);
}

StatusWith<DistLockManager::ScopedDistLock> ReplSetDistLockManager::lockWithSessionID(
    OperationContext* txn,
    StringData name,
    StringData whyMessage,
    const OID lockSessionID,
    Milliseconds waitFor,
    Milliseconds lockTryInterval) {
    Timer timer(_serviceContext->getTickSource());
    Timer msgTimer(_serviceContext->getTickSource());

    // Counts how many attempts have been made to grab the lock, which have failed with network
    // error. This value is reset for each lock acquisition attempt because these are
    // independent write operations.
    int networkErrorRetries = 0;

    auto configShard = Grid::get(txn)->shardRegistry()->getConfigShard();
    // Distributed lock acquisition works by tring to update the state of the lock to 'taken'. If
    // the lock is currently taken, we will back off and try the acquisition again, repeating this
    // until the lockTryInterval has been reached. If a network error occurs at each lock
    // acquisition attempt, the lock acquisition will be retried immediately.
    while (waitFor <= Milliseconds::zero() || Milliseconds(timer.millis()) < waitFor) {
        const string who = str::stream() << _processID << ":" << getThreadName();

        auto lockExpiration = _lockExpiration;
        MONGO_FAIL_POINT_BLOCK(setDistLockTimeout, customTimeout) {
            const BSONObj& data = customTimeout.getData();
            lockExpiration = Milliseconds(data["timeoutMs"].numberInt());
        }

        LOG(1) << "trying to acquire new distributed lock for " << name
               << " ( lock timeout : " << durationCount<Milliseconds>(lockExpiration)
               << " ms, ping interval : " << durationCount<Milliseconds>(_pingInterval)
               << " ms, process : " << _processID << " )"
               << " with lockSessionID: " << lockSessionID << ", why: " << whyMessage;

        auto lockResult = _catalog->grabLock(
            txn, name, lockSessionID, who, _processID, Date_t::now(), whyMessage);

        auto status = lockResult.getStatus();

        if (status.isOK()) {
            // Lock is acquired since findAndModify was able to successfully modify
            // the lock document.
            log() << "distributed lock '" << name << "' acquired for '" << whyMessage
                  << "', ts : " << lockSessionID;
            return ScopedDistLock(txn, lockSessionID, this);
        }

        // If a network error occurred, unlock the lock synchronously and try again
        if (configShard->isRetriableError(status.code(), Shard::RetryPolicy::kIdempotent) &&
            networkErrorRetries < kMaxNumLockAcquireRetries) {
            LOG(1) << "Failed to acquire distributed lock because of retriable error. Retrying "
                      "acquisition by first unlocking the stale entry, which possibly exists now"
                   << causedBy(status);

            networkErrorRetries++;

            status = _catalog->unlock(txn, lockSessionID);
            if (status.isOK()) {
                // We certainly do not own the lock, so we can retry
                continue;
            }

            // Fall-through to the error checking logic below
            invariant(status != ErrorCodes::LockStateChangeFailed);

            LOG(1)
                << "Failed to retry acqusition of distributed lock. No more attempts will be made"
                << causedBy(status);
        }

        if (status != ErrorCodes::LockStateChangeFailed) {
            // An error occurred but the write might have actually been applied on the
            // other side. Schedule an unlock to clean it up just in case.
            queueUnlock(lockSessionID);
            return status;
        }

        // Get info from current lock and check if we can overtake it.
        auto getLockStatusResult = _catalog->getLockByName(txn, name);
        const auto& getLockStatus = getLockStatusResult.getStatus();

        if (!getLockStatusResult.isOK() && getLockStatus != ErrorCodes::LockNotFound) {
            return getLockStatus;
        }

        // Note: Only attempt to overtake locks that actually exists. If lock was not
        // found, use the normal grab lock path to acquire it.
        if (getLockStatusResult.isOK()) {
            auto currentLock = getLockStatusResult.getValue();
            auto isLockExpiredResult = isLockExpired(txn, currentLock, lockExpiration);

            if (!isLockExpiredResult.isOK()) {
                return isLockExpiredResult.getStatus();
            }

            if (isLockExpiredResult.getValue() || (lockSessionID == currentLock.getLockID())) {
                auto overtakeResult = _catalog->overtakeLock(txn,
                                                             name,
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
                    return ScopedDistLock(txn, lockSessionID, this);
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

        if (waitFor == Milliseconds::zero()) {
            break;
        }

        // Periodically message for debugging reasons
        if (msgTimer.seconds() > 10) {
            LOG(0) << "waited " << timer.seconds() << "s for distributed lock " << name << " for "
                   << whyMessage;

            msgTimer.reset();
        }

        // A new lock acquisition attempt will begin now (because the previous found the lock to be
        // busy, so reset the retries counter)
        networkErrorRetries = 0;

        const Milliseconds timeRemaining =
            std::max(Milliseconds::zero(), waitFor - Milliseconds(timer.millis()));
        sleepFor(std::min(lockTryInterval, timeRemaining));
    }

    return {ErrorCodes::LockBusy, str::stream() << "timed out waiting for " << name};
}

void ReplSetDistLockManager::unlock(OperationContext* txn, const DistLockHandle& lockSessionID) {
    auto unlockStatus = _catalog->unlock(txn, lockSessionID);

    if (!unlockStatus.isOK()) {
        queueUnlock(lockSessionID);
    } else {
        LOG(0) << "distributed lock with " << LocksType::lockID() << ": " << lockSessionID
               << "' unlocked.";
    }
}

void ReplSetDistLockManager::unlockAll(OperationContext* txn, const std::string& processID) {
    Status status = _catalog->unlockAll(txn, processID);
    if (!status.isOK()) {
        warning() << "Error while trying to unlock existing distributed locks" << causedBy(status);
    }
}

Status ReplSetDistLockManager::checkStatus(OperationContext* txn,
                                           const DistLockHandle& lockHandle) {
    return _catalog->getLockByTS(txn, lockHandle).getStatus();
}

void ReplSetDistLockManager::queueUnlock(const DistLockHandle& lockSessionID) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _unlockList.push_back(lockSessionID);
}

}  // namespace mongo
