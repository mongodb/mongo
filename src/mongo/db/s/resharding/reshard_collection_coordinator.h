// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator_service.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/db/s/resharding/reshard_collection_coordinator_document_gen.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"

#include <memory>

#include <boost/optional/optional.hpp>

namespace mongo {
class [[MONGO_MOD_PUBLIC]] ReshardCollectionCoordinator
    : public RecoverableShardingDDLCoordinator<ReshardCollectionCoordinatorDocument> {
public:
    ReshardCollectionCoordinator(ShardingCoordinatorService* service, const BSONObj& initialState);

    [[MONGO_MOD_PRIVATE]] void checkIfOptionsConflict(const BSONObj& coorDoc) const override;

    [[MONGO_MOD_PRIVATE]] void appendCommandInfo(BSONObjBuilder* cmdInfoBuilder) const override;

protected:
    ReshardCollectionCoordinator(ShardingCoordinatorService* service,
                                 const BSONObj& initialState,
                                 bool persistCoordinatorDocument);

    bool isInCriticalSection(Phase phase) const override;

private:
    BSONObj _computeFinalShardKey(const CurrentChunkManager& cmOld);

    ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                  const CancellationToken& token) noexcept override;

    const mongo::ReshardCollectionRequest _request;
};

}  // namespace mongo
