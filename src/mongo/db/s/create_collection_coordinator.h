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

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <memory>
#include <string>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/db/s/config/initial_split_policy.h"
#include "mongo/db/s/create_collection_coordinator_document_gen.h"
#include "mongo/db/s/participant_block_gen.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/sharding_ddl_coordinator.h"
#include "mongo/db/s/sharding_ddl_coordinator_service.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/uuid.h"

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

// This interface allows the retrieval of the outcome of a shardCollection request (which may be
// served by different types of Coordinator)
class CreateCollectionResponseProvider {
public:
    virtual CreateCollectionResponse getResult(OperationContext* opCtx) = 0;
    virtual ~CreateCollectionResponseProvider() {}
};

struct OptionsAndIndexes {
    BSONObj options;
    std::vector<BSONObj> indexSpecs;
    BSONObj idIndexSpec;
};

// TODO (SERVER-79304): Remove once 8.0 becomes last LTS.
class CreateCollectionCoordinatorLegacy
    : public RecoverableShardingDDLCoordinator<CreateCollectionCoordinatorDocumentLegacy,
                                               CreateCollectionCoordinatorPhaseLegacyEnum>,
      public CreateCollectionResponseProvider {
public:
    using CoordDoc = CreateCollectionCoordinatorDocumentLegacy;
    using Phase = CreateCollectionCoordinatorPhaseLegacyEnum;

    CreateCollectionCoordinatorLegacy(ShardingDDLCoordinatorService* service,
                                      const BSONObj& initialState)
        : RecoverableShardingDDLCoordinator(service, "CreateCollectionCoordinator", initialState),
          _request(_doc.getShardsvrCreateCollectionRequest()),
          _critSecReason(BSON("command"
                              << "createCollection"
                              << "ns"
                              << NamespaceStringUtil::serialize(
                                     originalNss(), SerializationContext::stateDefault()))) {}

    ~CreateCollectionCoordinatorLegacy() = default;


    void checkIfOptionsConflict(const BSONObj& coorDoc) const override;

    void appendCommandInfo(BSONObjBuilder* cmdInfoBuilder) const override;

    /**
     * Waits for the termination of the parent DDLCoordinator (so all the resources are liberated)
     * and then return the
     */
    CreateCollectionResponse getResult(OperationContext* opCtx) override;

protected:
    const NamespaceString& nss() const override;

private:
    StringData serializePhase(const Phase& phase) const override {
        return CreateCollectionCoordinatorPhaseLegacy_serializer(phase);
    }

    OptionsAndIndexes _getCollectionOptionsAndIndexes(OperationContext* opCtx,
                                                      const NamespaceStringOrUUID& nssOrUUID);

    ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                  const CancellationToken& token) noexcept override;

    ExecutorFuture<void> _cleanupOnAbort(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                         const CancellationToken& token,
                                         const Status& status) noexcept override;

    mongo::ShardsvrCreateCollectionRequest _request;

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

class CreateCollectionCoordinator
    : public RecoverableShardingDDLCoordinator<CreateCollectionCoordinatorDocument,
                                               CreateCollectionCoordinatorPhaseEnum>,
      public CreateCollectionResponseProvider {
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

    ~CreateCollectionCoordinator() = default;


    void checkIfOptionsConflict(const BSONObj& coorDoc) const override;

    void appendCommandInfo(BSONObjBuilder* cmdInfoBuilder) const override;

    CreateCollectionResponse getResult(OperationContext* opCtx) override;

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
    void _checkPreconditions();

    // Enter to the critical section on the coordinator for the namespace and its buckets namespace.
    // Only blocks writes.
    void _enterWriteCriticalSectionOnCoordinator();

    // Translate the request parameters and persist them in the coordinator document.
    void _translateRequestParameters();

    // Enter to the critical section on the coordinator for the namespace and its buckets namespace.
    // Only blocks writes. Additionally, checks if the collection is empty and sets the
    // collectionExistsAndIsEmpty parameter on the coordiantor document.
    void _enterWriteCriticalSectionOnDataShardAndCheckCollectionEmpty(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
        const CancellationToken& token);

    // Clone the indexes from the data shard to the coordinator. This ensures that the coordinator
    // has the most up to date indexes.
    void _syncIndexesOnCoordinator(const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
                                   const CancellationToken& token);

    // Ensure that the collection is created locally and build the shard key index if necessary.
    void _createCollectionOnCoordinator(
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
    void _enterCriticalSection(const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
                               const CancellationToken& token);

    // Fetches the collection options and indexes from the specified shard.
    OptionsAndIndexes _getCollectionOptionsAndIndexes(OperationContext* opCtx,
                                                      const ShardId& fromShard);

    // Broadcast create collection to the other shards.
    void _createCollectionOnParticipants(
        const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

    // Commits the create collection operation to the sharding catalog within a transaction.
    void _commitOnShardingCatalog(const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

    // Ensure that the change stream event gets emitted and install the new filtering metadata.
    void _setPostCommitMetadata(const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

    // Exit from the critical section on all the shards, unblocking reads and writes. On the
    // participant shards, it is set to clear the filtering metadata after exiting the critical
    // section.
    void _exitCriticalSection(const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
                              const CancellationToken& token);

    // Exit critical sections on participant shards.
    void _exitCriticalSectionOnShards(OperationContext* opCtx,
                                      bool throwIfReasonDiffers,
                                      std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                      const CancellationToken& token,
                                      const std::vector<ShardId>& shardIds);

    mongo::ShardsvrCreateCollectionRequest _request;

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
