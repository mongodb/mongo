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

#include "mongo/client/authenticate.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/internal_auth.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/client/sasl_client_session.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/sasl_command_constants.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/md5.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/password_digest.h"

#include <memory>
#include <queue>
#include <string_view>
#include <utility>

#include <boost/move/utility_core.hpp>

namespace {
using namespace std::literals::string_view_literals;

using namespace mongo;

/**
 * Utility class to support tests in this file.  Allows caller to load
 * with pre-made responses and requests to interject into authentication methods.
 */
class AuthClientTest : public mongo::unittest::Test {
public:
    AuthClientTest()
        : _mockHost(),
          _millis(100),
          _username("PinkPanther"),
          _password("shhhhhhh"),
          _password_digest(createPasswordDigest(_username, _password)),
          _nonce("7ca422a24f326f2a"),
          _requests(),
          _responses() {
        _runCommandCallback = [this](OpMsgRequest request) {
            return runCommand(std::move(request));
        };

        // create our digest
        md5digest d;
        {
            md5_state_t st;
            md5_init_state_deprecated(&st);
            md5_append_deprecated(&st, (const md5_byte_t*)_nonce.c_str(), _nonce.size());
            md5_append_deprecated(&st, (const md5_byte_t*)_username.c_str(), _username.size());
            md5_append_deprecated(
                &st, (const md5_byte_t*)_password_digest.c_str(), _password_digest.size());
            md5_finish_deprecated(&st, d);
        }
        _digest = digestToString(d);
    }

    // protected:
    Future<BSONObj> runCommand(OpMsgRequest request) {
        // Validate the received request
        ASSERT(!_requests.empty());
        auto& expected = _requests.front();
        ASSERT_EQ(expected.parseDbName(), request.parseDbName());
        ASSERT_BSONOBJ_EQ(expected.body, request.body);
        _requests.pop();

        // Then pop a response and call the handler
        ASSERT(!_responses.empty());
        auto ret = _responses.front();
        _responses.pop();
        return ret;
    }

    void reset() {
        // If there are things left then we did something wrong.
        ASSERT(_responses.empty());
        ASSERT(_requests.empty());
    }

    void pushResponse(const BSONObj& cmd) {
        _responses.emplace(cmd);
    }

    void pushRequest(const DatabaseName& dbname, const BSONObj& cmd) {
        _requests.emplace(
            OpMsgRequestBuilder::create(auth::ValidatedTenancyScope::kNotRequired, dbname, cmd));
    }

    BSONObj loadX509Conversation() {
        // 1. Client sends 'authenticate' command
        pushRequest(DatabaseName::kExternal,
                    BSON("authenticate" << 1 << "mechanism"
                                        << "MONGODB-X509"
                                        << "user" << _username));

        // 2. Client receives 'ok'
        pushResponse(BSON("ok" << 1));

        // Call clientAuthenticate()
        return BSON("mechanism" << "MONGODB-X509"
                                << "db"
                                << "$external"
                                << "user" << _username);
    }


    auth::RunCommandHook _runCommandCallback;

    // Auth code doesn't use HostAndPort information.
    HostAndPort _mockHost;
    Milliseconds _millis;

    // Some credentials
    std::string _username;
    std::string _password;
    std::string _password_digest;
    std::string _digest;
    std::string _nonce;

    std::queue<OpMsgRequest> _requests;
    std::queue<BSONObj> _responses;
};

#ifdef MONGO_CONFIG_SSL
TEST_F(AuthClientTest, X509) {
    auto params = loadX509Conversation();
    auth::authenticateClient(uassertStatusOK(auth::Credential::fromBSON(params)),
                             HostAndPort(),
                             _username,
                             _runCommandCallback)
        .get();
}

TEST_F(AuthClientTest, asyncX509) {
    auto params = loadX509Conversation();
    ASSERT_OK(auth::authenticateClient(uassertStatusOK(auth::Credential::fromBSON(params)),
                                       HostAndPort(),
                                       _username,
                                       _runCommandCallback)
                  .getNoThrow());
}
#endif

// Returns the "speculativeAuthenticate" sub-object from a "hello" request body built by one of the
// speculative auth helpers.
BSONObj getSpeculativeAuthenticate(const BSONObj& helloRequest) {
    auto elem = helloRequest[auth::kSpeculativeAuthenticate];
    ASSERT_EQ(elem.type(), BSONType::object)
        << "missing speculativeAuthenticate field in " << helloRequest;
    return elem.Obj().getOwned();
}

// Asserts that a saslStart payload produced by the speculative SASL path requests the
// skipEmptyExchange option. This is the behavior added in SERVER-126148 and is what allows
// speculative SCRAM auth to complete in two steps rather than three.
void assertSaslStartSkipsEmptyExchange(const BSONObj& specAuth,
                                       std::string_view expectedMechanism) {
    ASSERT_EQ(specAuth["saslStart"].numberInt(), 1) << specAuth;
    ASSERT_EQ(specAuth["mechanism"].str(), expectedMechanism) << specAuth;

    auto optionsElem = specAuth["options"];
    ASSERT_EQ(optionsElem.type(), BSONType::object)
        << "speculative saslStart is missing the options field: " << specAuth;
    auto options = optionsElem.Obj();
    auto skipElem = options[saslCommandOptionSkipEmptyExchange];
    ASSERT_EQ(skipElem.type(), BSONType::boolean)
        << "speculative saslStart options missing skipEmptyExchange: " << specAuth;
    ASSERT_TRUE(skipElem.boolean()) << specAuth;
}

TEST_F(AuthClientTest, SpeculateAuthScramRequestsSkipEmptyExchange) {
    auto swURI = MongoURI::parse("mongodb://a:b@localhost:27017/admin?authMechanism=SCRAM-SHA-256");
    ASSERT_OK(swURI.getStatus());

    BSONObjBuilder helloRequestBuilder;
    std::shared_ptr<SaslClientSession> session;
    auto specType = auth::speculateAuth(&helloRequestBuilder, swURI.getValue(), &session);

    ASSERT_TRUE(specType == auth::SpeculativeAuthType::kSaslStart);
    ASSERT(session);

    auto specAuth = getSpeculativeAuthenticate(helloRequestBuilder.obj());
    assertSaslStartSkipsEmptyExchange(specAuth, auth::kMechanismScramSha256);
    ASSERT_EQ(specAuth["db"].str(), "admin") << specAuth;
}

TEST_F(AuthClientTest, SpeculateAuthWithoutCredentialsReturnsNone) {
    // No username/password supplied, so there is nothing to speculate with.
    auto swURI = MongoURI::parse("mongodb://localhost:27017/admin?authMechanism=SCRAM-SHA-256");
    ASSERT_OK(swURI.getStatus());

    BSONObjBuilder helloRequestBuilder;
    std::shared_ptr<SaslClientSession> session;
    auto specType = auth::speculateAuth(&helloRequestBuilder, swURI.getValue(), &session);

    ASSERT_TRUE(specType == auth::SpeculativeAuthType::kNone);
    ASSERT_FALSE(session);
    ASSERT_FALSE(helloRequestBuilder.obj().hasField(auth::kSpeculativeAuthenticate));
}

#ifdef MONGO_CONFIG_SSL
TEST_F(AuthClientTest, SpeculateAuthX509UsesAuthenticate) {
    auto swURI = MongoURI::parse("mongodb://localhost:27017/?authMechanism=MONGODB-X509");
    ASSERT_OK(swURI.getStatus());

    BSONObjBuilder helloRequestBuilder;
    std::shared_ptr<SaslClientSession> session;
    auto specType = auth::speculateAuth(&helloRequestBuilder, swURI.getValue(), &session);

    ASSERT_TRUE(specType == auth::SpeculativeAuthType::kAuthenticate);
    // The X509 path does not run a SASL conversation.
    ASSERT_FALSE(session);

    auto specAuth = getSpeculativeAuthenticate(helloRequestBuilder.obj());
    ASSERT_EQ(specAuth[saslCommandMechanismFieldName].str(), auth::kMechanismMongoX509) << specAuth;
    ASSERT_EQ(specAuth[saslCommandUserDBFieldName].str(), "$external") << specAuth;
    // The authenticate path must not emit a saslStart payload or skipEmptyExchange option.
    ASSERT_FALSE(specAuth.hasField("saslStart")) << specAuth;
    ASSERT_FALSE(specAuth.hasField("options")) << specAuth;
}
#endif

class SpeculativeInternalAuthTest : public AuthClientTest {
public:
    void setUp() override {
        // speculateInternalAuth() reads the __system user's name to build SASL parameters.
        std::unique_ptr<UserRequest> systemLocal =
            std::make_unique<UserRequestGeneral>(UserName("__system"sv, "local"sv), boost::none);
        internalSecurity.setUser(std::make_shared<UserHandle>(User(std::move(systemLocal))));
    }
};

TEST_F(SpeculativeInternalAuthTest, SpeculateInternalAuthScramRequestsSkipEmptyExchange) {
    auth::setInternalAuthKeys({"hunter2"});

    // saslConfigureSession dasserts that the hostname is non-empty (SERVER-59876), so use a real
    // host rather than the default-constructed _mockHost.
    BSONObjBuilder helloRequestBuilder;
    std::shared_ptr<SaslClientSession> session;
    auto specType = auth::speculateInternalAuth(
        HostAndPort("localhost", 27017), &helloRequestBuilder, &session);

    ASSERT_TRUE(specType == auth::SpeculativeAuthType::kSaslStart);
    ASSERT(session);

    auto specAuth = getSpeculativeAuthenticate(helloRequestBuilder.obj());
    assertSaslStartSkipsEmptyExchange(specAuth, auth::kMechanismScramSha256);
    ASSERT_EQ(specAuth["db"].str(), "local") << specAuth;
}

}  // namespace
