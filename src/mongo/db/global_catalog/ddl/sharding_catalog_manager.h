/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/fetcher.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/ddl/notify_sharding_event_gen.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/global_catalog/type_namespace_placement_gen.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/local_catalog/ddl/coll_mod_gen.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_cache.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/topology/remove_shard_exception.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/functional.h"
#include "mongo/util/uuid.h"

#include <climits>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
/**
 * Implements modifications to the sharding catalog metadata.
 *
 * TODO: Currently the code responsible for writing the sharding catalog metadata is split between
 * this class and ShardingCatalogClient. Eventually all methods that write catalog data should be
 * moved out of ShardingCatalogClient and into this class.
 */
class ShardingCatalogManager {
    ShardingCatalogManager(const ShardingCatalogManager&) = delete;
    ShardingCatalogManager& operator=(const ShardingCatalogManager&) = delete;

public:
    ShardingCatalogManager(ServiceContext* serviceContext,
                           std::shared_ptr<executor::TaskExecutor> addShardExecutor,
                           std::shared_ptr<Shard> localConfigShard,
                           std::unique_ptr<ShardingCatalogClient> localCatalogClient);
    ~ShardingCatalogManager();

    struct ShardAndCollectionPlacementVersions {
        ChunkVersion shardPlacementVersion;
        ChunkVersion collectionPlacementVersion;
    };

    /**
     * Instantiates an instance of the sharding catalog manager and installs it on the specified
     * service context. This method is not thread-safe and must be called only once when the service
     * is starting.
     */
    static void create(ServiceContext* serviceContext,
                       std::shared_ptr<executor::TaskExecutor> addShardExecutor,
                       std::shared_ptr<Shard> localConfigShard,
                       std::unique_ptr<ShardingCatalogClient> localCatalogClient);

    /**
     * Retrieves the per-service instance of the ShardingCatalogManager. This instance is only
     * available if the node is running as a config server.
     */
    static ShardingCatalogManager* get(ServiceContext* serviceContext);
    static ShardingCatalogManager* get(OperationContext* operationContext);

    /**
     * Safe to call multiple times as long as the calls are externally synchronized to be
     * non-overlapping.
     */
    void startup();

    /**
     * Performs necessary cleanup when shutting down cleanly.
     */
    void shutDown();

    //
    // Sharded cluster initialization logic
    //

    /**
     * Checks if this is the first start of a newly instantiated config server and if so pre-creates
     * the catalog collections and their indexes. Also generates and persists the cluster's
     * identity.
     */
    Status initializeConfigDatabaseIfNeeded(OperationContext* opCtx);

    /**
     * Invoked on cluster identity metadata rollback after replication step down. Throws out any
     * cached identity information and causes it to be reloaded/re-created on the next attempt.
     */
    void discardCachedConfigDatabaseInitializationState();

    //
    // Zone Operations
    //

    /**
     * Adds the given shardName to the zone. Returns ErrorCodes::ShardNotFound if a shard by that
     * name does not exist.
     */
    Status addShardToZone(OperationContext* opCtx,
                          const std::string& shardName,
                          const std::string& zoneName);

    /**
     * Removes the given shardName from the zone. Returns ErrorCodes::ShardNotFound if a shard by
     * that name does not exist.
     */
    Status removeShardFromZone(OperationContext* opCtx,
                               const std::string& shardName,
                               const std::string& zoneName);

    /**
     * Assigns a range of a sharded collection to a particular shard zone. If range is a prefix of
     * the shard key, the range will be converted into a new range with full shard key filled with
     * MinKey values.
     */
    void assignKeyRangeToZone(OperationContext* opCtx,
                              const NamespaceString& nss,
                              const ChunkRange& range,
                              const std::string& zoneName);

    /**
     * Removes a range from a zone.
     *
     * NOTE: unlike assignKeyRangeToZone, the given range will never be converted to include the
     * full shard key.
     */
    void removeKeyRangeFromZone(OperationContext* opCtx,
                                const NamespaceString& nss,
                                const ChunkRange& range);

    //
    // General utilities related to the ShardingCatalogManager
    //

    static void withTransactionAPI(OperationContext* opCtx,
                                   const NamespaceString& namespaceForInitialFind,
                                   txn_api::Callback callback);

    /**
     * Starts and commits a transaction on the config server, with a no-op find on the specified
     * namespace in order to internally start the transaction. All writes done inside the
     * passed-in function must assume that they are run inside a transaction that will be commited
     * after the function itself has completely finished. Does not support running transaction
     * operations remotely.
     */
    static void withTransaction(OperationContext* opCtx,
                                const NamespaceString& namespaceForInitialFind,
                                unique_function<void(OperationContext*, TxnNumber)> func);
    static void withTransaction(OperationContext* opCtx,
                                const NamespaceString& namespaceForInitialFind,
                                unique_function<void(OperationContext*, TxnNumber)> func,
                                const WriteConcernOptions& writeConcern);

    /**
     * Runs the write 'request' on namespace 'nss' in a transaction with 'txnNumber'. Write must be
     * on a collection in the config database. If expectedNumModified is specified, the number of
     * documents modified must match expectedNumModified - throws otherwise. Does not support
     * running transaction operations remotely.
     */
    BSONObj writeToConfigDocumentInTxn(OperationContext* opCtx,
                                       const NamespaceString& nss,
                                       const BatchedCommandRequest& request,
                                       TxnNumber txnNumber);

    /**
     * Inserts 'docs' to namespace 'nss'. If a txnNumber is passed in, the write will be done in a
     * transaction with 'txnNumber'. Breaks into multiple batches if 'docs' is larger than the max
     * batch size. Write must be on a collection in the config database.
     */
    void insertConfigDocuments(OperationContext* opCtx,
                               const NamespaceString& nss,
                               std::vector<BSONObj> docs,
                               boost::optional<TxnNumber> txnNumber = boost::none);

    /**
     * Find a single document while under a local transaction.
     */
    boost::optional<BSONObj> findOneConfigDocumentInTxn(OperationContext* opCtx,
                                                        const NamespaceString& nss,
                                                        TxnNumber txnNumber,
                                                        const BSONObj& query);

    /**
     * Find a single document. Returns an empty BSONObj if no matching document is found.
     */
    BSONObj findOneConfigDocument(OperationContext* opCtx,
                                  const NamespaceString& nss,
                                  const BSONObj& query);

    /**
     * Checks if the shard keys are valid for the timeseries collections in the given database.
     * If dbName is DatabaseName::kEmpty it checks in all database.
     */
    Status checkTimeseriesShardKeys(OperationContext* opCtx,
                                    const DatabaseName& dbName = DatabaseName::kEmpty);

    //
    // Chunk Operations
    //

    /**
     * Bumps the major component of the placement version for each 'shardIds'.
     */
    void bumpMajorVersionOneChunkPerShard(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          TxnNumber txnNumber,
                                          const std::vector<ShardId>& shardIds);

    /**
     * Updates metadata in the config.chunks collection to show the given chunk as split into
     * smaller chunks at the specified split points.
     *
     * Returns a ShardAndCollectionPlacementVersions object with the newly produced chunk versions
     * after the migration:
     *   - shardPlacementVersion - The new placement version of the source shard
     *   - collectionPlacementVersion - The new collection placement version after the commit
     */
    StatusWith<ShardAndCollectionPlacementVersions> commitChunkSplit(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const OID& requestEpoch,
        const boost::optional<Timestamp>& requestTimestamp,
        const ChunkRange& range,
        const std::vector<BSONObj>& splitPoints,
        const std::string& shardName);

    /**
     * Updates metadata in the config.chunks collection so the chunks within the specified key range
     * are seen merged into a single larger chunk.
     *
     * Returns a ShardAndCollectionPlacementVersions object with the newly produced chunk versions
     * after the migration:
     *   - shardPlacementVersion - The new placement version of the source shard
     *   - collectionPlacementVersion - The new collection placement version after the commit
     */
    StatusWith<ShardAndCollectionPlacementVersions> commitChunksMerge(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const boost::optional<OID>& epoch,
        const boost::optional<Timestamp>& timestamp,
        const UUID& requestCollectionUUID,
        const ChunkRange& chunkRange,
        const ShardId& shardId);

    /**
     * Updates metadata in the config.chunks collection so that all mergeable chunks belonging to
     * the specified shard for the given collection are merged within one transaction.
     *
     * Returns a ShardAndCollectionPlacementVersions object containing the new collection placement
     * version produced by the merge(s) and the total number of chunks that were merged.
     */
    StatusWith<std::pair<ShardingCatalogManager::ShardAndCollectionPlacementVersions, int>>
    commitMergeAllChunksOnShard(OperationContext* opCtx,
                                const NamespaceString& nss,
                                const ShardId& shardId,
                                int maxNumberOfChunksToMerge = INT_MAX,
                                int maxTimeProcessingChunksMS = INT_MAX);

    /**
     * Updates metadata in config.chunks collection to show the given chunk in its new shard.
     *
     * Returns a ShardAndCollectionPlacementVersions object with the newly produced chunk versions
     * after the migration:
     *   - shardPlacementVersion - The new placement version of the source shard
     *   - collectionPlacementVersion - The new collection placement version after the commit
     */
    StatusWith<ShardAndCollectionPlacementVersions> commitChunkMigration(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const ChunkType& migratedChunk,
        const OID& collectionEpoch,
        const Timestamp& collectionTimestamp,
        const ShardId& fromShard,
        const ShardId& toShard);

    /**
     * Removes the jumbo flag from the specified chunk.
     */
    void clearJumboFlag(OperationContext* opCtx,
                        const NamespaceString& nss,
                        const OID& collectionEpoch,
                        const ChunkRange& chunk);
    /**
     * If a chunk matching 'requestedChunk' exists, bumps the chunk's version to one greater than
     * the current collection placement version.
     *
     * 'nss' and 'collUUID' were added to the ConfigsvrEnsureChunkVersionIsGreaterThanCommand
     * in 5.0. They are optional in 5.0 because the request may come from a previous version (4.4)
     * that doesn't pass these extra fields.
     */
    void ensureChunkVersionIsGreaterThan(OperationContext* opCtx,
                                         const UUID& collUUID,
                                         const BSONObj& minKey,
                                         const BSONObj& maxKey,
                                         const ChunkVersion& version);

    /**
     * In a single transaction, effectively bumps the placement version for each shard in the
     * collection to be the current collection placement version's major version + 1 inside an
     * already-running transaction.
     */
    void bumpCollectionPlacementVersionAndChangeMetadataInTxn(
        OperationContext* opCtx,
        const NamespaceString& nss,
        unique_function<void(OperationContext*, TxnNumber)> changeMetadataFunc);
    void bumpCollectionPlacementVersionAndChangeMetadataInTxn(
        OperationContext* opCtx,
        const NamespaceString& nss,
        unique_function<void(OperationContext*, TxnNumber)> changeMetadataFunc,
        const WriteConcernOptions& writeConcern);

    /**
     * Same as bumpCollectionPlacementVersionAndChangeMetadataInTxn, but bumps the placement version
     * for several collections in a single transaction.
     */
    void bumpMultipleCollectionPlacementVersionsAndChangeMetadataInTxn(
        OperationContext* opCtx,
        const std::vector<NamespaceString>& collNames,
        unique_function<void(OperationContext*, TxnNumber)> changeMetadataFunc);
    void bumpMultipleCollectionPlacementVersionsAndChangeMetadataInTxn(
        OperationContext* opCtx,
        const std::vector<NamespaceString>& collNames,
        unique_function<void(OperationContext*, TxnNumber)> changeMetadataFunc,
        const WriteConcernOptions& writeConcern);

    /**
     * Performs a split on the chunk with min value "minKey". If the split fails, it is marked as
     * jumbo.
     */
    void splitOrMarkJumbo(OperationContext* opCtx,
                          const NamespaceString& nss,
                          const BSONObj& minKey,
                          boost::optional<int64_t> optMaxChunkSizeBytes);

    /**
     * In a transaction, sets the 'allowMigrations' to the requested state and bumps the collection
     * version.
     */
    void setAllowMigrationsAndBumpOneChunk(OperationContext* opCtx,
                                           const NamespaceString& nss,
                                           const boost::optional<UUID>& collectionUUID,
                                           bool allowMigrations);

    /**
     * Bump the minor version of the newest chunk on each shard
     */
    void bumpCollectionMinorVersionInTxn(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         TxnNumber txnNumber) const;

    /*
     * Set the estimated size field for a chunk. Only used for defragmentation operations
     */
    void setChunkEstimatedSize(OperationContext* opCtx,
                               const ChunkType& chunk,
                               long long estimatedDataSizeBytes,
                               const WriteConcernOptions& writeConcern);

    /*
     * Clear the estimated size for all chunks of a given collection.
     * Returns true if at least once chunk was modified.
     */
    bool clearChunkEstimatedSize(OperationContext* opCtx, const UUID& uuid);

    //
    // Database Operations
    //

    /**
     * Checks if a database with the same name, optResolvedPrimaryShard and enableSharding state
     * already exists, and if not, creates a new one that matches these prerequisites. If a database
     * already exists and matches all the prerequisites returns success, otherwise throws
     * NamespaceNotFound.
     */
    DatabaseType createDatabase(OperationContext* opCtx,
                                const DatabaseName& dbName,
                                const boost::optional<ShardId>& optResolvedPrimaryShard,
                                const SerializationContext& serializationContext);

    /*
     * TODO (SERVER-97837): Refactor the function out of ShardingCatalogManager.
     * Commits the new database metadata for a createDatabase operation.
     *
     * Throws ShardNotFound if the proposed 'primaryShard' is found to not exist. Also throws
     * ShardNotFound if the user-specified primary shard is draining. This check (and the actual)
     * commit, is done under the _kShardMembershipLock to ensure synchronization with removeShard
     * operations.
     */
    DatabaseType commitCreateDatabase(OperationContext* opCtx,
                                      const DatabaseName& dbName,
                                      const ShardId& primaryShard,
                                      bool userSelectedPrimary);

    /**
     * Updates the metadata in config.databases collection with the new primary shard for the given
     * database. This also advances the database's lastmod.
     */
    void commitMovePrimary(OperationContext* opCtx,
                           const DatabaseName& dbName,
                           const DatabaseVersion& expectedDbVersion,
                           const ShardId& toShardId,
                           const SerializationContext& serializationContext);

    //
    // Collection Operations
    //

    /**
     * Executes the commit of the refine collection shard key of an existing collection with
     * namespace 'nss' using the new transaction API. Here, 'shardKey' denotes the new shard key,
     * which must contain the old shard key as a prefix.
     *
     * Throws exception on transaction errors.
     */
    void commitRefineCollectionShardKey(OperationContext* opCtx,
                                        const NamespaceString& nss,
                                        const ShardKeyPattern& newShardKey,
                                        const Timestamp& newTimestamp,
                                        const OID& newEpoch,
                                        const Timestamp& oldTimestamp);

    void configureCollectionBalancing(OperationContext* opCtx,
                                      const NamespaceString& nss,
                                      boost::optional<int32_t> chunkSizeMB,
                                      boost::optional<bool> defragmentCollection,
                                      boost::optional<bool> enableAutoMerger,
                                      boost::optional<bool> enableBalancing);

    /**
     * Updates the bucketing parameters of a time-series collection. Also bumps the placement
     * versions for all shards.
     */
    void updateTimeSeriesBucketingParameters(OperationContext* opCtx,
                                             const NamespaceString& nss,
                                             const CollModRequest& collModRequest);

    //
    // Shard Operations
    //

    /**
     *
     * Adds a new shard. It expects a standalone mongod process or replica set to be running on the
     * provided address.
     *
     * 'shardProposedName' is an optional string with the proposed name of the shard. If it is
     * nullptr, a name will be automatically generated; if not nullptr, it cannot
     *         contain the empty string.
     * 'shardConnectionString' is the complete connection string of the shard being added.
     *
     * On success returns the name of the newly added shard.
     */
    StatusWith<std::string> addShard(OperationContext* opCtx,
                                     const FixedFCVRegion& fcvRegion,
                                     const std::string* shardProposedName,
                                     const ConnectionString& shardConnectionString,
                                     bool isConfigShard);

    /**
     * Gets the parameters to transition to a config shard
     */
    std::pair<ConnectionString, std::string> getConfigShardParameters(OperationContext* opCtx);

    /**
     *
     * Adds the config server as a shard. Thin wrapper around 'addShard' that retrieves and uses the
     * config server connection string as the shard connection string.
     *
     */
    void addConfigShard(OperationContext* opCtx, const FixedFCVRegion& fixedFcvRegion);

    /**
     * Inserts the config server shard identity document using a sentinel shard id. Requires the
     * config server's ShardingState has not already been enabled. Throws on errors.
     */
    void installConfigShardIdentityDocument(OperationContext* opCtx,
                                            bool deferShardingInitialization);

    /**
     * Checks if the shard has already been removed from the cluster. If not, checks that
     * preconditions for shard removal are met, starts draining for that shard, and returns progress
     * to reflect this. If the shard is already in draining, returns boost::none to indicate the
     * removal process should proceed.
     */
    boost::optional<RemoveShardProgress> checkPreconditionsAndStartDrain(OperationContext* opCtx,
                                                                         const ShardId& shardId);


    /**
     * Checks if the shard has already been removed from the cluster. If not, checks that shard is
     * draining and stops the draining.
     */
    void stopDrain(OperationContext* opCtx, const ShardId& shardId);

    /**
     * Checks if the shard is still draining and returns the draining status if so. If not, returns
     * drainingComplete indicating that the commit of the shard removal can proceed.
     */
    RemoveShardProgress checkDrainingProgress(OperationContext* opCtx, const ShardId& shardId);

    bool isShardCurrentlyDraining(OperationContext* opCtx, const ShardId& shardId);
    /**
     * Tries to remove a shard. To completely remove a shard from a sharded cluster,
     * the data residing in that shard must be moved to the remaining shards in the
     * cluster by "draining" chunks from that shard.
     *
     * Because of the asynchronous nature of the draining mechanism, this method returns
     * the current draining status. See ShardDrainingStatus enum definition for more details.
     */
    RemoveShardProgress removeShard(OperationContext* opCtx, const ShardId& shardId);

    /**
     * Returns a scoped lock object, which holds the _kShardMembershipLock in shared mode. While
     * this lock is held no topology changes can occur.
     */
    [[nodiscard]] Lock::SharedLock enterStableTopologyRegion(OperationContext* opCtx);

    /**
     * Returns a scoped lock object, which holds the _kShardMembershipLock in exclusive mode. This
     * should only be acquired by topology change operations.
     */
    [[nodiscard]] Lock::ExclusiveLock acquireShardMembershipLockForTopologyChange(
        OperationContext* opCtx);

    /**
     * Returns a scoped lock object, which holds the _kClusterCardinalityParameterLock in exclusive
     * mode.
     */
    [[nodiscard]] Lock::ExclusiveLock acquireClusterCardinalityParameterLockForTopologyChange(
        OperationContext* opCtx);

    /**
     * Updates the "hasTwoOrMoreShard" cluster cardinality parameter based on the number of shards
     * in the shard registry after reloading it. Cannot be called while holding the
     * _kShardMembershipLock in exclusive mode since setting cluster parameters requires taking this
     * lock in shared mode.
     */
    Status updateClusterCardinalityParameterIfNeeded(OperationContext* opCtx);

    //
    // Cluster Upgrade Operations
    //

    /**
     * Runs the setFeatureCompatibilityVersion command on all shards.
     */
    Status setFeatureCompatibilityVersionOnShards(OperationContext* opCtx, const BSONObj& cmdObj);

    /**
     * Runs _shardsvrCloneAuthoritativeMetadata on all shards.
     */
    Status runCloneAuthoritativeMetadataOnShards(OperationContext* opCtx);

    //
    // For Diagnostics
    //

    /**
     * Append information about the connection pools owned by the CatalogManager.
     */
    void appendConnectionStats(executor::ConnectionPoolStats* stats);

    /**
     * Appends information on the status of shard draining to the passed in result BSONObjBuilder
     */
    void appendShardDrainingStatus(OperationContext* opCtx,
                                   BSONObjBuilder& result,
                                   RemoveShardProgress shardDrainingStatus,
                                   ShardId shardId);

    /**
     * Appends db and coll information on the status of shard draining to the passed in result
     * BSONObjBuilder
     */
    void appendDBAndCollDrainingInfo(OperationContext* opCtx,
                                     BSONObjBuilder& result,
                                     ShardId shardId);

    /**
     * Only used for unit-tests, clears a previously-created catalog manager from the specified
     * service context, so that 'create' can be called again.
     */
    static void clearForTests(ServiceContext* serviceContext);

    //
    // Upgrade/downgrade
    //

    /**
     * Upgrade the chunk metadata to include the history field.
     */
    void upgradeChunksHistory(OperationContext* opCtx,
                              const NamespaceString& nss,
                              bool force,
                              const Timestamp& validAfter);

    /**
     * Returns a catalog client that will always run commands locally. Can only be used on a
     * config server node.
     */
    ShardingCatalogClient* localCatalogClient();

    /**
     * Returns a Shard representing the config server that will always run commands locally. Can
     * only be used on a config server node.
     */
    const std::shared_ptr<Shard>& localConfigShard();

    /**
     * Initializes the config.placementHistory collection:
        - one entry per collection and its placement information at the current timestamp
        - one entry per database with the current primary shard at the current timestamp
     */
    void initializePlacementHistory(OperationContext* opCtx);

    /**
     * Returns the oldest timestamp that is supported for history preservation.
     */
    static Timestamp getOldestTimestampSupportedForSnapshotHistory(OperationContext* opCtx);

    /**
     * Removes from config.placementHistory any document that is no longer needed to describe
     * the data distribution of the cluster from earliestClusterTime onwards (and updates the
     * related initialization metadata).
     **/
    void cleanUpPlacementHistory(OperationContext* opCtx, const Timestamp& earliestClusterTime);

    /**
     * Queries config.placementHistory to retrieve placement metadata on the requested namespace at
     * a specific point in time. When no namespace is specified, placement metadata on the whole
     * cluster will be returned. This function is meant to be exclusively invoked by config server
     * nodes.
     */
    HistoricalPlacement getHistoricalPlacement(OperationContext* opCtx,
                                               const boost::optional<NamespaceString>& nss,
                                               const Timestamp& atClusterTime);


    /**
     * Schedules an asynchronous unset of the addOrRemoveShardInProgress cluster parameter, in case
     * a previous addShard/removeShard left it enabled after a failure or crash.
     */
    void scheduleAsyncUnblockDDLCoordinators(OperationContext* opCtx);

private:
    /**
     * Performs the necessary checks for version compatibility and creates a new config.version
     * document if the current cluster config is empty.
     */
    Status _initConfigVersion(OperationContext* opCtx);

    /**
     * Builds all the expected indexes on the config server.
     */
    Status _initConfigIndexes(OperationContext* opCtx);

    /**
     * Creates config.settings (if needed) and adds a schema to the collection.
     */
    Status _initConfigSettings(OperationContext* opCtx);

    /**
     * Ensure that config.collections exists upon configsvr startup
     */
    Status _initConfigCollections(OperationContext* opCtx);

    /**
     * Drops the sessions collection on the specified host.
     */
    Status _dropSessionsCollection(OperationContext* opCtx,
                                   std::shared_ptr<RemoteCommandTargeter> targeter);

    /**
     * Runs the listDatabases command on the specified host and returns the names of all databases
     * it returns excluding those named local, config and admin, since they serve administrative
     * purposes.
     */
    StatusWith<std::vector<DatabaseName>> _getDBNamesListFromShard(
        OperationContext* opCtx, std::shared_ptr<RemoteCommandTargeter> targeter);


    /**
     * Runs a command against a "shard" that is not yet in the cluster and thus not present in the
     * ShardRegistry.
     */
    StatusWith<Shard::CommandResponse> _runCommandForAddShard(OperationContext* opCtx,
                                                              RemoteCommandTargeter* targeter,
                                                              const DatabaseName& dbName,
                                                              const BSONObj& cmdObj);

    /**
     * Appends a read committed read concern to the request object.
     */
    void _appendReadConcern(BSONObjBuilder* builder);

    /**
     * Retrieve the full chunk description from the config.
     */
    StatusWith<ChunkType> _findChunkOnConfig(OperationContext* opCtx,
                                             const UUID& uuid,
                                             const OID& epoch,
                                             const Timestamp& timestamp,
                                             const BSONObj& key);

    /**
     * Returns true if the zone with the given name has chunk ranges associated with it and the
     * shard with the given name is the only shard that it belongs to.
     */
    StatusWith<bool> _isShardRequiredByZoneStillInUse(OperationContext* opCtx,
                                                      const ReadPreferenceSetting& readPref,
                                                      const std::string& shardName,
                                                      const std::string& zoneName);

    /**
     * Determines whether to absorb the cluster parameters on the newly added shard (if we're
     * converting from a replica set to a sharded cluster) or set the cluster parameters stored on
     * the config server in the newly added shard.
     */
    void _standardizeClusterParameters(OperationContext* opCtx, RemoteCommandTargeter& targeter);

    /**
     * Execute the migration chunk updates using the internal transaction API.
     */
    void _commitChunkMigrationInTransaction(OperationContext* opCtx,
                                            const NamespaceString& nss,
                                            const ChunkType& migratedChunk,
                                            const std::vector<ChunkType>& splitChunks,
                                            const boost::optional<ChunkType>& controlChunk,
                                            const ShardId& donorShardId);

    /**
     * Execute the merge chunk updates using the internal transaction API.
     */
    void _mergeChunksInTransaction(OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   const UUID& collectionUUID,
                                   const ChunkVersion& mergeVersion,
                                   const Timestamp& validAfter,
                                   const ChunkRange& chunkRange,
                                   const ShardId& shardId,
                                   std::shared_ptr<std::vector<ChunkType>> chunksToMerge);

    struct SplitChunkInTransactionResult {
        ChunkVersion currentMaxVersion;
        std::vector<ChunkType> newChunks;
    };

    /**
     * Execute the split chunk operations using the internal transaction API.
     */
    SplitChunkInTransactionResult _splitChunkInTransaction(OperationContext* opCtx,
                                                           const NamespaceString& nss,
                                                           const ChunkRange& range,
                                                           const std::string& shardName,
                                                           const ChunkType& origChunk,
                                                           const ChunkVersion& collPlacementVersion,
                                                           const std::vector<BSONObj>& splitPoints);

    /**
     * Updates the "hasTwoOrMoreShard" cluster cardinality parameter after an add or remove shard
     * operation if needed. Can only be called after refreshing the shard registry and while holding
     * _kClusterCardinalityParameterLock lock in exclusive mode to avoid interleaving with other
     * add/remove shard operation and its set cluster cardinality parameter operation.
     */
    Status _updateClusterCardinalityParameterAfterAddShardIfNeeded(const Lock::ExclusiveLock&,
                                                                   OperationContext* opCtx);
    Status _updateClusterCardinalityParameterAfterRemoveShardIfNeeded(const Lock::ExclusiveLock&,
                                                                      OperationContext* opCtx);

    // The owning service context
    ServiceContext* const _serviceContext;

    // Executor specifically used for sending commands to servers that are in the process of being
    // added as shards. Does not have any connection hook set on it, thus it can be used to talk to
    // servers that are not yet in the ShardRegistry.
    const std::shared_ptr<executor::TaskExecutor> _executorForAddShard;

    // A ShardLocal and ShardingCatalogClient with a ShardLocal used for local connections.
    const std::shared_ptr<Shard> _localConfigShard;
    const std::unique_ptr<ShardingCatalogClient> _localCatalogClient;

    //
    // All member variables are labeled with one of the following codes indicating the
    // synchronization rules for accessing them.
    //
    // (M) Must hold _mutex for access.
    // (R) Read only, can only be written during initialization.
    // (S) Self-synchronizing; access in any way from any context.
    //

    stdx::mutex _mutex;

    // True if startup() has been called.
    bool _started{false};  // (M)

    // True if initializeConfigDatabaseIfNeeded() has been called and returned successfully.
    bool _configInitialized{false};  // (M)

    // Resource lock order:
    // _kShardMembershipLock -> _kChunkOpLock
    // _kZoneOpLock

    /**
     * Lock that is held in exclusive mode during the commit phase of an add/remove shard operation.
     */
    Lock::ResourceMutex _kShardMembershipLock;

    /**
     * Lock that guards changes to the cluster cardinality parameter.
     */
    Lock::ResourceMutex _kClusterCardinalityParameterLock;

    /**
     * Lock for chunk split/merge/move operations. This should be acquired when doing split/merge/
     * move operations that can affect the config.chunks collection.
     * No other locks should be held when locking this. If an operation needs to take database
     * locks (for example to write to a local collection) those locks should be taken after
     * taking this.
     */
    Lock::ResourceMutex _kChunkOpLock;

    /**
     * Lock for shard zoning operations. This should be acquired when doing any operations that
     * can affect the config.tags collection or the tags field of the config.shards collection.
     * No other locks should be held when locking this. If an operation needs to take database
     * locks (for example to write to a local collection) those locks should be taken after
     * taking this.
     */
    Lock::ResourceMutex _kZoneOpLock;
};

}  // namespace mongo
