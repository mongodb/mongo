/*
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/client/native_sasl_client_session.h"
#include "mongo/crypto/mechanism_scram.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authz_manager_external_state_mock.h"
#include "mongo/db/auth/authz_session_external_state_mock.h"
#include "mongo/db/auth/native_sasl_authentication_session.h"
#include "mongo/db/auth/sasl_scramsha1_server_conversation.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/password_digest.h"

namespace mongo {

BSONObj generateSCRAMUserDocument(StringData username, StringData password) {
    const size_t scramIterationCount = 10000;
    auto database = "test"_sd;

    std::string digested = createPasswordDigest(username, password);
    BSONObj scramCred = scram::generateCredentials(digested, scramIterationCount);
    return BSON("_id" << (str::stream() << database << "." << username).operator StringData()
                      << AuthorizationManager::USER_NAME_FIELD_NAME
                      << username
                      << AuthorizationManager::USER_DB_FIELD_NAME
                      << database
                      << "credentials"
                      << BSON("SCRAM-SHA-1" << scramCred)
                      << "roles"
                      << BSONArray()
                      << "privileges"
                      << BSONArray());
}

BSONObj generateMONGODBCRUserDocument(StringData username, StringData password) {
    auto database = "test"_sd;

    std::string digested = createPasswordDigest(username, password);
    return BSON("_id" << (str::stream() << database << "." << username).operator StringData()
                      << AuthorizationManager::USER_NAME_FIELD_NAME
                      << username
                      << AuthorizationManager::USER_DB_FIELD_NAME
                      << database
                      << "credentials"
                      << BSON("MONGODB-CR" << digested)
                      << "roles"
                      << BSONArray()
                      << "privileges"
                      << BSONArray());
}

class SCRAMSHA1Fixture : public mongo::unittest::Test {
protected:
    enum TestOutcomes { kSuccess, kStep1ServerFailure, kStep2ServerFailure };
    void runSteps(TestOutcomes expectedOutcome,
                  NativeSaslAuthenticationSession* saslServerSession,
                  NativeSaslClientSession* saslClientSession) {
        std::string clientOutput, serverOutput;

        ASSERT_FALSE(saslClientSession->isDone());
        ASSERT_FALSE(saslServerSession->isDone());
        // step 1
        ASSERT_OK(saslClientSession->step("", &clientOutput));
        ASSERT_FALSE(saslClientSession->isDone());

        if (expectedOutcome == kStep1ServerFailure) {
            ASSERT_NOT_OK(saslServerSession->step(clientOutput, &serverOutput));
            return;
        } else {
            ASSERT_OK(saslServerSession->step(clientOutput, &serverOutput));
        }
        ASSERT_FALSE(saslServerSession->isDone());

        // step 2
        ASSERT_OK(saslClientSession->step(serverOutput, &clientOutput));
        ASSERT_FALSE(saslClientSession->isDone());

        if (expectedOutcome == kStep2ServerFailure) {
            ASSERT_NOT_OK(saslServerSession->step(clientOutput, &serverOutput));
            return;
        } else {
            ASSERT_OK(saslServerSession->step(clientOutput, &serverOutput));
        }
        ASSERT_FALSE(saslServerSession->isDone());

        // step 3
        ASSERT_OK(saslClientSession->step(serverOutput, &clientOutput));
        ASSERT_TRUE(saslClientSession->isDone());

        ASSERT_OK(saslServerSession->step(clientOutput, &serverOutput));
        ASSERT_TRUE(saslServerSession->isDone());
    }

    ServiceContextNoop serviceContext;
    ServiceContextNoop::UniqueClient client;
    ServiceContextNoop::UniqueOperationContext txn;

    AuthzManagerExternalStateMock* authzManagerExternalState;
    std::unique_ptr<AuthorizationManager> authzManager;
    std::unique_ptr<AuthorizationSession> authzSession;

    std::unique_ptr<NativeSaslAuthenticationSession> saslServerSession;
    std::unique_ptr<NativeSaslClientSession> saslClientSession;

    void setUp() {
        client = serviceContext.makeClient("test");
        txn = serviceContext.makeOperationContext(client.get());

        auto uniqueAuthzManagerExternalStateMock =
            stdx::make_unique<AuthzManagerExternalStateMock>();
        authzManagerExternalState = uniqueAuthzManagerExternalStateMock.get();
        authzManager =
            stdx::make_unique<AuthorizationManager>(std::move(uniqueAuthzManagerExternalStateMock));
        authzSession = stdx::make_unique<AuthorizationSession>(
            stdx::make_unique<AuthzSessionExternalStateMock>(authzManager.get()));

        saslServerSession = stdx::make_unique<NativeSaslAuthenticationSession>(authzSession.get());
        saslServerSession->setOpCtxt(txn.get());
        saslServerSession->start("test", "SCRAM-SHA-1", "mongodb", "MockServer.test", 1, false);
        saslClientSession = stdx::make_unique<NativeSaslClientSession>();
        saslClientSession->setParameter(NativeSaslClientSession::parameterMechanism, "SCRAM-SHA-1");
        saslClientSession->setParameter(NativeSaslClientSession::parameterServiceName, "mongodb");
        saslClientSession->setParameter(NativeSaslClientSession::parameterServiceHostname,
                                        "MockServer.test");
    }
};

TEST_F(SCRAMSHA1Fixture, testSCRAM) {
    authzManagerExternalState->insertPrivilegeDocument(
        txn.get(), generateSCRAMUserDocument("sajack", "sajack"), BSONObj());

    saslClientSession->setParameter(NativeSaslClientSession::parameterUser, "sajack");
    saslClientSession->setParameter(NativeSaslClientSession::parameterPassword,
                                    createPasswordDigest("sajack", "sajack"));

    ASSERT_OK(saslClientSession->initialize());

    runSteps(kSuccess, saslServerSession.get(), saslClientSession.get());
}

TEST_F(SCRAMSHA1Fixture, testNULLInPassword) {
    authzManagerExternalState->insertPrivilegeDocument(
        txn.get(), generateSCRAMUserDocument("sajack", "saj\0ack"), BSONObj());

    saslClientSession->setParameter(NativeSaslClientSession::parameterUser, "sajack");
    saslClientSession->setParameter(NativeSaslClientSession::parameterPassword,
                                    createPasswordDigest("sajack", "saj\0ack"));

    ASSERT_OK(saslClientSession->initialize());

    runSteps(kSuccess, saslServerSession.get(), saslClientSession.get());
}

TEST_F(SCRAMSHA1Fixture, testCommasInUsernameAndPassword) {
    authzManagerExternalState->insertPrivilegeDocument(
        txn.get(), generateSCRAMUserDocument("s,a,jack", "s,a,jack"), BSONObj());

    saslClientSession->setParameter(NativeSaslClientSession::parameterUser, "s,a,jack");
    saslClientSession->setParameter(NativeSaslClientSession::parameterPassword,
                                    createPasswordDigest("s,a,jack", "s,a,jack"));

    ASSERT_OK(saslClientSession->initialize());

    runSteps(kSuccess, saslServerSession.get(), saslClientSession.get());
}

TEST_F(SCRAMSHA1Fixture, testIncorrectUser) {
    saslClientSession->setParameter(NativeSaslClientSession::parameterUser, "sajack");
    saslClientSession->setParameter(NativeSaslClientSession::parameterPassword,
                                    createPasswordDigest("sajack", "sajack"));

    ASSERT_OK(saslClientSession->initialize());

    runSteps(kStep1ServerFailure, saslServerSession.get(), saslClientSession.get());
}

TEST_F(SCRAMSHA1Fixture, testIncorrectPassword) {
    authzManagerExternalState->insertPrivilegeDocument(
        txn.get(), generateSCRAMUserDocument("sajack", "sajack"), BSONObj());

    saslClientSession->setParameter(NativeSaslClientSession::parameterUser, "sajack");
    saslClientSession->setParameter(NativeSaslClientSession::parameterPassword,
                                    createPasswordDigest("sajack", "invalidPassword"));

    ASSERT_OK(saslClientSession->initialize());

    runSteps(kStep2ServerFailure, saslServerSession.get(), saslClientSession.get());
}

TEST_F(SCRAMSHA1Fixture, testMONGODBCR) {
    authzManagerExternalState->insertPrivilegeDocument(
        txn.get(), generateMONGODBCRUserDocument("sajack", "sajack"), BSONObj());

    saslClientSession->setParameter(NativeSaslClientSession::parameterUser, "sajack");
    saslClientSession->setParameter(NativeSaslClientSession::parameterPassword,
                                    createPasswordDigest("sajack", "sajack"));

    ASSERT_OK(saslClientSession->initialize());

    runSteps(kSuccess, saslServerSession.get(), saslClientSession.get());
}


}  // namespace mongo
