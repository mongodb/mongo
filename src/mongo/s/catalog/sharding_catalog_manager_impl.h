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

#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/catalog/sharding_catalog_manager.h"
#include "mongo/s/client/shard.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

class RemoteCommandTargeter;

/**
 * Implements the catalog manager for writing to replica set config servers.
 */
class ShardingCatalogManagerImpl final : public ShardingCatalogManager {
public:
    ShardingCatalogManagerImpl(std::unique_ptr<executor::TaskExecutor> addShardExecutor);
    virtual ~ShardingCatalogManagerImpl();

    /**
     * Safe to call multiple times as long as the calls are externally synchronized to be
     * non-overlapping.
     */
    Status startup() override;

    void shutDown(OperationContext* txn) override;

    Status initializeConfigDatabaseIfNeeded(OperationContext* txn) override;

    void discardCachedConfigDatabaseInitializationState() override;

    Status addShardToZone(OperationContext* txn,
                          const std::string& shardName,
                          const std::string& zoneName) override;

    Status removeShardFromZone(OperationContext* txn,
                               const std::string& shardName,
                               const std::string& zoneName) override;

    Status assignKeyRangeToZone(OperationContext* txn,
                                const NamespaceString& ns,
                                const ChunkRange& range,
                                const std::string& zoneName) override;

    Status removeKeyRangeFromZone(OperationContext* txn,
                                  const NamespaceString& ns,
                                  const ChunkRange& range) override;

    Status commitChunkSplit(OperationContext* txn,
                            const NamespaceString& ns,
                            const OID& requestEpoch,
                            const ChunkRange& range,
                            const std::vector<BSONObj>& splitPoints,
                            const std::string& shardName) override;

    Status commitChunkMerge(OperationContext* txn,
                            const NamespaceString& ns,
                            const OID& requestEpoch,
                            const std::vector<BSONObj>& chunkBoundaries,
                            const std::string& shardName) override;

    StatusWith<BSONObj> commitChunkMigration(OperationContext* txn,
                                             const NamespaceString& nss,
                                             const ChunkType& migratedChunk,
                                             const boost::optional<ChunkType>& controlChunk,
                                             const OID& collectionEpoch,
                                             const ShardId& fromShard,
                                             const ShardId& toShard) override;

    void appendConnectionStats(executor::ConnectionPoolStats* stats) override;

    StatusWith<std::string> addShard(OperationContext* txn,
                                     const std::string* shardProposedName,
                                     const ConnectionString& shardConnectionString,
                                     const long long maxSize) override;

    BSONObj createShardIdentityUpsertForAddShard(OperationContext* txn,
                                                 const std::string& shardName) override;

    Status setFeatureCompatibilityVersionOnShards(OperationContext* txn,
                                                  const std::string& version) override;

private:
    /**
     * Performs the necessary checks for version compatibility and creates a new config.version
     * document if the current cluster config is empty.
     */
    Status _initConfigVersion(OperationContext* txn);

    /**
     * Builds all the expected indexes on the config server.
     */
    Status _initConfigIndexes(OperationContext* txn);

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
        OperationContext* txn,
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
    StatusWith<ShardType> _validateHostAsShard(OperationContext* txn,
                                               std::shared_ptr<RemoteCommandTargeter> targeter,
                                               const std::string* shardProposedName,
                                               const ConnectionString& connectionString);

    /**
     * Runs the listDatabases command on the specified host and returns the names of all databases
     * it returns excluding those named local, config and admin, since they serve administrative
     * purposes.
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

    // Executor specifically used for sending commands to servers that are in the process of being
    // added as shards.  Does not have any connection hook set on it, thus it can be used to talk
    // to servers that are not yet in the ShardRegistry.
    std::unique_ptr<executor::TaskExecutor> _executorForAddShard;  // (R)

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
