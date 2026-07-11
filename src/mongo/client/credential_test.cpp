// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/client/credential.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bson_matcher.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/auth_mechanism.h"
#include "mongo/unittest/unittest.h"

#include <string_view>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace auth {
namespace {

struct ParseSuccessCase {
    std::string_view name;
    BSONObj params;
    Credential expected;
};

struct ParseFailureCase {
    std::string_view name;
    BSONObj params;
    boost::optional<ErrorCodes::Error> expectedCode;
};

void assertCredentialEq(std::string_view name,
                        const Credential& actual,
                        const Credential& expected) {
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
        std::string_view name;
        std::string_view mechanism;
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

