// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/topology/cluster_parameters/cluster_server_parameter_common.h"

#include "mongo/db/client.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/server_options.h"
#include "mongo/db/shard_role/ddl/create_gen.h"
#include "mongo/db/shard_role/shard_catalog/create_collection.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/shard_local.h"
#include "mongo/db/sharding_environment/shard_retry_server_parameters_gen.h"
#include "mongo/db/sharding_environment/shard_shared_state_cache.h"
#include "mongo/db/topology/cluster_parameters/cluster_server_parameter_test_util.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <memory>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>

namespace mongo {
namespace {
using namespace cluster_server_parameter_test_util;

class ClusterServerParameterCommonTest : public ClusterServerParameterTestBase {
public:
    void setUp() override {
        ClusterServerParameterTestBase::setUp();
        serverGlobalParams.clusterRole = {ClusterRole::ShardServer, ClusterRole::ConfigServer};
        auto& shardSharedStateCache = ShardSharedStateCache::get(getServiceContext());
        _shardLocal = std::make_unique<ShardLocal>(
            ShardHandle(ShardId::kConfigServerId, UUID::gen()),
            shardSharedStateCache.getShardState(ShardId::kConfigServerId));
    }

    void tearDown() override {
        _shardLocal = nullptr;
        auto& shardSharedStateCache = ShardSharedStateCache::get(getServiceContext());
        shardSharedStateCache.forgetShardState(ShardId::kConfigServerId);
        serverGlobalParams.clusterRole = ClusterRole::None;
        ClusterServerParameterTestBase::tearDown();
    }

protected:
    std::unique_ptr<ShardLocal> _shardLocal;
};

TEST_F(ClusterServerParameterCommonTest, GetTenants) {
    // Test that getTenantsWithConfigDbsOnShard accurately returns the tenants with config DBs.
    auto opCtx = cc().makeOperationContext();

    // First, we only create config.clusterParameters, so we expect only the "none" tenant.
    ASSERT_OK(
        createCollection(opCtx.get(), CreateCommand(NamespaceString::kClusterParametersNamespace)));

    auto tenantIds =
        uassertStatusOK(getTenantsWithConfigDbsOnShard(opCtx.get(), *_shardLocal.get()));
    ASSERT_EQ(tenantIds.size(), 1);
    ASSERT_EQ(*tenantIds.begin(), boost::none);

    // After creating kTenantId_config.clusterParameters, we expect kTenantId as well.
    ASSERT_OK(createCollection(
        opCtx.get(), CreateCommand(NamespaceString::makeClusterParametersNSS(kTenantId))));

    tenantIds = uassertStatusOK(getTenantsWithConfigDbsOnShard(opCtx.get(), *_shardLocal.get()));
    ASSERT_EQ(tenantIds.size(), 2);
    auto it = tenantIds.begin();
    ASSERT_EQ(*it, boost::none);
    ASSERT_EQ(*(++it), kTenantId);
}

}  // namespace
}  // namespace mongo
