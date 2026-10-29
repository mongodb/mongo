// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/service_context_d_test_fixture.h"

#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class DeferredSetupTest : public ServiceContextMongoDTest {};

TEST_F(DeferredSetupTest, SetUpBuildsTheServiceContext) {
    ASSERT(getServiceContext());
    ASSERT(getService());
    ASSERT(opObserverRegistry());
}

TEST_F(DeferredSetupTest, UsesDefaultOptionsWhenMakeOptionsIsNotOverridden) {
    ASSERT_FALSE(storageGlobalParams.forceDisableTableLogging);
}

TEST_F(DeferredSetupTest, DoesNotSetUpATransportLayerByDefault) {
    ASSERT_FALSE(getServiceContext()->getTransportLayerManager());
}

class MakeOptionsOverrideTest : public DeferredSetupTest {
public:
    Options makeServiceContextOptions() override {
        return DeferredSetupTest::makeServiceContextOptions().forceDisableTableLogging();
    }
};

TEST_F(MakeOptionsOverrideTest, OverriddenOptionsReachTheServiceContext) {
    ASSERT_TRUE(storageGlobalParams.forceDisableTableLogging);
}

class TransportLayerOverrideTest : public DeferredSetupTest {
public:
    bool shouldSetupTransportLayer() const override {
        return true;
    }
};

TEST_F(TransportLayerOverrideTest, OverrideCausesATransportLayerToBeInstalled) {
    ASSERT(getServiceContext()->getTransportLayerManager());
}

class ClusterRoleOverrideTest : public DeferredSetupTest {
public:
    boost::optional<ClusterRole> getClusterRoleOverride() const override {
        return ClusterRole{ClusterRole::ShardServer};
    }
};

TEST_F(ClusterRoleOverrideTest, OverrideIsInEffectForTheDurationOfTheTest) {
    ASSERT_TRUE(serverGlobalParams.clusterRole.has(ClusterRole::ShardServer));
}

class ConcreteClusterRoleOverrideTest : public ClusterRoleOverrideTest {
public:
    void TestBody() override {}
};

TEST(ServiceContextTestClusterRoleOverrideTest, RestoresThePriorClusterRoleAfterDestruction) {
    ClusterRole original = serverGlobalParams.clusterRole;
    {
        ConcreteClusterRoleOverrideTest fixture;
        fixture.setUp();
        ASSERT_TRUE(serverGlobalParams.clusterRole.has(ClusterRole::ShardServer));
    }
    ASSERT_TRUE(serverGlobalParams.clusterRole.hasExclusively(original));
}

}  // namespace
}  // namespace mongo
