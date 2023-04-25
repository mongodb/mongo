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

#include "mongo/platform/basic.h"

#include "mongo/db/keys_collection_manager.h"

#include <memory>

#include "mongo/db/key_generator.h"
#include "mongo/db/keys_collection_cache.h"
#include "mongo/db/keys_collection_client.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/vector_clock.h"
#include "mongo/logv2/log.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {

const std::string KeysCollectionManager::kKeyManagerPurposeString = "HMAC";

namespace {

Milliseconds kDefaultRefreshWaitTime(30 * 1000);
Milliseconds kRefreshIntervalIfErrored(200);
Milliseconds kMaxRefreshWaitTimeIfErrored(5 * 60 * 1000);
// Never wait more than the number of milliseconds in 20 days to avoid sleeping for a number greater
// than can fit in a signed 32 bit integer.
// 20 days = 1000 * 60 * 60 * 24 * 20 = 1,728,000,000 vs signed integer max of 2,147,483,648.
Milliseconds kMaxRefreshWaitTimeOnSuccess(Days(20));

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
        return kRefreshIntervalIfErrored;
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
        _refresher.refreshNow(opCtx);
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
    _refresher.refreshNow(opCtx);
}

void KeysCollectionManager::startMonitoring(ServiceContext* service) {
    _keysCache.resetCache();
    _refresher.setFunc([this](OperationContext* opCtx) { return _keysCache.refresh(opCtx); });
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
} catch (const ExceptionForCat<ErrorCategory::ShutdownError>& ex) {
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

void KeysCollectionManager::PeriodicRunner::refreshNow(OperationContext* opCtx) {
    auto refreshRequest = [this]() {
        stdx::lock_guard<Latch> lk(_mutex);

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
    ThreadClient tc(threadName, service);

    {
        stdx::lock_guard<Client> lk(*tc.get());
        tc.get()->setSystemOperationKillableByStepdown(lk);
    }

    ON_BLOCK_EXIT([this]() mutable { _hasSeenKeys.store(false); });

    unsigned errorCount = 0;
    while (true) {
        decltype(_refreshRequest) request;
        std::shared_ptr<RefreshFunc> doRefresh;
        {
            stdx::lock_guard<Latch> lock(_mutex);

            if (_inShutdown) {
                break;
            }

            invariant(_doRefresh.get() != nullptr);
            doRefresh = _doRefresh;
            request = std::move(_refreshRequest);
        }

        Milliseconds nextWakeup = kRefreshIntervalIfErrored;

        {
            auto opCtx = cc().makeOperationContext();

            auto latestKeyStatusWith = (*doRefresh)(opCtx.get());
            if (latestKeyStatusWith.getStatus().isOK()) {
                errorCount = 0;
                const auto& latestKey = latestKeyStatusWith.getValue();
                const auto currentTime = VectorClock::get(service)->getTime();

                _hasSeenKeys.store(true);

                nextWakeup = keys_collection_manager_util::howMuchSleepNeedFor(
                    currentTime.clusterTime(), latestKey.getExpiresAt(), refreshInterval);
            } else {
                errorCount += 1;
                nextWakeup = Milliseconds(kRefreshIntervalIfErrored.count() * errorCount);
                if (nextWakeup > kMaxRefreshWaitTimeIfErrored) {
                    nextWakeup = kMaxRefreshWaitTimeIfErrored;
                }
                LOGV2(4939300,
                      "Failed to refresh key cache",
                      "error"_attr = redact(latestKeyStatusWith.getStatus()),
                      "nextWakeup"_attr = nextWakeup);
            }

            // Notify all waiters that the refresh has finished and they can move on
            if (request) {
                request->set();
            }
        }

        maxKeyRefreshWaitTimeOverrideMS.execute([&](const BSONObj& data) {
            nextWakeup = std::min(nextWakeup, Milliseconds(data["overrideMS"].numberInt()));
        });

        stdx::unique_lock<Latch> lock(_mutex);
        if (_refreshRequest) {
            // A fresh request came in, fulfill the request before going to sleep.
            continue;
        }

        if (_inShutdown) {
            break;
        }

        // Use a new opCtx so we won't be holding any RecoveryUnit while this thread goes to sleep.
        auto opCtx = cc().makeOperationContext();

        MONGO_IDLE_THREAD_BLOCK;
        try {
            opCtx->waitForConditionOrInterruptFor(
                _refreshNeededCV, lock, nextWakeup, [&]() -> bool {
                    return _inShutdown || _refreshRequest;
                });
        } catch (const DBException& e) {
            LOGV2_DEBUG(20705, 1, "Unable to wait for refresh request due to: {e}", "e"_attr = e);

            if (ErrorCodes::isShutdownError(e)) {
                return;
            }
        }
    }
}

void KeysCollectionManager::PeriodicRunner::setFunc(RefreshFunc newRefreshStrategy) {
    stdx::lock_guard<Latch> lock(_mutex);
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
    stdx::lock_guard<Latch> lock(_mutex);
    invariant(!_backgroundThread.joinable());
    invariant(!_inShutdown);

    _backgroundThread = stdx::thread([this, service, threadName, refreshInterval] {
        _doPeriodicRefresh(service, threadName, refreshInterval);
    });
}

void KeysCollectionManager::PeriodicRunner::stop() {
    {
        stdx::lock_guard<Latch> lock(_mutex);
        if (!_backgroundThread.joinable()) {
            return;
        }

        _inShutdown = true;
        _refreshNeededCV.notify_all();
    }

    _backgroundThread.join();
}

bool KeysCollectionManager::PeriodicRunner::hasSeenKeys() const noexcept {
    return _hasSeenKeys.load();
}

bool KeysCollectionManager::PeriodicRunner::isInShutdown() const {
    stdx::lock_guard<Latch> lock(_mutex);
    return _inShutdown;
}

}  // namespace mongo
