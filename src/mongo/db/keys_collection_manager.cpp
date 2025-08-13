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

#include "mongo/db/keys_collection_manager.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/key_generator.h"
#include "mongo/db/keys_collection_cache.h"
#include "mongo/db/keys_collection_client.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/vector_clock/vector_clock.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <utility>

#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {

const unsigned KeysCollectionManager::kReadConcernMajorityNotAvailableYetMaxTries = 10;
const Milliseconds KeysCollectionManager::kRefreshIntervalIfErrored(200);
const std::string KeysCollectionManager::kKeyManagerPurposeString = "HMAC";

namespace {

Milliseconds kDefaultRefreshWaitTime(30 * 1000);
Milliseconds kMaxRefreshWaitTimeIfErrored(5 * 60 * 1000);
// Never wait more than the number of milliseconds in 20 days to avoid sleeping for a number greater
// than can fit in a signed 32 bit integer.
// 20 days = 1000 * 60 * 60 * 24 * 20 = 1,728,000,000 vs signed integer max of 2,147,483,648.
Milliseconds kMaxRefreshWaitTimeOnSuccess(Days(20));

MONGO_FAIL_POINT_DEFINE(keyRefreshFailWithReadConcernMajorityNotAvailableYet);
// Prevents the refresher thread from waiting longer than the given number of milliseconds, even on
// a successful refresh.
MONGO_FAIL_POINT_DEFINE(maxKeyRefreshWaitTimeOverrideMS);

}  // unnamed namespace

namespace keys_collection_manager_util {

Milliseconds howMuchSleepNeedFor(const LogicalTime& currentTime,
                                 const LogicalTime& latestExpiredAt,
                                 const Milliseconds& interval) {
    auto currentSecs = Seconds(currentTime.asTimestamp().getSecs());
    auto expiredSecs = Seconds(latestExpiredAt.asTimestamp().getSecs());

    if (currentSecs >= expiredSecs) {
        // This means that the last round didn't generate a usable key for the current time.
        // However, we don't want to poll too hard as well, so use a low interval.
        return KeysCollectionManager::kRefreshIntervalIfErrored;
    }

    Milliseconds millisBeforeExpire = Milliseconds(expiredSecs) - Milliseconds(currentSecs);

    return std::min({millisBeforeExpire, interval, kMaxRefreshWaitTimeOnSuccess});
}

}  // namespace keys_collection_manager_util

KeysCollectionManager::KeysCollectionManager(std::string purpose,
                                             std::unique_ptr<KeysCollectionClient> client,
                                             Seconds keyValidForInterval)
    : _client(std::move(client)),
      _purpose(std::move(purpose)),
      _keyValidForInterval(keyValidForInterval),
      _keysCache(_purpose, _client.get()) {}


StatusWith<std::vector<KeysCollectionDocument>> KeysCollectionManager::getKeysForValidation(
    OperationContext* opCtx, long long keyId, const LogicalTime& forThisTime) {
    auto swInternalKey = _keysCache.getInternalKeyById(keyId, forThisTime);

    if (swInternalKey == ErrorCodes::KeyNotFound) {
        refreshNow(opCtx);
        swInternalKey = _keysCache.getInternalKeyById(keyId, forThisTime);
    }

    std::vector<KeysCollectionDocument> keys;

    if (swInternalKey.isOK()) {
        keys.push_back(std::move(swInternalKey.getValue()));
    }

    auto swExternalKeys = _keysCache.getExternalKeysById(keyId, forThisTime);

    if (swExternalKeys.isOK()) {
        for (auto& externalKey : swExternalKeys.getValue()) {
            KeysCollectionDocument key(externalKey.getKeyId());
            key.setKeysCollectionDocumentBase(externalKey.getKeysCollectionDocumentBase());
            keys.push_back(std::move(key));
        };
    }

    if (keys.empty()) {
        return {ErrorCodes::KeyNotFound,
                str::stream() << "No keys found for " << _purpose << " that is valid for time: "
                              << forThisTime.toString() << " with id: " << keyId};
    }

    return std::move(keys);
}

StatusWith<KeysCollectionDocument> KeysCollectionManager::getKeyForSigning(
    OperationContext* opCtx, const LogicalTime& forThisTime) {
    auto swKey = _keysCache.getInternalKey(forThisTime);

    if (!swKey.isOK()) {
        return swKey;
    }

    const auto& key = swKey.getValue();

    if (key.getExpiresAt() < forThisTime) {
        return {ErrorCodes::KeyNotFound,
                str::stream() << "No keys found for " << _purpose << " that is valid for "
                              << forThisTime.toString()};
    }

    return key;
}

void KeysCollectionManager::refreshNow(OperationContext* opCtx) {
    auto refreshRequest = _refresher.requestRefreshAsync(opCtx);
    // note: waitFor waits min(maxTimeMS, kDefaultRefreshWaitTime).
    // waitFor also throws if timeout, so also throw when notification was not satisfied after
    // waiting.
    if (!refreshRequest->waitFor(opCtx, kDefaultRefreshWaitTime)) {
        uasserted(ErrorCodes::ExceededTimeLimit, "timed out waiting for refresh");
    }
}

void KeysCollectionManager::requestRefreshAsync(OperationContext* opCtx) {
    _refresher.requestRefreshAsync(opCtx);
}

void KeysCollectionManager::startMonitoring(ServiceContext* service) {
    _keysCache.resetCache();
    _refresher.setFunc([this](OperationContext* opCtx) -> StatusWith<KeysCollectionDocument> {
        if (MONGO_unlikely(keyRefreshFailWithReadConcernMajorityNotAvailableYet.shouldFail())) {
            return {ErrorCodes::ReadConcernMajorityNotAvailableYet, "Failing keys cache refresh"};
        }
        return _keysCache.refresh(opCtx);
    });
    _refresher.start(
        service, str::stream() << "monitoring-keys-for-" << _purpose, _keyValidForInterval);
}

void KeysCollectionManager::stopMonitoring() {
    _refresher.stop();
}

void KeysCollectionManager::enableKeyGenerator(OperationContext* opCtx, bool doEnable) try {
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
} catch (const ExceptionFor<ErrorCategory::ShutdownError>& ex) {
    LOGV2(518091, "Exception during key generation", "error"_attr = ex, "enable"_attr = doEnable);
    return;
}

bool KeysCollectionManager::hasSeenKeys() {
    return _refresher.hasSeenKeys();
}

void KeysCollectionManager::clearCache() {
    _keysCache.resetCache();
}

void KeysCollectionManager::cacheExternalKey(ExternalKeysCollectionDocument key) {
    // If the refresher has been shut down, we don't cache external keys because refresh is relied
    // on to clear expired keys. This is OK because the refresher is only shut down in cases where
    // keys aren't needed, like on an arbiter.
    if (!_refresher.isInShutdown()) {
        _keysCache.cacheExternalKey(std::move(key));
    }
}

std::shared_ptr<Notification<void>> KeysCollectionManager::PeriodicRunner::requestRefreshAsync(
    OperationContext* opCtx) {
    auto refreshRequest = [this]() {
        stdx::lock_guard<stdx::mutex> lk(_mutex);

        if (_inShutdown) {
            uasserted(ErrorCodes::ShutdownInProgress,
                      "aborting keys cache refresh because node is shutting down");
        }

        if (!_refreshRequest) {
            _refreshRequest = std::make_shared<Notification<void>>();
        }
        _refreshNeededCV.notify_all();
        return _refreshRequest;
    }();
    return refreshRequest;
}

void KeysCollectionManager::PeriodicRunner::_doPeriodicRefresh(ServiceContext* service,
                                                               std::string threadName,
                                                               Milliseconds refreshInterval) {
    ThreadClient tc(threadName, service->getService());
    ON_BLOCK_EXIT([this]() mutable { _hasSeenKeys.store(false); });

    // The helper for sleeping for the specified duration or until this node shuts down or there is
    // a new refresh request. Returns true if there is a shutdown error during the sleep.
    auto waitUntilShutDownOrNextRequest = [&](stdx::unique_lock<stdx::mutex>& lock,
                                              Milliseconds duration) {
        maxKeyRefreshWaitTimeOverrideMS.execute([&](const BSONObj& data) {
            duration = std::min(duration, Milliseconds(data["overrideMS"].numberInt()));
        });

        // Use a new opCtx so we won't be holding any RecoveryUnit while this thread goes to sleep.
        auto opCtx = cc().makeOperationContext();

        MONGO_IDLE_THREAD_BLOCK;
        try {
            opCtx->waitForConditionOrInterruptFor(_refreshNeededCV, lock, duration, [&]() -> bool {
                return _inShutdown || _refreshRequest;
            });
        } catch (const DBException& e) {
            LOGV2_DEBUG(20705, 1, "Unable to wait for refresh request due to: {e}", "e"_attr = e);
            if (ErrorCodes::isShutdownError(e)) {
                return true;
            }
        }
        // Possible that we were signaled by condvar in the shutdown of this class without the opCtx
        // being killed.
        return _inShutdown;
    };

    unsigned totalErrorCount = 0;
    unsigned readConcernMajorityNotAvailableYetErrorCount = 0;
    // Track all unfulfilled refresh requests.
    std::vector<decltype(_refreshRequest)> requests;

    while (true) {
        std::shared_ptr<RefreshFunc> doRefresh;
        {
            stdx::lock_guard<stdx::mutex> lock(_mutex);

            if (_inShutdown) {
                break;
            }

            invariant(_doRefresh.get() != nullptr);
            doRefresh = _doRefresh;
            if (_refreshRequest) {
                requests.push_back(std::move(_refreshRequest));
            }
        }

        Status latestKeyStatus = Status::OK();
        Milliseconds nextRefreshInterval = kRefreshIntervalIfErrored;

        {
            auto opCtx = cc().makeOperationContext();

            auto latestKeyStatusWith = (*doRefresh)(opCtx.get());
            latestKeyStatus = latestKeyStatusWith.getStatus();
            if (latestKeyStatus.isOK()) {
                totalErrorCount = 0;
                readConcernMajorityNotAvailableYetErrorCount = 0;
                const auto& latestKey = latestKeyStatusWith.getValue();
                const auto currentTime = VectorClock::get(service)->getTime();

                _hasSeenKeys.store(true);
                nextRefreshInterval = keys_collection_manager_util::howMuchSleepNeedFor(
                    currentTime.clusterTime(), latestKey.getExpiresAt(), refreshInterval);
            } else {
                totalErrorCount += 1;

                if (latestKeyStatus != ErrorCodes::NotWritablePrimary) {
                    nextRefreshInterval =
                        Milliseconds(kRefreshIntervalIfErrored.count() * totalErrorCount);
                    if (nextRefreshInterval > kMaxRefreshWaitTimeIfErrored) {
                        nextRefreshInterval = kMaxRefreshWaitTimeIfErrored;
                    }
                }
                LOGV2(4939300,
                      "Failed to refresh key cache",
                      "error"_attr = redact(latestKeyStatus),
                      "nextRefreshInterval"_attr = nextRefreshInterval);
            }
        }

        if (latestKeyStatus == ErrorCodes::ReadConcernMajorityNotAvailableYet &&
            readConcernMajorityNotAvailableYetErrorCount++ <
                kReadConcernMajorityNotAvailableYetMaxTries) {
            // This error means that this mongod has just restarted and no majority committed
            // snapshot is available yet. To avoid returning a KeyNotFound error to the waiters, try
            // refreshing again if the number of tries has not been exhausted.
            stdx::unique_lock<stdx::mutex> lock(_mutex);
            if (auto inShutdown = waitUntilShutDownOrNextRequest(lock, nextRefreshInterval);
                inShutdown) {
                return;
            }
            continue;
        }

        // Notify the waiters of all unfulfilled refresh requests the refresh has finished and they
        // can move on.
        for (auto& request : requests) {
            if (request) {
                request->set();
            }
        }
        requests.clear();

        stdx::unique_lock<stdx::mutex> lock(_mutex);
        if (_refreshRequest) {
            // A fresh request came in, fulfill the request before going to sleep.
            continue;
        }
        if (_inShutdown) {
            break;
        }
        if (auto inShutdown = waitUntilShutDownOrNextRequest(lock, nextRefreshInterval);
            inShutdown) {
            return;
        }
    }
}

void KeysCollectionManager::PeriodicRunner::setFunc(RefreshFunc newRefreshStrategy) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    if (_inShutdown) {
        uasserted(ErrorCodes::ShutdownInProgress,
                  "aborting KeysCollectionManager::PeriodicRunner::setFunc because node is "
                  "shutting down");
    }

    _doRefresh = std::make_shared<RefreshFunc>(std::move(newRefreshStrategy));
    if (!_refreshRequest) {
        _refreshRequest = std::make_shared<Notification<void>>();
    }
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
        _refreshNeededCV.notify_all();
    }

    _backgroundThread.join();
}

bool KeysCollectionManager::PeriodicRunner::hasSeenKeys() const {
    return _hasSeenKeys.load();
}

bool KeysCollectionManager::PeriodicRunner::isInShutdown() const {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    return _inShutdown;
}

}  // namespace mongo
