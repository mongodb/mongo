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
#include "mongo/db/s/create_collection_coordinator_params.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/sharding_ddl_coordinator.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/util/future.h"

namespace mongo {

class CreateCollectionCoordinator
    : public RecoverableShardingDDLCoordinator<CreateCollectionCoordinatorDocument,
                                               CreateCollectionCoordinatorPhaseEnum> {
public:
    using CoordDoc = CreateCollectionCoordinatorDocument;
    using Phase = CreateCollectionCoordinatorPhaseEnum;

    CreateCollectionCoordinator(ShardingDDLCoordinatorService* service, const BSONObj& initialState)
        : RecoverableShardingDDLCoordinator(service, "CreateCollectionCoordinator", initialState),
          _request(_doc.getCreateCollectionRequest(), originalNss()),
          _critSecReason(BSON("command"
                              << "createCollection"
                              << "ns" << originalNss().toString())) {}

    ~CreateCollectionCoordinator() = default;


    void checkIfOptionsConflict(const BSONObj& coorDoc) const override;

    void appendCommandInfo(BSONObjBuilder* cmdInfoBuilder) const override;

    /**
     * Waits for the termination of the parent DDLCoordinator (so all the resources are liberated)
     * and then return the
     */
    CreateCollectionResponse getResult(OperationContext* opCtx) {
        getCompletionFuture().get(opCtx);
        invariant(_result.is_initialized());
        return *_result;
    }

protected:
    const NamespaceString& nss() const override;

private:
    StringData serializePhase(const Phase& phase) const override {
        return CreateCollectionCoordinatorPhase_serializer(phase);
    }

    ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                  const CancellationToken& token) noexcept override;

    /**
     * Performs all required checks before holding the critical sections.
     */
    void _checkCommandArguments(OperationContext* opCtx);

    /**
     * Checks that the collection has UUID matching the collectionUUID parameter, if provided.
     */
    void _checkCollectionUUIDMismatch(OperationContext* opCtx) const;

    void _acquireCriticalSections(OperationContext* opCtx) const;

    void _promoteCriticalSectionsToBlockReads(OperationContext* opCtx) const;

    void _releaseCriticalSections(OperationContext* opCtx) const;

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
                                             const OperationSessionInfo& osi);

    /**
     * Does the following writes:
     * 1. Updates the config.collections entry for the new sharded collection
     * 2. Updates config.chunks entries for the new sharded collection
     */
    void _commit(OperationContext* opCtx);

    /**
     * Helper function to audit and log the shard collection event.
     */
    void _logStartCreateCollection(OperationContext* opCtx);

    /**
     * Helper function to log the end of the shard collection event.
     */
    void _logEndCreateCollection(OperationContext* opCtx);

    CreateCollectionCoordinatorParams _request;

    const BSONObj _critSecReason;

    // Set on successful completion of the coordinator
    boost::optional<CreateCollectionResponse> _result;

    // The fields below are only populated if the coordinator enters in the branch where the
    // collection is not already sharded (i.e., they will not be present on early return)

    boost::optional<UUID> _collectionUUID;

    std::unique_ptr<InitialSplitPolicy> _splitPolicy;
    boost::optional<InitialSplitPolicy::ShardCollectionConfig> _initialChunks;
    boost::optional<bool> _collectionEmpty;
};

}  // namespace mongo
