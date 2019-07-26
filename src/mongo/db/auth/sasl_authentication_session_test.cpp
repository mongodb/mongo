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
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"
#include "mongo/util/password_digest.h"

namespace mongo {

namespace {

class SaslConversation : public ServiceContextTest {
public:
    explicit SaslConversation(std::string mech);

    void testSuccessfulAuthentication();
    void testNoSuchUser();
    void testBadPassword();
    void testWrongClientMechanism();
    void testWrongServerMechanism();

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
    : opCtx(makeOperationContext()),
      authManagerExternalState(new AuthzManagerExternalStateMock),
      authManager(new AuthorizationManagerImpl(
          std::unique_ptr<AuthzManagerExternalState>(authManagerExternalState),
          AuthorizationManagerImpl::InstallMockForTestingOrAuthImpl{})),
      authSession(authManager->makeAuthorizationSession()),
      registry({"SCRAM-SHA-1", "SCRAM-SHA-256", "PLAIN"}),
      mechanism(mech) {

    AuthorizationManager::set(getServiceContext(),
                              std::unique_ptr<AuthorizationManager>(authManager));

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

    ASSERT_OK(
        authManagerExternalState->insert(opCtx.get(),
                                         NamespaceString("admin.system.users"),
                                         BSON("_id"
                                              << "test.andy"
                                              << "user"
                                              << "andy"
                                              << "db"
                                              << "test"
                                              << "credentials" << creds << "roles" << BSONArray()),
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
    } while (!client->isSuccess());
    ASSERT_FALSE(serverResponse.isOK() && clientStatus.isOK() && client->isSuccess() &&
                 server->isSuccess());
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
    } while (!client->isSuccess());
    ASSERT_TRUE(server->isSuccess());
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

}  // namespace

}  // namespace mongo
