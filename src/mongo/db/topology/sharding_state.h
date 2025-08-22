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
#include "mongo/bson/oid.h"
#include "mongo/db/global_catalog/ddl/sharding_migration_critical_section.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/logv2/log_severity_suppressor.h"
#include "mongo/stdx/mutex.h"

#include <string>

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * There is one instance of this object per service context on each shard node (primary or
 * secondary). It sits at the top of the hierarchy of the Shard Role runtime-authoritative caches
 * (the subordinate ones being the DatabaseShardingState and CollectionShardingState) and contains
 * global information about the shardedness of the current process, such as its shardId and the
 * clusterId to which it belongs.
 *
 * SYNCHRONISATION: This class can only be initialised once and if 'setInitialized' is called, it
 * never gets destroyed or uninitialized. Because of this it does not require external
 * synchronisation. Initialisation is driven from outside (specifically
 * ShardingInitializationMongoD, which should be its only caller).
 */
class ShardingState {
    ShardingState(const ShardingState&) = delete;
    ShardingState& operator=(const ShardingState&) = delete;

public:
    ShardingState(bool inMaintenanceMode);
    ~ShardingState();

    static void create(ServiceContext* serviceContext);

    static ShardingState* get(ServiceContext* serviceContext);
    static ShardingState* get(OperationContext* operationContext);

    /**
     * Returns whether this node is in sharding maintenance mode, which means the cluste role will
     * never be recovered. Sharding maintenance mode means that the node will serve as replica set
     * or sharded clusters but none of the sharding infrastructure will be enabled or consulted.
     */
    bool inMaintenanceMode() const;

    struct RecoveredClusterRole {
        OID clusterId;
        ClusterRole role;
        ConnectionString configShardConnectionString;

        // Will be empty if running as a MongoS exclusively, otherwise must be set
        ShardId shardId;

        std::string toString() const;
    };

    /**
     * Non-blocking method which can be used to wait for the cluster role recovery to complete,
     * ensuring that any sharding-state dependent services can now proceed.
     *
     * When the future returned by this method is signaled (whether with success or an error) it is
     * guaranteed that the node's role in the cluster will no longer change.
     */
    SharedSemiFuture<RecoveredClusterRole> awaitClusterRoleRecovery();

    /**
     * This is the polling variant of `awaitClusterRoleRecovery` above. Until the cluster role
     * recovery process has not yet completed, it will keep returning ClusterRole::None. If the
     * recovery completes with an success it will return the role of the node, which is guaranteed
     * to not change. If the recovery completes with an error, it will throw and never return
     * success.
     */
    boost::optional<ClusterRole> pollClusterRole() const;

    /**
     * Based on pollClusterRole above, returns true if recovery has completed successfully and the
     * node is running under the ShardServer role.
     *
     * TODO (SERVER-89417): Usages of this method should go away because we shouldn't have code that
     * makes decisions based on the role of the node. Instead, code should just use acquisitions
     * which under the hood will ensure that the correct behaviour is achieved based on the current
     * role of the node and any other code should rely on awaitClusterRoleRecovery in order to
     * switch its execution flow.
     */
    bool enabled() const;

    /**
     * Asserts that this node has finished recovering its cluster state and can accept shard role
     * commands.
     */
    void assertCanAcceptShardedCommands() const;

    /**
     * Returns the shard id to which this node belongs.
     */
    ShardId shardId() const;

    /**
     * Returns the cluster id of the cluster to which this node belongs.
     */
    OID clusterId() const;

    /**
     * Puts the sharding state singleton in the "initialization completed" state with either
     * successful initialization or an error. This method may only be called once for the lifetime
     * of the object.
     */
    void setRecoveryCompleted(RecoveredClusterRole role);
    void setRecoveryFailed(Status failedStatus);

    /**
     * Returns the severity the direct shard operation warnings should be logged at. This is
     * determined by the amount of time that has passed since the last warning was logged.
     */
    logv2::LogSeverity directConnectionLogSeverity() {
        return _directConnectionLogSuppressor();
    }

private:
    const bool _inMaintenanceMode;

    mutable stdx::mutex _mutex;

    // Promise/future pair which will be set when the recovery of the shard role completes
    SharedPromise<RecoveredClusterRole> _awaitClusterRoleRecoveryPromise;
    Promise<RecoveredClusterRole> _promise;
    Future<RecoveredClusterRole> _future;

    // Log severity suppressor for direct connection checks
    logv2::SeveritySuppressor _directConnectionLogSuppressor{
        Hours{1}, logv2::LogSeverity::Warning(), logv2::LogSeverity::Debug(2)};
};

}  // namespace mongo
