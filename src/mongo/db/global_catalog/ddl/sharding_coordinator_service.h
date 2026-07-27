// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator_external_state.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/s/active_migrations_registry.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/ddl/ddl_lock_manager.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

namespace mongo {
using namespace std::literals::string_view_literals;

class ShardingCoordinator;

class [[MONGO_MOD_NEEDS_REPLACEMENT]] ShardingCoordinatorService final
    : public repl::PrimaryOnlyService,
      public DDLLockManager::Recoverable,
      public ActiveMigrationsRegistry::Recoverable {
public:
    static constexpr std::string_view kServiceName = "ShardingCoordinator"sv;

    explicit ShardingCoordinatorService(
        ServiceContext* serviceContext,
        std::unique_ptr<ShardingCoordinatorExternalStateFactory> externalStateFactory =
            std::make_unique<ShardingCoordinatorExternalStateFactoryImpl>())
        : PrimaryOnlyService(serviceContext),
          _externalStateFactory(std::move(externalStateFactory)) {}


    ~ShardingCoordinatorService() override = default;

    static ShardingCoordinatorService* getService(OperationContext* opCtx);

    using repl::PrimaryOnlyService::getAllInstances;
    using FCV = multiversion::FeatureCompatibilityVersion;

    std::string_view getServiceName() const override {
        return kServiceName;
    }

    NamespaceString getStateDocumentsNS() const override {
        // Even though the class is no longer named `ShardingDDLCoordinatorService` and the
        // coordinator instances are not necessarily DDL coordinators, the namespace is still
        // `config.system.sharding_ddl_coordinators` for backward compatibility reasons.
        return NamespaceString::kShardingDDLCoordinatorsNamespace;
    }

    ThreadPoolLimits getThreadPoolLimits() const override {
        return {.maxThreads = ThreadPool::Options::kUnlimited};
    }

    // The service implemented its own conflict check before this method was added.
    void checkIfConflictsWithOtherInstances(
        OperationContext* opCtx,
        BSONObj initialState,
        const std::vector<const PrimaryOnlyService::Instance*>& existingInstances) override;

    std::shared_ptr<Instance> constructInstance(BSONObj initialState) override;

    std::shared_ptr<ShardingCoordinatorExternalState> createExternalState() const;

    std::shared_ptr<Instance> getOrCreateInstance(OperationContext* opCtx,
                                                  BSONObj initialState,
                                                  const FixedFCVRegion& fcvRegion,
                                                  bool checkOptions = true);

    std::shared_ptr<executor::TaskExecutor> getInstanceCleanupExecutor() const;

    void waitForCoordinatorsOfGivenOfcvToComplete(
        OperationContext* opCtx, std::function<bool(boost::optional<FCV>)> pred) const;

    // TODO SERVER-99655: remove once gSnapshotFCVInDDLCoordinators is enabled on last LTS
    void waitForCoordinatorsOfGivenTypeToComplete(OperationContext* opCtx,
                                                  CoordinatorTypeEnum type) const;

    /**
     * Waits for all currently running coordinators matching the predicate 'pred' to finish. While
     * waiting here, new coordinators may start, but they will not be waited for.
     */
    void waitForOngoingCoordinatorsToFinish(OperationContext* opCtx,
                                            std::function<bool(const ShardingCoordinator&)> pred = {
                                                [](const ShardingCoordinator&) {
                                                    return true;
                                                }});

    void waitForRecovery(OperationContext* opCtx) const override;

    /**
     * Waits for the primary only service's recovery and then checks if there are any coordinators
     * of the given type that have not yet completed. There is no guarantee that new coordinators
     * do not start during (or immediately after) this call and it is up to the caller to provide
     * that stability if needed.
     */
    bool areAllCoordinatorsOfTypeFinished(OperationContext* opCtx,
                                          CoordinatorTypeEnum coordinatorType);

private:
    friend class ShardingCoordinatorServiceTest;

    ExecutorFuture<void> _rebuildService(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                         const CancellationToken& token) override;

    void _onServiceInitialization() override;
    void _onServiceTermination() override;

    size_t _countActiveCoordinators(
        std::function<bool(CoordinatorTypeEnum, boost::optional<FCV>)> pred) const;

    void _transitionToRecovered(WithLock lk, OperationContext* opCtx);

    void _waitForRecovery(OperationContext* opCtx, std::unique_lock<std::mutex>& lock) const;

    mutable std::mutex _mutex;

    // When the node stepDown the state is set to kPaused and all the new DDL operation will be
    // blocked. On step-up we set _coordinatorsToWait to the numbers of coordinators that needs to
    // be recovered and we enter in kRecovering state. Once all coordinators have been recovered we
    // move to kRecovered state and we unblock all new incoming DDL.
    enum class State {
        kPaused,
        kRecovering,
        kRecovered,
    };

    State _state = State::kPaused;

    mutable stdx::condition_variable _recoveredOrCoordinatorCompletedCV;

    // This counter is set up at stepUp and represent the number of coordinator instances
    // that needs to be recovered from disk.
    size_t _numCoordinatorsToWait{0};

    // TODO SERVER-99655: make the 'FCV' key non-optional
    stdx::unordered_map<std::pair<CoordinatorTypeEnum, boost::optional<FCV>>, size_t>
        _numActiveCoordinatorsPerTypeAndOfcv;

    std::unique_ptr<ShardingCoordinatorExternalStateFactory> _externalStateFactory;
};

}  // namespace mongo
