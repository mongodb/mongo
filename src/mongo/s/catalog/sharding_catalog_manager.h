/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status_with.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

class OperationContext;
class RemoteCommandTargeter;
class ServiceContext;

/**
 * Implements modifications to the sharding catalog metadata.
 *
 * TODO: Currently the code responsible for writing the sharding catalog metadata is split between
 * this class and ShardingCatalogClient. Eventually all methods that write catalog data should be
 * moved out of ShardingCatalogClient and into this class.
 */
class ShardingCatalogManager {
    MONGO_DISALLOW_COPYING(ShardingCatalogManager);

public:
    ShardingCatalogManager(ServiceContext* serviceContext,
                           std::unique_ptr<executor::TaskExecutor> addShardExecutor);
    ~ShardingCatalogManager();

    /**
     * Instantiates an instance of the sharding catalog manager and installs it on the specified
     * service context. This method is not thread-safe and must be called only once when the service
     * is starting.
     */
    static void create(ServiceContext* serviceContext,
                       std::unique_ptr<executor::TaskExecutor> addShardExecutor);

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
    Status assignKeyRangeToZone(OperationContext* opCtx,
                                const NamespaceString& ns,
                                const ChunkRange& range,
                                const std::string& zoneName);

    /**
     * Removes a range from a zone.
     *
     * NOTE: unlike assignKeyRangeToZone, the given range will never be converted to include the
     * full shard key.
     */
    Status removeKeyRangeFromZone(OperationContext* opCtx,
                                  const NamespaceString& ns,
                                  const ChunkRange& range);

    //
    // Chunk Operations
    //

    /**
     * Updates metadata in the config.chunks collection to show the given chunk as split into
     * smaller chunks at the specified split points.
     */
    Status commitChunkSplit(OperationContext* opCtx,
                            const NamespaceString& ns,
                            const OID& requestEpoch,
                            const ChunkRange& range,
                            const std::vector<BSONObj>& splitPoints,
                            const std::string& shardName);

    /**
     * Updates metadata in the config.chunks collection so the chunks with given boundaries are seen
     * merged into a single larger chunk.
     */
    Status commitChunkMerge(OperationContext* opCtx,
                            const NamespaceString& ns,
                            const OID& requestEpoch,
                            const std::vector<BSONObj>& chunkBoundaries,
                            const std::string& shardName);

    /**
     * Updates metadata in config.chunks collection to show the given chunk in its new shard.
     */
    StatusWith<BSONObj> commitChunkMigration(OperationContext* opCtx,
                                             const NamespaceString& nss,
                                             const ChunkType& migratedChunk,
                                             const boost::optional<ChunkType>& controlChunk,
                                             const OID& collectionEpoch,
                                             const ShardId& fromShard,
                                             const ShardId& toShard);

    //
    // Database Operations
    //

    /**
     * Creates a new database or updates the sharding status for an existing one. Cannot be
     * used for the admin/config/local DBs, which should not be created or sharded manually
     * anyways.
     *
     * Returns Status::OK on success or any error code indicating the failure. These are some
     * of the known failures:
     *  - DatabaseDifferCase - database already exists, but with a different case
     *  - ShardNotFound - could not find a shard to place the DB on
     */
    Status enableSharding(OperationContext* opCtx, const std::string& dbName);


    //
    // Collection Operations
    //

    /**
     * Shards a collection. Assumes that the database is enabled for sharding.
     *
     * @param ns: namespace of collection to shard
     * @param fieldsAndOrder: shardKey pattern
     * @param defaultCollation: the default collation for the collection, to be written to
     *     config.collections. If empty, the collection default collation is simple binary
     *     comparison. Note the the shard key collation will always be simple binary comparison,
     *     even if the collection default collation is non-simple.
     * @param unique: if true, ensure underlying index enforces a unique constraint.
     * @param initPoints: create chunks based on a set of specified split points.
     * @param initShardIds: If non-empty, specifies the set of shards to assign chunks between.
     *     Otherwise all chunks will be assigned to the primary shard for the database.
     */
    void shardCollection(OperationContext* opCtx,
                         const std::string& ns,
                         const ShardKeyPattern& fieldsAndOrder,
                         const BSONObj& defaultCollation,
                         bool unique,
                         const std::vector<BSONObj>& initPoints,
                         const bool distributeInitialChunks);

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
     * 'maxSize' is the optional space quota in bytes. Zero means there's no limitation to space
     * usage.
     *
     * On success returns the name of the newly added shard.
     */
    StatusWith<std::string> addShard(OperationContext* opCtx,
                                     const std::string* shardProposedName,
                                     const ConnectionString& shardConnectionString,
                                     const long long maxSize);

    //
    // Cluster Upgrade Operations
    //

    /**
     * Returns a BSON representation of an update request that can be used to insert a shardIdentity
     * doc into the shard for the given shardType (or update the shard's existing shardIdentity
     * doc's configsvrConnString if the _id, shardName, and clusterId do not conflict).
     */
    BSONObj createShardIdentityUpsertForAddShard(OperationContext* opCtx,
                                                 const std::string& shardName);

    /**
     * Runs the setFeatureCompatibilityVersion command on all shards.
     */
    Status setFeatureCompatibilityVersionOnShards(OperationContext* opCtx,
                                                  const std::string& version);

    //
    // For Diagnostics
    //

    /**
     * Append information about the connection pools owned by the CatalogManager.
     */
    void appendConnectionStats(executor::ConnectionPoolStats* stats);

    /**
     * Only used for unit-tests, clears a previously-created catalog manager from the specified
     * service context, so that 'create' can be called again.
     */
    static void clearForTests(ServiceContext* serviceContext);

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
     * Used during addShard to determine if there is already an existing shard that matches the
     * shard that is currently being added.  An OK return with boost::none indicates that there
     * is no conflicting shard, and we can proceed trying to add the new shard.  An OK return
     * with a ShardType indicates that there is an existing shard that matches the shard being added
     * but since the options match, this addShard request can do nothing and return success.  A
     * non-OK return either indicates a problem reading the existing shards from disk or more likely
     * indicates that an existing shard conflicts with the shard being added and they have different
     * options, so the addShard attempt must be aborted.
     */
    StatusWith<boost::optional<ShardType>> _checkIfShardExists(
        OperationContext* opCtx,
        const ConnectionString& propsedShardConnectionString,
        const std::string* shardProposedName,
        long long maxSize);

    /**
     * Validates that the specified endpoint can serve as a shard server. In particular, this
     * this function checks that the shard can be contacted and that it is not already member of
     * another sharded cluster.
     *
     * @param targeter For sending requests to the shard-to-be.
     * @param shardProposedName Optional proposed name for the shard. Can be omitted in which case
     *      a unique name for the shard will be generated from the shard's connection string. If it
     *      is not omitted, the value cannot be the empty string.
     *
     * On success returns a partially initialized ShardType object corresponding to the requested
     * shard. It will have the hostName field set and optionally the name, if the name could be
     * generated from either the proposed name or the connection string set name. The returned
     * shard's name should be checked and if empty, one should be generated using some uniform
     * algorithm.
     */
    StatusWith<ShardType> _validateHostAsShard(OperationContext* opCtx,
                                               std::shared_ptr<RemoteCommandTargeter> targeter,
                                               const std::string* shardProposedName,
                                               const ConnectionString& connectionString);

    /**
     * Runs the listDatabases command on the specified host and returns the names of all databases
     * it returns excluding those named local, config and admin, since they serve administrative
     * purposes.
     */
    StatusWith<std::vector<std::string>> _getDBNamesListFromShard(
        OperationContext* opCtx, std::shared_ptr<RemoteCommandTargeter> targeter);

    /**
     * Runs a command against a "shard" that is not yet in the cluster and thus not present in the
     * ShardRegistry.
     */
    StatusWith<Shard::CommandResponse> _runCommandForAddShard(OperationContext* opCtx,
                                                              RemoteCommandTargeter* targeter,
                                                              const std::string& dbName,
                                                              const BSONObj& cmdObj);

    // The owning service context
    ServiceContext* const _serviceContext;

    // Executor specifically used for sending commands to servers that are in the process of being
    // added as shards. Does not have any connection hook set on it, thus it can be used to talk to
    // servers that are not yet in the ShardRegistry.
    const std::unique_ptr<executor::TaskExecutor> _executorForAddShard;

    //
    // All member variables are labeled with one of the following codes indicating the
    // synchronization rules for accessing them.
    //
    // (M) Must hold _mutex for access.
    // (R) Read only, can only be written during initialization.
    // (S) Self-synchronizing; access in any way from any context.
    //

    stdx::mutex _mutex;

    // True if shutDown() has been called. False, otherwise.
    bool _inShutdown{false};  // (M)

    // True if startup() has been called.
    bool _started{false};  // (M)

    // True if initializeConfigDatabaseIfNeeded() has been called and returned successfully.
    bool _configInitialized{false};  // (M)

    /**
     * Lock for shard zoning operations. This should be acquired when doing any operations that
     * can affect the config.tags collection or the tags field of the config.shards collection.
     * No other locks should be held when locking this. If an operation needs to take database
     * locks (for example to write to a local collection) those locks should be taken after
     * taking this.
     */
    Lock::ResourceMutex _kZoneOpLock;

    /**
     * Lock for chunk split/merge/move operations. This should be acquired when doing split/merge/
     * move operations that can affect the config.chunks collection.
     * No other locks should be held when locking this. If an operation needs to take database
     * locks (for example to write to a local collection) those locks should be taken after
     * taking this.
     */
    Lock::ResourceMutex _kChunkOpLock;

    /**
     * Lock that guards changes to the set of shards in the cluster (ie addShard and removeShard
     * requests).
     * TODO: Currently only taken during addShard requests, this should also be taken in X mode
     * during removeShard, once removeShard is moved to run on the config server primary instead of
     * on mongos.  At that point we should also change any operations that expect the shard not to
     * be removed while they are running (such as removeShardFromZone) to take this in shared mode.
     */
    Lock::ResourceMutex _kShardMembershipLock;
};

}  // namespace mongo
