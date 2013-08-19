/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

/**
 * Unit tests of the PrivilegeDocumentParser type.
 */

#include "mongo/base/status.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/privilege_document_parser.h"
#include "mongo/db/jsobj.h"
#include "mongo/unittest/unittest.h"

#define ASSERT_NULL(EXPR) ASSERT_FALSE(EXPR)
#define ASSERT_NON_NULL(EXPR) ASSERT_TRUE(EXPR)

namespace mongo {
namespace {

    class PrivilegeDocumentParsing : public ::mongo::unittest::Test {
    public:
        PrivilegeDocumentParsing() {}

        scoped_ptr<User> user;
        scoped_ptr<User> adminUser;
        V1PrivilegeDocumentParser v1parser;
        V2PrivilegeDocumentParser v2parser;

        void setUp() {
            user.reset(new User(UserName("spencer", "test")));
            adminUser.reset(new User(UserName("admin", "admin")));
        }
    };


    TEST_F(PrivilegeDocumentParsing, V0DocumentValidation) {

        // Good documents, with and without "readOnly" fields.
        ASSERT_OK(v1parser.checkValidPrivilegeDocument(
                "test", BSON("user" << "andy" << "pwd" << "a")));
        ASSERT_OK(v1parser.checkValidPrivilegeDocument(
                "test", BSON("user" << "andy" << "pwd" << "a" << "readOnly" << 1)));
        ASSERT_OK(v1parser.checkValidPrivilegeDocument(
                "test", BSON("user" << "andy" << "pwd" << "a" << "readOnly" << false)));
        ASSERT_OK(v1parser.checkValidPrivilegeDocument(
                "test", BSON("user" << "andy" << "pwd" << "a" << "readOnly" << "yes")));

        // Must have a "pwd" field.
        ASSERT_NOT_OK(v1parser.checkValidPrivilegeDocument(
                "test", BSON("user" << "andy")));

        // "pwd" field must be a string.
        ASSERT_NOT_OK(v1parser.checkValidPrivilegeDocument(
                "test", BSON("user" << "andy" << "pwd" << 100)));

        // "pwd" field string must not be empty.
        ASSERT_NOT_OK(v1parser.checkValidPrivilegeDocument(
                "test", BSON("user" << "andy" << "pwd" << "")));

        // Must have a "user" field.
        ASSERT_NOT_OK(v1parser.checkValidPrivilegeDocument(
                "test", BSON("pwd" << "a")));

        // "user" field must be a string.
        ASSERT_NOT_OK(v1parser.checkValidPrivilegeDocument(
                "test", BSON("user" << 100 << "pwd" << "a")));

        // "user" field string must not be empty.
        ASSERT_NOT_OK(v1parser.checkValidPrivilegeDocument(
                "test", BSON("user" << "" << "pwd" << "a")));
    }


    class CompatibilityModeDisabler {
    public:
        CompatibilityModeDisabler() {
            AuthorizationManager::setSupportOldStylePrivilegeDocuments(false);
        }
        ~CompatibilityModeDisabler() {
            AuthorizationManager::setSupportOldStylePrivilegeDocuments(true);
        }
    };

    TEST_F(PrivilegeDocumentParsing, DisableCompatibilityMode) {
        CompatibilityModeDisabler disabler;

        ASSERT_NOT_OK(v1parser.checkValidPrivilegeDocument("test",
                BSON("user" << "andy" << "pwd" << "a")));
        ASSERT_NOT_OK(v1parser.checkValidPrivilegeDocument("test",
                BSON("user" << "andy" << "pwd" << "a" << "readOnly" << 1)));
        ASSERT_NOT_OK(v1parser.checkValidPrivilegeDocument("test",
                BSON("user" << "andy" << "pwd" << "a" << "readOnly" << false)));
        ASSERT_NOT_OK(v1parser.checkValidPrivilegeDocument("test",
                BSON("user" << "andy" << "pwd" << "a" << "readOnly" << "yes")));

        ASSERT_OK(v1parser.checkValidPrivilegeDocument("test",
                BSON("user" << "andy" << "pwd" << "a" <<
                     "roles" << BSON_ARRAY("dbAdmin" << "read"))));
    }

    TEST_F(PrivilegeDocumentParsing, V1DocumentValidation) {
        // Document describing new-style user on "test".
        ASSERT_OK(v1parser.checkValidPrivilegeDocument("test",
                BSON("user" << "andy" << "pwd" << "a" << "roles" << BSON_ARRAY("read"))));

        // Document giving roles on "test" to a user from "test2".
        ASSERT_OK(v1parser.checkValidPrivilegeDocument("test",
                BSON("user" << "andy" << "userSource" << "test2" <<
                     "roles" << BSON_ARRAY("read"))));

        // Cannot have "userSource" field value == dbname.
        ASSERT_NOT_OK(v1parser.checkValidPrivilegeDocument("test",
                BSON("user" << "andy" << "userSource" << "test" <<
                     "roles" << BSON_ARRAY("read"))));

        // Cannot have both "userSource" and "pwd"
        ASSERT_NOT_OK(v1parser.checkValidPrivilegeDocument("test",
                BSON("user" << "andy" << "userSource" << "test2" << "pwd" << "a" <<
                     "roles" << BSON_ARRAY("read"))));

        // Cannot have an otherDBRoles field except in the admin database.
        ASSERT_NOT_OK(v1parser.checkValidPrivilegeDocument("test",
                            BSON("user" << "andy" << "userSource" << "test2" <<
                                 "roles" << BSON_ARRAY("read") <<
                                 "otherDBRoles" << BSON("test2" << BSON_ARRAY("readWrite")))));

        ASSERT_OK(v1parser.checkValidPrivilegeDocument("admin",
                        BSON("user" << "andy" << "userSource" << "test2" <<
                             "roles" << BSON_ARRAY("read") <<
                             "otherDBRoles" << BSON("test2" << BSON_ARRAY("readWrite")))));

        // Must have "roles" to have "otherDBRoles".
        ASSERT_NOT_OK(v1parser.checkValidPrivilegeDocument("admin",
                            BSON("user" << "andy" << "pwd" << "a" <<
                                 "otherDBRoles" << BSON("test2" << BSON_ARRAY("readWrite")))));

        ASSERT_OK(v1parser.checkValidPrivilegeDocument("admin",
                        BSON("user" << "andy" << "pwd" << "a" <<
                             "roles" << BSONArrayBuilder().arr() <<
                             "otherDBRoles" << BSON("test2" << BSON_ARRAY("readWrite")))));

        // "otherDBRoles" may be empty.
        ASSERT_OK(v1parser.checkValidPrivilegeDocument("admin",
                        BSON("user" << "andy" << "pwd" << "a" <<
                             "roles" << BSONArrayBuilder().arr() <<
                             "otherDBRoles" << BSONObjBuilder().obj())));

        // Cannot omit "roles" if "userSource" is present.
        ASSERT_NOT_OK(v1parser.checkValidPrivilegeDocument("test",
                BSON("user" << "andy" << "userSource" << "test2")));

        // Cannot have both "roles" and "readOnly".
        ASSERT_NOT_OK(v1parser.checkValidPrivilegeDocument("test",
                BSON("user" << "andy" << "pwd" << "a" << "readOnly" << 1 <<
                     "roles" << BSON_ARRAY("read"))));

        // Roles must be strings, not empty.
        ASSERT_NOT_OK(v1parser.checkValidPrivilegeDocument("test",
                BSON("user" << "andy" << "pwd" << "a" <<
                     "roles" << BSON_ARRAY("read" << ""))));

        ASSERT_NOT_OK(v1parser.checkValidPrivilegeDocument("test",
                BSON("user" << "andy" << "pwd" << "a" <<
                     "roles" << BSON_ARRAY(1 << "read"))));

        // Multiple roles OK.
        ASSERT_OK(v1parser.checkValidPrivilegeDocument("test",
                BSON("user" << "andy" << "pwd" << "a" <<
                     "roles" << BSON_ARRAY("dbAdmin" << "read"))));

        // Empty roles list OK.
        ASSERT_OK(v1parser.checkValidPrivilegeDocument("test",
                BSON("user" << "andy" << "pwd" << "a" <<
                     "roles" << BSONArrayBuilder().arr())));
    }

    TEST_F(PrivilegeDocumentParsing, testParsingV0PrivilegeDocuments) {
        User user(UserName("Spencer", "test"));
        User adminUser(UserName("Spencer", "admin"));
        BSONObj invalid;
        BSONObj readWrite = BSON("user" << "Spencer" << "pwd" << "passwordHash");
        BSONObj readOnly = BSON("user" << "Spencer" << "pwd" << "passwordHash" <<
                                "readOnly" << true);

        ASSERT_NOT_OK(v1parser.initializeUserFromPrivilegeDocument(&user, invalid));

        ASSERT_OK(v1parser.initializeUserFromPrivilegeDocument(&user, readOnly));
        ASSERT(user.getActionsForResource("test").contains(ActionType::find));
        ASSERT(!user.getActionsForResource("test").contains(ActionType::insert));

        ASSERT_OK(v1parser.initializeUserFromPrivilegeDocument(&user, readWrite));
        ASSERT(user.getActionsForResource("test").contains(ActionType::find));
        ASSERT(user.getActionsForResource("test").contains(ActionType::insert));
        ASSERT(user.getActionsForResource("test").contains(ActionType::userAdmin));
        ASSERT(user.getActionsForResource("test").contains(ActionType::compact));
        ASSERT(!user.getActionsForResource("test").contains(ActionType::shutdown));
        ASSERT(!user.getActionsForResource("test").contains(ActionType::addShard));
        ASSERT(!user.getActionsForResource("admin").contains(ActionType::find));
        ASSERT(!user.getActionsForResource("*").contains(ActionType::find));

        ASSERT_OK(v1parser.initializeUserFromPrivilegeDocument(&adminUser, readOnly));
        ASSERT(adminUser.getActionsForResource("*").contains(ActionType::find));
        ASSERT(!adminUser.getActionsForResource("admin").contains(ActionType::insert));
        ASSERT(!adminUser.getActionsForResource("*").contains(ActionType::insert));

        ASSERT_OK(v1parser.initializeUserFromPrivilegeDocument(&adminUser, readWrite));
        ASSERT(adminUser.getActionsForResource("*").contains(ActionType::find));
        ASSERT(adminUser.getActionsForResource("*").contains(ActionType::insert));
    }

    TEST_F(PrivilegeDocumentParsing, VerifyRolesFieldMustBeAnArray) {
        ASSERT_NOT_OK(v1parser.initializeUserFromPrivilegeDocument(
                user.get(),
                BSON("user" << "spencer" << "pwd" << "" << "roles" << "read")));
        ASSERT(user->getActionsForResource("test").empty());
    }

    TEST_F(PrivilegeDocumentParsing, VerifyInvalidRoleGrantsNoPrivileges) {
        ASSERT_OK(v1parser.initializeUserFromPrivilegeDocument(
                user.get(),
                BSON("user" << "spencer" << "pwd" << "" << "roles" << BSON_ARRAY("frim"))));
        ASSERT(user->getActionsForResource("test").empty());
    }

    TEST_F(PrivilegeDocumentParsing, VerifyInvalidRoleStillAllowsOtherRoles) {
        ASSERT_OK(v1parser.initializeUserFromPrivilegeDocument(
                user.get(),
                BSON("user" << "spencer" <<
                     "pwd" << "" <<
                     "roles" << BSON_ARRAY("read" << "frim"))));
        ASSERT(user->getActionsForResource("test").contains(ActionType::find));
    }

    TEST_F(PrivilegeDocumentParsing, VerifyCannotGrantClusterAdminRoleFromNonAdminDatabase) {
        ASSERT_OK(v1parser.initializeUserFromPrivilegeDocument(
                user.get(),
                BSON("user" << "spencer" <<
                     "pwd" << "" <<
                     "roles" << BSON_ARRAY("read" << "clusterAdmin"))));
        ASSERT(user->getActionsForResource("test").contains(ActionType::find));
        ASSERT(!user->getActionsForResource("test").contains(ActionType::shutdown));
        ASSERT(!user->getActionsForResource("test").contains(ActionType::dropDatabase));
    }

    TEST_F(PrivilegeDocumentParsing, VerifyCannotGrantClusterReadFromNonAdminDatabase) {
        ASSERT_OK(v1parser.initializeUserFromPrivilegeDocument(
                user.get(),
                BSON("user" << "spencer" <<
                     "pwd" << "" <<
                     "roles" << BSON_ARRAY("read" << "readAnyDatabase"))));
        ASSERT(user->getActionsForResource("test").contains(ActionType::find));
        ASSERT(!user->getActionsForResource("test2").contains(ActionType::find));
    }

    TEST_F(PrivilegeDocumentParsing, VerifyCannotGrantClusterReadWriteFromNonAdminDatabase) {
        ASSERT_OK(v1parser.initializeUserFromPrivilegeDocument(
                user.get(),
                BSON("user" << "spencer" <<
                     "pwd" << "" <<
                     "roles" << BSON_ARRAY("read" << "readWriteAnyDatabase"))));
        ASSERT(user->getActionsForResource("test").contains(ActionType::find));
        ASSERT(!user->getActionsForResource("test").contains(ActionType::insert));
        ASSERT(!user->getActionsForResource("test2").contains(ActionType::insert));
    }

    TEST_F(PrivilegeDocumentParsing, VerifyCannotGrantClusterUserAdminFromNonAdminDatabase) {
        ASSERT_OK(v1parser.initializeUserFromPrivilegeDocument(
                user.get(),
                BSON("user" << "spencer" <<
                     "pwd" << "" <<
                     "roles" << BSON_ARRAY("read" << "userAdminAnyDatabase"))));
        ASSERT(user->getActionsForResource("test").contains(ActionType::find));
        ASSERT(!user->getActionsForResource("test").contains(ActionType::userAdmin));
        ASSERT(!user->getActionsForResource("test2").contains(ActionType::userAdmin));
    }

    TEST_F(PrivilegeDocumentParsing, VerifyCannotGrantClusterDBAdminFromNonAdminDatabase) {
        ASSERT_OK(v1parser.initializeUserFromPrivilegeDocument(
                user.get(),
                BSON("user" << "spencer" <<
                     "pwd" << "" <<
                     "roles" << BSON_ARRAY("read" << "dbAdminAnyDatabase"))));
        ASSERT(user->getActionsForResource("test").contains(ActionType::find));
        ASSERT(!user->getActionsForResource("test").contains(ActionType::clean));
        ASSERT(!user->getActionsForResource("test2").contains(ActionType::clean));
    }

    TEST_F(PrivilegeDocumentParsing, VerifyOtherDBRolesMustBeAnObjectOfArraysOfStrings) {
        ASSERT_NOT_OK(v1parser.initializeUserFromPrivilegeDocument(
                adminUser.get(),
                BSON("user" << "admin" <<
                     "pwd" << "" <<
                     "roles" << BSON_ARRAY("read") <<
                     "otherDBRoles" << BSON_ARRAY("read"))));
        ASSERT(!adminUser->getActionsForResource("test").contains(ActionType::find));
        ASSERT(!adminUser->getActionsForResource("test2").contains(ActionType::find));
        ASSERT(!adminUser->getActionsForResource("admin").contains(ActionType::find));

        ASSERT_NOT_OK(v1parser.initializeUserFromPrivilegeDocument(
                adminUser.get(),
                BSON("user" << "admin" <<
                     "pwd" << "" <<
                     "roles" << BSON_ARRAY("read") <<
                     "otherDBRoles" << BSON("test2" << "read"))));
        ASSERT(!adminUser->getActionsForResource("test").contains(ActionType::find));
        ASSERT(!adminUser->getActionsForResource("test2").contains(ActionType::find));
        ASSERT(!adminUser->getActionsForResource("admin").contains(ActionType::find));
    }

    TEST_F(PrivilegeDocumentParsing, VerifyCannotGrantPrivilegesOnOtherDatabasesNormally) {
        // Cannot grant privileges on other databases, except from admin database.
        ASSERT_NOT_OK(v1parser.initializeUserFromPrivilegeDocument(
                user.get(),
                BSON("user" << "spencer" <<
                     "pwd" << "" <<
                     "roles" << BSON_ARRAY("read") <<
                     "otherDBRoles" << BSON("test2" << BSON_ARRAY("read")))));
        ASSERT(!user->getActionsForResource("test").contains(ActionType::find));
        ASSERT(!user->getActionsForResource("test2").contains(ActionType::find));
        ASSERT(!user->getActionsForResource("admin").contains(ActionType::find));
    }

    TEST_F(PrivilegeDocumentParsing, SuccessfulSimpleReadGrant) {
        // Grant read on test.
        ASSERT_OK(v1parser.initializeUserFromPrivilegeDocument(
                user.get(),
                BSON("user" << "spencer" <<
                     "pwd" << "" <<
                     "roles" << BSON_ARRAY("read"))));
        ASSERT(user->getActionsForResource("test").contains(ActionType::find));
        ASSERT(!user->getActionsForResource("test2").contains(ActionType::find));
        ASSERT(!user->getActionsForResource("admin").contains(ActionType::find));
    }

    TEST_F(PrivilegeDocumentParsing, SuccessfulSimpleUserAdminTest) {
        // Grant userAdmin on "test" database.
        ASSERT_OK(v1parser.initializeUserFromPrivilegeDocument(
                user.get(),
                BSON("user" << "spencer" <<
                     "pwd" << "" <<
                     "roles" << BSON_ARRAY("userAdmin"))));
        ASSERT(user->getActionsForResource("test").contains(ActionType::userAdmin));
        ASSERT(!user->getActionsForResource("test2").contains(ActionType::userAdmin));
        ASSERT(!user->getActionsForResource("admin").contains(ActionType::userAdmin));
    }

    TEST_F(PrivilegeDocumentParsing, GrantUserAdminOnAdmin) {
        // Grant userAdmin on admin.
        ASSERT_OK(v1parser.initializeUserFromPrivilegeDocument(
                adminUser.get(),
                BSON("user" << "admin" <<
                     "pwd" << "" <<
                     "roles" << BSON_ARRAY("userAdmin"))));
        ASSERT(!adminUser->getActionsForResource("test").contains(ActionType::userAdmin));
        ASSERT(!adminUser->getActionsForResource("test2").contains(ActionType::userAdmin));
        ASSERT(adminUser->getActionsForResource("admin").contains(ActionType::userAdmin));
    }

    TEST_F(PrivilegeDocumentParsing, GrantUserAdminOnTestViaAdmin) {
        // Grant userAdmin on test via admin.
        ASSERT_OK(v1parser.initializeUserFromPrivilegeDocument(
                adminUser.get(),
                BSON("user" << "admin" <<
                     "pwd" << "" <<
                     "roles" << BSONArrayBuilder().arr() <<
                     "otherDBRoles" << BSON("test" << BSON_ARRAY("userAdmin")))));
        ASSERT(adminUser->getActionsForResource("test").contains(ActionType::userAdmin));
        ASSERT(!adminUser->getActionsForResource("test2").contains(ActionType::userAdmin));
        ASSERT(!adminUser->getActionsForResource("admin").contains(ActionType::userAdmin));
    }

    TEST_F(PrivilegeDocumentParsing, SuccessfulClusterAdminTest) {
        // Grant userAdminAnyDatabase.
        ASSERT_OK(v1parser.initializeUserFromPrivilegeDocument(
                adminUser.get(),
                BSON("user" << "admin" <<
                     "pwd" << "" <<
                     "roles" << BSON_ARRAY("userAdminAnyDatabase"))));
        ASSERT(adminUser->getActionsForResource("*").contains(ActionType::userAdmin));
    }


    TEST_F(PrivilegeDocumentParsing, GrantClusterReadWrite) {
        // Grant readWrite on everything via the admin database.
        ASSERT_OK(v1parser.initializeUserFromPrivilegeDocument(
                adminUser.get(),
                BSON("user" << "admin" <<
                     "pwd" << "" <<
                     "roles" << BSON_ARRAY("readWriteAnyDatabase"))));
        ASSERT(adminUser->getActionsForResource("*").contains(ActionType::find));
        ASSERT(adminUser->getActionsForResource("*").contains(ActionType::insert));
    }

    TEST_F(PrivilegeDocumentParsing, ProhibitGrantOnWildcard) {
        // Cannot grant readWrite to everything using "otherDBRoles".
        ASSERT_NOT_OK(v1parser.initializeUserFromPrivilegeDocument(
                adminUser.get(),
                BSON("user" << "admin" <<
                     "pwd" << "" <<
                     "roles" << BSONArrayBuilder().arr() <<
                     "otherDBRoles" << BSON("*" << BSON_ARRAY("readWrite")))));
        ASSERT(!adminUser->getActionsForResource("test").contains(ActionType::find));
        ASSERT(!adminUser->getActionsForResource("test2").contains(ActionType::find));
        ASSERT(!adminUser->getActionsForResource("admin").contains(ActionType::find));
        ASSERT(!adminUser->getActionsForResource("test").contains(ActionType::insert));
        ASSERT(!adminUser->getActionsForResource("test2").contains(ActionType::insert));
        ASSERT(!adminUser->getActionsForResource("admin").contains(ActionType::insert));
    }

    TEST_F(PrivilegeDocumentParsing, GrantClusterAdmin) {
        // Grant cluster admin
        ASSERT_OK(v1parser.initializeUserFromPrivilegeDocument(
                adminUser.get(),
                BSON("user" << "admin" <<
                     "pwd" << "" <<
                     "roles" << BSON_ARRAY("clusterAdmin"))));
        ASSERT(adminUser->getActionsForResource("*").contains(ActionType::dropDatabase));
        ASSERT(adminUser->getActionsForResource("*").contains(ActionType::shutdown));
        ASSERT(adminUser->getActionsForResource("*").contains(ActionType::moveChunk));
    }

    TEST_F(PrivilegeDocumentParsing, GetPrivilegesFromPrivilegeDocumentInvalid) {
        // Try to mix fields from V0 and V1 privilege documents and make sure it fails.
        ASSERT_NOT_OK(v1parser.initializeUserFromPrivilegeDocument(
                user.get(),
                BSON("user" << "spencer" <<
                     "pwd" << "passwordHash" <<
                     "readOnly" << false <<
                     "roles" << BSON_ARRAY("read"))));
        ASSERT(!adminUser->getActionsForResource("test").contains(ActionType::find));
    }

    TEST_F(PrivilegeDocumentParsing, V2DocumentValidation) {
        BSONArray emptyArray = BSONArrayBuilder().arr();

        // V1 documents don't work
        ASSERT_NOT_OK(v2parser.checkValidPrivilegeDocument("test",
                BSON("user" << "spencer" << "pwd" << "a" <<
                     "roles" << BSON_ARRAY("read"))));

        // Need user field
        ASSERT_NOT_OK(v2parser.checkValidPrivilegeDocument("test",
                BSON("userSource" << "test" <<
                     "credentials" << BSON("MONGODB-CR" << "a") <<
                     "roles" << emptyArray <<
                     "delegatableRoles" << emptyArray)));

        // Need userSource field
        ASSERT_NOT_OK(v2parser.checkValidPrivilegeDocument("test",
                BSON("user" << "spencer" <<
                     "credentials" << BSON("MONGODB-CR" << "a") <<
                     "roles" << emptyArray <<
                     "delegatableRoles" << emptyArray)));

        // Need credentials field
        ASSERT_NOT_OK(v2parser.checkValidPrivilegeDocument("test",
                BSON("user" << "spencer" <<
                     "userSource" << "test" <<
                     "roles" << emptyArray <<
                     "delegatableRoles" << emptyArray)));

        // Need roles field
        ASSERT_NOT_OK(v2parser.checkValidPrivilegeDocument("test",
                BSON("user" << "spencer" <<
                     "userSource" << "test" <<
                     "credentials" << BSON("MONGODB-CR" << "a") <<
                     "delegatableRoles" << emptyArray)));

        // Need delegatableRoles field
        ASSERT_NOT_OK(v2parser.checkValidPrivilegeDocument("test",
                BSON("user" << "spencer" <<
                     "userSource" << "test" <<
                     "credentials" << BSON("MONGODB-CR" << "a") <<
                     "roles" << emptyArray)));

        // db the user command is run on must match the userSource
        ASSERT_NOT_OK(v2parser.checkValidPrivilegeDocument("test",
                BSON("user" << "spencer" <<
                     "userSource" << "test2" <<
                     "credentials" << BSON("MONGODB-CR" << "a") <<
                     "roles" << emptyArray <<
                     "delegatableRoles" << emptyArray)));

        // Don't need credentials field if userSource is $external
        ASSERT_OK(v2parser.checkValidPrivilegeDocument("$external",
                BSON("user" << "spencer" <<
                     "userSource" << "$external" <<
                     "roles" << emptyArray <<
                     "delegatableRoles" << emptyArray)));

        // Empty roles arrays are OK
        ASSERT_OK(v2parser.checkValidPrivilegeDocument("test",
                BSON("user" << "spencer" <<
                     "userSource" << "test" <<
                     "credentials" << BSON("MONGODB-CR" << "a") <<
                     "roles" << emptyArray <<
                     "delegatableRoles" << emptyArray)));

        // Roles must be objects
        ASSERT_NOT_OK(v2parser.checkValidPrivilegeDocument("test",
                BSON("user" << "spencer" <<
                     "userSource" << "test" <<
                     "credentials" << BSON("MONGODB-CR" << "a") <<
                     "roles" << BSON_ARRAY("read") <<
                     "delegatableRoles" << emptyArray)));

        // Role needs source
        ASSERT_NOT_OK(v2parser.checkValidPrivilegeDocument("test",
                BSON("user" << "spencer" <<
                     "userSource" << "test" <<
                     "credentials" << BSON("MONGODB-CR" << "a") <<
                     "roles" << BSON_ARRAY(BSON("name" << "roleA")) <<
                     "delegatableRoles" << emptyArray)));

        // Role needs name
        ASSERT_NOT_OK(v2parser.checkValidPrivilegeDocument("test",
                BSON("user" << "spencer" <<
                     "userSource" << "test" <<
                     "credentials" << BSON("MONGODB-CR" << "a") <<
                     "roles" << BSON_ARRAY(BSON("source" << "dbA")) <<
                     "delegatableRoles" << emptyArray)));

        // Basic valid privilege document
        ASSERT_OK(v2parser.checkValidPrivilegeDocument("test",
                BSON("user" << "spencer" <<
                     "userSource" << "test" <<
                     "credentials" << BSON("MONGODB-CR" << "a") <<
                     "roles" << BSON_ARRAY(BSON("name" << "roleA" << "source" << "dbA")) <<
                     "delegatableRoles" << BSON_ARRAY(BSON("name" << "roleA" <<
                                                           "source" << "dbA")))));

        // Multiple roles OK
        ASSERT_OK(v2parser.checkValidPrivilegeDocument("test",
                BSON("user" << "spencer" <<
                     "userSource" << "test" <<
                     "credentials" << BSON("MONGODB-CR" << "a") <<
                     "roles" << BSON_ARRAY(BSON("name" << "roleA" << "source" << "dbA") <<
                                           BSON("name" << "roleB" << "source" << "dbB")) <<
                     "delegatableRoles" << emptyArray)));

        // Optional extraData field OK
        ASSERT_OK(v2parser.checkValidPrivilegeDocument("test",
                BSON("user" << "spencer" <<
                     "userSource" << "test" <<
                     "credentials" << BSON("MONGODB-CR" << "a") <<
                     "extraData" << BSON("foo" << "bar") <<
                     "roles" << BSON_ARRAY(BSON("name" << "roleA" << "source" << "dbA")) <<
                     "delegatableRoles" << emptyArray)));
    }

    TEST_F(PrivilegeDocumentParsing, V2CredentialExtraction) {
        // Old "pwd" field not valid
        ASSERT_NOT_OK(v2parser.initializeUserCredentialsFromPrivilegeDocument(
                user.get(),
                BSON("user" << "spencer" <<
                     "userSource" << "test" <<
                     "pwd" << "")));

        // Credentials must be provided (so long as userSource is not $external)
        ASSERT_NOT_OK(v2parser.initializeUserCredentialsFromPrivilegeDocument(
                user.get(),
                BSON("user" << "spencer" <<
                     "userSource" << "test")));

        // Credentials must be object
        ASSERT_NOT_OK(v2parser.initializeUserCredentialsFromPrivilegeDocument(
                user.get(),
                BSON("user" << "spencer" <<
                     "userSource" << "test" <<
                     "credentials" << "a")));

        // Must specify credentials for MONGODB-CR
        ASSERT_NOT_OK(v2parser.initializeUserCredentialsFromPrivilegeDocument(
                user.get(),
                BSON("user" << "spencer" <<
                     "userSource" << "test" <<
                     "credentials" << BSON("foo" << "bar"))));

        // Make sure extracting valid credentials works
        ASSERT_OK(v2parser.initializeUserCredentialsFromPrivilegeDocument(
                user.get(),
                BSON("user" << "spencer" <<
                     "userSource" << "test" <<
                     "credentials" << BSON("MONGODB-CR" << "a"))));
        ASSERT(user->getCredentials().password == "a");
        ASSERT(!user->getCredentials().isExternal);

        // Leaving out 'credentials' field is OK so long as userSource is $external
        ASSERT_OK(v2parser.initializeUserCredentialsFromPrivilegeDocument(
                user.get(),
                BSON("user" << "spencer" <<
                     "userSource" << "$external")));
        ASSERT(user->getCredentials().password.empty());
        ASSERT(user->getCredentials().isExternal);

    }

}  // namespace
}  // namespace mongo
