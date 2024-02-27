/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/s/sharding_ddl_coordinator_external_state.h"
#include "mongo/db/s/sharding_ddl_coordinator_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/executor/task_executor.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/future.h"

namespace mongo {

class ShardingDDLCoordinator;

class ShardingDDLCoordinatorService final : public repl::PrimaryOnlyService {
public:
    static constexpr StringData kServiceName = "ShardingDDLCoordinator"_sd;

    explicit ShardingDDLCoordinatorService(
        ServiceContext* serviceContext,
        std::unique_ptr<ShardingDDLCoordinatorExternalStateFactory> externalStateFactory =
            std::make_unique<ShardingDDLCoordinatorExternalStateFactoryImpl>())
        : PrimaryOnlyService(serviceContext),
          _externalStateFactory(std::move(externalStateFactory)) {}

    ~ShardingDDLCoordinatorService() = default;

    static ShardingDDLCoordinatorService* getService(OperationContext* opCtx);

    using repl::PrimaryOnlyService::getAllInstances;

    StringData getServiceName() const override {
        return kServiceName;
    }

    NamespaceString getStateDocumentsNS() const override {
        return NamespaceString::kShardingDDLCoordinatorsNamespace;
    }

    ThreadPool::Limits getThreadPoolLimits() const override {
        ThreadPool::Limits limits;
        limits.maxThreads = ThreadPool::Options::kUnlimited;
        return limits;
    }

    // The service implemented its own conflict check before this method was added.
    void checkIfConflictsWithOtherInstances(
        OperationContext* opCtx,
        BSONObj initialState,
        const std::vector<const PrimaryOnlyService::Instance*>& existingInstances) override{};

    std::shared_ptr<Instance> constructInstance(BSONObj initialState) override;

    std::shared_ptr<ShardingDDLCoordinatorExternalState> createExternalState() const;

    std::shared_ptr<Instance> getOrCreateInstance(OperationContext* opCtx,
                                                  BSONObj initialState,
                                                  bool checkOptions = true);

    std::shared_ptr<executor::TaskExecutor> getInstanceCleanupExecutor() const;

    void waitForCoordinatorsOfGivenTypeToComplete(OperationContext* opCtx,
                                                  DDLCoordinatorTypeEnum type) const;

    /**
     * Waits for all currently running coordinators matching the predicate 'pred' to finish. While
     * waiting here, new coordinators may start, but they will not be waited for.
     */
    void waitForOngoingCoordinatorsToFinish(
        OperationContext* opCtx,
        std::function<bool(const ShardingDDLCoordinator&)> pred = {
            [](const ShardingDDLCoordinator&) {
                return true;
            }});

    void waitForRecoveryCompletion(OperationContext* opCtx) const;

private:
    friend class ShardingDDLCoordinatorServiceTest;

    ExecutorFuture<void> _rebuildService(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                         const CancellationToken& token) override;

    void _onServiceTermination() override;
    size_t _countCoordinatorDocs(OperationContext* opCtx);

    void _transitionToRecovered(WithLock lk, OperationContext* opCtx);

    mutable Mutex _mutex = MONGO_MAKE_LATCH("ShardingDDLCoordinatorService::_mutex");

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

    // This counter is set up at stepUp and reprensent the number of coordinator instances
    // that needs to be recovered from disk.
    size_t _numCoordinatorsToWait{0};

    stdx::unordered_map<DDLCoordinatorTypeEnum, size_t> _numActiveCoordinatorsPerType;

    std::unique_ptr<ShardingDDLCoordinatorExternalStateFactory> _externalStateFactory;
};

}  // namespace mongo
