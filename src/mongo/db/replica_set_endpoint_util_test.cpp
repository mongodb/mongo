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

#include <memory>

#include "mongo/db/replica_set_endpoint_util.h"

#include "mongo/bson/json.h"
#include "mongo/db/auth/authz_manager_external_state_mock.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands_test_example.h"
#include "mongo/db/commands_test_example_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/replica_set_endpoint_sharding_state.h"
#include "mongo/db/replica_set_endpoint_test_fixture.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/tenant_id.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/grid.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace replica_set_endpoint {
namespace {

class ReplicaSetEndpointUtilTest : public ServiceContextMongoDTest, public ReplicaSetEndpointTest {
protected:
    explicit ReplicaSetEndpointUtilTest(Options options = {})
        : ServiceContextMongoDTest(options.useReplSettings(true)) {}

    void setUp() {
        ServiceContextMongoDTest::setUp();

        // The requirements for a mongod to be a replica set endpoint.
        Grid::get(getServiceContext())->setShardingInitialized();
        serverGlobalParams.clusterRole = {
            ClusterRole::ShardServer, ClusterRole::ConfigServer, ClusterRole::RouterServer};
        ReplicaSetEndpointShardingState::get(getServiceContext())->setIsConfigShard(true);
        setHasTwoOrShardsClusterParameter(false);
        ASSERT_FALSE(getHasTwoOrShardsClusterParameter());

        auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(getServiceContext());
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_SECONDARY));
        repl::ReplicationCoordinator::set(getServiceContext(), std::move(replCoord));
    }

    transport::TransportLayerMock& getTransportLayer() {
        return _transportLayer;
    }

    repl::ReplicationCoordinator* getReplicationCoordinator() {
        return repl::ReplicationCoordinator::get(getServiceContext());
    }

    const std::string kTestDbName = "testDb";
    const std::string kTestCollName = "testColl";
    const TenantId kTestTenantId{OID::gen()};

private:
    RAIIServerParameterControllerForTest _replicaSetEndpointController{
        "featureFlagReplicaSetEndpoint", true};
    RAIIServerParameterControllerForTest _multitenanyController{"multitenancySupport", false};

    transport::TransportLayerMock _transportLayer;
};

TEST_F(ReplicaSetEndpointUtilTest, IsReplicaSetEndpointClient) {
    std::shared_ptr<transport::Session> session = getTransportLayer().createSession();
    auto client = getServiceContext()->getService()->makeClient("NotIsConfigShard", session);
    ASSERT(isReplicaSetEndpointClient(client.get()));
}

TEST_F(ReplicaSetEndpointUtilTest, IsReplicaSetEndpointClient_RouterPort) {
    std::shared_ptr<transport::Session> session =
        getTransportLayer().createSession(true /* isFromRouterPort */);
    auto client = getServiceContext()->getService()->makeClient("RouterPort", session);
    ASSERT_FALSE(isReplicaSetEndpointClient(client.get()));
}

TEST_F(ReplicaSetEndpointUtilTest, IsReplicaSetEndpointClient_NotRouterServer) {
    serverGlobalParams.clusterRole = {ClusterRole::ShardServer, ClusterRole::ConfigServer};

    std::shared_ptr<transport::Session> session = getTransportLayer().createSession();
    auto client = getServiceContext()->getService()->makeClient("NotRouterServer", session);
    ASSERT_FALSE(isReplicaSetEndpointClient(client.get()));
}

TEST_F(ReplicaSetEndpointUtilTest, IsReplicaSetEndpointClient_NotIsConfigShard) {
    ReplicaSetEndpointShardingState::get(getServiceContext())->setIsConfigShard(false);

    std::shared_ptr<transport::Session> session = getTransportLayer().createSession();
    auto client = getServiceContext()->getService()->makeClient("NotIsConfigShard", session);
    ASSERT_FALSE(isReplicaSetEndpointClient(client.get()));
}

TEST_F(ReplicaSetEndpointUtilTest, IsReplicaSetEndpointClient_HasTwoOrMoreShards) {
    setHasTwoOrShardsClusterParameter(true);
    ASSERT(getHasTwoOrShardsClusterParameter());

    std::shared_ptr<transport::Session> session = getTransportLayer().createSession();
    auto client = getServiceContext()->getService()->makeClient("HasTwoOrMoreShards", session);
    ASSERT_FALSE(isReplicaSetEndpointClient(client.get()));
}

TEST_F(ReplicaSetEndpointUtilTest, IsReplicaSetEndpointClient_FeatureFlagDisabled) {
    RAIIServerParameterControllerForTest replicaSetEndpointController{
        "featureFlagReplicaSetEndpoint", false};

    std::shared_ptr<transport::Session> session = getTransportLayer().createSession();
    auto client = getServiceContext()->getService()->makeClient("FeatureFlagDisabled", session);
    ASSERT_FALSE(isReplicaSetEndpointClient(client.get()));
}

// Add test commands to the command registries.
MONGO_REGISTER_COMMAND(commands_test_example::ExampleIncrementCommand).forShard().forRouter();
MONGO_REGISTER_COMMAND(commands_test_example::ExampleMinimalCommand).forShard();
MONGO_REGISTER_COMMAND(commands_test_example::ExampleVoidCommand).forRouter();
MONGO_REGISTER_COMMAND(commands_test_example::ExampleVoidCommandNeverAllowedOnSecondary)
    .forRouter()
    .forShard();
MONGO_REGISTER_COMMAND(commands_test_example::ExampleVoidCommandAlwaysAllowedOnSecondary)
    .forRouter()
    .forShard();
MONGO_REGISTER_COMMAND(commands_test_example::ExampleVoidCommandAllowedOnSecondaryIfOptedIn)
    .forRouter()
    .forShard();

TEST_F(ReplicaSetEndpointUtilTest, ShouldRoute_RouterAndShardCommand) {
    std::shared_ptr<transport::Session> session = getTransportLayer().createSession();
    auto client = getServiceContext()->getService()->makeClient("RouterAndShardCommand", session);
    auto opCtx = client->makeOperationContext();

    auto ns = NamespaceString::createNamespaceString_forTest(kTestDbName, kTestCollName);
    commands_test_example::ExampleIncrement incrementCmd(ns, 0);
    auto opMsgRequest = mongo::OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::get(opCtx.get()), ns.dbName(), incrementCmd.toBSON({}));

    ASSERT(shouldRouteRequest(opCtx.get(), opMsgRequest));
}

TEST_F(ReplicaSetEndpointUtilTest, ShouldNotRoute_ShardOnlyCommand) {
    std::shared_ptr<transport::Session> session = getTransportLayer().createSession();
    auto client = getServiceContext()->getService()->makeClient("ShardOnlyCommand", session);
    auto opCtx = client->makeOperationContext();

    auto ns = NamespaceString::createNamespaceString_forTest(kTestDbName, kTestCollName);
    commands_test_example::ExampleMinimal minimalCmd(ns, 0);
    auto opMsgRequest = mongo::OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::get(opCtx.get()), ns.dbName(), minimalCmd.toBSON({}));

    ASSERT_FALSE(shouldRouteRequest(opCtx.get(), opMsgRequest));
}

TEST_F(ReplicaSetEndpointUtilTest, ShouldRoute_RouterOnlyCommand) {
    std::shared_ptr<transport::Session> session = getTransportLayer().createSession();
    auto client = getServiceContext()->getService()->makeClient("RouterOnlyCommand", session);
    auto opCtx = client->makeOperationContext();

    auto ns = NamespaceString::createNamespaceString_forTest(kTestDbName, kTestCollName);
    commands_test_example::ExampleVoid voidCmd(ns, 0);
    auto opMsgRequest = mongo::OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::get(opCtx.get()), ns.dbName(), voidCmd.toBSON({}));

    ASSERT(shouldRouteRequest(opCtx.get(), opMsgRequest));
}

TEST_F(ReplicaSetEndpointUtilTest, ShouldRoute_RouterAndShardCommand_NeverAllowedOnSecondary) {
    std::shared_ptr<transport::Session> session = getTransportLayer().createSession();
    auto client = getServiceContext()->getService()->makeClient(
        "RouterAndShardCommand_NeverAllowedOnSecondary", session);
    auto opCtx = client->makeOperationContext();

    auto ns = NamespaceString::createNamespaceString_forTest(kTestDbName, kTestCollName);
    commands_test_example::ExampleVoidNeverAllowedOnSecondary voidCmd(ns, 0);
    auto opMsgRequest = mongo::OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::kNotRequired, ns.dbName(), voidCmd.toBSON({}));

    ASSERT_OK(getReplicationCoordinator()->setFollowerMode(repl::MemberState::RS_PRIMARY));
    ASSERT(shouldRouteRequest(opCtx.get(), opMsgRequest));
    ASSERT_OK(getReplicationCoordinator()->setFollowerMode(repl::MemberState::RS_SECONDARY));
    ASSERT_THROWS_CODE(
        shouldRouteRequest(opCtx.get(), opMsgRequest), DBException, ErrorCodes::NotWritablePrimary);
}

TEST_F(ReplicaSetEndpointUtilTest, ShouldRoute_RouterAndShardCommand_AlwaysAllowedOnSecondary) {
    std::shared_ptr<transport::Session> session = getTransportLayer().createSession();
    auto client = getServiceContext()->getService()->makeClient(
        "RouterAndShardCommand_AlwaysAllowedOnSecondary", session);
    auto opCtx = client->makeOperationContext();

    auto ns = NamespaceString::createNamespaceString_forTest(kTestDbName, kTestCollName);
    commands_test_example::ExampleVoidAlwaysAllowedOnSecondary voidCmd(ns, 0);
    auto opMsgRequest = mongo::OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::kNotRequired, ns.dbName(), voidCmd.toBSON({}));

    ASSERT_OK(getReplicationCoordinator()->setFollowerMode(repl::MemberState::RS_PRIMARY));
    ASSERT(shouldRouteRequest(opCtx.get(), opMsgRequest));
    ASSERT_OK(getReplicationCoordinator()->setFollowerMode(repl::MemberState::RS_SECONDARY));
    ASSERT(shouldRouteRequest(opCtx.get(), opMsgRequest));
}

TEST_F(ReplicaSetEndpointUtilTest, ShouldRoute_RouterAndShardCommand_AllowedOnSecondaryIfOptedIn) {
    std::shared_ptr<transport::Session> session = getTransportLayer().createSession();
    auto client = getServiceContext()->getService()->makeClient(
        "RouterAndShardCommand_AllowedOnSecondaryIfOptedIn", session);
    auto opCtx = client->makeOperationContext();

    auto ns = NamespaceString::createNamespaceString_forTest(kTestDbName, kTestCollName);
    commands_test_example::ExampleVoidAllowedOnSecondaryIfOptedIn voidCmd(ns, 0);
    auto opMsgRequest = mongo::OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::kNotRequired, ns.dbName(), voidCmd.toBSON({}));

    ASSERT_OK(getReplicationCoordinator()->setFollowerMode(repl::MemberState::RS_PRIMARY));
    ASSERT(shouldRouteRequest(opCtx.get(), opMsgRequest));
    ASSERT_OK(getReplicationCoordinator()->setFollowerMode(repl::MemberState::RS_SECONDARY));
    ASSERT(shouldRouteRequest(opCtx.get(), opMsgRequest));
}

TEST_F(ReplicaSetEndpointUtilTest, ShouldNotRoute_LocalDatabase) {
    std::shared_ptr<transport::Session> session = getTransportLayer().createSession();
    auto client = getServiceContext()->getService()->makeClient("LocalDatabase", session);
    auto opCtx = client->makeOperationContext();

    auto ns = NamespaceString::makeLocalCollection(kTestCollName);
    commands_test_example::ExampleIncrement incrementCmd(ns, 0);
    auto opMsgRequest = mongo::OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::get(opCtx.get()), ns.dbName(), incrementCmd.toBSON({}));

    ASSERT_FALSE(shouldRouteRequest(opCtx.get(), opMsgRequest));
}

TEST_F(ReplicaSetEndpointUtilTest, ShouldRoute_ConfigDatabase) {
    std::shared_ptr<transport::Session> session = getTransportLayer().createSession();
    auto client = getServiceContext()->getService()->makeClient("ConfigDatabase", session);
    auto opCtx = client->makeOperationContext();

    auto ns = NamespaceString::createNamespaceString_forTest(DatabaseName::kConfig, kTestCollName);
    commands_test_example::ExampleIncrement incrementCmd(ns, 0);
    auto opMsgRequest = mongo::OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::get(opCtx.get()), ns.dbName(), incrementCmd.toBSON({}));

    ASSERT(shouldRouteRequest(opCtx.get(), opMsgRequest));
}

TEST_F(ReplicaSetEndpointUtilTest, ShouldRoute_AdminDatabase) {
    std::shared_ptr<transport::Session> session = getTransportLayer().createSession();
    auto client = getServiceContext()->getService()->makeClient("AdminDatabase", session);
    auto opCtx = client->makeOperationContext();

    auto ns = NamespaceString::createNamespaceString_forTest(DatabaseName::kAdmin, kTestCollName);
    commands_test_example::ExampleIncrement incrementCmd(ns, 0);
    auto opMsgRequest = mongo::OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::get(opCtx.get()), ns.dbName(), incrementCmd.toBSON({}));

    ASSERT(shouldRouteRequest(opCtx.get(), opMsgRequest));
}

TEST_F(ReplicaSetEndpointUtilTest, ShouldRoute_SystemDotProfileCollection) {
    std::shared_ptr<transport::Session> session = getTransportLayer().createSession();
    auto client =
        getServiceContext()->getService()->makeClient("SystemDotProfileCollection", session);
    auto opCtx = client->makeOperationContext();

    auto dbName =
        DatabaseName::createDatabaseName_forTest(boost::none /* tenantId */, {kTestDbName});
    auto ns = NamespaceString::makeSystemDotProfileNamespace(dbName);
    commands_test_example::ExampleIncrement incrementCmd(ns, 0);
    auto opMsgRequest = mongo::OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::get(opCtx.get()), ns.dbName(), incrementCmd.toBSON({}));

    ASSERT(shouldRouteRequest(opCtx.get(), opMsgRequest));
}

TEST_F(ReplicaSetEndpointUtilTest, ShouldRoute_ConfigSystemSessionsCollection) {
    std::shared_ptr<transport::Session> session = getTransportLayer().createSession();
    auto client =
        getServiceContext()->getService()->makeClient("ConfigSystemSessionsCollection", session);
    auto opCtx = client->makeOperationContext();

    auto ns = NamespaceString::kLogicalSessionsNamespace;
    commands_test_example::ExampleIncrement incrementCmd(ns, 0);
    auto opMsgRequest = mongo::OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::get(opCtx.get()), ns.dbName(), incrementCmd.toBSON({}));

    ASSERT(shouldRouteRequest(opCtx.get(), opMsgRequest));
}

TEST_F(ReplicaSetEndpointUtilTest, ShouldRoute_AdminSystemUsersCollection) {
    std::shared_ptr<transport::Session> session = getTransportLayer().createSession();
    auto client =
        getServiceContext()->getService()->makeClient("AdminSystemUsersCollection", session);
    auto opCtx = client->makeOperationContext();

    auto ns = NamespaceString::createNamespaceString_forTest(NamespaceString::kSystemUsers);
    commands_test_example::ExampleIncrement incrementCmd(ns, 0);
    auto opMsgRequest = mongo::OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::get(opCtx.get()), ns.dbName(), incrementCmd.toBSON({}));

    ASSERT(shouldRouteRequest(opCtx.get(), opMsgRequest));
}

TEST_F(ReplicaSetEndpointUtilTest, ShouldRoute_UserSystemCollection) {
    std::shared_ptr<transport::Session> session = getTransportLayer().createSession();
    auto client = getServiceContext()->getService()->makeClient("UserSystemCollection", session);
    auto opCtx = client->makeOperationContext();

    auto ns =
        NamespaceString::createNamespaceString_forTest(kTestDbName, "system." + kTestCollName);
    commands_test_example::ExampleIncrement incrementCmd(ns, 0);
    auto opMsgRequest = mongo::OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::get(opCtx.get()), ns.dbName(), incrementCmd.toBSON({}));

    ASSERT(shouldRouteRequest(opCtx.get(), opMsgRequest));
}

TEST_F(ReplicaSetEndpointUtilTest, ShouldRoute_UserNonSystemCollection) {
    std::shared_ptr<transport::Session> session = getTransportLayer().createSession();
    auto client = getServiceContext()->getService()->makeClient("UserNonSystemCollection", session);
    auto opCtx = client->makeOperationContext();

    auto ns0 =
        NamespaceString::createNamespaceString_forTest(kTestDbName, "system-" + kTestCollName);
    commands_test_example::ExampleIncrement incrementCmd0(ns0, 0);
    auto opMsgRequest0 = mongo::OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::get(opCtx.get()), ns0.dbName(), incrementCmd0.toBSON({}));

    ASSERT(shouldRouteRequest(opCtx.get(), opMsgRequest0));

    auto ns1 =
        NamespaceString::createNamespaceString_forTest(kTestDbName, kTestCollName + "system");
    commands_test_example::ExampleIncrement incrementCmd1(ns1, 0);
    auto opMsgRequest1 = mongo::OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::get(opCtx.get()), ns1.dbName(), incrementCmd1.toBSON({}));

    ASSERT(shouldRouteRequest(opCtx.get(), opMsgRequest1));

    auto ns2 =
        NamespaceString::createNamespaceString_forTest(kTestDbName, kTestCollName + ".system.foo");
    commands_test_example::ExampleIncrement incrementCmd2(ns2, 0);
    auto opMsgRequest2 = mongo::OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::get(opCtx.get()), ns2.dbName(), incrementCmd2.toBSON({}));

    ASSERT(shouldRouteRequest(opCtx.get(), opMsgRequest2));
}

TEST_F(ReplicaSetEndpointUtilTest, ShouldNotRoute_TargetedCommand) {
    std::shared_ptr<transport::Session> session = getTransportLayer().createSession();
    auto client = getServiceContext()->getService()->makeClient("TargetedCommand", session);
    auto opCtx = client->makeOperationContext();

    for (const auto& cmdName : kTargetedCmdNames) {
        auto opMsgRequest = mongo::OpMsgRequestBuilder::create(
            auth::ValidatedTenancyScope::get(opCtx.get()),
            DatabaseName::createDatabaseName_forTest(boost::none, kTestDbName),
            BSON(cmdName << 1));
        ASSERT_FALSE(shouldRouteRequest(opCtx.get(), opMsgRequest));
    }
}

TEST_F(ReplicaSetEndpointUtilTest, ShouldNotRoute_InternalClientWithNoSession) {
    auto client = getServiceContext()->getService()->makeClient("InternalClientNoSession");
    auto opCtx = client->makeOperationContext();

    auto ns = NamespaceString::createNamespaceString_forTest(kTestDbName, kTestCollName);
    commands_test_example::ExampleIncrement incrementCmd(ns, 0);
    auto opMsgRequest = mongo::OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::get(opCtx.get()), ns.dbName(), incrementCmd.toBSON({}));

    ASSERT_FALSE(shouldRouteRequest(opCtx.get(), opMsgRequest));
}

TEST_F(ReplicaSetEndpointUtilTest, ShouldNotRoute_InternalClientWithSession) {
    std::shared_ptr<transport::Session> session = getTransportLayer().createSession();
    auto client =
        getServiceContext()->getService()->makeClient("InternalClientWithSession", session);
    client->setIsInternalClient(true);
    auto opCtx = client->makeOperationContext();

    auto ns = NamespaceString::createNamespaceString_forTest(kTestDbName, kTestCollName);
    commands_test_example::ExampleIncrement incrementCmd(ns, 0);
    auto opMsgRequest = mongo::OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::get(opCtx.get()), ns.dbName(), incrementCmd.toBSON({}));

    ASSERT_FALSE(shouldRouteRequest(opCtx.get(), opMsgRequest));
}

TEST_F(ReplicaSetEndpointUtilTest, ShouldNotRoute_InternalClientDirect) {
    std::shared_ptr<transport::Session> session = getTransportLayer().createSession();
    auto client = getServiceContext()->getService()->makeClient("InternalClientDirect", session);
    client->setInDirectClient(true);
    auto opCtx = client->makeOperationContext();

    auto ns = NamespaceString::createNamespaceString_forTest(kTestDbName, kTestCollName);
    commands_test_example::ExampleIncrement incrementCmd(ns, 0);
    auto opMsgRequest = mongo::OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::get(opCtx.get()), ns.dbName(), incrementCmd.toBSON({}));

    ASSERT_FALSE(shouldRouteRequest(opCtx.get(), opMsgRequest));
}

TEST_F(ReplicaSetEndpointUtilTest, ShouldNotRoute_Multitenant) {
    RAIIServerParameterControllerForTest multitenanyController{"multitenancySupport", true};

    std::shared_ptr<transport::Session> session = getTransportLayer().createSession();
    auto client = getServiceContext()->getService()->makeClient("Multitenant", session);
    auto opCtx = client->makeOperationContext();

    auto ns =
        NamespaceString::createNamespaceString_forTest(kTestTenantId, kTestDbName, kTestCollName);
    commands_test_example::ExampleIncrement incrementCmd(ns, 0);
    auto opMsgRequest = mongo::OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::get(opCtx.get()),
        ns.dbName(),
        incrementCmd.toBSON(BSON("$tenantId" << *ns.tenantId())));

    ASSERT_FALSE(shouldRouteRequest(opCtx.get(), opMsgRequest));
}

TEST_F(ReplicaSetEndpointUtilTest, ShouldNotRoute_GridUninitialized) {
    Grid::get(getServiceContext())->clearForUnitTests();

    std::shared_ptr<transport::Session> session = getTransportLayer().createSession();
    auto client = getServiceContext()->getService()->makeClient("GridUninitialized", session);
    auto opCtx = client->makeOperationContext();

    auto ns = NamespaceString::createNamespaceString_forTest(kTestDbName, kTestCollName);
    commands_test_example::ExampleIncrement incrementCmd(ns, 0);
    auto opMsgRequest = mongo::OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::get(opCtx.get()), ns.dbName(), incrementCmd.toBSON({}));

    ASSERT_FALSE(shouldRouteRequest(opCtx.get(), opMsgRequest));
}

DEATH_TEST_F(ReplicaSetEndpointUtilTest, ShouldNotRoute_RouterPort, "invariant") {
    std::shared_ptr<transport::Session> session =
        getTransportLayer().createSession(true /* isFromRouterPort */);
    auto client = getServiceContext()->getService()->makeClient("RouterPort", session);
    auto opCtx = client->makeOperationContext();

    auto ns = NamespaceString::createNamespaceString_forTest(kTestDbName, kTestCollName);
    commands_test_example::ExampleIncrement incrementCmd(ns, 0);
    auto opMsgRequest = mongo::OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::get(opCtx.get()), ns.dbName(), incrementCmd.toBSON({}));

    // shouldRouteRequest() invariants that the operation is running not on the router port.
    shouldRouteRequest(opCtx.get(), opMsgRequest);
}

TEST_F(ReplicaSetEndpointUtilTest, ShouldNotRoute_NotRouterServer) {
    serverGlobalParams.clusterRole = {ClusterRole::ShardServer, ClusterRole::ConfigServer};

    std::shared_ptr<transport::Session> session = getTransportLayer().createSession();
    auto client = getServiceContext()->getService()->makeClient("NotRouterServer", session);
    auto opCtx = client->makeOperationContext();

    auto ns = NamespaceString::createNamespaceString_forTest(kTestDbName, kTestCollName);
    commands_test_example::ExampleIncrement incrementCmd(ns, 0);
    auto opMsgRequest = mongo::OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::get(opCtx.get()), ns.dbName(), incrementCmd.toBSON({}));

    ASSERT_FALSE(shouldRouteRequest(opCtx.get(), opMsgRequest));
}

TEST_F(ReplicaSetEndpointUtilTest, ShouldNotRoute_NotIsConfigShard) {
    std::shared_ptr<transport::Session> session = getTransportLayer().createSession();
    auto client = getServiceContext()->getService()->makeClient("NotIsConfigShard", session);
    auto opCtx = client->makeOperationContext();
    ReplicaSetEndpointShardingState::get(opCtx.get())->setIsConfigShard(false);

    auto ns = NamespaceString::createNamespaceString_forTest(kTestDbName, kTestCollName);
    commands_test_example::ExampleIncrement incrementCmd(ns, 0);
    auto opMsgRequest = mongo::OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::get(opCtx.get()), ns.dbName(), incrementCmd.toBSON({}));

    ASSERT_FALSE(shouldRouteRequest(opCtx.get(), opMsgRequest));
}

TEST_F(ReplicaSetEndpointUtilTest, ShouldNotRoute_HasTwoOrMoreShards) {
    setHasTwoOrShardsClusterParameter(true);
    ASSERT(getHasTwoOrShardsClusterParameter());

    std::shared_ptr<transport::Session> session = getTransportLayer().createSession();
    auto client = getServiceContext()->getService()->makeClient("HasTwoOrMoreShards", session);
    auto opCtx = client->makeOperationContext();

    auto ns = NamespaceString::createNamespaceString_forTest(kTestDbName, kTestCollName);
    commands_test_example::ExampleIncrement incrementCmd(ns, 0);
    auto opMsgRequest = mongo::OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::get(opCtx.get()), ns.dbName(), incrementCmd.toBSON({}));

    ASSERT_FALSE(shouldRouteRequest(opCtx.get(), opMsgRequest));
}

TEST_F(ReplicaSetEndpointUtilTest, ShouldNotRoute_FeatureFlagDisabled) {
    RAIIServerParameterControllerForTest replicaSetEndpointController{
        "featureFlagReplicaSetEndpoint", false};

    std::shared_ptr<transport::Session> session = getTransportLayer().createSession();
    auto client = getServiceContext()->getService()->makeClient("FeatureFlagDisabled", session);
    auto opCtx = client->makeOperationContext();

    auto ns = NamespaceString::createNamespaceString_forTest(kTestDbName, kTestCollName);
    commands_test_example::ExampleIncrement incrementCmd(ns, 0);
    auto opMsgRequest = mongo::OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::get(opCtx.get()), ns.dbName(), incrementCmd.toBSON({}));

    ASSERT_FALSE(shouldRouteRequest(opCtx.get(), opMsgRequest));
}

}  // namespace
}  // namespace replica_set_endpoint
}  // namespace mongo
