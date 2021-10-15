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
#include "mongo/db/s/config/initial_split_policy.h"
#include "mongo/db/s/create_collection_coordinator_document_gen.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/sharding_ddl_coordinator.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/util/future.h"

namespace mongo {

class CreateCollectionCoordinator final : public ShardingDDLCoordinator {
public:
    using CoordDoc = CreateCollectionCoordinatorDocument;
    using Phase = CreateCollectionCoordinatorPhaseEnum;

    CreateCollectionCoordinator(ShardingDDLCoordinatorService* service,
                                const BSONObj& initialState);
    ~CreateCollectionCoordinator() = default;


    void checkIfOptionsConflict(const BSONObj& coorDoc) const override;

    boost::optional<BSONObj> reportForCurrentOp(
        MongoProcessInterface::CurrentOpConnectionsMode connMode,
        MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept override;

    /**
     * Waits for the termination of the parent DDLCoordinator (so all the resources are liberated)
     * and then return the
     */
    CreateCollectionResponse getResult(OperationContext* opCtx) {
        getCompletionFuture().get(opCtx);
        invariant(_result.is_initialized());
        return *_result;
    }

private:
    ShardingDDLCoordinatorMetadata const& metadata() const override {
        return _doc.getShardingDDLCoordinatorMetadata();
    }

    ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                  const CancellationToken& token) noexcept override;

    template <typename Func>
    auto _executePhase(const Phase& newPhase, Func&& func) {
        return [=] {
            const auto& currPhase = _doc.getPhase();

            if (currPhase > newPhase) {
                // Do not execute this phase if we already reached a subsequent one.
                return;
            }
            if (currPhase < newPhase) {
                // Persist the new phase if this is the first time we are executing it.
                _enterPhase(newPhase);
            }
            return func();
        };
    };

    void _enterPhase(Phase newState);

    /**
     * Performs all required checks before holding the critical sections.
     */
    void _checkCommandArguments(OperationContext* opCtx);

    /**
     * Ensures the collection is created locally and has the appropiate shard index.
     */
    void _createCollectionAndIndexes(OperationContext* opCtx);

    /**
     * Creates the appropiate split policy.
     */
    void _createPolicy(OperationContext* opCtx);

    /**
     * Given the appropiate split policy, create the initial chunks.
     */
    void _createChunks(OperationContext* opCtx);

    /**
     * If the optimized path can be taken, ensure the collection is already created in all the
     * participant shards.
     */
    void _createCollectionOnNonPrimaryShards(OperationContext* opCtx,
                                             const boost::optional<OperationSessionInfo>& osi);

    /**
     * Does the following writes:
     * 1. Updates the config.collections entry for the new sharded collection
     * 2. Updates config.chunks entries for the new sharded collection
     */
    void _commit(OperationContext* opCtx);

    /**
     * Refresh all participant shards and log creation.
     */
    void _finalize(OperationContext* opCtx);

    /**
     * Helper function to audit and log the shard collection event.
     */
    void _logStartCreateCollection(OperationContext* opCtx);

    /**
     * Helper function to log the end of the shard collection event.
     */
    void _logEndCreateCollection(OperationContext* opCtx);

    void _performNoopRetryableWriteOnParticipants(
        OperationContext* opCtx, const std::shared_ptr<executor::TaskExecutor>& executor);


    CreateCollectionCoordinatorDocument _doc;
    const BSONObj _critSecReason;

    // Objects generated on each execution.
    boost::optional<ShardKeyPattern> _shardKeyPattern;
    boost::optional<BSONObj> _collationBSON;
    boost::optional<UUID> _collectionUUID;
    std::unique_ptr<InitialSplitPolicy> _splitPolicy;
    InitialSplitPolicy::ShardCollectionConfig _initialChunks;
    boost::optional<CreateCollectionResponse> _result;
    boost::optional<bool> _collectionEmpty;
    boost::optional<size_t> _numChunks;
};

}  // namespace mongo
