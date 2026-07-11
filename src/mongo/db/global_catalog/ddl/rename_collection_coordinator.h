// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharded_rename_collection_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator_service.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"

#include <memory>
#include <set>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class RenameCollectionCoordinator final
    : public RecoverableShardingDDLCoordinator<RenameCollectionCoordinatorDocument> {
public:
    RenameCollectionCoordinator(ShardingCoordinatorService* service, const BSONObj& initialState);

    void checkIfOptionsConflict(const BSONObj& doc) const override;

    void appendCommandInfo(BSONObjBuilder* cmdInfoBuilder) const override;

    /**
     * Waits for the rename to complete and returns the collection version.
     */
    RenameCollectionResponse getResponse(OperationContext* opCtx) {
        getCompletionFuture().get(opCtx);
        tassert(10644505, "Expected _response to be set", _response);
        return *_response;
    }

protected:
    logv2::DynamicAttributes getCoordinatorLogAttrs() const override;

    bool isInCriticalSection(Phase phase) const override;

private:
    bool _mustAlwaysMakeProgress() override {
        return _doc.getPhase() >= Phase::kFreezeMigrations;
    };

    ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                  const CancellationToken& token) noexcept override;

    std::set<NamespaceString> _getAdditionalLocksToAcquire(OperationContext* opCtx) override;

    boost::optional<RenameCollectionResponse> _response;
    const RenameCollectionRequest _request;
};

}  // namespace mongo
