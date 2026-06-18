/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
