// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/credential.h"
#include "mongo/client/sasl_client_authenticate.h"
#include "mongo/client/sasl_client_session.h"
#include "mongo/db/auth/auth_mechanism.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/hostandport.h"

#include <string>
#include <string_view>

namespace mongo {
namespace {

class StubSaslClientSession : public SaslClientSession {
public:
    Status initialize() override {
        return Status::OK();
    }
    Status step(std::string_view, std::string*) override {
        return Status::OK();
    }
    bool isSuccess() const override {
        return true;
    }
};

const HostAndPort kHost{"localhost", 27017};

TEST(SaslConfigureSessionTest, DigestPasswordWrongTypeReturnsError) {
    StubSaslClientSession session;
    auth::Credential cred;
    cred.mechanism = auth::AuthMechanism::kScramSha1;
    cred.username = std::string{"alice"};
    cred.password = std::string{"secret"};
    cred.mechanismProperties = BSON("digestPassword" << "notabool");

    auto status = saslConfigureSession(&session, kHost, cred);

    ASSERT_NOT_OK(status);
    ASSERT_EQ(status.code(), ErrorCodes::TypeMismatch);
}

TEST(SaslConfigureSessionTest, MissingPasswordForNonExternalUserReturnsAuthenticationFailed) {
    StubSaslClientSession session;
    auth::Credential cred;
    cred.mechanism = auth::AuthMechanism::kScramSha256;
    cred.username = std::string{"alice"};
    cred.db = std::string{"admin"};

    auto status = saslConfigureSession(&session, kHost, cred);

    ASSERT_NOT_OK(status);
    ASSERT_EQ(status.code(), ErrorCodes::AuthenticationFailed);
}

}  // namespace
}  // namespace mongo
