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

class CreateCollectionCoordinator : public ShardingDDLCoordinator {
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

protected:
    mutable Mutex _docMutex = MONGO_MAKE_LATCH("CreateCollectionCoordinator::_docMutex");
    CoordDoc _doc;

    const mongo::CreateCollectionRequest _request;

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
     * Checks that the collection has UUID matching the collectionUUID parameter, if provided.
     */
    void _checkCollectionUUIDMismatch(OperationContext* opCtx) const;

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
     * Helper function to audit and log the shard collection event.
     */
    void _logStartCreateCollection(OperationContext* opCtx);

    /**
     * Helper function to log the end of the shard collection event.
     */
    void _logEndCreateCollection(OperationContext* opCtx);

    /**
     * Returns the BSONObj used as critical section reason
     *
     * TODO SERVER-64720 remove this function, directly access _critSecReason
     *
     */
    virtual const BSONObj& _getCriticalSectionReason() const {
        return _critSecReason;
    };

    const BSONObj _critSecReason;

    // The shard key of the collection, static for the duration of the coordinator and reflects the
    // original command
    boost::optional<ShardKeyPattern> _shardKeyPattern;

    // Set on successful completion of the coordinator
    boost::optional<CreateCollectionResponse> _result;

    // The fields below are only populated if the coordinator enters in the branch where the
    // collection is not already sharded (i.e., they will not be present on early return)

    boost::optional<BSONObj> _collationBSON;
    boost::optional<UUID> _collectionUUID;

    std::unique_ptr<InitialSplitPolicy> _splitPolicy;
    boost::optional<InitialSplitPolicy::ShardCollectionConfig> _initialChunks;
    boost::optional<bool> _collectionEmpty;
};

class CreateCollectionCoordinatorDocumentPre60Compatible final
    : public CreateCollectionCoordinatorDocument {
    // TODO SERVER-64720 remove once 6.0 becomes last LTS
public:
    using CreateCollectionCoordinatorDocument::CreateCollectionCoordinatorDocument;

    static const BSONObj kPre60IncompatibleFields;
    void serialize(BSONObjBuilder* builder) const;
    BSONObj toBSON() const;
};

class CreateCollectionCoordinatorPre60Compatible final : public CreateCollectionCoordinator {
    // TODO SERVER-64720 remove once 6.0 becomes last LTS
public:
    using CreateCollectionCoordinator::CreateCollectionCoordinator;
    using CoordDoc = CreateCollectionCoordinatorDocumentPre60Compatible;

    CreateCollectionCoordinatorPre60Compatible(ShardingDDLCoordinatorService* service,
                                               const BSONObj& initialState);

    virtual const BSONObj& _getCriticalSectionReason() const override {
        return _critSecReason;
    };

private:
    const BSONObj _critSecReason;
};

}  // namespace mongo
