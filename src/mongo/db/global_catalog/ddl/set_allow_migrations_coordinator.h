// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/global_catalog/ddl/set_allow_migrations_coordinator_document_gen.h"
#include "mongo/db/global_catalog/ddl/set_allow_migrations_gen.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator_service.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator.h"
#include "mongo/db/operation_context.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"

#include <memory>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class SetAllowMigrationsCoordinator final
    : public NonRecoverableShardingDDLCoordinator<SetAllowMigrationsCoordinatorDocument> {

public:
    SetAllowMigrationsCoordinator(ShardingCoordinatorService* service, const BSONObj& initialState)
        : NonRecoverableShardingDDLCoordinator(
              service, "SetAllowMigrationsCoordinator", initialState),
          _allowMigrations(_doc.getAllowMigrations()) {}

    void checkIfOptionsConflict(const BSONObj& coorDoc) const override;

    void appendCommandInfo(BSONObjBuilder* cmdInfoBuilder) const override;

    bool canAlwaysStartWhenUserWritesAreDisabled() const override {
        return true;
    }

protected:
    bool isInCriticalSection(Phase phase) const override;

private:
    ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                  const CancellationToken& token) noexcept override;

    const bool _allowMigrations;
};
}  // namespace mongo
