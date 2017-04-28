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
#include "mongo/client/scram_sha1_client_cache.h"
#include "mongo/crypto/mechanism_scram.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authz_manager_external_state_mock.h"
#include "mongo/db/auth/authz_session_external_state_mock.h"
#include "mongo/db/auth/native_sasl_authentication_session.h"
#include "mongo/db/auth/sasl_scramsha1_server_conversation.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/base64.h"
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

std::string corruptEncodedPayload(const std::string& message,
                                  std::string::const_iterator begin,
                                  std::string::const_iterator end) {
    std::string raw = base64::decode(
        message.substr(std::distance(message.begin(), begin), std::distance(begin, end)));
    if (raw[0] == std::numeric_limits<char>::max()) {
        raw[0] -= 1;
    } else {
        raw[0] += 1;
    }
    return base64::encode(raw);
}

class SaslTestState {
public:
    enum Participant { kClient, kServer };
    SaslTestState() : SaslTestState(kClient, 0) {}
    SaslTestState(Participant participant, size_t stage) : participant(participant), stage(stage) {}

private:
    // Define members here, so that they can be used in declaration of lens(). In C++14, lens()
    // can be declared with a return of decltype(auto), without a trailing return type, and these
    // members can go at the end of the class.
    Participant participant;
    size_t stage;

public:
    auto lens() const -> decltype(std::tie(this->stage, this->participant)) {
        return std::tie(stage, participant);
    }

    friend bool operator==(const SaslTestState& lhs, const SaslTestState& rhs) {
        return lhs.lens() == rhs.lens();
    }

    friend bool operator<(const SaslTestState& lhs, const SaslTestState& rhs) {
        return lhs.lens() < rhs.lens();
    }

    void next() {
        if (participant == kClient) {
            participant = kServer;
        } else {
            participant = kClient;
            stage++;
        }
    }

    std::string toString() const {
        std::stringstream ss;
        if (participant == kClient) {
            ss << "Client";
        } else {
            ss << "Server";
        }
        ss << "Step" << stage;

        return ss.str();
    }
};

class SCRAMMutators {
public:
    SCRAMMutators() {}

    void setMutator(SaslTestState state, stdx::function<void(std::string&)> fun) {
        mutators.insert(std::make_pair(state, fun));
    }

    void execute(SaslTestState state, std::string& str) {
        auto it = mutators.find(state);
        if (it != mutators.end()) {
            it->second(str);
        }
    }

private:
    std::map<SaslTestState, stdx::function<void(std::string&)>> mutators;
};

struct SCRAMStepsResult {
    SCRAMStepsResult() : outcome(SaslTestState::kClient, 1), status(Status::OK()) {}
    SCRAMStepsResult(SaslTestState outcome, Status status) : outcome(outcome), status(status) {}
    bool operator==(const SCRAMStepsResult& other) const {
        return outcome == other.outcome && status.code() == other.status.code() &&
            status.reason() == other.status.reason();
    }
    SaslTestState outcome;
    Status status;

    friend std::ostream& operator<<(std::ostream& os, const SCRAMStepsResult& result) {
        return os << "{outcome: " << result.outcome.toString() << ", status: " << result.status
                  << "}";
    }
};

SCRAMStepsResult runSteps(NativeSaslAuthenticationSession* saslServerSession,
                          NativeSaslClientSession* saslClientSession,
                          SCRAMMutators interposers = SCRAMMutators{}) {
    SCRAMStepsResult result{};
    std::string clientOutput = "";
    std::string serverOutput = "";

    for (size_t step = 1; step <= 3; step++) {
        ASSERT_FALSE(saslClientSession->isDone());
        ASSERT_FALSE(saslServerSession->isDone());

        // Client step
        result.status = saslClientSession->step(serverOutput, &clientOutput);
        if (result.status != Status::OK()) {
            return result;
        }
        std::cout << result.outcome.toString() << ": " << clientOutput << std::endl;
        interposers.execute(result.outcome, clientOutput);
        result.outcome.next();

        // Server step
        result.status = saslServerSession->step(clientOutput, &serverOutput);
        if (result.status != Status::OK()) {
            return result;
        }
        interposers.execute(result.outcome, serverOutput);
        std::cout << result.outcome.toString() << ": " << serverOutput << std::endl;
        result.outcome.next();
    }
    ASSERT_TRUE(saslClientSession->isDone());
    ASSERT_TRUE(saslServerSession->isDone());

    return result;
}

class SCRAMSHA1Fixture : public mongo::unittest::Test {
protected:
    const SCRAMStepsResult goalState =
        SCRAMStepsResult(SaslTestState(SaslTestState::kClient, 4), Status::OK());

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
        saslClientSession->setParameter(NativeSaslClientSession::parameterServiceHostAndPort,
                                        "MockServer.test:27017");
    }
};

TEST_F(SCRAMSHA1Fixture, testServerStep1DoesNotIncludeNonceFromClientStep1) {
    authzManagerExternalState->insertPrivilegeDocument(
        txn.get(), generateSCRAMUserDocument("sajack", "sajack"), BSONObj());

    saslClientSession->setParameter(NativeSaslClientSession::parameterUser, "sajack");
    saslClientSession->setParameter(NativeSaslClientSession::parameterPassword,
                                    createPasswordDigest("sajack", "sajack"));

    ASSERT_OK(saslClientSession->initialize());

    SCRAMMutators mutator;
    mutator.setMutator(SaslTestState(SaslTestState::kServer, 1), [](std::string& serverMessage) {
        std::string::iterator nonceBegin = serverMessage.begin() + serverMessage.find("r=");
        std::string::iterator nonceEnd = std::find(nonceBegin, serverMessage.end(), ',');
        serverMessage = serverMessage.replace(nonceBegin, nonceEnd, "r=");

    });
    ASSERT_EQ(SCRAMStepsResult(SaslTestState(SaslTestState::kClient, 2),
                               Status(ErrorCodes::BadValue,
                                      "Server SCRAM-SHA-1 nonce does not match client nonce: r=")),
              runSteps(saslServerSession.get(), saslClientSession.get(), mutator));
}

TEST_F(SCRAMSHA1Fixture, testClientStep2DoesNotIncludeNonceFromServerStep1) {
    authzManagerExternalState->insertPrivilegeDocument(
        txn.get(), generateSCRAMUserDocument("sajack", "sajack"), BSONObj());

    saslClientSession->setParameter(NativeSaslClientSession::parameterUser, "sajack");
    saslClientSession->setParameter(NativeSaslClientSession::parameterPassword,
                                    createPasswordDigest("sajack", "sajack"));

    ASSERT_OK(saslClientSession->initialize());

    SCRAMMutators mutator;
    mutator.setMutator(SaslTestState(SaslTestState::kClient, 2), [](std::string& clientMessage) {
        std::string::iterator nonceBegin = clientMessage.begin() + clientMessage.find("r=");
        std::string::iterator nonceEnd = std::find(nonceBegin, clientMessage.end(), ',');
        clientMessage = clientMessage.replace(nonceBegin, nonceEnd, "r=");
    });
    ASSERT_EQ(SCRAMStepsResult(
                  SaslTestState(SaslTestState::kServer, 2),
                  Status(ErrorCodes::BadValue, "Incorrect SCRAM-SHA-1 client|server nonce: r=")),
              runSteps(saslServerSession.get(), saslClientSession.get(), mutator));
}

TEST_F(SCRAMSHA1Fixture, testClientStep2GivesBadProof) {
    authzManagerExternalState->insertPrivilegeDocument(
        txn.get(), generateSCRAMUserDocument("sajack", "sajack"), BSONObj());

    saslClientSession->setParameter(NativeSaslClientSession::parameterUser, "sajack");
    saslClientSession->setParameter(NativeSaslClientSession::parameterPassword,
                                    createPasswordDigest("sajack", "sajack"));

    ASSERT_OK(saslClientSession->initialize());

    SCRAMMutators mutator;
    mutator.setMutator(SaslTestState(SaslTestState::kClient, 2), [](std::string& clientMessage) {
        std::string::iterator proofBegin = clientMessage.begin() + clientMessage.find("p=") + 2;
        std::string::iterator proofEnd = std::find(proofBegin, clientMessage.end(), ',');
        clientMessage = clientMessage.replace(
            proofBegin, proofEnd, corruptEncodedPayload(clientMessage, proofBegin, proofEnd));

    });

    ASSERT_EQ(SCRAMStepsResult(SaslTestState(SaslTestState::kServer, 2),
                               Status(ErrorCodes::AuthenticationFailed,
                                      "SCRAM-SHA-1 authentication failed, storedKey mismatch")),
              runSteps(saslServerSession.get(), saslClientSession.get(), mutator));
}

TEST_F(SCRAMSHA1Fixture, testServerStep2GivesBadVerifier) {
    authzManagerExternalState->insertPrivilegeDocument(
        txn.get(), generateSCRAMUserDocument("sajack", "sajack"), BSONObj());

    saslClientSession->setParameter(NativeSaslClientSession::parameterUser, "sajack");
    saslClientSession->setParameter(NativeSaslClientSession::parameterPassword,
                                    createPasswordDigest("sajack", "sajack"));

    ASSERT_OK(saslClientSession->initialize());

    std::string encodedVerifier;
    SCRAMMutators mutator;
    mutator.setMutator(
        SaslTestState(SaslTestState::kServer, 2), [&encodedVerifier](std::string& serverMessage) {
            std::string::iterator verifierBegin =
                serverMessage.begin() + serverMessage.find("v=") + 2;
            std::string::iterator verifierEnd = std::find(verifierBegin, serverMessage.end(), ',');
            encodedVerifier = corruptEncodedPayload(serverMessage, verifierBegin, verifierEnd);

            serverMessage = serverMessage.replace(verifierBegin, verifierEnd, encodedVerifier);

        });

    auto result = runSteps(saslServerSession.get(), saslClientSession.get(), mutator);

    ASSERT_EQ(
        SCRAMStepsResult(
            SaslTestState(SaslTestState::kClient, 3),
            Status(ErrorCodes::BadValue,
                   str::stream() << "Client failed to verify SCRAM-SHA-1 ServerSignature, received "
                                 << encodedVerifier)),
        result);
}


TEST_F(SCRAMSHA1Fixture, testSCRAM) {
    authzManagerExternalState->insertPrivilegeDocument(
        txn.get(), generateSCRAMUserDocument("sajack", "sajack"), BSONObj());

    saslClientSession->setParameter(NativeSaslClientSession::parameterUser, "sajack");
    saslClientSession->setParameter(NativeSaslClientSession::parameterPassword,
                                    createPasswordDigest("sajack", "sajack"));

    ASSERT_OK(saslClientSession->initialize());

    ASSERT_EQ(goalState, runSteps(saslServerSession.get(), saslClientSession.get()));
}

TEST_F(SCRAMSHA1Fixture, testNULLInPassword) {
    authzManagerExternalState->insertPrivilegeDocument(
        txn.get(), generateSCRAMUserDocument("sajack", "saj\0ack"), BSONObj());

    saslClientSession->setParameter(NativeSaslClientSession::parameterUser, "sajack");
    saslClientSession->setParameter(NativeSaslClientSession::parameterPassword,
                                    createPasswordDigest("sajack", "saj\0ack"));

    ASSERT_OK(saslClientSession->initialize());

    ASSERT_EQ(goalState, runSteps(saslServerSession.get(), saslClientSession.get()));
}


TEST_F(SCRAMSHA1Fixture, testCommasInUsernameAndPassword) {
    authzManagerExternalState->insertPrivilegeDocument(
        txn.get(), generateSCRAMUserDocument("s,a,jack", "s,a,jack"), BSONObj());

    saslClientSession->setParameter(NativeSaslClientSession::parameterUser, "s,a,jack");
    saslClientSession->setParameter(NativeSaslClientSession::parameterPassword,
                                    createPasswordDigest("s,a,jack", "s,a,jack"));

    ASSERT_OK(saslClientSession->initialize());

    ASSERT_EQ(goalState, runSteps(saslServerSession.get(), saslClientSession.get()));
}

TEST_F(SCRAMSHA1Fixture, testIncorrectUser) {
    saslClientSession->setParameter(NativeSaslClientSession::parameterUser, "sajack");
    saslClientSession->setParameter(NativeSaslClientSession::parameterPassword,
                                    createPasswordDigest("sajack", "sajack"));

    ASSERT_OK(saslClientSession->initialize());

    ASSERT_EQ(SCRAMStepsResult(SaslTestState(SaslTestState::kServer, 1),
                               Status(ErrorCodes::UserNotFound, "Could not find user sajack@test")),
              runSteps(saslServerSession.get(), saslClientSession.get()));
}

TEST_F(SCRAMSHA1Fixture, testIncorrectPassword) {
    authzManagerExternalState->insertPrivilegeDocument(
        txn.get(), generateSCRAMUserDocument("sajack", "sajack"), BSONObj());

    saslClientSession->setParameter(NativeSaslClientSession::parameterUser, "sajack");
    saslClientSession->setParameter(NativeSaslClientSession::parameterPassword,
                                    createPasswordDigest("sajack", "invalidPassword"));

    ASSERT_OK(saslClientSession->initialize());

    ASSERT_EQ(SCRAMStepsResult(SaslTestState(SaslTestState::kServer, 2),
                               Status(ErrorCodes::AuthenticationFailed,
                                      "SCRAM-SHA-1 authentication failed, storedKey mismatch")),
              runSteps(saslServerSession.get(), saslClientSession.get()));
}

TEST_F(SCRAMSHA1Fixture, testMONGODBCR) {
    authzManagerExternalState->insertPrivilegeDocument(
        txn.get(), generateMONGODBCRUserDocument("sajack", "sajack"), BSONObj());

    saslClientSession->setParameter(NativeSaslClientSession::parameterUser, "sajack");
    saslClientSession->setParameter(NativeSaslClientSession::parameterPassword,
                                    createPasswordDigest("sajack", "sajack"));

    ASSERT_OK(saslClientSession->initialize());

    ASSERT_EQ(goalState, runSteps(saslServerSession.get(), saslClientSession.get()));
}

TEST(SCRAMSHA1Cache, testGetFromEmptyCache) {
    SCRAMSHA1ClientCache cache;
    std::string saltStr("saltsaltsaltsalt");
    std::vector<std::uint8_t> salt(saltStr.begin(), saltStr.end());
    HostAndPort host("localhost:27017");

    ASSERT_FALSE(cache.getCachedSecrets(host, scram::SCRAMPresecrets("aaa", salt, 10000)));
}


TEST(SCRAMSHA1Cache, testSetAndGet) {
    SCRAMSHA1ClientCache cache;
    std::string saltStr("saltsaltsaltsalt");
    std::string badSaltStr("s@lts@lts@lts@lt");
    std::vector<std::uint8_t> salt(saltStr.begin(), saltStr.end());
    std::vector<std::uint8_t> badSalt(badSaltStr.begin(), badSaltStr.end());
    HostAndPort host("localhost:27017");

    auto secret = scram::generateSecrets(scram::SCRAMPresecrets("aaa", salt, 10000));
    cache.setCachedSecrets(host, scram::SCRAMPresecrets("aaa", salt, 10000), secret);
    auto cachedSecret = cache.getCachedSecrets(host, scram::SCRAMPresecrets("aaa", salt, 10000));
    ASSERT_TRUE(cachedSecret);
    ASSERT_TRUE(secret->clientKey == cachedSecret->clientKey);
    ASSERT_TRUE(secret->serverKey == cachedSecret->serverKey);
    ASSERT_TRUE(secret->storedKey == cachedSecret->storedKey);
}


TEST(SCRAMSHA1Cache, testSetAndGetWithDifferentParameters) {
    SCRAMSHA1ClientCache cache;
    std::string saltStr("saltsaltsaltsalt");
    std::string badSaltStr("s@lts@lts@lts@lt");
    std::vector<std::uint8_t> salt(saltStr.begin(), saltStr.end());
    std::vector<std::uint8_t> badSalt(badSaltStr.begin(), badSaltStr.end());
    HostAndPort host("localhost:27017");

    auto secret = scram::generateSecrets(scram::SCRAMPresecrets("aaa", salt, 10000));
    cache.setCachedSecrets(host, scram::SCRAMPresecrets("aaa", salt, 10000), secret);

    ASSERT_FALSE(cache.getCachedSecrets(HostAndPort("localhost:27018"),
                                        scram::SCRAMPresecrets("aaa", salt, 10000)));
    ASSERT_FALSE(cache.getCachedSecrets(host, scram::SCRAMPresecrets("aab", salt, 10000)));
    ASSERT_FALSE(cache.getCachedSecrets(host, scram::SCRAMPresecrets("aaa", badSalt, 10000)));
    ASSERT_FALSE(cache.getCachedSecrets(host, scram::SCRAMPresecrets("aaa", salt, 10001)));
}


TEST(SCRAMSHA1Cache, testSetAndReset) {
    SCRAMSHA1ClientCache cache;
    StringData saltStr("saltsaltsaltsalt");
    std::vector<std::uint8_t> salt(saltStr.begin(), saltStr.end());
    HostAndPort host("localhost:27017");

    auto secret = scram::generateSecrets(scram::SCRAMPresecrets("aaa", salt, 10000));
    cache.setCachedSecrets(host, scram::SCRAMPresecrets("aaa", salt, 10000), secret);
    auto newSecret = scram::generateSecrets(scram::SCRAMPresecrets("aab", salt, 10000));
    cache.setCachedSecrets(host, scram::SCRAMPresecrets("aab", salt, 10000), newSecret);

    ASSERT_FALSE(cache.getCachedSecrets(host, scram::SCRAMPresecrets("aaa", salt, 10000)));
    auto cachedSecret = cache.getCachedSecrets(host, scram::SCRAMPresecrets("aab", salt, 10000));
    ASSERT_TRUE(cachedSecret);
    ASSERT_TRUE(newSecret->clientKey == cachedSecret->clientKey);
    ASSERT_TRUE(newSecret->serverKey == cachedSecret->serverKey);
    ASSERT_TRUE(newSecret->storedKey == cachedSecret->storedKey);
}

}  // namespace mongo
