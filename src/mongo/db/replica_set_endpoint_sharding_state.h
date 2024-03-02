/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <shared_mutex>

#include "mongo/db/operation_context.h"

namespace mongo {
namespace replica_set_endpoint {

class ReplicaSetEndpointShardingState {
    ReplicaSetEndpointShardingState(const ReplicaSetEndpointShardingState&) = delete;
    ReplicaSetEndpointShardingState& operator=(const ReplicaSetEndpointShardingState&) = delete;

public:
    ReplicaSetEndpointShardingState() = default;
    ~ReplicaSetEndpointShardingState() = default;

    static ReplicaSetEndpointShardingState* get(ServiceContext* serviceContext);
    static ReplicaSetEndpointShardingState* get(OperationContext* opCtx);

    /**
     * Sets '_isConfigShard' to true or false. Can only be invoked on a mongod with the configsvr
     * role.
     */
    void setIsConfigShard(bool value);

    /**
     * Returns true if this mongod belongs to a config shard.
     */
    bool isConfigShardForTest();

    /**
     * Returns true if this mongod supports replica set endpoint, meaning it is part of
     * a single-shard cluster consisting of config shard with router role.
     */
    bool supportsReplicaSetEndpoint();

private:
    mutable std::shared_mutex _mutex;  // NOLINT

    // Set to true if this mongod belongs to a config shard.
    bool _isConfigShard;
};

/**
 * Returns true if the feature flag is enabled, not ignoring the feature compatibility version.
 */
bool isFeatureFlagEnabled();

/**
 * Returns true if the feature flag is enabled, ignoring the feature compatibility version.
 * To be used only by the machinery for maintaining the ReplicaSetEndpointShardingState.
 */
bool isFeatureFlagEnabledIgnoreFCV();

}  // namespace replica_set_endpoint
}  // namespace mongo
