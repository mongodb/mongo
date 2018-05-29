/**
 *    Copyright (C) 2017 MongoDB Inc.
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
#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/keys_collection_manager.h"

#include "mongo/db/key_generator.h"
#include "mongo/db/keys_collection_cache.h"
#include "mongo/db/keys_collection_client.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/time_support.h"

namespace mongo {

const Seconds KeysCollectionManager::kKeyValidInterval{3 * 30 * 24 * 60 * 60};  // ~3 months
const std::string KeysCollectionManager::kKeyManagerPurposeString = "HMAC";

MONGO_EXPORT_STARTUP_SERVER_PARAMETER(KeysRotationIntervalSec,
                                      int,
                                      KeysCollectionManager::kKeyValidInterval.count());

namespace {

Milliseconds kDefaultRefreshWaitTime(30 * 1000);
Milliseconds kRefreshIntervalIfErrored(200);
Milliseconds kMaxRefreshWaitTime(10 * 60 * 1000);

// Prevents the refresher thread from waiting longer than the given number of milliseconds, even on
// a successful refresh.
MONGO_FAIL_POINT_DEFINE(maxKeyRefreshWaitTimeOverrideMS);

/**
 * Returns the amount of time to wait until the monitoring thread should attempt to refresh again.
 */
Milliseconds howMuchSleepNeedFor(const LogicalTime& currentTime,
                                 const LogicalTime& latestExpiredAt,
                                 const Milliseconds& interval) {
    auto currentSecs = currentTime.asTimestamp().getSecs();
    auto expiredSecs = latestExpiredAt.asTimestamp().getSecs();

    if (currentSecs >= expiredSecs) {
        // This means that the last round didn't generate a usable key for the current time.
        // However, we don't want to poll too hard as well, so use a low interval.
        return kRefreshIntervalIfErrored;
    }

    auto millisBeforeExpire = 1000 * (expiredSecs - currentSecs);

    if (interval.count() <= millisBeforeExpire) {
        return interval;
    }

    return Milliseconds(millisBeforeExpire);
}

}  // unnamed namespace

KeysCollectionManager::KeysCollectionManager(std::string purpose,
                                             std::unique_ptr<KeysCollectionClient> client,
                                             Seconds keyValidForInterval)
    : _client(std::move(client)),
      _purpose(std::move(purpose)),
      _keyValidForInterval(keyValidForInterval),
      _keysCache(_purpose, _client.get()) {}


StatusWith<KeysCollectionDocument> KeysCollectionManager::getKeyForValidation(
    OperationContext* opCtx, long long keyId, const LogicalTime& forThisTime) {
    auto keyStatus = _getKeyWithKeyIdCheck(keyId, forThisTime);

    if (keyStatus != ErrorCodes::KeyNotFound) {
        return keyStatus;
    }

    _refresher.refreshNow(opCtx);

    return _getKeyWithKeyIdCheck(keyId, forThisTime);
}

StatusWith<KeysCollectionDocument> KeysCollectionManager::getKeyForSigning(
    OperationContext* opCtx, const LogicalTime& forThisTime) {
    return _getKey(forThisTime);
}

StatusWith<KeysCollectionDocument> KeysCollectionManager::_getKeyWithKeyIdCheck(
    long long keyId, const LogicalTime& forThisTime) {
    auto keyStatus = _keysCache.getKeyById(keyId, forThisTime);

    if (!keyStatus.isOK()) {
        return keyStatus;
    }

    return keyStatus.getValue();
}

StatusWith<KeysCollectionDocument> KeysCollectionManager::_getKey(const LogicalTime& forThisTime) {
    auto keyStatus = _keysCache.getKey(forThisTime);

    if (!keyStatus.isOK()) {
        return keyStatus;
    }

    const auto& key = keyStatus.getValue();

    if (key.getExpiresAt() < forThisTime) {
        return {ErrorCodes::KeyNotFound,
                str::stream() << "No keys found for " << _purpose << " that is valid for "
                              << forThisTime.toString()};
    }

    return key;
}

void KeysCollectionManager::refreshNow(OperationContext* opCtx) {
    _refresher.refreshNow(opCtx);
}

void KeysCollectionManager::startMonitoring(ServiceContext* service) {
    _keysCache.resetCache();
    _refresher.setFunc([this](OperationContext* opCtx) { return _keysCache.refresh(opCtx); });
    _refresher.start(
        service, str::stream() << "monitoring keys for " << _purpose, _keyValidForInterval);
}

void KeysCollectionManager::stopMonitoring() {
    _refresher.stop();
}

void KeysCollectionManager::enableKeyGenerator(OperationContext* opCtx, bool doEnable) {
    if (doEnable) {
        _refresher.switchFunc(opCtx, [this](OperationContext* opCtx) {
            KeyGenerator keyGenerator(_purpose, _client.get(), _keyValidForInterval);
            auto keyGenerationStatus = keyGenerator.generateNewKeysIfNeeded(opCtx);

            if (ErrorCodes::isShutdownError(keyGenerationStatus.code())) {
                return StatusWith<KeysCollectionDocument>(keyGenerationStatus);
            }

            // An error encountered by the keyGenerator should not prevent refreshing the cache
            auto cacheRefreshStatus = _keysCache.refresh(opCtx);

            if (!keyGenerationStatus.isOK()) {
                return StatusWith<KeysCollectionDocument>(keyGenerationStatus);
            }

            return cacheRefreshStatus;
        });
    } else {
        _refresher.switchFunc(
            opCtx, [this](OperationContext* opCtx) { return _keysCache.refresh(opCtx); });
    }
}

bool KeysCollectionManager::hasSeenKeys() {
    return _refresher.hasSeenKeys();
}

void KeysCollectionManager::clearCache() {
    _keysCache.resetCache();
}

void KeysCollectionManager::PeriodicRunner::refreshNow(OperationContext* opCtx) {
    auto refreshRequest = [this]() {
        stdx::lock_guard<stdx::mutex> lk(_mutex);

        if (_inShutdown) {
            uasserted(ErrorCodes::ShutdownInProgress,
                      "aborting keys cache refresh because node is shutting down");
        }

        if (_refreshRequest) {
            return _refreshRequest;
        }

        _refreshNeededCV.notify_all();
        _refreshRequest = std::make_shared<Notification<void>>();
        return _refreshRequest;
    }();

    // note: waitFor waits min(maxTimeMS, kDefaultRefreshWaitTime).
    // waitFor also throws if timeout, so also throw when notification was not satisfied after
    // waiting.
    if (!refreshRequest->waitFor(opCtx, kDefaultRefreshWaitTime)) {
        uasserted(ErrorCodes::ExceededTimeLimit, "timed out waiting for refresh");
    }
}

void KeysCollectionManager::PeriodicRunner::_doPeriodicRefresh(ServiceContext* service,
                                                               std::string threadName,
                                                               Milliseconds refreshInterval) {
    Client::initThreadIfNotAlready(threadName);

    while (true) {
        bool hasRefreshRequestInitially = false;
        unsigned errorCount = 0;
        std::shared_ptr<RefreshFunc> doRefresh;
        {
            stdx::lock_guard<stdx::mutex> lock(_mutex);

            if (_inShutdown) {
                break;
            }

            invariant(_doRefresh.get() != nullptr);
            doRefresh = _doRefresh;
            hasRefreshRequestInitially = _refreshRequest.get() != nullptr;
        }

        Milliseconds nextWakeup = kRefreshIntervalIfErrored;

        {
            auto opCtx = cc().makeOperationContext();

            auto latestKeyStatusWith = (*doRefresh)(opCtx.get());
            if (latestKeyStatusWith.getStatus().isOK()) {
                errorCount = 0;
                const auto& latestKey = latestKeyStatusWith.getValue();
                auto currentTime = LogicalClock::get(service)->getClusterTime();

                {
                    stdx::unique_lock<stdx::mutex> lock(_mutex);
                    _hasSeenKeys = true;
                }

                nextWakeup =
                    howMuchSleepNeedFor(currentTime, latestKey.getExpiresAt(), refreshInterval);
            } else {
                errorCount += 1;
                nextWakeup = Milliseconds(kRefreshIntervalIfErrored.count() * errorCount);
                if (nextWakeup > kMaxRefreshWaitTime) {
                    nextWakeup = kMaxRefreshWaitTime;
                }
            }
        }

        MONGO_FAIL_POINT_BLOCK(maxKeyRefreshWaitTimeOverrideMS, data) {
            const BSONObj& dataObj = data.getData();
            auto overrideMS = Milliseconds(dataObj["overrideMS"].numberInt());
            if (nextWakeup > overrideMS) {
                nextWakeup = overrideMS;
            }
        }

        stdx::unique_lock<stdx::mutex> lock(_mutex);

        if (_refreshRequest) {
            if (!hasRefreshRequestInitially) {
                // A fresh request came in, fulfill the request before going to sleep.
                continue;
            }

            _refreshRequest->set();
            _refreshRequest.reset();
        }

        if (_inShutdown) {
            break;
        }

        // Use a new opCtx so we won't be holding any RecoveryUnit while this thread goes to sleep.
        auto opCtx = cc().makeOperationContext();

        MONGO_IDLE_THREAD_BLOCK;
        auto sleepStatus = opCtx->waitForConditionOrInterruptNoAssertUntil(
            _refreshNeededCV, lock, Date_t::now() + nextWakeup);

        if (ErrorCodes::isShutdownError(sleepStatus.getStatus().code())) {
            break;
        }
    }

    stdx::unique_lock<stdx::mutex> lock(_mutex);
    if (_refreshRequest) {
        _refreshRequest->set();
        _refreshRequest.reset();
    }
}

void KeysCollectionManager::PeriodicRunner::setFunc(RefreshFunc newRefreshStrategy) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    _doRefresh = std::make_shared<RefreshFunc>(std::move(newRefreshStrategy));
    _refreshNeededCV.notify_all();
}

void KeysCollectionManager::PeriodicRunner::switchFunc(OperationContext* opCtx,
                                                       RefreshFunc newRefreshStrategy) {
    setFunc(newRefreshStrategy);
}

void KeysCollectionManager::PeriodicRunner::start(ServiceContext* service,
                                                  const std::string& threadName,
                                                  Milliseconds refreshInterval) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    invariant(!_backgroundThread.joinable());
    invariant(!_inShutdown);

    _backgroundThread = stdx::thread([this, service, threadName, refreshInterval] {
        _doPeriodicRefresh(service, threadName, refreshInterval);
    });
}

void KeysCollectionManager::PeriodicRunner::stop() {
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        if (!_backgroundThread.joinable()) {
            return;
        }

        _inShutdown = true;
        _hasSeenKeys = false;
        _refreshNeededCV.notify_all();
    }

    _backgroundThread.join();
}

bool KeysCollectionManager::PeriodicRunner::hasSeenKeys() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _hasSeenKeys;
}

}  // namespace mongo
