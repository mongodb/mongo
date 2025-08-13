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

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/global_catalog/ddl/collmod_coordinator_document_gen.h"
#include "mongo/db/global_catalog/ddl/sharded_collmod_gen.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator_service.h"
#include "mongo/db/local_catalog/ddl/coll_mod_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"

#include <memory>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class CollModCoordinator final
    : public RecoverableShardingDDLCoordinator<CollModCoordinatorDocument,
                                               CollModCoordinatorPhaseEnum> {
public:
    using StateDoc = CollModCoordinatorDocument;
    using Phase = CollModCoordinatorPhaseEnum;

    CollModCoordinator(ShardingDDLCoordinatorService* service, const BSONObj& initialState);

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

    StringData serializePhase(const Phase& phase) const override {
        return CollModCoordinatorPhase_serializer(phase);
    }

    ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                  const CancellationToken& token) noexcept override;

    void _performNoopRetryableWriteOnParticipants(
        OperationContext* opCtx, const std::shared_ptr<executor::TaskExecutor>& executor);

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
