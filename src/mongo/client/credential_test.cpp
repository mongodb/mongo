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

#include "mongo/client/credential.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bson_matcher.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/auth_mechanism.h"
#include "mongo/unittest/unittest.h"

#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace auth {
namespace {

struct ParseSuccessCase {
    StringData name;
    BSONObj params;
    Credential expected;
};

struct ParseFailureCase {
    StringData name;
    BSONObj params;
    boost::optional<ErrorCodes::Error> expectedCode;
};

void assertCredentialEq(StringData name, const Credential& actual, const Credential& expected) {
    ASSERT_EQ(actual.mechanism, expected.mechanism) << name;
    ASSERT_EQ(actual.db, expected.db) << name << ": db";
    ASSERT_EQ(actual.username, expected.username) << name << ": username";
    ASSERT_EQ(actual.password, expected.password) << name << ": password";
    ASSERT_BSONOBJ_EQ_UNORDERED(actual.mechanismProperties, expected.mechanismProperties);
}

TEST(CredentialTest, ParseSuccessCases) {
    const std::vector<ParseSuccessCase> cases = {
        {.name = "ParseValidScramSha256Credential",
         .params = BSON("mechanism" << "SCRAM-SHA-256"
                                    << "user"
                                    << "testuser"
                                    << "db"
                                    << "admin"
                                    << "pwd"
                                    << "secret"),
         .expected = {.mechanism = AuthMechanism::kScramSha256,
                      .db = std::string{"admin"},
                      .username = std::string{"testuser"},
                      .password = std::string{"secret"}}},
        {.name = "ParseX509Credential",
         .params = BSON("mechanism" << "MONGODB-X509"
                                    << "db"
                                    << "$external"
                                    << "user"
                                    << "CN=test"),
         .expected = {.mechanism = AuthMechanism::kMongoX509,
                      .db = std::string{"$external"},
                      .username = std::string{"CN=test"}}},
        {.name = "ParseLegacyUserSourceField",
         .params = BSON("mechanism" << "SCRAM-SHA-1"
                                    << "user"
                                    << "testuser"
                                    << "userSource"
                                    << "admin"
                                    << "pwd"
                                    << "secret"),
         .expected = {.mechanism = AuthMechanism::kScramSha1,
                      .db = std::string{"admin"},
                      .username = std::string{"testuser"},
                      .password = std::string{"secret"}}},
        {.name = "ParseMechanismProperties",
         .params = BSON("mechanism" << "GSSAPI"
                                    << "user"
                                    << "testuser"
                                    << "db"
                                    << "$external"
                                    << "serviceName"
                                    << "mongodb"
                                    << "serviceHostname"
                                    << "localhost"),
         .expected = {.mechanism = AuthMechanism::kGSSAPI,
                      .db = std::string{"$external"},
                      .username = std::string{"testuser"},
                      .mechanismProperties = BSON("serviceName" << "mongodb"
                                                                << "serviceHostname"
                                                                << "localhost")}},
        {.name = "ParseMinimalCredential",
         .params = BSON("mechanism" << "MONGODB-AWS"),
         .expected = {.mechanism = AuthMechanism::kMongoAWS}},
        {.name = "ParseEmptyStringFields",
         .params = BSON("mechanism" << "SCRAM-SHA-256"
                                    << "user"
                                    << ""
                                    << "pwd"
                                    << ""
                                    << "db"
                                    << ""),
         .expected = {.mechanism = AuthMechanism::kScramSha256}},
        {.name = "DatabaseFieldPrecedence_UseDbWhenPresent",
         .params = BSON("mechanism" << "SCRAM-SHA-256"
                                    << "user"
                                    << "testuser"
                                    << "db"
                                    << "mydb"
                                    << "pwd"
                                    << "secret"),
         .expected = {.mechanism = AuthMechanism::kScramSha256,
                      .db = std::string{"mydb"},
                      .username = std::string{"testuser"},
                      .password = std::string{"secret"}}},
        {.name = "DatabaseFieldPrecedence_UseUserSourceWhenDbMissing",
         .params = BSON("mechanism" << "SCRAM-SHA-256"
                                    << "user"
                                    << "testuser"
                                    << "userSource"
                                    << "sourcedb"
                                    << "pwd"
                                    << "secret"),
         .expected = {.mechanism = AuthMechanism::kScramSha256,
                      .db = std::string{"sourcedb"},
                      .username = std::string{"testuser"},
                      .password = std::string{"secret"}}},
        {.name = "DatabaseFieldPrecedence_NoDbOrUserSource",
         .params = BSON("mechanism" << "SCRAM-SHA-256"
                                    << "user"
                                    << "testuser"
                                    << "pwd"
                                    << "secret"),
         .expected = {.mechanism = AuthMechanism::kScramSha256,
                      .username = std::string{"testuser"},
                      .password = std::string{"secret"}}},
        {.name = "EmptyDatabaseField",
         .params = BSON("mechanism" << "SCRAM-SHA-256"
                                    << "user"
                                    << "testuser"
                                    << "db"
                                    << ""
                                    << "pwd"
                                    << "secret"),
         .expected = {.mechanism = AuthMechanism::kScramSha256,
                      .username = std::string{"testuser"},
                      .password = std::string{"secret"}}},
        {.name = "SpecialCharactersInDatabase",
         .params = BSON("mechanism" << "SCRAM-SHA-256"
                                    << "user"
                                    << "testuser"
                                    << "db"
                                    << "my@db"
                                    << "pwd"
                                    << "secret"),
         .expected = {.mechanism = AuthMechanism::kScramSha256,
                      .db = std::string{"my@db"},
                      .username = std::string{"testuser"},
                      .password = std::string{"secret"}}},
        {.name = "ExternalDatabaseForX509",
         .params = BSON("mechanism" << "MONGODB-X509"
                                    << "user"
                                    << "CN=test"
                                    << "db"
                                    << "$external"),
         .expected = {.mechanism = AuthMechanism::kMongoX509,
                      .db = std::string{"$external"},
                      .username = std::string{"CN=test"}}},
        {.name = "ParsePlainCredential",
         .params = BSON("mechanism" << "PLAIN"
                                    << "user"
                                    << "testuser"
                                    << "db"
                                    << "$external"
                                    << "pwd"
                                    << "secret"),
         .expected = {.mechanism = AuthMechanism::kSaslPlain,
                      .db = std::string{"$external"},
                      .username = std::string{"testuser"},
                      .password = std::string{"secret"}}},
        {.name = "ParseOidcCredential",
         .params = BSON("mechanism" << "MONGODB-OIDC"
                                    << "db"
                                    << "$external"
                                    << "user"
                                    << "testuser"),
         .expected = {.mechanism = AuthMechanism::kMongoOIDC,
                      .db = std::string{"$external"},
                      .username = std::string{"testuser"}}},
        {.name = "ParseMongoCrCredential",
         .params = BSON("mechanism" << "MONGODB-CR"
                                    << "user"
                                    << "testuser"
                                    << "db"
                                    << "admin"
                                    << "pwd"
                                    << "secret"),
         .expected = {.mechanism = AuthMechanism::kMongoDbCr,
                      .db = std::string{"admin"},
                      .username = std::string{"testuser"},
                      .password = std::string{"secret"}}},
    };

    for (const auto& tc : cases) {
        auto swCred = Credential::fromBSON(tc.params);
        ASSERT_OK(swCred.getStatus()) << tc.name;
        assertCredentialEq(tc.name, swCred.getValue(), tc.expected);
    }
}

TEST(CredentialTest, ParseFailureCases) {
    const std::vector<ParseFailureCase> cases = {
        {.name = "RejectBothDbAndUserSource",
         .params = BSON("mechanism" << "SCRAM-SHA-256"
                                    << "user"
                                    << "testuser"
                                    << "db"
                                    << "test"
                                    << "userSource"
                                    << "admin"
                                    << "pwd"
                                    << "secret"),
         .expectedCode = ErrorCodes::AuthenticationFailed},
        {.name = "RejectMissingMechanism",
         .params = BSON("user" << "testuser"
                               << "pwd"
                               << "secret")},
        {.name = "RejectInvalidMechanism",
         .params = BSON("mechanism" << "INVALID-MECHANISM"
                                    << "user"
                                    << "testuser"),
         .expectedCode = ErrorCodes::InvalidOptions},
        {.name = "RejectEmptyMechanismString",
         .params = BSON("mechanism" << ""
                                    << "user"
                                    << "testuser"),
         .expectedCode = ErrorCodes::InvalidOptions},
        {.name = "RejectNonStringMechanism",
         .params = BSON("mechanism" << 42 << "user"
                                    << "testuser"),
         .expectedCode = ErrorCodes::TypeMismatch},
        {.name = "RejectNonStringDbField",
         .params = BSON("mechanism" << "SCRAM-SHA-256"
                                    << "db" << 42),
         .expectedCode = ErrorCodes::TypeMismatch},
        {.name = "RejectNonStringUserSourceField",
         .params = BSON("mechanism" << "SCRAM-SHA-256"
                                    << "userSource" << 42),
         .expectedCode = ErrorCodes::TypeMismatch},
        {.name = "RejectNonStringUserField",
         .params = BSON("mechanism" << "SCRAM-SHA-256"
                                    << "user" << 42),
         .expectedCode = ErrorCodes::TypeMismatch},
        {.name = "RejectNonStringPasswordField",
         .params = BSON("mechanism" << "SCRAM-SHA-256"
                                    << "pwd" << 42),
         .expectedCode = ErrorCodes::TypeMismatch},
    };

    for (const auto& tc : cases) {
        auto swCred = Credential::fromBSON(tc.params);
        ASSERT_NOT_OK(swCred.getStatus()) << tc.name;
        if (tc.expectedCode.has_value()) {
            ASSERT_EQ(swCred.getStatus().code(), *tc.expectedCode) << tc.name;
        }
    }
}

TEST(CredentialTest, ParseMechanismMappingCases) {
    struct TestCase {
        StringData name;
        StringData mechanism;
        AuthMechanism expected;
    };

    const std::vector<TestCase> cases = {
        {"ScramSha1", "SCRAM-SHA-1", AuthMechanism::kScramSha1},
        {"ScramSha256", "SCRAM-SHA-256", AuthMechanism::kScramSha256},
        {"MongoX509", "MONGODB-X509", AuthMechanism::kMongoX509},
        {"Plain", "PLAIN", AuthMechanism::kSaslPlain},
        {"Gssapi", "GSSAPI", AuthMechanism::kGSSAPI},
        {"MongoAws", "MONGODB-AWS", AuthMechanism::kMongoAWS},
        {"MongoOidc", "MONGODB-OIDC", AuthMechanism::kMongoOIDC},
        {"MongoCr", "MONGODB-CR", AuthMechanism::kMongoDbCr},
    };

    for (const auto& tc : cases) {
        auto swCred = Credential::fromBSON(BSON("mechanism" << tc.mechanism));
        ASSERT_OK(swCred.getStatus()) << tc.name;
        ASSERT_EQ(swCred.getValue().mechanism, tc.expected) << tc.name;
    }
}

}  // namespace
}  // namespace auth
}  // namespace mongo

