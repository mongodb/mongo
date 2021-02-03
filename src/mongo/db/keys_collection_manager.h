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

#pragma once

#include <functional>
#include <memory>

#include "mongo/base/status_with.h"
#include "mongo/db/key_generator.h"
#include "mongo/db/keys_collection_cache.h"
#include "mongo/db/keys_collection_document_gen.h"
#include "mongo/db/keys_collection_manager_gen.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/duration.h"

namespace mongo {

class OperationContext;
class LogicalTime;
class ServiceContext;
class KeysCollectionClient;

namespace keys_collection_manager_util {

/**
 * Returns the amount of time to wait until the monitoring thread should attempt to refresh again.
 */
Milliseconds howMuchSleepNeedFor(const LogicalTime& currentTime,
                                 const LogicalTime& latestExpiredAt,
                                 const Milliseconds& interval);

}  // namespace keys_collection_manager_util

/**
 * The KeysCollectionManager queries the config servers for keys that can be used for
 * HMAC computation. It maintains an internal background thread that is used to periodically
 * refresh the local key cache against the keys collection stored on the config servers.
 */
class KeysCollectionManager {
public:
    static const std::string kKeyManagerPurposeString;

    KeysCollectionManager(std::string purpose,
                          std::unique_ptr<KeysCollectionClient> client,
                          Seconds keyValidForInterval);

    /**
     * Returns the validation keys that are valid for the given time and also match the keyId. Does
     * a blocking refresh if there is no matching internal key. If there is a matching internal key,
     * includes it as first key in the resulting vector.
     *
     * Throws ExceededTimeLimit if the refresh times out, and KeyNotFound if there are no such keys.
     */
    StatusWith<std::vector<KeysCollectionDocument>> getKeysForValidation(
        OperationContext* opCtx, long long keyId, const LogicalTime& forThisTime);

    /**
     * Returns the signing key that is valid for the given time. Note that unlike
     * getKeysForValidation, this will never do a refresh.
     */
    StatusWith<KeysCollectionDocument> getKeyForSigning(OperationContext* opCtx,
                                                        const LogicalTime& forThisTime);

    /**
     * Request this manager to perform a refresh.
     */
    void refreshNow(OperationContext* opCtx);

    /**
     * Starts a background thread that will constantly update the internal cache of keys.
     *
     * Cannot call this after stopMonitoring was called at least once.
     */
    void startMonitoring(ServiceContext* service);

    /**
     * Stops the background thread updating the cache.
     */
    void stopMonitoring();

    /**
     * Enable writing new keys to the config server primary. Should only be called if current node
     * is the config primary.
     */
    void enableKeyGenerator(OperationContext* opCtx, bool doEnable);

    /**
     * Returns true if the refresher has ever successfully returned keys from the config server.
     */
    bool hasSeenKeys();

    /**
     * Clears the in memory cache of the keys.
     */
    void clearCache();

    /**
     * Loads the given external key into the keys collection cache.
     */
    void cacheExternalKey(ExternalKeysCollectionDocument key);

private:
    /**
     * This is responsible for periodically performing refresh in the background.
     */
    class PeriodicRunner {
    public:
        using RefreshFunc = std::function<StatusWith<KeysCollectionDocument>(OperationContext*)>;

        /**
         * Preemptively inform the monitoring thread it needs to perform a refresh. Returns an
         * object
         * that gets notified after the current round of refresh is over. Note that being notified
         * can
         * mean either of these things:
         *
         * 1. An error occurred and refresh was not performed.
         * 2. No error occurred but no new key was found.
         * 3. No error occurred and new keys were found.
         */
        void refreshNow(OperationContext* opCtx);

        /**
         * Sets the refresh function to use.
         * Should only be used to bootstrap this refresher with initial strategy.
         */
        void setFunc(RefreshFunc newRefreshStrategy);

        /**
         * Switches the current strategy with a new one. This also waits to make sure that the old
         * strategy is not being used and will no longer be used after this call.
         */
        void switchFunc(OperationContext* opCtx, RefreshFunc newRefreshStrategy);

        /**
         * Starts the refresh thread.
         */
        void start(ServiceContext* service,
                   const std::string& threadName,
                   Milliseconds refreshInterval);

        /**
         * Stops the refresh thread.
         */
        void stop();

        /**
         * Returns true if keys have ever successfully been returned from the config server.
         */
        bool hasSeenKeys() const noexcept;

        /**
         * Returns if the periodic runner has entered shutdown.
         */
        bool isInShutdown() const;

    private:
        void _doPeriodicRefresh(ServiceContext* service,
                                std::string threadName,
                                Milliseconds refreshInterval);

        AtomicWord<bool> _hasSeenKeys{false};

        // protects all the member variables below.
        mutable Mutex _mutex = MONGO_MAKE_LATCH("PeriodicRunner::_mutex");
        std::shared_ptr<Notification<void>> _refreshRequest;
        stdx::condition_variable _refreshNeededCV;

        stdx::thread _backgroundThread;
        std::shared_ptr<RefreshFunc> _doRefresh;

        bool _inShutdown = false;
    };

    std::unique_ptr<KeysCollectionClient> _client;
    const std::string _purpose;
    const Seconds _keyValidForInterval;

    // No mutex needed since the members below have their own mutexes.
    KeysCollectionCache _keysCache;
    PeriodicRunner _refresher;
};

}  // namespace mongo
