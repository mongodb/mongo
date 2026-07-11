// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/global_catalog/ddl/drop_indexes_coordinator_document_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <boost/optional/optional.hpp>

namespace mongo {

class DropIndexesCoordinator final
    : public RecoverableShardingDDLCoordinator<DropIndexesCoordinatorDocument> {
public:
    DropIndexesCoordinator(ShardingCoordinatorService* service, const BSONObj& initialState);

    void checkIfOptionsConflict(const BSONObj& doc) const override;

    void appendCommandInfo(BSONObjBuilder* cmdInfoBuilder) const override;

    boost::optional<BSONObj> getResult(OperationContext* opCtx) {
        return _copyDoc().getResult();
    }

protected:
    bool isInCriticalSection(Phase phase) const override;

private:
    ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                  const CancellationToken& token) noexcept override;

    void _dropIndexes(OperationContext* opCtx,
                      std::shared_ptr<executor::ScopedTaskExecutor> executor,
                      const CancellationToken& token);

    ExecutorFuture<void> _cleanupOnAbort(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                         const CancellationToken& token,
                                         const Status& status) noexcept override;

    const DropIndexesRequest _request;
};
}  // namespace mongo
