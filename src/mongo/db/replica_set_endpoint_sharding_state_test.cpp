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

#include "mongo/db/replica_set_endpoint_sharding_state.h"

#include "mongo/db/auth/authz_manager_external_state_mock.h"
#include "mongo/db/replica_set_endpoint_test_fixture.h"
#include "mongo/db/s/sharding_cluster_parameters_gen.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/death_test.h"

namespace mongo {
namespace replica_set_endpoint {
namespace {

class ReplicaSetEndpointShardingStateTest : public ServiceContextMongoDTest,
                                            public ReplicaSetEndpointTest {
protected:
    explicit ReplicaSetEndpointShardingStateTest(Options options = {})
        : ServiceContextMongoDTest(options.useReplSettings(true)) {}

private:
    RAIIServerParameterControllerForTest _replicaSetEndpointController{
        "featureFlagReplicaSetEndpoint", true};

    RAIIServerParameterControllerForTest _multitenanyController{"multitenancySupport", false};
};

TEST_F(ReplicaSetEndpointShardingStateTest, SetUnSetGetIsConfigShard) {
    serverGlobalParams.clusterRole = {ClusterRole::ShardServer, ClusterRole::ConfigServer};

    auto shardingState = ReplicaSetEndpointShardingState::get(getServiceContext());
    ASSERT_FALSE(shardingState->isConfigShardForTest());

    shardingState->setIsConfigShard(true);
    ASSERT(shardingState->isConfigShardForTest());

    shardingState->setIsConfigShard(false);
    ASSERT_FALSE(shardingState->isConfigShardForTest());
}

TEST_F(ReplicaSetEndpointShardingStateTest, SetIsConfigShardMultipleTimes) {
    serverGlobalParams.clusterRole = {ClusterRole::ShardServer, ClusterRole::ConfigServer};

    auto shardingState = ReplicaSetEndpointShardingState::get(getServiceContext());
    ASSERT_FALSE(shardingState->isConfigShardForTest());

    shardingState->setIsConfigShard(true);
    ASSERT(shardingState->isConfigShardForTest());

    shardingState->setIsConfigShard(true);
    ASSERT(shardingState->isConfigShardForTest());
}

TEST_F(ReplicaSetEndpointShardingStateTest, UnSetIsConfigShardMultipleTimes) {
    serverGlobalParams.clusterRole = {ClusterRole::ShardServer, ClusterRole::ConfigServer};

    auto shardingState = ReplicaSetEndpointShardingState::get(getServiceContext());
    ASSERT_FALSE(shardingState->isConfigShardForTest());

    shardingState->setIsConfigShard(true);
    ASSERT(shardingState->isConfigShardForTest());

    shardingState->setIsConfigShard(false);
    ASSERT_FALSE(shardingState->isConfigShardForTest());

    shardingState->setIsConfigShard(false);
    ASSERT_FALSE(shardingState->isConfigShardForTest());
}

DEATH_TEST_F(ReplicaSetEndpointShardingStateTest, SetIsConfigShard_NotConfigServer, "invariant") {
    serverGlobalParams.clusterRole = ClusterRole::ShardServer;

    auto shardingState = ReplicaSetEndpointShardingState::get(getServiceContext());
    shardingState->setIsConfigShard(true);
}

DEATH_TEST_F(ReplicaSetEndpointShardingStateTest, UnSetIsConfigShard_NotConfigServer, "invariant") {
    serverGlobalParams.clusterRole = ClusterRole::ShardServer;

    auto shardingState = ReplicaSetEndpointShardingState::get(getServiceContext());
    shardingState->setIsConfigShard(false);
}

TEST_F(ReplicaSetEndpointShardingStateTest, GetIsConfigShard_NotConfigServer) {
    serverGlobalParams.clusterRole = ClusterRole::ShardServer;

    auto shardingState = ReplicaSetEndpointShardingState::get(getServiceContext());
    ASSERT_FALSE(shardingState->isConfigShardForTest());
}

TEST_F(ReplicaSetEndpointShardingStateTest, SupportsReplicaSetEndpoint) {
    serverGlobalParams.clusterRole = {
        ClusterRole::ShardServer, ClusterRole::ConfigServer, ClusterRole::RouterServer};
    setHasTwoOrShardsClusterParameter(false);
    ASSERT_FALSE(getHasTwoOrShardsClusterParameter());

    auto shardingState = ReplicaSetEndpointShardingState::get(getServiceContext());
    shardingState->setIsConfigShard(true);

    ASSERT(shardingState->isConfigShardForTest());
    ASSERT(shardingState->supportsReplicaSetEndpoint());
}

TEST_F(ReplicaSetEndpointShardingStateTest, SupportsReplicaSetEndpoint_NotRouterServer) {
    serverGlobalParams.clusterRole = {ClusterRole::ShardServer, ClusterRole::ConfigServer};
    setHasTwoOrShardsClusterParameter(false);
    ASSERT_FALSE(getHasTwoOrShardsClusterParameter());

    auto shardingState = ReplicaSetEndpointShardingState::get(getServiceContext());
    shardingState->setIsConfigShard(true);

    ASSERT(shardingState->isConfigShardForTest());
    ASSERT_FALSE(shardingState->supportsReplicaSetEndpoint());
}

TEST_F(ReplicaSetEndpointShardingStateTest, SupportsReplicaSetEndpoint_NotConfigShard) {
    serverGlobalParams.clusterRole = {
        ClusterRole::ShardServer, ClusterRole::ConfigServer, ClusterRole::RouterServer};
    setHasTwoOrShardsClusterParameter(false);
    ASSERT_FALSE(getHasTwoOrShardsClusterParameter());

    auto shardingState = ReplicaSetEndpointShardingState::get(getServiceContext());

    ASSERT_FALSE(shardingState->isConfigShardForTest());
    ASSERT_FALSE(shardingState->supportsReplicaSetEndpoint());
}

TEST_F(ReplicaSetEndpointShardingStateTest, SupportsReplicaSetEndpoint_HasTwoOrMoreShards) {
    serverGlobalParams.clusterRole = {
        ClusterRole::ShardServer, ClusterRole::ConfigServer, ClusterRole::RouterServer};
    setHasTwoOrShardsClusterParameter(true);
    ASSERT(getHasTwoOrShardsClusterParameter());

    auto shardingState = ReplicaSetEndpointShardingState::get(getServiceContext());
    shardingState->setIsConfigShard(true);

    ASSERT_TRUE(shardingState->isConfigShardForTest());
    ASSERT_FALSE(shardingState->supportsReplicaSetEndpoint());
}

TEST_F(ReplicaSetEndpointShardingStateTest, SupportsReplicaSetEndpoint_FeatureFlagDisabled) {
    serverGlobalParams.clusterRole = {
        ClusterRole::ShardServer, ClusterRole::ConfigServer, ClusterRole::RouterServer};
    setHasTwoOrShardsClusterParameter(false);
    ASSERT_FALSE(getHasTwoOrShardsClusterParameter());
    RAIIServerParameterControllerForTest replicaSetEndpointController{
        "featureFlagReplicaSetEndpoint", false};

    auto shardingState = ReplicaSetEndpointShardingState::get(getServiceContext());
    shardingState->setIsConfigShard(true);

    ASSERT_TRUE(shardingState->isConfigShardForTest());
    ASSERT_FALSE(shardingState->supportsReplicaSetEndpoint());
}

TEST_F(ReplicaSetEndpointShardingStateTest, SupportsReplicaSetEndpoint_Multitenant) {
    serverGlobalParams.clusterRole = {
        ClusterRole::ShardServer, ClusterRole::ConfigServer, ClusterRole::RouterServer};
    setHasTwoOrShardsClusterParameter(false);
    ASSERT_FALSE(getHasTwoOrShardsClusterParameter());
    RAIIServerParameterControllerForTest replicaSetEndpointController{"multitenancySupport", true};

    auto shardingState = ReplicaSetEndpointShardingState::get(getServiceContext());
    shardingState->setIsConfigShard(true);

    ASSERT_TRUE(shardingState->isConfigShardForTest());
    ASSERT_FALSE(shardingState->supportsReplicaSetEndpoint());
}

}  // namespace
}  // namespace replica_set_endpoint
}  // namespace mongo
