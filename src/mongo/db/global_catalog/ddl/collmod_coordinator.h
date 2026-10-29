// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/global_catalog/ddl/collmod_coordinator_document_gen.h"
#include "mongo/db/global_catalog/ddl/sharded_collmod_gen.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator_service.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/db/shard_role/ddl/coll_mod_gen.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"

#include <memory>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class CollModCoordinator final
    : public RecoverableShardingDDLCoordinator<CollModCoordinatorDocument> {
public:
    CollModCoordinator(ShardingCoordinatorService* service, const BSONObj& initialState);

    void checkIfOptionsConflict(const BSONObj& doc) const override;

    void appendCommandInfo(BSONObjBuilder* cmdInfoBuilder) const override;

    /**
     * Waits for the termination of the parent DDLCoordinator (so all the resources are liberated)
     * and then return the result.
     */
    BSONObj getResult(OperationContext* opCtx) {
        getCompletionFuture().get(opCtx);
        tassert(10644503, "Expected _result to be initialized", _result.is_initialized());
        return *_result;
    }

protected:
    bool isInCriticalSection(Phase phase) const override;

private:
    struct CollectionInfo {
        bool isTracked;
        bool isSharded;
        boost::optional<TimeseriesOptions> timeSeriesOptions;
        // The targeting namespace can be different from the original namespace in some cases, like
        // time-series collections.
        //
        // TODO SERVER-105548 remove nsForTargeting once 9.0 becomes last LTS
        NamespaceString nsForTargeting;
    };

    struct ShardingInfo {
        // The primary shard for the collection, only set if the collection is sharded.
        ShardId primaryShard;
        // Flag that tells if the primary db shard has chunks for the collection.
        bool isPrimaryOwningChunks;
        // The participant shards owning chunks for the collection, only set if the collection is
        // sharded.
        std::vector<ShardId> participantsOwningChunks;
        // The participant shards not owning chunks for the collection, only set if the collection
        // is sharded.
        std::vector<ShardId> participantsNotOwningChunks;
    };

    bool _isTrackedTimeseriesUpdate() const;

    bool _mustAlwaysMakeProgress() override;

    ExecutorFuture<void> _cleanupOnAbort(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                         const CancellationToken& token,
                                         const Status& status) noexcept override;

    ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                  const CancellationToken& token) noexcept override;

    void _saveCollectionInfoOnCoordinatorIfNecessary(OperationContext* opCtx);

    void _saveShardingInfoOnCoordinatorIfNecessary(OperationContext* opCtx);

    std::vector<AsyncRequestsSender::Response> _sendCollModToPrimaryShard(
        OperationContext* opCtx,
        ShardsvrCollModParticipant& request,
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& token);

    std::vector<AsyncRequestsSender::Response> _sendCollModToParticipantShards(
        OperationContext* opCtx,
        ShardsvrCollModParticipant& request,
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& token);

    const mongo::CollModRequest _request;

    boost::optional<BSONObj> _result;
    boost::optional<CollectionInfo> _collInfo;
    boost::optional<ShardingInfo> _shardingInfo;
};

}  // namespace mongo
