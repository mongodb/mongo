/*    Copyright 2012 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

/**
 * Unit tests of the UserDocumentParser type.
 */

#include "mongo/platform/basic.h"

#include "mongo/base/status.h"
#include "mongo/bson/oid.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/user_document_parser.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/server_options.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/sock.h"

#define ASSERT_NULL(EXPR) ASSERT_FALSE(EXPR)
#define ASSERT_NON_NULL(EXPR) ASSERT_TRUE(EXPR)

namespace mongo {
namespace {

using std::unique_ptr;

class V1UserDocumentParsing : public ::mongo::unittest::Test {
public:
    V1UserDocumentParsing() {}

    unique_ptr<User> user;
    unique_ptr<User> adminUser;
    V1UserDocumentParser v1parser;

    void setUp() {
        resetUsers();
    }

    void resetUsers() {
        user.reset(new User(UserName("spencer", "test")));
        adminUser.reset(new User(UserName("admin", "admin")));
    }
};

TEST_F(V1UserDocumentParsing, testParsingV0UserDocuments) {
    BSONObj readWrite = BSON("user"
                             << "spencer"
                             << "pwd"
                             << "passwordHash");
    BSONObj readOnly = BSON("user"
                            << "spencer"
                            << "pwd"
                            << "passwordHash"
                            << "readOnly"
                            << true);
    BSONObj readWriteAdmin = BSON("user"
                                  << "admin"
                                  << "pwd"
                                  << "passwordHash");
    BSONObj readOnlyAdmin = BSON("user"
                                 << "admin"
                                 << "pwd"
                                 << "passwordHash"
                                 << "readOnly"
                                 << true);

    ASSERT_OK(v1parser.initializeUserRolesFromUserDocument(user.get(), readOnly, "test"));
    RoleNameIterator roles = user->getRoles();
    ASSERT_EQUALS(RoleName("read", "test"), roles.next());
    ASSERT_FALSE(roles.more());

    resetUsers();
    ASSERT_OK(v1parser.initializeUserRolesFromUserDocument(user.get(), readWrite, "test"));
    roles = user->getRoles();
    ASSERT_EQUALS(RoleName("dbOwner", "test"), roles.next());
    ASSERT_FALSE(roles.more());

    resetUsers();
    ASSERT_OK(
        v1parser.initializeUserRolesFromUserDocument(adminUser.get(), readOnlyAdmin, "admin"));
    roles = adminUser->getRoles();
    ASSERT_EQUALS(RoleName("readAnyDatabase", "admin"), roles.next());
    ASSERT_FALSE(roles.more());

    resetUsers();
    ASSERT_OK(
        v1parser.initializeUserRolesFromUserDocument(adminUser.get(), readWriteAdmin, "admin"));
    roles = adminUser->getRoles();
    ASSERT_EQUALS(RoleName("root", "admin"), roles.next());
    ASSERT_FALSE(roles.more());
}

TEST_F(V1UserDocumentParsing, VerifyRolesFieldMustBeAnArray) {
    ASSERT_NOT_OK(v1parser.initializeUserRolesFromUserDocument(user.get(),
                                                               BSON("user"
                                                                    << "spencer"
                                                                    << "pwd"
                                                                    << ""
                                                                    << "roles"
                                                                    << "read"),
                                                               "test"));
    ASSERT_FALSE(user->getRoles().more());
}

TEST_F(V1UserDocumentParsing, VerifySemanticallyInvalidRolesStillParse) {
    ASSERT_OK(v1parser.initializeUserRolesFromUserDocument(user.get(),
                                                           BSON("user"
                                                                << "spencer"
                                                                << "pwd"
                                                                << ""
                                                                << "roles"
                                                                << BSON_ARRAY("read"
                                                                              << "frim")),
                                                           "test"));
    RoleNameIterator roles = user->getRoles();
    RoleName role = roles.next();
    if (role == RoleName("read", "test")) {
        ASSERT_EQUALS(RoleName("frim", "test"), roles.next());
    } else {
        ASSERT_EQUALS(RoleName("frim", "test"), role);
        ASSERT_EQUALS(RoleName("read", "test"), roles.next());
    }
    ASSERT_FALSE(roles.more());
}

TEST_F(V1UserDocumentParsing, VerifyOtherDBRolesMustBeAnObjectOfArraysOfStrings) {
    ASSERT_NOT_OK(v1parser.initializeUserRolesFromUserDocument(adminUser.get(),
                                                               BSON("user"
                                                                    << "admin"
                                                                    << "pwd"
                                                                    << ""
                                                                    << "roles"
                                                                    << BSON_ARRAY("read")
                                                                    << "otherDBRoles"
                                                                    << BSON_ARRAY("read")),
                                                               "admin"));

    ASSERT_NOT_OK(v1parser.initializeUserRolesFromUserDocument(adminUser.get(),
                                                               BSON("user"
                                                                    << "admin"
                                                                    << "pwd"
                                                                    << ""
                                                                    << "roles"
                                                                    << BSON_ARRAY("read")
                                                                    << "otherDBRoles"
                                                                    << BSON("test2"
                                                                            << "read")),
                                                               "admin"));
}

TEST_F(V1UserDocumentParsing, VerifyCannotGrantPrivilegesOnOtherDatabasesNormally) {
    // Cannot grant roles on other databases, except from admin database.
    ASSERT_NOT_OK(
        v1parser.initializeUserRolesFromUserDocument(user.get(),
                                                     BSON("user"
                                                          << "spencer"
                                                          << "pwd"
                                                          << ""
                                                          << "roles"
                                                          << BSONArrayBuilder().arr()
                                                          << "otherDBRoles"
                                                          << BSON("test2" << BSON_ARRAY("read"))),
                                                     "test"));
    ASSERT_FALSE(user->getRoles().more());
}

TEST_F(V1UserDocumentParsing, GrantUserAdminOnTestViaAdmin) {
    // Grant userAdmin on test via admin.
    ASSERT_OK(v1parser.initializeUserRolesFromUserDocument(adminUser.get(),
                                                           BSON("user"
                                                                << "admin"
                                                                << "pwd"
                                                                << ""
                                                                << "roles"
                                                                << BSONArrayBuilder().arr()
                                                                << "otherDBRoles"
                                                                << BSON("test" << BSON_ARRAY(
                                                                            "userAdmin"))),
                                                           "admin"));
    RoleNameIterator roles = adminUser->getRoles();
    ASSERT_EQUALS(RoleName("userAdmin", "test"), roles.next());
    ASSERT_FALSE(roles.more());
}

TEST_F(V1UserDocumentParsing, MixedV0V1UserDocumentsAreInvalid) {
    // Try to mix fields from V0 and V1 user documents and make sure it fails.
    ASSERT_NOT_OK(v1parser.initializeUserRolesFromUserDocument(user.get(),
                                                               BSON("user"
                                                                    << "spencer"
                                                                    << "pwd"
                                                                    << "passwordHash"
                                                                    << "readOnly"
                                                                    << false
                                                                    << "roles"
                                                                    << BSON_ARRAY("read")),
                                                               "test"));
    ASSERT_FALSE(user->getRoles().more());
}

class V2UserDocumentParsing : public ::mongo::unittest::Test {
public:
    V2UserDocumentParsing() {}

    unique_ptr<User> user;
    unique_ptr<User> adminUser;
    V2UserDocumentParser v2parser;

    void setUp() {
        serverGlobalParams.featureCompatibility.version.store(
            ServerGlobalParams::FeatureCompatibility::Version::k36);
        user.reset(new User(UserName("spencer", "test")));
        adminUser.reset(new User(UserName("admin", "admin")));
    }
};


TEST_F(V2UserDocumentParsing, V2DocumentValidation) {
    BSONArray emptyArray = BSONArrayBuilder().arr();

    // V1 documents don't work
    ASSERT_NOT_OK(v2parser.checkValidUserDocument(BSON("user"
                                                       << "spencer"
                                                       << "pwd"
                                                       << "a"
                                                       << "roles"
                                                       << BSON_ARRAY("read"))));

    // Need name field
    ASSERT_NOT_OK(v2parser.checkValidUserDocument(BSON("db"
                                                       << "test"
                                                       << "credentials"
                                                       << BSON("MONGODB-CR"
                                                               << "a")
                                                       << "roles"
                                                       << emptyArray)));

    // Id field should be OID
    ASSERT_OK(v2parser.checkValidUserDocument(BSON("user"
                                                   << "spencer"
                                                   << "userId"
                                                   << OID::gen()
                                                   << "db"
                                                   << "test"
                                                   << "credentials"
                                                   << BSON("MONGODB-CR"
                                                           << "a")
                                                   << "roles"
                                                   << emptyArray)));

    // Non-OID id fields are rejected
    ASSERT_NOT_OK(v2parser.checkValidUserDocument(BSON("user"
                                                       << "spencer"
                                                       << "userId"
                                                       << "notAnOID"
                                                       << "db"
                                                       << "test"
                                                       << "credentials"
                                                       << BSON("MONGODB-CR"
                                                               << "a")
                                                       << "roles"
                                                       << emptyArray)));

    // Need source field
    ASSERT_NOT_OK(v2parser.checkValidUserDocument(BSON("user"
                                                       << "spencer"
                                                       << "credentials"
                                                       << BSON("MONGODB-CR"
                                                               << "a")
                                                       << "roles"
                                                       << emptyArray)));

    // Need credentials field
    ASSERT_NOT_OK(v2parser.checkValidUserDocument(BSON("user"
                                                       << "spencer"
                                                       << "db"
                                                       << "test"
                                                       << "roles"
                                                       << emptyArray)));

    // Need roles field
    ASSERT_NOT_OK(v2parser.checkValidUserDocument(BSON("user"
                                                       << "spencer"
                                                       << "db"
                                                       << "test"
                                                       << "credentials"
                                                       << BSON("MONGODB-CR"
                                                               << "a"))));

    // authenticationRestricitons must be an array if it exists
    ASSERT_NOT_OK(v2parser.checkValidUserDocument(BSON("user"
                                                       << "spencer"
                                                       << "db"
                                                       << "test"
                                                       << "authenticationRestrictions"
                                                       << "bogus")));

    // Empty authenticationRestrictions is OK
    ASSERT_OK(v2parser.checkValidUserDocument(BSON("user"
                                                   << "spencer"
                                                   << "db"
                                                   << "test"
                                                   << "credentials"
                                                   << BSON("MONGODB-CR"
                                                           << "a")
                                                   << "roles"
                                                   << emptyArray
                                                   << "authenticationRestrictions"
                                                   << emptyArray)));

    // Empty roles arrays are OK
    ASSERT_OK(v2parser.checkValidUserDocument(BSON("user"
                                                   << "spencer"
                                                   << "db"
                                                   << "test"
                                                   << "credentials"
                                                   << BSON("MONGODB-CR"
                                                           << "a")
                                                   << "roles"
                                                   << emptyArray)));

    // Need credentials of {external: true} if user's db is $external
    ASSERT_OK(v2parser.checkValidUserDocument(BSON("user"
                                                   << "spencer"
                                                   << "db"
                                                   << "$external"
                                                   << "credentials"
                                                   << BSON("external" << true)
                                                   << "roles"
                                                   << emptyArray)));

    // Roles must be objects
    ASSERT_NOT_OK(v2parser.checkValidUserDocument(BSON("user"
                                                       << "spencer"
                                                       << "db"
                                                       << "test"
                                                       << "credentials"
                                                       << BSON("MONGODB-CR"
                                                               << "a")
                                                       << "roles"
                                                       << BSON_ARRAY("read"))));

    // Role needs name
    ASSERT_NOT_OK(v2parser.checkValidUserDocument(BSON("user"
                                                       << "spencer"
                                                       << "db"
                                                       << "test"
                                                       << "credentials"
                                                       << BSON("MONGODB-CR"
                                                               << "a")
                                                       << "roles"
                                                       << BSON_ARRAY(BSON("db"
                                                                          << "dbA")))));

    // Role needs source
    ASSERT_NOT_OK(v2parser.checkValidUserDocument(BSON("user"
                                                       << "spencer"
                                                       << "db"
                                                       << "test"
                                                       << "credentials"
                                                       << BSON("MONGODB-CR"
                                                               << "a")
                                                       << "roles"
                                                       << BSON_ARRAY(BSON("role"
                                                                          << "roleA")))));


    // Basic valid user document
    ASSERT_OK(v2parser.checkValidUserDocument(BSON("user"
                                                   << "spencer"
                                                   << "db"
                                                   << "test"
                                                   << "credentials"
                                                   << BSON("MONGODB-CR"
                                                           << "a")
                                                   << "roles"
                                                   << BSON_ARRAY(BSON("role"
                                                                      << "roleA"
                                                                      << "db"
                                                                      << "dbA")))));

    // Multiple roles OK
    ASSERT_OK(v2parser.checkValidUserDocument(BSON("user"
                                                   << "spencer"
                                                   << "db"
                                                   << "test"
                                                   << "credentials"
                                                   << BSON("MONGODB-CR"
                                                           << "a")
                                                   << "roles"
                                                   << BSON_ARRAY(BSON("role"
                                                                      << "roleA"
                                                                      << "db"
                                                                      << "dbA")
                                                                 << BSON("role"
                                                                         << "roleB"
                                                                         << "db"
                                                                         << "dbB")))));

    // Optional authenticationRestrictions field OK
    ASSERT_OK(v2parser.checkValidUserDocument(BSON("user"
                                                   << "spencer"
                                                   << "db"
                                                   << "test"
                                                   << "credentials"
                                                   << BSON("MONGODB-CR"
                                                           << "a")
                                                   << "authenticationRestrictions"
                                                   << BSON_ARRAY(BSON("clientSource"
                                                                      << BSON_ARRAY("127.0.0.1/8")
                                                                      << "serverAddress"
                                                                      << BSON_ARRAY("127.0.0.1/8")))
                                                   << "roles"
                                                   << BSON_ARRAY(BSON("role"
                                                                      << "roleA"
                                                                      << "db"
                                                                      << "dbA")))));

    // Optional extraData field OK
    ASSERT_OK(v2parser.checkValidUserDocument(BSON("user"
                                                   << "spencer"
                                                   << "db"
                                                   << "test"
                                                   << "credentials"
                                                   << BSON("MONGODB-CR"
                                                           << "a")
                                                   << "extraData"
                                                   << BSON("foo"
                                                           << "bar")
                                                   << "roles"
                                                   << BSON_ARRAY(BSON("role"
                                                                      << "roleA"
                                                                      << "db"
                                                                      << "dbA")))));
}

TEST_F(V2UserDocumentParsing, V2UserIDExtraction) {
    OID oid = OID::gen();

    // No id is present
    ASSERT(!v2parser.extractUserIDFromUserDocument(BSON("user"
                                                        << "sam"
                                                        << "db"
                                                        << "test")));
    // Valid OID is present
    auto res = v2parser.extractUserIDFromUserDocument(BSON("user"
                                                           << "sam"
                                                           << "userId"
                                                           << oid
                                                           << "db"
                                                           << "test"));
    ASSERT(res);
    ASSERT(res == oid);
}

TEST_F(V2UserDocumentParsing, V2CredentialExtraction) {
    // Old "pwd" field not valid
    ASSERT_NOT_OK(v2parser.initializeUserCredentialsFromUserDocument(user.get(),
                                                                     BSON("user"
                                                                          << "spencer"
                                                                          << "db"
                                                                          << "test"
                                                                          << "pwd"
                                                                          << "")));

    // Credentials must be provided
    ASSERT_NOT_OK(v2parser.initializeUserCredentialsFromUserDocument(user.get(),
                                                                     BSON("user"
                                                                          << "spencer"
                                                                          << "db"
                                                                          << "test")));

    // Credentials must be object
    ASSERT_NOT_OK(v2parser.initializeUserCredentialsFromUserDocument(user.get(),
                                                                     BSON("user"
                                                                          << "spencer"
                                                                          << "db"
                                                                          << "test"
                                                                          << "credentials"
                                                                          << "a")));

    // Must specify credentials for MONGODB-CR
    ASSERT_NOT_OK(v2parser.initializeUserCredentialsFromUserDocument(user.get(),
                                                                     BSON("user"
                                                                          << "spencer"
                                                                          << "db"
                                                                          << "test"
                                                                          << "credentials"
                                                                          << BSON("foo"
                                                                                  << "bar"))));

    // Make sure extracting valid credentials works
    ASSERT_OK(v2parser.initializeUserCredentialsFromUserDocument(user.get(),
                                                                 BSON("user"
                                                                      << "spencer"
                                                                      << "db"
                                                                      << "test"
                                                                      << "credentials"
                                                                      << BSON("MONGODB-CR"
                                                                              << "a"))));
    ASSERT(user->getCredentials().password == "a");
    ASSERT(!user->getCredentials().isExternal);

    // Credentials are {external:true if users's db is $external
    ASSERT_OK(
        v2parser.initializeUserCredentialsFromUserDocument(user.get(),
                                                           BSON("user"
                                                                << "spencer"
                                                                << "db"
                                                                << "$external"
                                                                << "credentials"
                                                                << BSON("external" << true))));
    ASSERT(user->getCredentials().password.empty());
    ASSERT(user->getCredentials().isExternal);
}

TEST_F(V2UserDocumentParsing, V2RoleExtraction) {
    // "roles" field must be provided
    ASSERT_NOT_OK(v2parser.initializeUserRolesFromUserDocument(BSON("user"
                                                                    << "spencer"),
                                                               user.get()));

    // V1-style roles arrays no longer work
    ASSERT_NOT_OK(v2parser.initializeUserRolesFromUserDocument(BSON("user"
                                                                    << "spencer"
                                                                    << "roles"
                                                                    << BSON_ARRAY("read")),
                                                               user.get()));

    // Roles must have "db" field
    ASSERT_NOT_OK(v2parser.initializeUserRolesFromUserDocument(BSON("user"
                                                                    << "spencer"
                                                                    << "roles"
                                                                    << BSON_ARRAY(BSONObj())),
                                                               user.get()));

    ASSERT_NOT_OK(
        v2parser.initializeUserRolesFromUserDocument(BSON("user"
                                                          << "spencer"
                                                          << "roles"
                                                          << BSON_ARRAY(BSON("role"
                                                                             << "roleA"))),
                                                     user.get()));

    ASSERT_NOT_OK(v2parser.initializeUserRolesFromUserDocument(BSON("user"
                                                                    << "spencer"
                                                                    << "roles"
                                                                    << BSON_ARRAY(BSON("user"
                                                                                       << "roleA"
                                                                                       << "db"
                                                                                       << "dbA"))),
                                                               user.get()));

    // Valid role names are extracted successfully
    ASSERT_OK(v2parser.initializeUserRolesFromUserDocument(BSON("user"
                                                                << "spencer"
                                                                << "roles"
                                                                << BSON_ARRAY(BSON("role"
                                                                                   << "roleA"
                                                                                   << "db"
                                                                                   << "dbA"))),
                                                           user.get()));
    RoleNameIterator roles = user->getRoles();
    ASSERT_EQUALS(RoleName("roleA", "dbA"), roles.next());
    ASSERT_FALSE(roles.more());

    // Multiple roles OK
    ASSERT_OK(v2parser.initializeUserRolesFromUserDocument(BSON("user"
                                                                << "spencer"
                                                                << "roles"
                                                                << BSON_ARRAY(BSON("role"
                                                                                   << "roleA"
                                                                                   << "db"
                                                                                   << "dbA")
                                                                              << BSON("role"
                                                                                      << "roleB"
                                                                                      << "db"
                                                                                      << "dbB"))),
                                                           user.get()));
    roles = user->getRoles();
    RoleName role = roles.next();
    if (role == RoleName("roleA", "dbA")) {
        ASSERT_EQUALS(RoleName("roleB", "dbB"), roles.next());
    } else {
        ASSERT_EQUALS(RoleName("roleB", "dbB"), role);
        ASSERT_EQUALS(RoleName("roleA", "dbA"), roles.next());
    }
    ASSERT_FALSE(roles.more());
}

TEST_F(V2UserDocumentParsing,
       V2AuthenticationRestrictionsExtractioniFailsOnOldFeatureCompatibilityVersion) {
    serverGlobalParams.featureCompatibility.version.store(
        ServerGlobalParams::FeatureCompatibility::Version::k34);
    Status status = v2parser.initializeAuthenticationRestrictionsFromUserDocument(
        BSON("user"
             << "spencer"
             << "authenticationRestrictions"
             << BSON_ARRAY(BSON("clientSource" << BSON_ARRAY("::1")))),
        user.get());
    ASSERT_EQ(ErrorCodes::UnsupportedFormat, status.code());
}

TEST_F(V2UserDocumentParsing, V2AuthenticationRestrictionsExtraction) {
    const auto emptyArray = BSONArrayBuilder().arr();
    const auto emptyObj = BSONObjBuilder().obj();

    // "authenticationRestrictions" field is optional
    ASSERT_OK(v2parser.initializeAuthenticationRestrictionsFromUserDocument(BSON("user"
                                                                                 << "spencer"),
                                                                            user.get()));
    ASSERT_OK(v2parser.initializeAuthenticationRestrictionsFromUserDocument(
        BSON("user"
             << "spencer"
             << "authenticationRestrictions"
             << emptyArray),
        user.get()));

    // authenticationRestrictions must have at least one of "clientSource"/"serverAdddress" fields
    ASSERT_NOT_OK(v2parser.initializeAuthenticationRestrictionsFromUserDocument(
        BSON("user"
             << "spencer"
             << "authenticationRestrictions"
             << BSON_ARRAY(emptyObj)),
        user.get()));

    // authenticationRestrictions must not have unexpected elements
    ASSERT_NOT_OK(v2parser.initializeAuthenticationRestrictionsFromUserDocument(
        BSON("user"
             << "spencer"
             << "authenticationRestrictions"
             << BSON_ARRAY(BSON("foo"
                                << "bar"))),
        user.get()));

    // authenticationRestrictions may have only one of "clientSource"/"serverAddress" fields
    ASSERT_OK(v2parser.initializeAuthenticationRestrictionsFromUserDocument(
        BSON("user"
             << "spencer"
             << "authenticationRestrictions"
             << BSON_ARRAY(BSON("clientSource" << BSON_ARRAY("::1")))),
        user.get()));
    ASSERT_OK(v2parser.initializeAuthenticationRestrictionsFromUserDocument(
        BSON("user"
             << "spencer"
             << "authenticationRestrictions"
             << BSON_ARRAY(BSON("serverAddress" << BSON_ARRAY("::1")))),
        user.get()));

    // authenticationRestrictions may have both "clientSource"/"serverAddress" fields
    ASSERT_OK(v2parser.initializeAuthenticationRestrictionsFromUserDocument(
        BSON("user"
             << "spencer"
             << "authenticationRestrictions"
             << BSON_ARRAY(BSON("clientSource" << BSON_ARRAY("::1") << "serverAddress"
                                               << BSON_ARRAY("::1")))),
        user.get()));

    // authenticationRestrictions addresses must be valid CIDR strings
    ASSERT_NOT_OK(v2parser.initializeAuthenticationRestrictionsFromUserDocument(
        BSON("user"
             << "spencer"
             << "authenticationRestrictions"
             << BSON_ARRAY(BSON("clientSource" << BSON_ARRAY("1.2.3.4.5")))),
        user.get()));
    ASSERT_NOT_OK(v2parser.initializeAuthenticationRestrictionsFromUserDocument(
        BSON("user"
             << "spencer"
             << "authenticationRestrictions"
             << BSON_ARRAY(BSON("serverAddress" << BSON_ARRAY(":::1")))),
        user.get()));
    ASSERT_NOT_OK(v2parser.initializeAuthenticationRestrictionsFromUserDocument(
        BSON("user"
             << "spencer"
             << "authenticationRestrictions"
             << BSON_ARRAY(BSON("clientSource" << BSON_ARRAY("::1") << "serverAddress"
                                               << BSON_ARRAY(":::1")))),
        user.get()));
}

TEST_F(V2UserDocumentParsing, V2AuthenticationRestrictionsExtractionAndRetreival) {
    serverGlobalParams.featureCompatibility.version.store(
        ServerGlobalParams::FeatureCompatibility::Version::k36);
    enableIPv6(true);
    ASSERT_OK(v2parser.initializeAuthenticationRestrictionsFromUserDocument(
        BSON("user"
             << "spencer"
             << "authenticationRestrictions"
             << BSON_ARRAY(BSON("clientSource" << BSON_ARRAY("169.254.12.0/22") << "serverAddress"
                                               << BSON_ARRAY("fe80::/10")))),
        user.get()));

    const auto& doc = user->getRestrictions();
    const struct {
        std::string client;
        std::string server;
        bool valid;
    } tests[] = {
        {"169.254.12.1", "fe80::1", true},
        {"169.254.15.255", "fe80:0000:0000:0000:ffff:ffff:ffff:ffff", true},
        {"169.254.12.1", "fec0::1", false},
        {"169.254.16.0", "fe80::1", false},
        {"169.254.16.0", "fec0::1", false},
        {"127.0.0.1", "::1", false},
    };
    for (const auto& p : tests) {
        const RestrictionEnvironment re(SockAddr(p.client, 1024, AF_UNSPEC),
                                        SockAddr(p.server, 1025, AF_UNSPEC));
        ASSERT_EQ(doc.validate(re).isOK(), p.valid);
    }
}

}  // namespace
}  // namespace mongo
