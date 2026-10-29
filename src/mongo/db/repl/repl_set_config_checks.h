// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/version_context.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {

class ServiceContext;

namespace repl {

class ReplicationCoordinatorExternalState;
class ReplSetConfig;

/**
 * Checks if the member given by the config index is electable in the new config.
 */
Status checkElectable(const ReplSetConfig& newConfig, int configIndex);

/**
 * Checks if two configs are the same in content, ignoring 'version' and 'term' fields.
 */
bool sameConfigContents(const ReplSetConfig& oldConfig, const ReplSetConfig& newConfig);

/**
 * Finds the index of the one member configuration in "newConfig" that corresponds
 * to the current node (as identified by "externalState").
 *
 * Returns an error if the current node does not appear or appears multiple times in
 * "newConfig".
 */
StatusWith<int> findSelfInConfig(ReplicationCoordinatorExternalState* externalState,
                                 const ReplSetConfig& newConfig,
                                 ServiceContext* ctx);

/**
 * Like findSelfInConfig, above, but also returns an error if the member configuration
 * for this node is not electable, as this is a requirement for nodes accepting
 * reconfig or initiate commands.
 */
StatusWith<int> findSelfInConfigIfElectable(ReplicationCoordinatorExternalState* externalState,
                                            const ReplSetConfig& newConfig,
                                            ServiceContext* ctx);

/**
 * Does a quick pass to see whether a host exists in the new config. Not as precise as
 * findSelfInConfig.
 */
int findOwnHostInConfigQuick(const ReplSetConfig& newConfig,
                             HostAndPort host,
                             boost::optional<int> priorityPort);

/**
 * Validates that "newConfig" is a legal configuration that the current
 * node can accept from its local storage during startup.
 *
 * Returns the index of the current node's member configuration in "newConfig",
 * on success, and an indicative error on failure.
 */
StatusWith<int> validateConfigForStartUp(ReplicationCoordinatorExternalState* externalState,
                                         const ReplSetConfig& newConfig,
                                         ServiceContext* ctx);

/**
 * Validates that "newConfig" is a legal initial configuration that can be
 * initiated by the current node (identified via "externalState").
 *
 * Returns the index of the current node's member configuration in "newConfig",
 * on success, and an indicative error on failure.
 */
StatusWith<int> validateConfigForInitiate(ReplicationCoordinatorExternalState* externalState,
                                          const ReplSetConfig& newConfig,
                                          OperationContext* opCtx);

/**
 * Validates that "newConfig" is a legal successor configuration to "oldConfig" that can be
 * initiated by the current node (identified via "externalState").
 *
 * If "force" is set to true, then the single node change requirement is not checked.
 *
 * If "allowSplitHorizonIP" is set to true, skips checking whether an IP address exists in
 * split horizon configuration.
 *
 * Returns an indicative error on validation failure.
 */
Status validateConfigForReconfig(const VersionContext& vCtx,
                                 const ReplSetConfig& oldConfig,
                                 const ReplSetConfig& newConfig,
                                 bool force,
                                 bool allowSplitHorizonIP);

/**
 * Validates that "newConfig" is an acceptable configuration when received in a heartbeat response.
 *
 * If the new configuration omits the current node, but is otherwise valid, returns
 * ErrorCodes::NodeNotFound. If the configuration is wholly valid, returns Status::OK(). Otherwise,
 * returns some other error status.
 */
StatusWith<int> validateConfigForHeartbeatReconfig(
    ReplicationCoordinatorExternalState* externalState,
    const ReplSetConfig& newConfig,
    HostAndPort ownHost,
    boost::optional<int> ownPriorityPort,
    ServiceContext* ctx);
}  // namespace repl
}  // namespace mongo
