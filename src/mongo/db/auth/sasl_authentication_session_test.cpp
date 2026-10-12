// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/sasl_client_session.h"
#include "mongo/crypto/mechanism_scram.h"
#include "mongo/crypto/sha1_block.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/db/auth/authorization_backend_interface.h"
#include "mongo/db/auth/authorization_backend_local.h"
#include "mongo/db/auth/authorization_backend_mock.h"
#include "mongo/db/auth/authorization_client_handle_shard.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_factory_mock.h"
#include "mongo/db/auth/authorization_manager_impl.h"
#include "mongo/db/auth/authorization_router_impl.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/sasl_command_constants.h"
#include "mongo/db/auth/sasl_mechanism_registry.h"
#include "mongo/db/auth/sasl_options.h"
#include "mongo/db/auth/sasl_plain_server_conversation.h"
#include "mongo/db/auth/sasl_scram_server_conversation.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/service_entry_point_shard_role.h"
#include "mongo/platform/atomic.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/password_digest.h"

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>

namespace mongo {
namespace {

class SaslConversation : public ServiceContextTest,
                         public testing::WithParamInterface<std::string> {
public:
    void setUp() override;

    void assertConversationFailure();

    virtual bool skipServerSetup() const {
        return false;
    }

    ServiceContext::UniqueOperationContext opCtx;
    auth::AuthorizationBackendMock* authzBackend = nullptr;
    // Needs the Service, so it cannot be built before setUp() has run.
    boost::optional<SASLServerMechanismRegistry> registry;
    std::string mechanism = GetParam();
    std::unique_ptr<SaslClientSession> client;
    std::unique_ptr<ServerMechanismBase> server;
};

const std::string mockServiceName = "mocksvc";
const std::string mockHostName = "host.mockery.com";

void SaslConversation::setUp() {
    ServiceContextTest::setUp();
    opCtx = makeOperationContext();
    registry.emplace(getService(),
                     std::vector<std::string>{"SCRAM-SHA-1", "SCRAM-SHA-256", "PLAIN"});

    // Initialize the serviceEntryPoint so that DBDirectClient can function.
    getService()->setServiceEntryPoint(std::make_unique<ServiceEntryPointShardRole>());

    // Setup the repl coordinator in standalone mode so we don't need an oplog etc.
    repl::ReplicationCoordinator::set(getServiceContext(),
                                      std::make_unique<repl::ReplicationCoordinatorMock>(
                                          getServiceContext(), repl::ReplSettings()));

    auto globalAuthzManagerFactory = std::make_unique<AuthorizationManagerFactoryMock>();

    AuthorizationManager::set(getService(), globalAuthzManagerFactory->createShard(getService()));

    auth::AuthorizationBackendInterface::set(
        getService(), globalAuthzManagerFactory->createBackendInterface(getService()));
    authzBackend = reinterpret_cast<auth::AuthorizationBackendMock*>(
        auth::AuthorizationBackendInterface::get(getService()));

    client.reset(SaslClientSession::create(mechanism));

    registry->registerFactory<PLAINServerFactory>(
        SASLServerMechanismRegistry::kNoValidateGlobalMechanisms);
    registry->registerFactory<SCRAMSHA1ServerFactory>(
        SASLServerMechanismRegistry::kNoValidateGlobalMechanisms);
    registry->registerFactory<SCRAMSHA256ServerFactory>(
        SASLServerMechanismRegistry::kNoValidateGlobalMechanisms);

    // PLAIN mechanism uses the same hashed password as SCRAM-SHA-1,
    // but SCRAM-SHA-1's implementation makes the assumption the hashing has already
    // happened at both ends.
    // SCRAM-SHA-256 doesn't have this problem and always uses a pure password as input.
    const auto pwHash = (mechanism == "PLAIN") ? createPasswordDigest("andy", "frim") : "frim";
    const auto creds =
        BSON("SCRAM-SHA-1" << scram::Secrets<SHA1Block>::generateCredentials(
                                  pwHash, saslGlobalParams.scramSHA1IterationCount.load())
                           << "SCRAM-SHA-256"
                           << scram::Secrets<SHA256Block>::generateCredentials(
                                  "frim", saslGlobalParams.scramSHA256IterationCount.load()));

    ASSERT_OK(
        authzBackend->insert(opCtx.get(),
                             NamespaceString::createNamespaceString_forTest("admin.system.users"),
                             BSON("_id" << "test.andy"
                                        << "user"
                                        << "andy"
                                        << "db"
                                        << "test"
                                        << "credentials" << creds << "roles" << BSONArray()),
                             BSONObj()));

    if (!skipServerSetup()) {
        auto swServer = registry->getServerMechanism(mechanism, "test");
        ASSERT_OK(swServer.getStatus());
        server = std::move(swServer.getValue());
    }
}

void SaslConversation::assertConversationFailure() {
    std::string clientMessage;
    Status clientStatus(ErrorCodes::InternalError, "");
    StatusWith<std::string> serverResponse("");
    do {
        clientStatus = client->step(serverResponse.getValue(), &clientMessage);
        if (!clientStatus.isOK())
            break;
        serverResponse = server->step(opCtx.get(), clientMessage);
        if (!serverResponse.isOK())
            break;
    } while (!client->isSuccess());
    ASSERT_FALSE(serverResponse.isOK() && clientStatus.isOK() && client->isSuccess() &&
                 server->isSuccess());
}

TEST_P(SaslConversation, SuccessfulAuthentication) {
    client->setParameter(SaslClientSession::parameterServiceName, mockServiceName);
    client->setParameter(SaslClientSession::parameterServiceHostname, mockHostName);
    client->setParameter(SaslClientSession::parameterMechanism, mechanism);
    client->setParameter(SaslClientSession::parameterUser, "andy");
    client->setParameter(SaslClientSession::parameterPassword, "frim");
    ASSERT_OK(client->initialize());

    std::string clientMessage;
    StatusWith<std::string> serverResponse("");
    do {
        ASSERT_OK(client->step(serverResponse.getValue(), &clientMessage));
        serverResponse = server->step(opCtx.get(), clientMessage);
        ASSERT_OK(serverResponse.getStatus());
    } while (!client->isSuccess());
    ASSERT_TRUE(server->isSuccess());
}

TEST_P(SaslConversation, NoSuchUser) {
    client->setParameter(SaslClientSession::parameterServiceName, mockServiceName);
    client->setParameter(SaslClientSession::parameterServiceHostname, mockHostName);
    client->setParameter(SaslClientSession::parameterMechanism, mechanism);
    client->setParameter(SaslClientSession::parameterUser, "nobody");
    client->setParameter(SaslClientSession::parameterPassword, "frim");
    ASSERT_OK(client->initialize());

    assertConversationFailure();
}

TEST_P(SaslConversation, BadPassword) {
    client->setParameter(SaslClientSession::parameterServiceName, mockServiceName);
    client->setParameter(SaslClientSession::parameterServiceHostname, mockHostName);
    client->setParameter(SaslClientSession::parameterMechanism, mechanism);
    client->setParameter(SaslClientSession::parameterUser, "andy");
    client->setParameter(SaslClientSession::parameterPassword, "WRONG");
    ASSERT_OK(client->initialize());


    assertConversationFailure();
}

TEST_P(SaslConversation, WrongClientMechanism) {
    client->setParameter(SaslClientSession::parameterServiceName, mockServiceName);
    client->setParameter(SaslClientSession::parameterServiceHostname, mockHostName);
    client->setParameter(SaslClientSession::parameterMechanism,
                         mechanism != "SCRAM-SHA-1" ? "SCRAM-SHA-1" : "PLAIN");
    client->setParameter(SaslClientSession::parameterUser, "andy");
    client->setParameter(SaslClientSession::parameterPassword, "frim");
    ASSERT_OK(client->initialize());

    assertConversationFailure();
}

TEST_P(SaslConversation, WrongServerMechanism) {
    client->setParameter(SaslClientSession::parameterServiceName, mockServiceName);
    client->setParameter(SaslClientSession::parameterServiceHostname, mockHostName);
    client->setParameter(SaslClientSession::parameterMechanism, mechanism);
    client->setParameter(SaslClientSession::parameterUser, "andy");
    client->setParameter(SaslClientSession::parameterPassword, "frim");
    ASSERT_OK(client->initialize());

    auto swServer =
        registry->getServerMechanism(mechanism != "SCRAM-SHA-1" ? "SCRAM-SHA-1" : "PLAIN", "test");
    ASSERT_OK(swServer.getStatus());
    server = std::move(swServer.getValue());

    assertConversationFailure();
}

TEST_P(SaslConversation, SCRAMSkipEmptyExchange) {
    if ((mechanism != "SCRAM-SHA-1") && (mechanism != "SCRAM-SHA-256")) {
        return;
    }

    for (bool enabled : {true, false}) {
        client.reset(SaslClientSession::create(mechanism));
        client->setParameter(SaslClientSession::parameterServiceName, mockServiceName);
        client->setParameter(SaslClientSession::parameterServiceHostname, mockHostName);
        client->setParameter(SaslClientSession::parameterMechanism, mechanism);
        client->setParameter(SaslClientSession::parameterUser, "andy");
        client->setParameter(SaslClientSession::parameterPassword, "frim");
        ASSERT_OK(client->initialize());

        auto swServer = registry->getServerMechanism(mechanism, "test");
        ASSERT_OK(swServer.getStatus());
        server = std::move(swServer.getValue());
        ASSERT_OK(server->setOptions(BSON(saslCommandOptionSkipEmptyExchange << enabled)));

        const std::size_t expected = enabled ? 2 : 3;
        std::size_t step = 0;

        std::string clientMsg = "";
        StatusWith<std::string> serverMsg = "";
        for (;;) {
            ASSERT_OK(client->step(serverMsg.getValue(), &clientMsg));
            if (client->isSuccess() && server->isSuccess()) {
                break;
            }

            if (step > expected) {
                break;
            }
            ++step;

            serverMsg = server->step(opCtx.get(), clientMsg);
            ASSERT_OK(serverMsg.getStatus());
            if (client->isSuccess() && server->isSuccess()) {
                break;
            }
        }

        ASSERT_TRUE(client->isSuccess());
        ASSERT_TRUE(server->isSuccess());
        ASSERT_EQ(step, expected);
    }
}

class IllegalSaslConversation : public SaslConversation {
public:
    bool skipServerSetup() const override {
        return true;
    }
};

TEST_P(IllegalSaslConversation, IllegalClientMechanism) {
    client->setParameter(SaslClientSession::parameterServiceName, mockServiceName);
    client->setParameter(SaslClientSession::parameterServiceHostname, mockHostName);
    client->setParameter(SaslClientSession::parameterMechanism, "FAKE");
    client->setParameter(SaslClientSession::parameterUser, "andy");
    client->setParameter(SaslClientSession::parameterPassword, "frim");

    std::string clientMessage;
    std::string serverMessage;
    ASSERT(!client->initialize().isOK() || !client->step(serverMessage, &clientMessage).isOK());
}

INSTANTIATE_TEST_SUITE_P(,
                         SaslConversation,
                         testing::Values("SCRAM-SHA-1", "SCRAM-SHA-256", "PLAIN"));
INSTANTIATE_TEST_SUITE_P(, IllegalSaslConversation, testing::Values("ILLEGAL"));

}  // namespace

}  // namespace mongo
