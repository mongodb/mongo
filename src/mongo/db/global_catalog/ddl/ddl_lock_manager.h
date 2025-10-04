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

#include "mongo/base/string_data.h"
#include "mongo/db/database_name.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <random>
#include <string>

#include <absl/container/flat_hash_map.h>
#include <boost/none.hpp>

namespace mongo {

/**
 * Service to manage DDL locks.
 */
class DDLLockManager {

    /**
     * ScopedBaseDDLLock will hold a DDL lock for the given resource without performing any check.
     */
    class ScopedBaseDDLLock {
    public:
        ScopedBaseDDLLock(OperationContext* opCtx,
                          Locker* locker,
                          const NamespaceString& nss,
                          StringData reason,
                          LockMode mode,
                          bool waitForRecovery,
                          boost::optional<Milliseconds> timeout = boost::none);

        ScopedBaseDDLLock(OperationContext* opCtx,
                          Locker* locker,
                          const DatabaseName& db,
                          StringData reason,
                          LockMode mode,
                          bool waitForRecovery,
                          boost::optional<Milliseconds> timeout = boost::none);

        virtual ~ScopedBaseDDLLock();

        ScopedBaseDDLLock(ScopedBaseDDLLock&& other);

        StringData getResourceName() const {
            return _resourceName;
        }
        StringData getReason() const {
            return _reason;
        }

    protected:
        ScopedBaseDDLLock(const ScopedBaseDDLLock&) = delete;
        ScopedBaseDDLLock& operator=(const ScopedBaseDDLLock&) = delete;

        ScopedBaseDDLLock(OperationContext* opCtx,
                          Locker* locker,
                          StringData resName,
                          const ResourceId& resId,
                          StringData reason,
                          LockMode mode,
                          bool waitForRecovery,
                          Milliseconds timeout);

        static const Minutes kDefaultLockTimeout;
        static Milliseconds _getTimeout();

        // Attributes
        std::string _resourceName;
        ResourceId _resourceId;
        std::string _reason;
        LockMode _mode;
        LockResult _result;
        Locker* _locker;
        DDLLockManager* _lockManager;
    };

public:
    /**
     * Interface to inject to be able to wait for recovery
     * If you try to lock with `waitForRecovery == true` the base lock will call the
     * `waitForRecovery` funciton on this interface
     */
    class Recoverable {
    public:
        virtual ~Recoverable() = default;

        virtual void waitForRecovery(OperationContext* opCtx) const = 0;
    };

    // Timeout value, which specifies that if the lock is not available immediately, no attempt
    // should be made to wait for it to become free.
    static const Milliseconds kSingleLockAttemptTimeout;

    // Backoff strategy for tryLock functions
    class BackoffStrategy {
    public:
        virtual ~BackoffStrategy() = default;

        /**
         * Executes the backoff strategy
         *
         * @fn The function to execute in the strategy
         *
         * Returns true on successful execution of the fn function
         */
        virtual bool execute(const std::function<bool()>& fn,
                             const std::function<void(Milliseconds)>& sleepFn) = 0;
    };

private:
    template <size_t RetryCount,
              size_t BaseWaitTimeMs,
              size_t WaitExponentialFactor,
              unsigned int MaxWaitTimeMs>
    class BackoffStrategyImpl : public BackoffStrategy {
    public:
        BackoffStrategyImpl() : _retries(0), _sleepTime(BaseWaitTimeMs) {
            static_assert(RetryCount > 0);
            static_assert(WaitExponentialFactor > 0);
            static_assert(BaseWaitTimeMs <= MaxWaitTimeMs);
        }

        bool execute(const std::function<bool()>& fn,
                     const std::function<void(Milliseconds)>& sleepFn) override {
            while (_retries < RetryCount) {
                _retries++;

                if (fn()) {
                    return true;
                }

                if (_retries == RetryCount) {
                    break;
                }

                // Half-jitter implementation.
                // Half of the sleep time is random, so the sleep will be [sleepTime / 2, sleepTime]
                const auto sleepHalf = _sleepTime / 2;
                std::uniform_int_distribution<> distrib(0, sleepHalf);
                const auto jitter = static_cast<unsigned int>(distrib(_gen));

                sleepFn(Milliseconds(sleepHalf + jitter));
                _sleepTime *= WaitExponentialFactor;
                _sleepTime = std::min(_sleepTime, MaxWaitTimeMs);
            }
            return false;
        }

    private:
        size_t _retries;
        unsigned int _sleepTime;

        SecureUrbg _rd;
        std::mt19937_64 _gen{_rd()};
    };

public:
    /**
     * SingleTryBackoffStrategy
     *
     * Executes the callback function only once.
     * No retry on fail.
     */
    using SingleTryBackoffStrategy = BackoffStrategyImpl<1, 0, 1, 0>;

    /**
     * ConstantBackoffStrategy
     *
     * Executes the callback function up to `RetryCount` times if it fails.
     * Waits `BaseWaitTimeMs` milliseconds between retries.
     */
    template <size_t RetryCount, size_t BaseWaitTimeMs>
    using ConstantBackoffStrategy =
        BackoffStrategyImpl<RetryCount, BaseWaitTimeMs, 1, BaseWaitTimeMs>;

    /**
     * TruncatedExponentialBackoffStrategy
     *
     * Executes the callback function up to `RetryCount` times if it fails.
     * Waits `min(MaxWaitTimeMs, BaseWaitTimeMs * (2 ^ retryCount))` milliseconds between retries.
     */
    template <size_t RetryCount, size_t BaseWaitTimeMs, size_t MaxWaitTimeMs>
    using TruncatedExponentialBackoffStrategy =
        BackoffStrategyImpl<RetryCount, BaseWaitTimeMs, 2, MaxWaitTimeMs>;

    // RAII-style class to acquire a DDL lock on the given database
    class ScopedDatabaseDDLLock {
    public:
        /**
         * Constructs a ScopedDatabaseDDLLock object
         *
         * @db      Database to lock.
         * @reason 	Reason for which the lock is being acquired (e.g. 'createCollection').
         * @mode    Lock mode.
         *
         * Throws:
         *     ErrorCodes::LockBusy in case the timeout is reached.
         *     ErrorCodes::LockTimeout when not being on kPrimaryAndRecovered state and the internal
         *            timeout is reached.
         *     ErrorCategory::Interruption in case the operation context is interrupted.
         *     ErrorCodes::IllegalOperation in case of not being on the db primary shard.
         *
         * It's caller's responsibility to ensure this lock is acquired only on primary node of
         * replica set and released on step-down.
         */
        ScopedDatabaseDDLLock(OperationContext* opCtx,
                              const DatabaseName& db,
                              StringData reason,
                              LockMode mode,
                              boost::optional<BackoffStrategy&> backoffStrategy = boost::none);

    private:
        bool _tryLock(OperationContext* opCtx,
                      const DatabaseName& db,
                      StringData reason,
                      LockMode mode,
                      BackoffStrategy& backoffStrategy);

        void _lock(OperationContext* opCtx,
                   const DatabaseName& db,
                   StringData reason,
                   LockMode mode,
                   boost::optional<Milliseconds> timeout);

        boost::optional<ScopedBaseDDLLock> _dbLock;
    };

    // RAII-style class to acquire a DDL lock on the given collection. The database DDL lock will
    // also be implicitly acquired in the corresponding intent mode.
    class ScopedCollectionDDLLock {
    public:
        /**
         * Constructs a ScopedCollectionDDLLock object
         *
         * @ns      Collection to lock.
         * @reason 	Reason for which the lock is being acquired (e.g. 'createCollection').
         * @mode    Lock mode.
         *
         * Throws:
         *     ErrorCodes::LockBusy in case the timeout is reached.
         *     ErrorCodes::LockTimeout when not being on kPrimaryAndRecovered state and the internal
         *            timeout is reached.
         *     ErrorCategory::Interruption in case the operation context is interrupted.
         *     ErrorCodes::IllegalOperation in case of not being on the db primary shard.
         *
         * It's caller's responsibility to ensure this lock is acquired only on primary node of
         * replica set and released on step-down.
         */
        ScopedCollectionDDLLock(OperationContext* opCtx,
                                const NamespaceString& ns,
                                StringData reason,
                                LockMode mode,
                                boost::optional<BackoffStrategy&> backoffStrategy = boost::none);

    private:
        bool _tryLock(OperationContext* opCtx,
                      const NamespaceString& ns,
                      StringData reason,
                      LockMode mode,
                      BackoffStrategy& backoffStrategy);

        void _lock(OperationContext* opCtx,
                   const NamespaceString& ns,
                   StringData reason,
                   LockMode mode,
                   boost::optional<Milliseconds> timeout);

        // Make sure _dbLock is instantiated before _collLock to don't break the hierarchy locking
        // acquisition order
        boost::optional<ScopedBaseDDLLock> _dbLock;
        boost::optional<ScopedBaseDDLLock> _collLock;
    };

    DDLLockManager() = default;
    ~DDLLockManager() = default;

    /**
     * Retrieves the DDLLockManager singleton.
     */
    static DDLLockManager* get(ServiceContext* service);
    static DDLLockManager* get(OperationContext* opCtx);

    // Function to inject an instance of the Recoverable interface
    void setRecoverable(Recoverable* recoverable);

protected:
    stdx::mutex _mutex;

    // Stored Recoverable instance to use when we have to wait for recovery on locking
    // For more information, check the documentation at Recoverable interface
    Recoverable* _recoverable{nullptr};

    void _lock(OperationContext* opCtx,
               Locker* locker,
               StringData ns,
               const ResourceId& resId,
               StringData reason,
               LockMode mode,
               Date_t deadline,
               bool waitForRecovery);

    void _unlock(
        Locker* locker, StringData ns, const ResourceId& resId, StringData reason, LockMode mode);

    // Stores how many holders either are trying to acquire or are holding a specific resource at
    // that moment.
    stdx::unordered_map<ResourceId, int32_t> _numHoldersPerResource;

    /**
     * Register/Unregister a resourceName into the ResourceCatalog for debuggability purposes.
     */
    void _registerResourceName(ResourceId resId, StringData resName);
    void _unregisterResourceNameIfNoLongerNeeded(ResourceId resId, StringData resName);


    friend class DDLLockManagerTest;
    friend class ShardingCatalogManager;
    friend class ShardingDDLCoordinator;
    friend class ShardingDDLCoordinatorService;
    friend class ShardingDDLCoordinatorServiceTest;
    // TODO (SERVER-102647): Remove this friend declaration.
    friend class ConfigSvrCreateDatabaseCommand;
};

}  // namespace mongo
