// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/s/resharding/resharding_coordinator.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/modules.h"

#include <string_view>
#include <vector>

namespace mongo {
using namespace std::literals::string_view_literals;
class ReshardingCoordinator;

class [[MONGO_MOD_PUBLIC]] ReshardingCoordinatorService : public repl::PrimaryOnlyService {
public:
    static constexpr std::string_view kServiceName = "ReshardingCoordinatorService"sv;

    explicit ReshardingCoordinatorService(ServiceContext* serviceContext)
        : PrimaryOnlyService(serviceContext), _serviceContext(serviceContext) {}
    ~ReshardingCoordinatorService() override = default;

    friend ReshardingCoordinator;

    [[MONGO_MOD_PRIVATE]] std::string_view getServiceName() const override {
        return kServiceName;
    }

    [[MONGO_MOD_PRIVATE]] NamespaceString getStateDocumentsNS() const override {
        return NamespaceString::kConfigReshardingOperationsNamespace;
    }

    [[MONGO_MOD_PRIVATE]] ThreadPoolLimits getThreadPoolLimits() const override;

    // The service implemented its own conflict check before this method was added.
    [[MONGO_MOD_PRIVATE]] void checkIfConflictsWithOtherInstances(
        OperationContext* opCtx,
        BSONObj initialState,
        const std::vector<const PrimaryOnlyService::Instance*>& existingInstances) override;

    [[MONGO_MOD_PRIVATE]] std::shared_ptr<PrimaryOnlyService::Instance> constructInstance(
        BSONObj initialState) override;

    [[MONGO_MOD_PRIVATE]] std::vector<std::shared_ptr<PrimaryOnlyService::Instance>>
    getAllReshardingInstances(OperationContext* opCtx) {
        return getAllInstances(opCtx);
    }

    /**
     * Tries to abort all active reshardCollection operations. Note that this doesn't differentiate
     * between operations interrupted due to stepdown or abort. Callers who wish to confirm that
     * the abort successfully went through should follow up with an inspection on the resharding
     * coordinator docs to ensure that they are empty.
     *
     * This call skips quiesce periods for all aborted coordinators.
     */
    [[MONGO_MOD_NEEDS_REPLACEMENT]] void abortAllReshardCollection(
        OperationContext* opCtx, ReshardingCoordinator::AbortRequest abortRequest);

    [[MONGO_MOD_PRIVATE]] void stepDown_forTest();
    [[MONGO_MOD_PRIVATE]] void stepUp_forTest();

private:
    ExecutorFuture<void> _rebuildService(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                         const CancellationToken& token) override;

    ServiceContext* _serviceContext;
};

}  // namespace mongo
