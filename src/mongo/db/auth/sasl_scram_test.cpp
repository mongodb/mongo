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


#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/native_sasl_client_session.h"
#include "mongo/client/sasl_client_session.h"
#include "mongo/client/sasl_scram_client_conversation.h"
#include "mongo/client/scram_client_cache.h"
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
#include "mongo/db/auth/authorization_session_impl.h"
#include "mongo/db/auth/authz_session_external_state_mock.h"
#include "mongo/db/auth/sasl_mechanism_registry.h"
#include "mongo/db/auth/sasl_scram_server_conversation.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_entry_point_shard_role.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/base64.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/password_digest.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <compare>
#include <cstddef>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

BSONObj generateSCRAMUserDocument(StringData username, StringData password) {
    const auto database = "test"_sd;

    const auto digested = createPasswordDigest(username, password);
    const auto sha1Cred = scram::Secrets<SHA1Block>::generateCredentials(digested, 10000);
    const auto sha256Cred =
        scram::Secrets<SHA256Block>::generateCredentials(std::string{password}, 15000);
    return BSON("_id" << (str::stream() << database << "." << username).operator StringData()
                      << AuthorizationManager::USER_NAME_FIELD_NAME << username
                      << AuthorizationManager::USER_DB_FIELD_NAME << database << "credentials"
                      << BSON("SCRAM-SHA-1" << sha1Cred << "SCRAM-SHA-256" << sha256Cred) << "roles"
                      << BSONArray() << "privileges" << BSONArray());
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

    void setMutator(SaslTestState state, std::function<void(std::string&)> fun) {
        mutators.insert(std::make_pair(state, fun));
    }

    void execute(SaslTestState state, std::string& str) {
        auto it = mutators.find(state);
        if (it != mutators.end()) {
            it->second(str);
        }
    }

private:
    std::map<SaslTestState, std::function<void(std::string&)>> mutators;
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

class SCRAMFixture : public mongo::unittest::Test {
protected:
    const SCRAMStepsResult goalState =
        SCRAMStepsResult(SaslTestState(SaslTestState::kClient, 4), Status::OK());

    ServiceContext* serviceContext;
    ServiceContext::UniqueClient client;
    ServiceContext::UniqueOperationContext opCtx;

    auth::AuthorizationBackendMock* authzBackend;

    std::unique_ptr<ServerMechanismBase> saslServerSession;
    std::unique_ptr<NativeSaslClientSession> saslClientSession;

    void setUp() final {
        auto serviceContextHolder = ServiceContext::make();
        serviceContext = serviceContextHolder.get();
        setGlobalServiceContext(std::move(serviceContextHolder));
        client = serviceContext->getService()->makeClient("test");
        opCtx = serviceContext->makeOperationContext(client.get());

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

        saslClientSession = std::make_unique<NativeSaslClientSession>();
        saslClientSession->setParameter(NativeSaslClientSession::parameterMechanism,
                                        saslServerSession->mechanismName());
        saslClientSession->setParameter(NativeSaslClientSession::parameterServiceName, "mongodb");
        saslClientSession->setParameter(NativeSaslClientSession::parameterServiceHostname,
                                        "MockServer.test");
        saslClientSession->setParameter(NativeSaslClientSession::parameterServiceHostAndPort,
                                        "MockServer.test:27017");
    }

    void tearDown() final {
        opCtx.reset();
        client.reset();
        setGlobalServiceContext(nullptr);
        serviceContext = nullptr;

        saslClientSession.reset();
        saslServerSession.reset();
    }

    std::string createPasswordDigest(StringData username, StringData password) {
        if (_digestPassword) {
            return mongo::createPasswordDigest(username, password);
        } else {
            return std::string{password};
        }
    }

    SCRAMStepsResult runSteps(SCRAMMutators interposers = SCRAMMutators{}) {
        SCRAMStepsResult result{};
        std::string clientOutput = "";
        std::string serverOutput = "";

        for (size_t step = 1; step <= 3; step++) {
            ASSERT_FALSE(saslClientSession->isSuccess());
            ASSERT_FALSE(saslServerSession->isSuccess());

            // Client step
            result.status = saslClientSession->step(serverOutput, &clientOutput);
            if (result.status != Status::OK()) {
                return result;
            }
            interposers.execute(result.outcome, clientOutput);
            result.outcome.next();

            // Server step
            StatusWith<std::string> swServerResult =
                saslServerSession->step(opCtx.get(), clientOutput);
            result.status = swServerResult.getStatus();
            if (result.status != Status::OK()) {
                return result;
            }
            serverOutput = std::move(swServerResult.getValue());

            interposers.execute(result.outcome, serverOutput);
            result.outcome.next();
        }
        ASSERT_TRUE(saslClientSession->isSuccess());
        ASSERT_TRUE(saslServerSession->isSuccess());

        return result;
    }


    bool _digestPassword;

public:
    void run() {
        LOGV2(20252, "SCRAM-SHA-1 variant");
        saslServerSession = std::make_unique<SaslSCRAMSHA1ServerMechanism>("test");
        _digestPassword = true;
        Test::run();

        LOGV2(20253, "SCRAM-SHA-256 variant");
        saslServerSession = std::make_unique<SaslSCRAMSHA256ServerMechanism>("test");
        _digestPassword = false;
        Test::run();
    }
};

TEST_F(SCRAMFixture, testServerStep1DoesNotIncludeNonceFromClientStep1) {
    ASSERT_OK(authzBackend->insertUserDocument(
        opCtx.get(), generateSCRAMUserDocument("sajack", "sajack"), BSONObj()));

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
    ASSERT_EQ(
        SCRAMStepsResult(SaslTestState(SaslTestState::kClient, 2),
                         Status(ErrorCodes::BadValue, "Incorrect SCRAM client|server nonce: r=")),
        runSteps(mutator));
}

TEST_F(SCRAMFixture, testClientStep2DoesNotIncludeNonceFromServerStep1) {
    ASSERT_OK(authzBackend->insertUserDocument(
        opCtx.get(), generateSCRAMUserDocument("sajack", "sajack"), BSONObj()));

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
    ASSERT_EQ(
        SCRAMStepsResult(SaslTestState(SaslTestState::kServer, 2),
                         Status(ErrorCodes::BadValue, "Incorrect SCRAM client|server nonce: r=")),
        runSteps(mutator));
}

TEST_F(SCRAMFixture, testClientStep2GivesBadProof) {
    ASSERT_OK(authzBackend->insertUserDocument(
        opCtx.get(), generateSCRAMUserDocument("sajack", "sajack"), BSONObj()));

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
                                      "SCRAM authentication failed, storedKey mismatch")),

              runSteps(mutator));
}

TEST_F(SCRAMFixture, testServerStep2GivesBadVerifier) {
    ASSERT_OK(authzBackend->insertUserDocument(
        opCtx.get(), generateSCRAMUserDocument("sajack", "sajack"), BSONObj()));

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

    auto result = runSteps(mutator);

    ASSERT_EQ(SCRAMStepsResult(
                  SaslTestState(SaslTestState::kClient, 3),
                  Status(ErrorCodes::BadValue,
                         str::stream() << "Client failed to verify SCRAM ServerSignature, received "
                                       << encodedVerifier)),
              result);
}


TEST_F(SCRAMFixture, testSCRAM) {
    ASSERT_OK(authzBackend->insertUserDocument(
        opCtx.get(), generateSCRAMUserDocument("sajack", "sajack"), BSONObj()));

    saslClientSession->setParameter(NativeSaslClientSession::parameterUser, "sajack");
    saslClientSession->setParameter(NativeSaslClientSession::parameterPassword,
                                    createPasswordDigest("sajack", "sajack"));

    ASSERT_OK(saslClientSession->initialize());

    ASSERT_EQ(goalState, runSteps());
}

TEST_F(SCRAMFixture, testSCRAMWithChannelBindingSupportedByClient) {
    ASSERT_OK(authzBackend->insertUserDocument(
        opCtx.get(), generateSCRAMUserDocument("sajack", "sajack"), BSONObj()));

    saslClientSession->setParameter(NativeSaslClientSession::parameterUser, "sajack");
    saslClientSession->setParameter(NativeSaslClientSession::parameterPassword,
                                    createPasswordDigest("sajack", "sajack"));

    ASSERT_OK(saslClientSession->initialize());

    SCRAMMutators mutator;
    mutator.setMutator(SaslTestState(SaslTestState::kClient, 1), [](std::string& clientMessage) {
        clientMessage.replace(clientMessage.begin(), clientMessage.begin() + 1, "y");
    });

    ASSERT_EQ(goalState, runSteps(mutator));
}

TEST_F(SCRAMFixture, testSCRAMWithChannelBindingRequiredByClient) {
    ASSERT_OK(authzBackend->insertUserDocument(
        opCtx.get(), generateSCRAMUserDocument("sajack", "sajack"), BSONObj()));

    saslClientSession->setParameter(NativeSaslClientSession::parameterUser, "sajack");
    saslClientSession->setParameter(NativeSaslClientSession::parameterPassword,
                                    createPasswordDigest("sajack", "sajack"));

    ASSERT_OK(saslClientSession->initialize());

    SCRAMMutators mutator;
    mutator.setMutator(SaslTestState(SaslTestState::kClient, 1), [](std::string& clientMessage) {
        clientMessage.replace(clientMessage.begin(), clientMessage.begin() + 1, "p=tls-unique");
    });

    ASSERT_EQ(
        SCRAMStepsResult(SaslTestState(SaslTestState::kServer, 1),
                         Status(ErrorCodes::BadValue, "Server does not support channel binding")),
        runSteps(mutator));
}

TEST_F(SCRAMFixture, testSCRAMWithInvalidChannelBinding) {
    ASSERT_OK(authzBackend->insertUserDocument(
        opCtx.get(), generateSCRAMUserDocument("sajack", "sajack"), BSONObj()));

    saslClientSession->setParameter(NativeSaslClientSession::parameterUser, "sajack");
    saslClientSession->setParameter(NativeSaslClientSession::parameterPassword,
                                    createPasswordDigest("sajack", "sajack"));

    ASSERT_OK(saslClientSession->initialize());

    SCRAMMutators mutator;
    mutator.setMutator(SaslTestState(SaslTestState::kClient, 1), [](std::string& clientMessage) {
        clientMessage.replace(clientMessage.begin(), clientMessage.begin() + 1, "v=illegalGarbage");
    });

    ASSERT_EQ(SCRAMStepsResult(SaslTestState(SaslTestState::kServer, 1),
                               Status(ErrorCodes::BadValue,
                                      "Incorrect SCRAM client message prefix: v=illegalGarbage")),
              runSteps(mutator));
}

TEST_F(SCRAMFixture, testNULLInPassword) {
    ASSERT_OK(authzBackend->insertUserDocument(
        opCtx.get(), generateSCRAMUserDocument("sajack", "saj\0ack"), BSONObj()));

    saslClientSession->setParameter(NativeSaslClientSession::parameterUser, "sajack");
    saslClientSession->setParameter(NativeSaslClientSession::parameterPassword,
                                    createPasswordDigest("sajack", "saj\0ack"));

    ASSERT_OK(saslClientSession->initialize());

    ASSERT_EQ(goalState, runSteps());
}


TEST_F(SCRAMFixture, testCommasInUsernameAndPassword) {
    ASSERT_OK(authzBackend->insertUserDocument(
        opCtx.get(), generateSCRAMUserDocument("s,a,jack", "s,a,jack"), BSONObj()));

    saslClientSession->setParameter(NativeSaslClientSession::parameterUser, "s,a,jack");
    saslClientSession->setParameter(NativeSaslClientSession::parameterPassword,
                                    createPasswordDigest("s,a,jack", "s,a,jack"));

    ASSERT_OK(saslClientSession->initialize());

    ASSERT_EQ(goalState, runSteps());
}

TEST_F(SCRAMFixture, testIncorrectUser) {
    saslClientSession->setParameter(NativeSaslClientSession::parameterUser, "sajack");
    saslClientSession->setParameter(NativeSaslClientSession::parameterPassword,
                                    createPasswordDigest("sajack", "sajack"));

    ASSERT_OK(saslClientSession->initialize());

    ASSERT_EQ(SCRAMStepsResult(SaslTestState(SaslTestState::kServer, 1),
                               Status(ErrorCodes::UserNotFound,
                                      "Could not find user \"sajack\" for db \"test\"")),
              runSteps());
}

TEST_F(SCRAMFixture, testIncorrectPassword) {
    ASSERT_OK(authzBackend->insertUserDocument(
        opCtx.get(), generateSCRAMUserDocument("sajack", "sajack"), BSONObj()));

    saslClientSession->setParameter(NativeSaslClientSession::parameterUser, "sajack");
    saslClientSession->setParameter(NativeSaslClientSession::parameterPassword,
                                    createPasswordDigest("sajack", "invalidPassword"));

    ASSERT_OK(saslClientSession->initialize());

    ASSERT_EQ(SCRAMStepsResult(SaslTestState(SaslTestState::kServer, 2),
                               Status(ErrorCodes::AuthenticationFailed,
                                      "SCRAM authentication failed, storedKey mismatch")),
              runSteps());
}

TEST_F(SCRAMFixture, testOptionalClientExtensions) {
    // Verify server ignores unknown/optional extensions sent by client.
    ASSERT_OK(authzBackend->insertUserDocument(
        opCtx.get(), generateSCRAMUserDocument("sajack", "sajack"), BSONObj()));

    saslClientSession->setParameter(NativeSaslClientSession::parameterUser, "sajack");
    saslClientSession->setParameter(NativeSaslClientSession::parameterPassword,
                                    createPasswordDigest("sajack", "sajack"));

    ASSERT_OK(saslClientSession->initialize());

    SCRAMMutators mutator;
    mutator.setMutator(SaslTestState(SaslTestState::kClient, 1), [](std::string& clientMessage) {
        clientMessage += ",x=unsupported-extension";
    });

    // Optional client extension is successfully ignored, or we'd have failed in step 1.
    // We still fail at step 2, because client was unaware of the injected extension.
    ASSERT_EQ(SCRAMStepsResult(SaslTestState(SaslTestState::kServer, 2),
                               Status(ErrorCodes::AuthenticationFailed,
                                      "SCRAM authentication failed, storedKey mismatch")),
              runSteps(mutator));
}

TEST_F(SCRAMFixture, testOptionalServerExtensions) {
    // Verify client errors on unknown/optional extensions sent by server.
    ASSERT_OK(authzBackend->insertUserDocument(
        opCtx.get(), generateSCRAMUserDocument("sajack", "sajack"), BSONObj()));

    saslClientSession->setParameter(NativeSaslClientSession::parameterUser, "sajack");
    saslClientSession->setParameter(NativeSaslClientSession::parameterPassword,
                                    createPasswordDigest("sajack", "sajack"));

    ASSERT_OK(saslClientSession->initialize());

    SCRAMMutators mutator;
    mutator.setMutator(SaslTestState(SaslTestState::kServer, 1), [](std::string& serverMessage) {
        serverMessage += ",x=unsupported-extension";
    });

    // As with testOptionalClientExtensions, we can be confident that the optionality
    // is respected because we would have failed at client step 2.
    // We do still fail at server step 2 because server was unaware of injected extension.
    ASSERT_EQ(SCRAMStepsResult(SaslTestState(SaslTestState::kServer, 2),
                               Status(ErrorCodes::AuthenticationFailed,
                                      "SCRAM authentication failed, storedKey mismatch")),
              runSteps(mutator));
}

template <typename HashBlock>
void assertCacheStats(const SCRAMClientCache<HashBlock>& cache,
                      int64_t hits,
                      int64_t misses,
                      int64_t count) {
    auto stats = cache.getStats();
    ASSERT_EQ(stats.hits, hits);
    ASSERT_EQ(stats.misses, misses);
    ASSERT_EQ(stats.count, count);
}

template <typename HashBlock>
void runTestClientConversationUsesCacheTest(SaslClientSession* saslClientSession) {
    const auto salt = scram::Presecrets<SHA256Block>::generateSecureRandomSalt();

    saslClientSession->setParameter(NativeSaslClientSession::parameterUser, "sajack");
    saslClientSession->setParameter(NativeSaslClientSession::parameterPassword,
                                    createPasswordDigest("sajack", "sajack"));

    ASSERT_OK(saslClientSession->initialize());

    SCRAMClientCache<SHA256Block> cache;
    SaslSCRAMClientConversationImpl<SHA256Block> conv(saslClientSession, &cache);

    // The first time we generate a client proof, the cache is empty; we miss once and then insert
    // the presecrets + secrets into the cache.
    conv.generateClientProof(salt, 5001);
    assertCacheStats(cache, 0, 1, 1);

    // When we generate again with the same presecrets, we hit the cache.
    conv.generateClientProof(salt, 5001);
    assertCacheStats(cache, 1, 1, 1);
    conv.generateClientProof(salt, 5001);
    assertCacheStats(cache, 2, 1, 1);

    // When we generate with different presecrets, we will miss due to stale cached secret and
    // overwrite with new presecrets + secrets (note that this doesn't increase count)
    conv.generateClientProof(salt, 5002);
    assertCacheStats(cache, 2, 2, 1);

    // Now we expect the new presecrets/secrets to be cached; should hit, and the old one should
    // miss and overwrite.
    conv.generateClientProof(salt, 5002);
    assertCacheStats(cache, 3, 2, 1);
    conv.generateClientProof(salt, 5001);
    assertCacheStats(cache, 3, 3, 1);

    // With a new host, we will get another miss and a new entry upon proof generation.
    saslClientSession->setParameter(NativeSaslClientSession::parameterServiceHostAndPort,
                                    "MockServer.test:27018");
    conv.generateClientProof(salt, 5001);
    assertCacheStats(cache, 3, 4, 2);
    conv.generateClientProof(salt, 5001);
    assertCacheStats(cache, 4, 4, 2);

    // The old host is still cached.
    saslClientSession->setParameter(NativeSaslClientSession::parameterServiceHostAndPort,
                                    "MockServer.test:27017");
    conv.generateClientProof(salt, 5001);
    assertCacheStats(cache, 5, 4, 2);
}

TEST_F(SCRAMFixture, testClientConversationUsesCacheSHA1) {
    runTestClientConversationUsesCacheTest<SHA1Block>(saslClientSession.get());
}

TEST_F(SCRAMFixture, testClientConversationUsesCacheSHA256) {
    runTestClientConversationUsesCacheTest<SHA256Block>(saslClientSession.get());
}


template <typename HashBlock>
void testGetFromEmptyCache() {
    SCRAMClientCache<HashBlock> cache;
    assertCacheStats(cache, 0, 0, 0);
    const auto salt = scram::Presecrets<HashBlock>::generateSecureRandomSalt();
    HostAndPort host("localhost:27017");
    ASSERT_FALSE(cache.getCachedSecrets(host, scram::Presecrets<HashBlock>("aaa", salt, 10000)));
    assertCacheStats(cache, 0, 1, 0);
}

TEST(SCRAMCache, testGetFromEmptyCache) {
    testGetFromEmptyCache<SHA1Block>();
    testGetFromEmptyCache<SHA256Block>();
}

template <typename HashBlock>
void testSetAndGet() {
    SCRAMClientCache<HashBlock> cache;
    const auto salt = scram::Presecrets<HashBlock>::generateSecureRandomSalt();
    HostAndPort host("localhost:27017");

    const auto presecrets = scram::Presecrets<HashBlock>("aaa", salt, 10000);
    const auto secrets = scram::Secrets<HashBlock>(presecrets);
    cache.setCachedSecrets(host, presecrets, secrets);
    assertCacheStats(cache, 0, 0, 1);
    const auto cachedSecrets = cache.getCachedSecrets(host, presecrets);
    assertCacheStats(cache, 1, 0, 1);

    ASSERT_TRUE(cachedSecrets);
    ASSERT_TRUE(secrets.clientKey() == cachedSecrets.clientKey());
    ASSERT_TRUE(secrets.serverKey() == cachedSecrets.serverKey());
    ASSERT_TRUE(secrets.storedKey() == cachedSecrets.storedKey());
}

TEST(SCRAMCache, testSetAndGet) {
    testSetAndGet<SHA1Block>();
    testSetAndGet<SHA256Block>();
}

template <typename HashBlock>
void testSetAndGetWithDifferentParameters() {
    SCRAMClientCache<HashBlock> cache;
    const auto salt = scram::Presecrets<HashBlock>::generateSecureRandomSalt();
    HostAndPort host("localhost:27017");

    const auto presecrets = scram::Presecrets<HashBlock>("aaa", salt, 10000);
    const auto secrets = scram::Secrets<HashBlock>(presecrets);
    cache.setCachedSecrets(host, presecrets, secrets);
    assertCacheStats(cache, 0, 0, 1);
    ASSERT_TRUE(cache.getCachedSecrets(host, presecrets));
    assertCacheStats(cache, 1, 0, 1);

    // Alter each of: host, password, salt, iterationCount.
    // Any one of which should fail to retreive from cache.
    ASSERT_FALSE(cache.getCachedSecrets(HostAndPort("localhost:27018"), presecrets));
    assertCacheStats(cache, 1, 1, 1);
    ASSERT_FALSE(cache.getCachedSecrets(host, scram::Presecrets<HashBlock>("aab", salt, 10000)));
    assertCacheStats(cache, 1, 2, 1);
    const auto badSalt = scram::Presecrets<HashBlock>::generateSecureRandomSalt();
    ASSERT_FALSE(cache.getCachedSecrets(host, scram::Presecrets<HashBlock>("aaa", badSalt, 10000)));
    assertCacheStats(cache, 1, 3, 1);
    ASSERT_FALSE(cache.getCachedSecrets(host, scram::Presecrets<HashBlock>("aaa", salt, 10001)));
    assertCacheStats(cache, 1, 4, 1);
}

TEST(SCRAMCache, testSetAndGetWithDifferentParameters) {
    testSetAndGetWithDifferentParameters<SHA1Block>();
    testSetAndGetWithDifferentParameters<SHA256Block>();
}

template <typename HashBlock>
void testSetAndReset() {
    SCRAMClientCache<HashBlock> cache;
    const auto salt = scram::Presecrets<HashBlock>::generateSecureRandomSalt();
    HostAndPort host("localhost:27017");

    const auto presecretsA = scram::Presecrets<HashBlock>("aaa", salt, 10000);
    const auto secretsA = scram::Secrets<HashBlock>(presecretsA);
    cache.setCachedSecrets(host, presecretsA, secretsA);
    assertCacheStats(cache, 0, 0, 1);
    const auto presecretsB = scram::Presecrets<HashBlock>("aab", salt, 10000);
    const auto secretsB = scram::Secrets<HashBlock>(presecretsB);
    cache.setCachedSecrets(host, presecretsB, secretsB);
    assertCacheStats(cache, 0, 0, 1);

    ASSERT_FALSE(cache.getCachedSecrets(host, presecretsA));
    assertCacheStats(cache, 0, 1, 1);
    const auto cachedSecret = cache.getCachedSecrets(host, presecretsB);
    ASSERT_TRUE(cachedSecret);
    assertCacheStats(cache, 1, 1, 1);
    ASSERT_TRUE(secretsB.clientKey() == cachedSecret.clientKey());
    ASSERT_TRUE(secretsB.serverKey() == cachedSecret.serverKey());
    ASSERT_TRUE(secretsB.storedKey() == cachedSecret.storedKey());
}

TEST(SCRAMCache, testSetAndReset) {
    testSetAndReset<SHA1Block>();
    testSetAndReset<SHA256Block>();
}

}  // namespace
}  // namespace mongo
