/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/modules.h"

#include <vector>

namespace mongo {
class ReshardingCoordinator;

class MONGO_MOD_PUBLIC ReshardingCoordinatorService : public repl::PrimaryOnlyService {
public:
    static constexpr StringData kServiceName = "ReshardingCoordinatorService"_sd;

    explicit ReshardingCoordinatorService(ServiceContext* serviceContext)
        : PrimaryOnlyService(serviceContext), _serviceContext(serviceContext) {}
    ~ReshardingCoordinatorService() override = default;

    friend ReshardingCoordinator;

    MONGO_MOD_PRIVATE StringData getServiceName() const override {
        return kServiceName;
    }

    MONGO_MOD_PRIVATE NamespaceString getStateDocumentsNS() const override {
        return NamespaceString::kConfigReshardingOperationsNamespace;
    }

    MONGO_MOD_PRIVATE ThreadPool::Limits getThreadPoolLimits() const override;

    // The service implemented its own conflict check before this method was added.
    MONGO_MOD_PRIVATE void checkIfConflictsWithOtherInstances(
        OperationContext* opCtx,
        BSONObj initialState,
        const std::vector<const PrimaryOnlyService::Instance*>& existingInstances) override;

    MONGO_MOD_PRIVATE std::shared_ptr<PrimaryOnlyService::Instance> constructInstance(
        BSONObj initialState) override;

    MONGO_MOD_PRIVATE std::vector<std::shared_ptr<PrimaryOnlyService::Instance>>
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
    MONGO_MOD_NEEDS_REPLACEMENT void abortAllReshardCollection(OperationContext* opCtx);

private:
    ExecutorFuture<void> _rebuildService(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                         const CancellationToken& token) override;

    ServiceContext* _serviceContext;
};

}  // namespace mongo
