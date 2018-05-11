/*
 * Copyright (C) 2013 10gen, Inc.  All Rights Reserved.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kAccessControl

#include <string>
#include <vector>

#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/element.h"
#include "mongo/client/sasl_client_session.h"
#include "mongo/crypto/mechanism_scram.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_impl.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/authz_manager_external_state_mock.h"
#include "mongo/db/auth/authz_session_external_state_mock.h"
#include "mongo/db/auth/sasl_mechanism_registry.h"
#include "mongo/db/auth/sasl_options.h"
#include "mongo/db/auth/sasl_plain_server_conversation.h"
#include "mongo/db/auth/sasl_scram_server_conversation.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"
#include "mongo/util/password_digest.h"

namespace mongo {

namespace {

class SaslConversation : public unittest::Test {
public:
    explicit SaslConversation(std::string mech);

    void testSuccessfulAuthentication();
    void testNoSuchUser();
    void testBadPassword();
    void testWrongClientMechanism();
    void testWrongServerMechanism();

    ServiceContextNoop serviceContext;
    ServiceContext::UniqueClient opClient;
    ServiceContext::UniqueOperationContext opCtx;
    AuthzManagerExternalStateMock* authManagerExternalState;
    AuthorizationManager* authManager;
    std::unique_ptr<AuthorizationSession> authSession;
    SASLServerMechanismRegistry registry;
    std::string mechanism;
    std::unique_ptr<SaslClientSession> client;
    std::unique_ptr<ServerMechanismBase> server;

private:
    void assertConversationFailure();
};

class SaslIllegalConversation : public SaslConversation {
public:
    SaslIllegalConversation() : SaslConversation("ILLEGAL") {}
};

const std::string mockServiceName = "mocksvc";
const std::string mockHostName = "host.mockery.com";

SaslConversation::SaslConversation(std::string mech)
    : opClient(serviceContext.makeClient("saslTest")),
      opCtx(serviceContext.makeOperationContext(opClient.get())),
      authManagerExternalState(new AuthzManagerExternalStateMock),
      authManager(new AuthorizationManagerImpl(
          std::unique_ptr<AuthzManagerExternalState>(authManagerExternalState),
          AuthorizationManagerImpl::InstallMockForTestingOrAuthImpl{})),
      authSession(authManager->makeAuthorizationSession()),
      mechanism(mech) {

    AuthorizationManager::set(&serviceContext, std::unique_ptr<AuthorizationManager>(authManager));

    client.reset(SaslClientSession::create(mechanism));

    registry.registerFactory<PLAINServerFactory>(
        SASLServerMechanismRegistry::kNoValidateGlobalMechanisms);
    registry.registerFactory<SCRAMSHA1ServerFactory>(
        SASLServerMechanismRegistry::kNoValidateGlobalMechanisms);
    registry.registerFactory<SCRAMSHA256ServerFactory>(
        SASLServerMechanismRegistry::kNoValidateGlobalMechanisms);

    ASSERT_OK(authManagerExternalState->updateOne(
        opCtx.get(),
        AuthorizationManager::versionCollectionNamespace,
        AuthorizationManager::versionDocumentQuery,
        BSON("$set" << BSON(AuthorizationManager::schemaVersionFieldName
                            << AuthorizationManager::schemaVersion26Final)),
        true,
        BSONObj()));

    // PLAIN mechanism uses the same hashed password as SCRAM-SHA-1,
    // but SCRAM-SHA-1's implementation makes the assumption the hashing has already
    // happened at both ends.
    // SCRAM-SHA-256 doesn't have this problem and always uses a pure password as input.
    const auto pwHash = (mech == "PLAIN") ? createPasswordDigest("andy", "frim") : "frim";
    const auto creds =
        BSON("SCRAM-SHA-1" << scram::Secrets<SHA1Block>::generateCredentials(
                                  pwHash, saslGlobalParams.scramSHA1IterationCount.load())
                           << "SCRAM-SHA-256"
                           << scram::Secrets<SHA256Block>::generateCredentials(
                                  "frim", saslGlobalParams.scramSHA256IterationCount.load()));

    ASSERT_OK(authManagerExternalState->insert(opCtx.get(),
                                               NamespaceString("admin.system.users"),
                                               BSON("_id"
                                                    << "test.andy"
                                                    << "user"
                                                    << "andy"
                                                    << "db"
                                                    << "test"
                                                    << "credentials"
                                                    << creds
                                                    << "roles"
                                                    << BSONArray()),
                                               BSONObj()));
}

void SaslConversation::assertConversationFailure() {
    std::string clientMessage;
    std::string serverMessage;
    Status clientStatus(ErrorCodes::InternalError, "");
    StatusWith<std::string> serverResponse("");
    do {
        clientStatus = client->step(serverResponse.getValue(), &clientMessage);
        if (!clientStatus.isOK())
            break;
        serverResponse = server->step(opCtx.get(), clientMessage);
        if (!serverResponse.isOK())
            break;
    } while (!client->isDone());
    ASSERT_FALSE(serverResponse.isOK() && clientStatus.isOK() && client->isDone() &&
                 server->isDone());
}

void SaslConversation::testSuccessfulAuthentication() {
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
    } while (!client->isDone());
    ASSERT_TRUE(server->isDone());
}

void SaslConversation::testNoSuchUser() {
    client->setParameter(SaslClientSession::parameterServiceName, mockServiceName);
    client->setParameter(SaslClientSession::parameterServiceHostname, mockHostName);
    client->setParameter(SaslClientSession::parameterMechanism, mechanism);
    client->setParameter(SaslClientSession::parameterUser, "nobody");
    client->setParameter(SaslClientSession::parameterPassword, "frim");
    ASSERT_OK(client->initialize());

    assertConversationFailure();
}

void SaslConversation::testBadPassword() {
    client->setParameter(SaslClientSession::parameterServiceName, mockServiceName);
    client->setParameter(SaslClientSession::parameterServiceHostname, mockHostName);
    client->setParameter(SaslClientSession::parameterMechanism, mechanism);
    client->setParameter(SaslClientSession::parameterUser, "andy");
    client->setParameter(SaslClientSession::parameterPassword, "WRONG");
    ASSERT_OK(client->initialize());


    assertConversationFailure();
}

void SaslConversation::testWrongClientMechanism() {
    client->setParameter(SaslClientSession::parameterServiceName, mockServiceName);
    client->setParameter(SaslClientSession::parameterServiceHostname, mockHostName);
    client->setParameter(SaslClientSession::parameterMechanism,
                         mechanism != "SCRAM-SHA-1" ? "SCRAM-SHA-1" : "PLAIN");
    client->setParameter(SaslClientSession::parameterUser, "andy");
    client->setParameter(SaslClientSession::parameterPassword, "frim");
    ASSERT_OK(client->initialize());

    assertConversationFailure();
}

void SaslConversation::testWrongServerMechanism() {
    client->setParameter(SaslClientSession::parameterServiceName, mockServiceName);
    client->setParameter(SaslClientSession::parameterServiceHostname, mockHostName);
    client->setParameter(SaslClientSession::parameterMechanism, mechanism);
    client->setParameter(SaslClientSession::parameterUser, "andy");
    client->setParameter(SaslClientSession::parameterPassword, "frim");
    ASSERT_OK(client->initialize());

    auto swServer =
        registry.getServerMechanism(mechanism != "SCRAM-SHA-1" ? "SCRAM-SHA-1" : "PLAIN", "test");
    ASSERT_OK(swServer.getStatus());
    server = std::move(swServer.getValue());

    assertConversationFailure();
}

#define DEFINE_MECHANISM_FIXTURE(CLASS_SUFFIX, MECH_NAME)                   \
    class SaslConversation##CLASS_SUFFIX : public SaslConversation {        \
    public:                                                                 \
        SaslConversation##CLASS_SUFFIX() : SaslConversation(MECH_NAME) {    \
            auto swServer = registry.getServerMechanism(MECH_NAME, "test"); \
            ASSERT_OK(swServer.getStatus());                                \
            server = std::move(swServer.getValue());                        \
        }                                                                   \
    }

#define DEFINE_MECHANISM_TEST(FIXTURE_NAME, TEST_NAME) \
    TEST_F(FIXTURE_NAME, TEST_NAME) {                  \
        test##TEST_NAME();                             \
    }

#define DEFINE_ALL_MECHANISM_TESTS(FIXTURE_NAME)                  \
    DEFINE_MECHANISM_TEST(FIXTURE_NAME, SuccessfulAuthentication) \
    DEFINE_MECHANISM_TEST(FIXTURE_NAME, NoSuchUser)               \
    DEFINE_MECHANISM_TEST(FIXTURE_NAME, BadPassword)              \
    DEFINE_MECHANISM_TEST(FIXTURE_NAME, WrongClientMechanism)     \
    DEFINE_MECHANISM_TEST(FIXTURE_NAME, WrongServerMechanism)

#define TEST_MECHANISM(CLASS_SUFFIX, MECH_NAME)        \
    DEFINE_MECHANISM_FIXTURE(CLASS_SUFFIX, MECH_NAME); \
    DEFINE_ALL_MECHANISM_TESTS(SaslConversation##CLASS_SUFFIX)

TEST_MECHANISM(SCRAMSHA1, "SCRAM-SHA-1")
TEST_MECHANISM(SCRAMSHA256, "SCRAM-SHA-256")
TEST_MECHANISM(PLAIN, "PLAIN")

TEST_F(SaslIllegalConversation, IllegalClientMechanism) {
    client->setParameter(SaslClientSession::parameterServiceName, mockServiceName);
    client->setParameter(SaslClientSession::parameterServiceHostname, mockHostName);
    client->setParameter(SaslClientSession::parameterMechanism, "FAKE");
    client->setParameter(SaslClientSession::parameterUser, "andy");
    client->setParameter(SaslClientSession::parameterPassword, "frim");

    std::string clientMessage;
    std::string serverMessage;
    ASSERT(!client->initialize().isOK() || !client->step(serverMessage, &clientMessage).isOK());
}

TEST_F(SaslIllegalConversation, IllegalServerMechanism) {
    SASLServerMechanismRegistry registry;
    auto swServer = registry.getServerMechanism("FAKE", "test");
    ASSERT_NOT_OK(swServer.getStatus());
}

}  // namespace

}  // namespace mongo
