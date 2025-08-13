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

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/global_catalog/ddl/create_collection_coordinator_document_gen.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator_service.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/local_catalog/shard_role_catalog/participant_block_gen.h"
#include "mongo/db/local_catalog/shard_role_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/db/s/config/initial_split_policy.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

namespace create_collection_util {
/**
 * Returns the optimization strategy for building initial chunks based on the input parameters
 * and the collection state.
 *
 * If dataShard is specified, isUnsplittable must be true, because we can only select the shard
 * that will hold the data for unsplittable collections.
 */
std::unique_ptr<InitialSplitPolicy> createPolicy(
    OperationContext* opCtx,
    const ShardKeyPattern& shardKeyPattern,
    bool presplitHashedZones,
    std::vector<TagsType> tags,
    size_t numShards,
    bool collectionIsEmpty,
    bool isUnsplittable,
    boost::optional<ShardId> dataShard,
    boost::optional<std::vector<ShardId>> availableShardIds = boost::none);

/**
 * Generates, using a ShardsvrCreateCollectionRequest as source, a CreateCommand that
 * can be used with the command execution framework to create a collection on this
 * shard server.
 *
 * TODO(SERVER-81447): build CreateCommand by simply extracting CreateCollectionRequest
 * from ShardsvrCreateCollectionRequest. Also, see SERVER-65865.
 */
CreateCommand makeCreateCommand(OperationContext* opCtx,
                                const NamespaceString& nss,
                                const ShardsvrCreateCollectionRequest& request);

}  // namespace create_collection_util

struct OptionsAndIndexes {
    BSONObj options;
    std::vector<BSONObj> indexSpecs;
    BSONObj idIndexSpec;
};

class CreateCollectionCoordinator
    : public RecoverableShardingDDLCoordinator<CreateCollectionCoordinatorDocument,
                                               CreateCollectionCoordinatorPhaseEnum> {
public:
    using CoordDoc = CreateCollectionCoordinatorDocument;
    using Phase = CreateCollectionCoordinatorPhaseEnum;

    CreateCollectionCoordinator(ShardingDDLCoordinatorService* service, const BSONObj& initialState)
        : RecoverableShardingDDLCoordinator(service, "CreateCollectionCoordinator", initialState),
          _request(_doc.getShardsvrCreateCollectionRequest()),
          _critSecReason(BSON("command"
                              << "createCollection"
                              << "ns"
                              << NamespaceStringUtil::serialize(
                                     originalNss(), SerializationContext::stateDefault()))) {}

    ~CreateCollectionCoordinator() override = default;


    void checkIfOptionsConflict(const BSONObj& coorDoc) const override;

    void appendCommandInfo(BSONObjBuilder* cmdInfoBuilder) const override;

    CreateCollectionResponse getResult(OperationContext* opCtx);

    const ShardsvrCreateCollectionRequest& getOriginalRequest() {
        return _request;
    };

protected:
    const NamespaceString& nss() const override;

private:
    StringData serializePhase(const Phase& phase) const override {
        return CreateCollectionCoordinatorPhase_serializer(phase);
    }

    bool _mustAlwaysMakeProgress() override;

    ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                  const CancellationToken& token) noexcept override;

    ExecutorFuture<void> _cleanupOnAbort(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                         const CancellationToken& token,
                                         const Status& status) noexcept override;

    // Check the command arguments passed and validate that the collection has not been tracked from
    // another request.
    void _checkPreconditions(OperationContext* opCtx);

    // Enter to the critical section on the coordinator for the namespace and its buckets namespace.
    // Only blocks writes.
    void _enterWriteCriticalSectionOnCoordinator(OperationContext* opCtx);

    // Translate the request parameters and persist them in the coordinator document.
    void _translateRequestParameters(OperationContext* opCtx);

    // Enter to the critical section on the coordinator for the namespace and its buckets namespace.
    // Only blocks writes. Additionally, checks if the collection is empty and sets the
    // collectionExistsAndIsEmpty parameter on the coordiantor document.
    void _enterWriteCriticalSectionOnDataShardAndCheckCollectionEmpty(
        OperationContext* opCtx,
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& token);

    // Clone the indexes from the data shard to the coordinator. This ensures that the coordinator
    // has the most up to date indexes.
    void _syncIndexesOnCoordinator(OperationContext* opCtx,
                                   const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
                                   const CancellationToken& token);

    // Ensure that the collection is created locally and build the shard key index if necessary.
    void _createCollectionOnCoordinator(
        OperationContext* opCtx,
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& token);

    // Enter to the critical section on the specified shards. Blocks writes and reads.
    void _enterCriticalSectionOnShards(
        OperationContext* opCtx,
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& token,
        const NamespaceString& nss,
        const std::vector<ShardId>& shardIds,
        CriticalSectionBlockTypeEnum blockType);

    // Enter to the critical section on all the shards. Blocks writes and reads.
    void _enterCriticalSection(OperationContext* opCtx,
                               const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
                               const CancellationToken& token);

    // Fetches the collection options and indexes from the specified shard.
    OptionsAndIndexes _getCollectionOptionsAndIndexes(OperationContext* opCtx,
                                                      const ShardId& fromShard);

    // Broadcast create collection to the other shards.
    void _createCollectionOnParticipants(
        OperationContext* opCtx, const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

    // Commits the create collection operation to the global catalog within a transaction.
    void _commitOnShardingCatalog(OperationContext* opCtx,
                                  const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
                                  const CancellationToken& token);

    // Ensure that the change stream event gets emitted and install the new filtering metadata.
    void _setPostCommitMetadata(OperationContext* opCtx,
                                const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

    // Exit from the critical section on all the shards, unblocking reads and writes. On the
    // participant shards, it is set to clear the filtering metadata after exiting the critical
    // section.
    void _exitCriticalSection(OperationContext* opCtx,
                              const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
                              const CancellationToken& token);

    // Exit critical sections on participant shards.
    void _exitCriticalSectionOnShards(OperationContext* opCtx,
                                      bool throwIfReasonDiffers,
                                      std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                      const CancellationToken& token,
                                      const std::vector<ShardId>& shardIds);

    // Support methods to generate the events required by change stream readers before/after the
    // commit to the global catalog.
    void _notifyChangeStreamReadersOnUpcomingCommit(
        OperationContext* opCtx,
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& token);
    void _notifyChangeStreamReadersOnPlacementChanged(
        OperationContext* opCtx,
        const Timestamp& commitTime,
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& token);

    const mongo::ShardsvrCreateCollectionRequest _request;

    const BSONObj _critSecReason;

    // Set on successful completion of the coordinator
    boost::optional<CreateCollectionResponse> _result;

    // The fields below are populated on the first execution. They will need to be re-calculated on
    // following executions of the coordinator in case of error.
    boost::optional<UUID> _uuid;
    boost::optional<bool> _collectionEmpty;
    boost::optional<InitialSplitPolicy::ShardCollectionConfig> _initialChunks;
};

}  // namespace mongo
