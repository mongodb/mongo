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

#include "mongo/db/s/collmod_coordinator_document_gen.h"
#include "mongo/db/s/sharding_ddl_coordinator.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"

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
        invariant(_result.is_initialized());
        return *_result;
    }

private:
    struct CollectionInfo {
        bool isSharded;
        boost::optional<TimeseriesOptions> timeSeriesOptions;
        // The targeting namespace can be different from the original namespace in some cases, like
        // time-series collections.
        NamespaceString nsForTargeting;
    };

    struct ShardingInfo {
        // The primary shard for the collection, only set if the collection is sharded.
        ShardId primaryShard;
        // The shards owning chunks for the collection, only set if the collection is sharded.
        std::vector<ShardId> shardsOwningChunks;
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

    // TODO SERVER-68008 Remove once 7.0 becomes last LTS
    bool _isPre61Compatible() const;

    const mongo::CollModRequest _request;

    boost::optional<BSONObj> _result;
    boost::optional<CollectionInfo> _collInfo;
    boost::optional<ShardingInfo> _shardingInfo;
};

}  // namespace mongo
