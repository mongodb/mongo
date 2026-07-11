// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/global_catalog/ddl/drop_database_coordinator_document_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator_service.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"

#include <memory>

#include <boost/optional/optional.hpp>

namespace mongo {

class DropDatabaseCoordinator final
    : public RecoverableShardingDDLCoordinator<DropDatabaseCoordinatorDocument> {

public:
    DropDatabaseCoordinator(ShardingCoordinatorService* service, const BSONObj& initialState)
        : RecoverableShardingDDLCoordinator(service, "DropDatabaseCoordinator", initialState),
          _dbName(nss().dbName()),
          _critSecReason(BSON("dropDatabase" << DatabaseNameUtil::serialize(
                                  _dbName, SerializationContext::stateCommandRequest()))) {}
    ~DropDatabaseCoordinator() override = default;

    void checkIfOptionsConflict(const BSONObj& doc) const final {}

protected:
    bool isInCriticalSection(Phase phase) const override;

private:
    bool _mustAlwaysMakeProgress() override {
        return _doc.getPhase() > Phase::kUnset;
    }

    ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                  const CancellationToken& token) noexcept override;

    void _dropTrackedCollection(OperationContext* opCtx,
                                const CollectionType& coll,
                                const ShardId& changeStreamsNotifierShardId,
                                std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                const CancellationToken& token);

    void _clearDatabaseInfoOnPrimary(OperationContext* opCtx);

    void _clearDatabaseInfoOnSecondaries(OperationContext* opCtx);

    DatabaseName _dbName;

    const BSONObj _critSecReason;
};

}  // namespace mongo
