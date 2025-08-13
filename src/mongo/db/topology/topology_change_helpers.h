/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/fetcher.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/global_catalog/type_shard_identity.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/topology/remove_shard_draining_progress_gen.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/task_executor.h"

#include <climits>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
class ShardsvrAddShard;
class BSONObj;
class OperationContext;

class ShardId;

// TODO (SERVER-97816): remove these helpers and move the implementations into the add/remove shard
// coordinators once 9.0 becomes last LTS.
namespace topology_change_helpers {

// Returns the count of range deletion tasks locally on the config server.
long long getRangeDeletionCount(OperationContext* opCtx);

// Calls ShardsvrJoinMigrations locally on the config server.
void joinMigrations(OperationContext* opCtx);

/**
 * Used during addShard to determine if there is already an existing shard that matches the shard
 * that is currently being added. A boost::none indicates that there is no conflicting shard, and we
 * can proceed trying to add the new shard. A ShardType return indicates that there is an existing
 * shard that matches the shard being added but since the options match, this addShard request can
 * do nothing and return success. An exception indicates either a problem reading the existing
 * shards from disk or more likely indicates that an existing shard conflicts with the shard being
 * added and they have different options, so the addShard attempt must be aborted.
 */
boost::optional<ShardType> getExistingShard(OperationContext* opCtx,
                                            const ConnectionString& proposedShardConnectionString,
                                            const boost::optional<StringData>& proposedShardName,
                                            ShardingCatalogClient& localCatalogClient);

/**
 * Runs a command against a "shard" that is not yet in the cluster and thus not present in the
 * ShardRegistry.
 */
Shard::CommandResponse runCommandForAddShard(
    OperationContext* opCtx,
    RemoteCommandTargeter& targeter,
    const DatabaseName& dbName,
    const BSONObj& cmdObj,
    std::shared_ptr<executor::TaskExecutor> executorForAddShard);

enum UserWriteBlockingLevel {
    None = 0u,                     ///< Don't block anything
    DDLOperations = (1u << 0),     ///< Block DDLOperations
    Writes = (1u << 1),            ///< Block direct user writes
    All = DDLOperations | Writes,  ///< Block everything
};

/**
 * Sets the user write blocking state on a given target
 *
 * @param opCtx: The operation context
 * @param targeter: The targeter for the remote host target
 * @param level: The level of blocking. See `UserWriteBlockingLevel` for clarification
 * @param block: Flag to turn blocking on or off on the target for the given level. Eg: block ==
 *                  true && level == DDLOperations means blocking on DDL Operation (but not user
 *                  writes). block == false && level == All means unblock (allow) all user level
 *                  write operations.
 * @param osiGenerator: A generator function for operation session info. If exists, the generated
 *                  session info will be attached to the requests (one unique for each request).
 * @param executor: A task executor to run the requests on.
 */
void setUserWriteBlockingState(
    OperationContext* opCtx,
    RemoteCommandTargeter& targeter,
    uint8_t level,
    bool block,
    boost::optional<std::function<OperationSessionInfo(OperationContext*)>> osiGenerator,
    std::shared_ptr<executor::TaskExecutor> executor);


/**
 * Retrieves all the user write blocks from the given replica set
 */
UserWriteBlockingLevel getUserWriteBlocksFromReplicaSet(
    OperationContext* opCtx,
    RemoteCommandTargeter& targeter,
    std::shared_ptr<executor::TaskExecutor> executor);

/**
 * Retrieves all the databaseName from the given replica set
 */
std::vector<DatabaseName> getDBNamesListFromReplicaSet(
    OperationContext* opCtx,
    RemoteCommandTargeter& targeter,
    std::shared_ptr<executor::TaskExecutor> executor);

/**
 * Removes the replica set monitor of the given connection
 */
void removeReplicaSetMonitor(OperationContext* opCtx, const ConnectionString& connectionString);

/**
 * Validates that the specified endpoint can serve as a shard server. In particular, this
 * this function checks that the shard can be contacted and that it is not already member of
 * another sharded cluster.
 */
void validateHostAsShard(OperationContext* opCtx,
                         RemoteCommandTargeter& targeter,
                         const ConnectionString& connectionString,
                         bool isConfigShard,
                         std::shared_ptr<executor::TaskExecutor> executor);

/**
 * Given the shard draining state, returns the message that should be included as part of the remove
 * shard response.
 */
std::string getRemoveShardMessage(const ShardDrainingStateEnum& status);

/**
 * Gets the cluster time keys on the given replica set and then saves them locally.
 */
void getClusterTimeKeysFromReplicaSet(OperationContext* opCtx,
                                      RemoteCommandTargeter& targeter,
                                      std::shared_ptr<executor::TaskExecutor> executor);

/**
 * Creates a valid name for the new shard
 */
std::string createShardName(OperationContext* opCtx,
                            RemoteCommandTargeter& targeter,
                            bool isConfigShard,
                            const boost::optional<StringData>& proposedShardName,
                            std::shared_ptr<executor::TaskExecutor> executor);

/**
 * Creates a ShardIdentity
 */
ShardIdentityType createShardIdentity(OperationContext* opCtx, const ShardId& shardName);

/**
 * Issues a command on the remote host to insert a shard identity document
 */
void installShardIdentity(
    OperationContext* opCtx,
    const ShardIdentityType& identity,
    RemoteCommandTargeter& targeter,
    boost::optional<APIParameters> apiParameters,
    boost::optional<std::function<OperationSessionInfo(OperationContext*)>> osiGenerator,
    std::shared_ptr<executor::TaskExecutor> executor);

/**
 * Installs a shard identity locally.
 * If a shard identity already exists, check if the new one is the same. If not, throws an
 * IllegalOperation.
 * Returns true if a new shard identity installed, false otherwise.
 */
bool installShardIdentity(OperationContext* opCtx, const ShardIdentityType& identity);

/**
 * Updates the shard identity document locally.
 */
void updateShardIdentity(OperationContext* opCtx, const ShardIdentityType& identity);

/**
 * Remove all existing cluster parameters on the replica set and sets the ones stored on the config
 * server.
 */
void setClusterParametersOnReplicaSet(
    OperationContext* opCtx,
    RemoteCommandTargeter& targeter,
    const TenantIdMap<std::vector<BSONObj>>& allClusterParameters,
    boost::optional<std::function<OperationSessionInfo(OperationContext*)>> osiGenerator,
    std::shared_ptr<executor::TaskExecutor> executor);

/**
 * Gets the cluster parameters set on the shard and then saves them locally.
 */
TenantIdMap<std::vector<BSONObj>> getClusterParametersFromReplicaSet(
    OperationContext* opCtx,
    RemoteCommandTargeter& targeter,
    std::shared_ptr<executor::TaskExecutor> executor);

/**
 * Given a vector of cluster parameters in disk format, sets them locally.
 */
void setClusterParametersLocally(OperationContext* opCtx,
                                 const TenantIdMap<std::vector<BSONObj>>& parameters);

TenantIdMap<std::vector<BSONObj>> getClusterParametersLocally(OperationContext* opCtx);

/*
 * Runs a count with the given query against the localConfigShard. Returns the result of that count
 * and throws any error that occurs while running this command.
 */
long long runCountCommandOnConfig(OperationContext* opCtx,
                                  std::shared_ptr<Shard> localConfigShard,
                                  const NamespaceString& nss,
                                  BSONObj query);

struct DrainingShardUsage {
    bool isFullyDrained() const {
        return removeShardCounts.getChunks() == 0 &&
            removeShardCounts.getCollectionsToMove() == 0 && removeShardCounts.getDbs() == 0 &&
            totalChunks == 0;
    }

    RemainingCounts removeShardCounts;
    // This is a failsafe check to be sure we do not accidentally remove a shard which has chunks
    // left, sharded or otherwise. It is not reported to the user and thus not included in
    // RemainingCounts.
    long long totalChunks;
};

/**
 * Counts the number of total chunks, sharded chunks, unsharded collections, and databases on the
 * shard being removed.
 */
DrainingShardUsage getDrainingProgress(OperationContext* opCtx,
                                       std::shared_ptr<Shard> localConfigShard,
                                       const std::string& shardName);

/**
 * Sends a command to all shards in the cluster to join ongoing DDL operations and retries until
 * this command completes on all shards within a specified timeout. After this completes, it is
 * assumed safe to block DDLs. After this, it sets a cluster parameter to prevent any new DDL
 * operations from beginning. Additionally persists a recovery document which tells that DDLs are
 * currently blocked.
 */
void blockDDLCoordinatorsAndDrain(OperationContext* opCtx, bool persistRecoveryDocument = true);

/**
 * Unsets the cluster parameter which prevents DDL operations from running. Additionally, cleans up
 * the recovery document.
 */
void unblockDDLCoordinators(OperationContext* opCtx, bool removeRecoveryDocument = true);

/**
 * Checks every collection in every database tracked on the config server to ensure that the local
 * copies of the collections are empty. If any collection is non-empty, returns RemoveShardProgress
 * reflecting that. If all collections are empty, drops all of the local, tracked databases and
 * returns boost::none.
 *
 * The sessions collection is an exception - this function will check that the collection is empty
 * and return RemoveShardProgress if not but it will not drop this collection and will rely on the
 * caller to drop this collection later on.
 */
boost::optional<RemoveShardProgress> dropLocalCollectionsAndDatabases(
    OperationContext* opCtx,
    const std::vector<DatabaseType>& trackedDBs,
    const std::string& shardName);

/**
 * This function commits a shard removal function and should only be called while holding the shard
 * membership lock in exclusive mode. It will find a control shard and generate a new topology time
 * and then commit the removal of the shard from config.shards and update of the topology time in a
 * transaction.
 */
void commitRemoveShard(const Lock::ExclusiveLock&,
                       OperationContext* opCtx,
                       std::shared_ptr<Shard> localConfigShard,
                       const std::string& shardName,
                       std::shared_ptr<executor::TaskExecutor> executor);

/**
 * Inserts new entries into the config catalog to describe the shard being added (and the
 * databases being imported) through the internal transaction API.
 */
void addShardInTransaction(OperationContext* opCtx,
                           const ShardType& newShard,
                           std::vector<DatabaseName>&& databasesInNewShard,
                           std::shared_ptr<executor::TaskExecutor> executor);

/**
 * Updates the "hasTwoOrMoreShard" cluster cardinality parameter. Can only be called while holding
 * the _kClusterCardinalityParameterLock in exclusive mode and not holding the _kShardMembershipLock
 * in exclusive mode since setting cluster parameters requires taking the latter in shared mode.
 */
void updateClusterCardinalityParameter(const Lock::ExclusiveLock& clusterCardinalityParameterLock,
                                       OperationContext* opCtx);

/**
 * Handles the hangAddShardBeforeUpdatingClusterCardinalityParameter failpoint that is used from
 * multiple places
 */
void hangAddShardBeforeUpdatingClusterCardinalityParameterFailpoint(OperationContext* opCtx);

/**
 * Checks if the shard exists and returns it if so.
 */
boost::optional<ShardType> getShardIfExists(OperationContext* opCtx,
                                            std::shared_ptr<Shard> localConfigShard,
                                            const ShardId& shardId);

/**
 * Collects user write blocking state locally (cluster parameter) and propagates it to the given
 * replica set
 */
void propagateClusterUserWriteBlockToReplicaSet(OperationContext* opCtx,
                                                RemoteCommandTargeter& targeter,
                                                std::shared_ptr<executor::TaskExecutor> executor);

/**
 * Creates a fetcher responsible for finding documents in a given namespace
 */
using FetcherDocsCallbackFn = std::function<bool(const std::vector<BSONObj>& batch)>;
using FetcherStatusCallbackFn = std::function<void(const Status& status)>;

std::unique_ptr<Fetcher> createFindFetcher(OperationContext* opCtx,
                                           RemoteCommandTargeter& targeter,
                                           const NamespaceString& nss,
                                           const BSONObj& filter,
                                           const repl::ReadConcernLevel& readConcernLevel,
                                           FetcherDocsCallbackFn processDocsCallback,
                                           FetcherStatusCallbackFn processStatusCallback,
                                           std::shared_ptr<executor::TaskExecutor> executor);

// If an add/removeShard recovery document is present on kServerConfigurationNamespace, unset the
// addOrRemoveShardInProgress cluster parameter. Must be called under the kConfigsvrShardsNamespace
// ddl lock.
void resetDDLBlockingForTopologyChangeIfNeeded(OperationContext* opCtx);

}  // namespace topology_change_helpers
}  // namespace mongo
