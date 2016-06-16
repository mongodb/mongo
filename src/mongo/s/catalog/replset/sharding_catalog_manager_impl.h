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

#include <vector>

#include "mongo/executor/task_executor.h"
#include "mongo/s/catalog/sharding_catalog_manager.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

class DatabaseType;
class RemoteCommandTargeter;
class ShardingCatalogClient;
class VersionType;

namespace executor {
class TaskExecutor;
}  // namespace executor

/**
 * Implements the catalog manager for writing to replica set config servers.
 */
class ShardingCatalogManagerImpl final : public ShardingCatalogManager {
public:
    ShardingCatalogManagerImpl(ShardingCatalogClient* catalogClient,
                               std::unique_ptr<executor::TaskExecutor> addShardExecutor);
    virtual ~ShardingCatalogManagerImpl();

    /**
     * Safe to call multiple times as long as the calls are externally synchronized to be
     * non-overlapping.
     */
    Status startup() override;

    void shutDown(OperationContext* txn) override;

    StatusWith<std::string> addShard(OperationContext* txn,
                                     const std::string* shardProposedName,
                                     const ConnectionString& shardConnectionString,
                                     const long long maxSize) override;

    Status addShardToZone(OperationContext* txn,
                          const std::string& shardName,
                          const std::string& zoneName) override;

    Status removeShardFromZone(OperationContext* txn,
                               const std::string& shardName,
                               const std::string& zoneName) override;

    void appendConnectionStats(executor::ConnectionPoolStats* stats) override;

    Status initializeConfigDatabaseIfNeeded(OperationContext* txn) override;

    Status upsertShardIdentityOnShard(OperationContext* txn, ShardType shardType) override;


    BSONObj createShardIdentityUpsertForAddShard(OperationContext* txn,
                                                 const std::string& shardName) override;

private:
    /**
     * Generates a unique name to be given to a newly added shard.
     */
    StatusWith<std::string> _generateNewShardName(OperationContext* txn);

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
    StatusWith<ShardType> _validateHostAsShard(OperationContext* txn,
                                               std::shared_ptr<RemoteCommandTargeter> targeter,
                                               const std::string* shardProposedName,
                                               const ConnectionString& connectionString);

    /**
     * Runs the listDatabases command on the specified host and returns the names of all databases
     * it returns excluding those named local and admin, since they serve administrative purpose.
     */
    StatusWith<std::vector<std::string>> _getDBNamesListFromShard(
        OperationContext* txn, std::shared_ptr<RemoteCommandTargeter> targeter);

    /**
     * Runs a command against a "shard" that is not yet in the cluster and thus not present in the
     * ShardRegistry.
     */
    StatusWith<Shard::CommandResponse> _runCommandForAddShard(OperationContext* txn,
                                                              RemoteCommandTargeter* targeter,
                                                              const std::string& dbName,
                                                              const BSONObj& cmdObj);

    /**
     * Returns the current cluster schema/protocol version.
     */
    StatusWith<VersionType> _getConfigVersion(OperationContext* txn);

    /**
     * Performs the necessary checks for version compatibility and creates a new config.version
     * document if the current cluster config is empty.
     */
    Status _initConfigVersion(OperationContext* txn);

    /**
     * Callback function used when rescheduling an addShard task after the first attempt failed.
     * Checks if the callback has been canceled, and if not, proceeds to call
     * _scheduleAddShardTask.
     */
    void _scheduleAddShardTaskUnlessCanceled(const executor::TaskExecutor::CallbackArgs& cbArgs,
                                             const ShardType shardType,
                                             std::shared_ptr<RemoteCommandTargeter> targeter,
                                             const BSONObj commandRequest);

    /**
     * For rolling upgrade and backwards compatibility with 3.2 mongos, schedules an asynchronous
     * task against the addShard executor to upsert a shardIdentity doc into the new shard
     * described by shardType. If there is an existing such task for this shardId (as tracked by
     * the _addShardHandles map), a new task is not scheduled. There could be an existing such task
     * if addShard was called previously, but the upsert has not yet succeeded on the shard.
     */
    void _scheduleAddShardTask(const ShardType shardType,
                               std::shared_ptr<RemoteCommandTargeter> targeter,
                               const BSONObj commandRequest,
                               const bool isRetry);

    /**
     * Callback function for the asynchronous upsert of the shardIdentity doc scheduled by
     * scheduleAddShardTaskIfNeeded. Checks the response from the shard, and updates config.shards
     * to mark the shard as shardAware on success. On failure to perform the upsert, this callback
     * schedules scheduleAddShardTaskIfNeeded to be called again after a delay.
     */
    void _handleAddShardTaskResponse(
        const executor::TaskExecutor::RemoteCommandCallbackArgs& cbArgs,
        ShardType shardType,
        std::shared_ptr<RemoteCommandTargeter> targeter);

    /**
     * Checks if a running or scheduled addShard task exists for the shard with id shardId.
     * The caller must hold _addShardHandlesMutex.
     */
    bool _hasAddShardHandle_inlock(const ShardId& shardId);

    /**
     * Adds CallbackHandle handle for the shard with id shardID to the map of running or scheduled
     * addShard tasks.
     * The caller must hold _addShardHandlesMutex.
     */
    void _trackAddShardHandle_inlock(
        const ShardId shardId, const StatusWith<executor::TaskExecutor::CallbackHandle>& handle);

    /**
     * Removes the handle to a running or scheduled addShard task callback for the shard with id
     * shardId.
     * The caller must hold _addShardHandlesMutex.
     */
    void _untrackAddShardHandle_inlock(const ShardId& shardId);

    /**
     * Builds all the expected indexes on the config server.
     */
    Status _initConfigIndexes(OperationContext* txn);
    //
    // All member variables are labeled with one of the following codes indicating the
    // synchronization rules for accessing them.
    //
    // (M) Must hold _mutex for access.
    // (R) Read only, can only be written during initialization.
    // (S) Self-synchronizing; access in any way from any context.
    //

    stdx::mutex _mutex;

    // Pointer to the ShardingCatalogClient that can be used to read config server data.
    // This pointer is not owned, so it is important that the object it points to continues to be
    // valid for the lifetime of this ShardingCatalogManager.
    ShardingCatalogClient* _catalogClient;  // (R)

    // Executor specifically used for sending commands to servers that are in the process of being
    // added as shards.  Does not have any connection hook set on it, thus it can be used to talk
    // to servers that are not yet in the ShardRegistry.
    std::unique_ptr<executor::TaskExecutor> _executorForAddShard;  // (R)

    // True if shutDown() has been called. False, otherwise.
    bool _inShutdown = false;  // (M)

    // True if startup() has been called.
    bool _started = false;  // (M)

    // True if initializeConfigDatabaseIfNeeded() has been called and returned successfully.
    bool _configInitialized = false;  // (M)

    // For rolling upgrade and backwards compatibility with 3.2 mongos, maintains a mapping of
    // a shardId to an outstanding addShard task scheduled against the _executorForAddShard.
    // A "addShard" task upserts the shardIdentity document into the new shard. Such a task is
    // scheduled:
    // 1) on a config server's transition to primary for each shard in config.shards that is not
    // marked as sharding aware
    // 2) on a direct insert to the config.shards collection (usually from a 3.2 mongos).
    // This map tracks that only one such task per shard can be running at a time.
    std::map<ShardId, executor::TaskExecutor::CallbackHandle> _addShardHandles;

    // Protects the _addShardHandles map.
    stdx::mutex _addShardHandlesMutex;
};

}  // namespace mongo
