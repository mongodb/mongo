/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/cluster_parameters/cluster_server_parameter_common.h"

#include "mongo/base/string_data.h"
#include "mongo/db/client.h"
#include "mongo/db/cluster_parameters/cluster_server_parameter_test_util.h"
#include "mongo/db/local_catalog/create_collection.h"
#include "mongo/db/local_catalog/ddl/create_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/server_options.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/shard_local.h"
#include "mongo/db/sharding_environment/shard_retry_server_parameters_gen.h"
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
        // Since Grid::init is not called in this test, we need to init the server parameter
        // pointers manually.
        Shard::initServerParameterPointersToAvoidLinking(&gShardRetryTokenReturnRate,
                                                         &gShardRetryTokenBucketCapacity);
        _shardLocal = std::make_unique<ShardLocal>(ShardId::kConfigServerId);
    }

    void tearDown() override {
        _shardLocal = nullptr;
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
