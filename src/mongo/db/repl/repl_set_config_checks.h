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

#include "mongo/base/status_with.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

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
int findOwnHostInConfigQuick(const ReplSetConfig& newConfig, HostAndPort host);

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
                                          ServiceContext* ctx);

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
Status validateConfigForReconfig(const ReplSetConfig& oldConfig,
                                 const ReplSetConfig& newConfig,
                                 bool force,
                                 bool allowSplitHorizonIP);

/**
 * Validates that "newConfig" is an acceptable configuration when
 * received in a heartbeat reasponse.
 *
 * If the new configuration omits the current node, but is
 * otherwise valid, returns ErrorCodes::NodeNotFound.  If the
 * configuration is wholly valid, returns Status::OK(). Otherwise,
 * returns some other error status.
 */
StatusWith<int> validateConfigForHeartbeatReconfig(
    ReplicationCoordinatorExternalState* externalState,
    const ReplSetConfig& newConfig,
    HostAndPort ownHost,
    ServiceContext* ctx);
}  // namespace repl
}  // namespace mongo
