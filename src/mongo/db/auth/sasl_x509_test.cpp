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

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/native_sasl_client_session.h"
#include "mongo/client/sasl_client_session.h"
#include "mongo/db/auth/authorization_backend_interface.h"
#include "mongo/db/auth/authorization_backend_local.h"
#include "mongo/db/auth/authorization_backend_mock.h"
#include "mongo/db/auth/authorization_client_handle_shard.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_factory_mock.h"
#include "mongo/db/auth/authorization_manager_impl.h"
#include "mongo/db/auth/authorization_router_impl.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/authorization_session_impl.h"
#include "mongo/db/auth/authz_session_external_state_mock.h"
#include "mongo/db/auth/cluster_auth_mode.h"
#include "mongo/db/auth/sasl_x509_server_conversation.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_entry_point_shard_role.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/logv2/log.h"
#include "mongo/transport/mock_session.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/unittest.h"

#include <memory>

#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::auth {

namespace {

constexpr auto kX509Str = "x509"_sd;
constexpr auto kX509Subject = "C=US,ST=New York,L=New York City,O=MongoDB,OU=Kernel,CN=client"_sd;
constexpr auto kX509UTF8String = 12;

BSONObj generateX509UserDocument(const StringData username) {
    const auto database = "$external"_sd;

    return BSON("_id" << fmt::format("{}.{}", database, username)
                      << AuthorizationManager::USER_NAME_FIELD_NAME << username
                      << AuthorizationManager::USER_DB_FIELD_NAME << database << "roles"
                      << BSONArray() << "privileges" << BSONArray() << "credentials"
                      << BSON("external" << true));
}

// Construct a simple, structured X509 name equivalent to "CN=mongodb.com"
SSLX509Name buildX509Name() {
    return SSLX509Name(std::vector<std::vector<SSLX509Name::Entry>>(
        {{{std::string{kOID_CommonName}, 19 /* Printable String */, "mongodb.com"}}}));
}

// Construct a simple X509 name equivalent to "OU=Kernel,O=MongoDB"
SSLX509Name buildClusterX509Name() {
    return SSLX509Name(std::vector<std::vector<SSLX509Name::Entry>>(
        {{{std::string{kOID_CountryName}, kX509UTF8String, "US"}},
         {{std::string{kOID_StateName}, kX509UTF8String, "New York"}},
         {{std::string{kOID_LocalityName}, kX509UTF8String, "New York City"}},
         {{std::string{kOID_OName}, kX509UTF8String, "MongoDB"}},
         {{std::string{kOID_OUName}, kX509UTF8String, "Kernel"}},
         {{std::string{kOID_CommonName}, kX509UTF8String, "client"}}}));
}

void setX509PeerInfo(const std::shared_ptr<transport::Session>& session, SSLPeerInfo info) {
    auto& sslPeerInfo = SSLPeerInfo::forSession(session);
    sslPeerInfo = std::make_shared<SSLPeerInfo>(info);
}


class SASLX509Test : public mongo::unittest::Test {
protected:
    void setUp() final try {
        auto serviceContextHolder = ServiceContext::make();
        serviceContext = serviceContextHolder.get();
        setGlobalServiceContext(std::move(serviceContextHolder));

        session = transport::MockSession::create(&transportLayer);
        Client::setCurrent(serviceContext->getService()->makeClient("test", session));

        client = Client::getCurrent();
        opCtx = serviceContext->makeOperationContext(client);

        SSLParams params;
        params.sslMode.store(::mongo::sslGlobalParams.SSLMode_requireSSL);
        params.sslPEMKeyFile = "jstests/libs/server.pem";
        params.sslCAFile = "jstests/libs/ca.pem";
        params.sslClusterFile = "jstests/libs/server.pem";
        params.clusterAuthX509ExtensionValue = std::string{kX509Subject};
        sslGlobalParams.clusterAuthX509ExtensionValue = std::string{kX509Subject};
        auto manager = SSLManagerInterface::create(params, true);

        session->setSSLManager(manager);

        // Initialize the serviceEntryPoint so that DBDirectClient can function.
        serviceContext->getService()->setServiceEntryPoint(
            std::make_unique<ServiceEntryPointShardRole>());

        // Setup the repl coordinator in standalone mode so we don't need an oplog etc.
        repl::ReplicationCoordinator::set(serviceContext,
                                          std::make_unique<repl::ReplicationCoordinatorMock>(
                                              serviceContext, repl::ReplSettings()));

        auto globalAuthzManagerFactory = std::make_unique<AuthorizationManagerFactoryMock>();
        AuthorizationManager::set(
            serviceContext->getService(),
            globalAuthzManagerFactory->createShard(serviceContext->getService()));

        auth::AuthorizationBackendInterface::set(
            serviceContext->getService(),
            globalAuthzManagerFactory->createBackendInterface(serviceContext->getService()));
        authzBackend = reinterpret_cast<auth::AuthorizationBackendMock*>(
            auth::AuthorizationBackendInterface::get(serviceContext->getService()));

        saslServerSession = std::make_unique<SaslX509ServerMechanism>("$external");
        saslClientSession = std::make_unique<NativeSaslClientSession>();

        saslClientSession->setParameter(NativeSaslClientSession::parameterMechanism,
                                        saslServerSession->mechanismName());
        saslClientSession->setParameter(NativeSaslClientSession::parameterServiceName, "mongodb");
        saslClientSession->setParameter(NativeSaslClientSession::parameterServiceHostname,
                                        "MockServer.test");
        saslClientSession->setParameter(NativeSaslClientSession::parameterServiceHostAndPort,
                                        "MockServer.test:27017");
    } catch (const DBException& e) {
        ::mongo::StringBuilder sb;
        sb << "SASLX509Test Fixture Setup Failed: " << e.what();
        FAIL(sb.str());
    }


    StatusWith<std::string> runSteps(boost::optional<std::string> username) {
        ASSERT_FALSE(saslClientSession->isSuccess());
        ASSERT_FALSE(saslServerSession->isSuccess());

        std::string clientOutput = "";
        if (username) {
            saslClientSession->setParameter(NativeSaslClientSession::parameterUser, username.get());
        }

        ASSERT_OK(saslClientSession->step("", &clientOutput));

        return saslServerSession->step(opCtx.get(), clientOutput);
    }

    ~SASLX509Test() override {
        opCtx.reset();
        Client::releaseCurrent();
        setGlobalServiceContext(nullptr);
        serviceContext = nullptr;

        saslClientSession.reset();
        saslServerSession.reset();
    }

public:
    void run() {
        LOGV2(82092, "X509 variant");
        Test::run();
    }

protected:
    ServiceContext* serviceContext;
    Client* client;
    ServiceContext::UniqueOperationContext opCtx;
    std::shared_ptr<transport::MockSession> session;

    std::shared_ptr<SSLManagerInterface> manager;

    transport::TransportLayerMock transportLayer;
    auth::AuthorizationBackendMock* authzBackend;

    std::unique_ptr<ServerMechanismBase> saslServerSession;
    std::unique_ptr<NativeSaslClientSession> saslClientSession;
};

// 1. Positive test case
// 2. Basic negative test case
// 3. Cluster user test case
// 4. Cluster authentication certificate, regular user test case
//   a. gEnforceUserSeparation on
//   b. gEnforceUserSeparation off
// 5. Cert that's both a cluster member and explicit user in db
TEST_F(SASLX509Test, testBasic) {
    RAIIServerParameterControllerForTest userAcquisitionRefactorFeatureFlag(
        "featureFlagRearchitectUserAcquisition", true);
    SSLX509Name name = buildX509Name();
    setX509PeerInfo(session, SSLPeerInfo(name));

    ASSERT_OK(saslClientSession->initialize());

    auto result = runSteps(name.toString());
    ASSERT_OK(result);

    ASSERT_TRUE(saslServerSession->isSuccess());
}

TEST_F(SASLX509Test, testBasicFailure) {
    RAIIServerParameterControllerForTest userAcquisitionRefactorFeatureFlag(
        "featureFlagRearchitectUserAcquisition", true);
    SSLX509Name name = buildX509Name();
    setX509PeerInfo(session, SSLPeerInfo(name));

    ASSERT_OK(saslClientSession->initialize());

    auto result = runSteps(std::string("Wrong name"));
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::AuthenticationFailed);
    ASSERT_EQ(result.getStatus().reason(),
              "There is no x.509 client certificate matching the user.");

    ASSERT_FALSE(saslServerSession->isSuccess());
}

TEST_F(SASLX509Test, testBasicNoUsername) {
    RAIIServerParameterControllerForTest userAcquisitionRefactorFeatureFlag(
        "featureFlagRearchitectUserAcquisition", true);
    SSLX509Name name = buildX509Name();
    setX509PeerInfo(session, SSLPeerInfo(name));
    ASSERT_OK(authzBackend->insertUserDocument(
        opCtx.get(), generateX509UserDocument(name.toString()), BSONObj()));

    ASSERT_OK(saslClientSession->initialize());

    auto result = runSteps(boost::none);
    ASSERT_OK(result);

    ASSERT_TRUE(saslServerSession->isSuccess());
}

TEST_F(SASLX509Test, testBasicEmptyUsername) {
    RAIIServerParameterControllerForTest userAcquisitionRefactorFeatureFlag(
        "featureFlagRearchitectUserAcquisition", true);
    SSLX509Name name = buildX509Name();
    setX509PeerInfo(session, SSLPeerInfo(name));
    ASSERT_OK(authzBackend->insertUserDocument(
        opCtx.get(), generateX509UserDocument(name.toString()), BSONObj()));

    ASSERT_OK(saslClientSession->initialize());

    auto result = runSteps(std::string(""));
    ASSERT_OK(result);

    ASSERT_TRUE(saslServerSession->isSuccess());
}

TEST_F(SASLX509Test, testIncorrectDatabase) {
    RAIIServerParameterControllerForTest userAcquisitionRefactorFeatureFlag(
        "featureFlagRearchitectUserAcquisition", true);
    saslServerSession = std::make_unique<SaslX509ServerMechanism>("test");

    SSLX509Name name = buildX509Name();
    setX509PeerInfo(session, SSLPeerInfo(name));

    ASSERT_OK(saslClientSession->initialize());

    auto result = runSteps(name.toString());

    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::BadValue);
    ASSERT_EQ(result.getStatus().reason(),
              "MONGODB-X509 is only available on the '$external' database.");

    ASSERT_FALSE(saslServerSession->isSuccess());
}

// The code below is exclusive to OpenSSL since there is no way to set
// ClusterAuthX509Config on any other platform.
#if MONGO_CONFIG_SSL_PROVIDER == MONGO_CONFIG_SSL_PROVIDER_OPENSSL
TEST_F(SASLX509Test, testBasicCluster) {
    RAIIServerParameterControllerForTest userAcquisitionRefactorFeatureFlag(
        "featureFlagRearchitectUserAcquisition", true);
    saslServerSession = std::make_unique<SaslX509ServerMechanism>("$external");

    SSLX509Name name = buildClusterX509Name();
    ClusterAuthMode::set(serviceContext, ClusterAuthMode::parse(kX509Str).getValue());
    auto peerInfo = SSLPeerInfo(name);
    peerInfo.setClusterMembership(name.toString());
    setX509PeerInfo(session, peerInfo);

    ASSERT_OK(saslClientSession->initialize());

    auto result = runSteps(name.toString());

    ASSERT_OK(result);

    ASSERT_TRUE(saslServerSession->isSuccess());
}

TEST_F(SASLX509Test, testSystemLocalWithClusterAuthFails) {
    RAIIServerParameterControllerForTest userAcquisitionRefactorFeatureFlag(
        "featureFlagRearchitectUserAcquisition", true);
    saslServerSession = std::make_unique<SaslX509ServerMechanism>("local");

    SSLX509Name name = buildClusterX509Name();
    ClusterAuthMode::set(serviceContext, ClusterAuthMode::parse(kX509Str).getValue());
    auto peerInfo = SSLPeerInfo(name);
    peerInfo.setClusterMembership(name.toString());
    setX509PeerInfo(session, peerInfo);

    ASSERT_OK(saslClientSession->initialize());

    auto result = runSteps(std::string("__system"));

    ASSERT_NOT_OK(result);

    ASSERT_EQ(result.getStatus().code(), ErrorCodes::AuthenticationFailed);
    ASSERT_EQ(result.getStatus().reason(),
              "There is no x.509 client certificate matching the user.");

    ASSERT_FALSE(saslServerSession->isSuccess());
}


TEST_F(SASLX509Test, testEnforceUserClusterSeparationFalse) {
    RAIIServerParameterControllerForTest userAcquisitionRefactorFeatureFlag(
        "featureFlagRearchitectUserAcquisition", true);
    RAIIServerParameterControllerForTest enforceClusterSeparation("enforceUserClusterSeparation",
                                                                  false);

    saslServerSession = std::make_unique<SaslX509ServerMechanism>("$external");

    SSLX509Name name = buildClusterX509Name();
    ClusterAuthMode::set(serviceContext, ClusterAuthMode::parse(kX509Str).getValue());
    auto peerInfo = SSLPeerInfo(name);
    peerInfo.setClusterMembership(name.toString());
    setX509PeerInfo(session, peerInfo);

    ASSERT_OK(authzBackend->insertUserDocument(
        opCtx.get(), generateX509UserDocument(kX509Subject), BSONObj()));

    ASSERT_OK(saslClientSession->initialize());

    auto result = runSteps(name.toString());

    ASSERT_OK(result);

    ASSERT_TRUE(saslServerSession->isSuccess());
}

TEST_F(SASLX509Test, testEnforceUserClusterSeparationTrue) {
    RAIIServerParameterControllerForTest userAcquisitionRefactorFeatureFlag(
        "featureFlagRearchitectUserAcquisition", true);
    RAIIServerParameterControllerForTest enforceClusterSeparation("enforceUserClusterSeparation",
                                                                  true);

    saslServerSession = std::make_unique<SaslX509ServerMechanism>("$external");

    ASSERT_OK(authzBackend->insertUserDocument(
        opCtx.get(), generateX509UserDocument(kX509Subject), BSONObj()));


    SSLX509Name name = buildClusterX509Name();
    ClusterAuthMode::set(serviceContext, ClusterAuthMode::parse(kX509Str).getValue());
    auto peerInfo = SSLPeerInfo(name);
    peerInfo.setClusterMembership(name.toString());
    setX509PeerInfo(session, peerInfo);

    ASSERT_OK(saslClientSession->initialize());

    auto result = runSteps(name.toString());

    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::AuthenticationFailed);
    ASSERT_EQ(result.getStatus().reason(),
              "The provided certificate represents both a cluster member and an explicit user "
              "which exists in the authzn database. Prohibiting authentication due to "
              "enforceUserClusterSeparation setting.");

    ASSERT_FALSE(saslServerSession->isSuccess());
}
#endif

}  // namespace

}  // namespace mongo::auth
