/*    Copyright 2013 10gen Inc.
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
 * Unit tests of the functions used for parsing the commands used for user management.
 */

#include <vector>

#include "mongo/base/status.h"
#include "mongo/client/auth_helpers.h"
#include "mongo/db/auth/authz_manager_external_state_mock.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/cmdline.h"
#include "mongo/db/commands/user_management_commands_parser.h"
#include "mongo/db/jsobj.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

    // Crutches to make the test compile
    CmdLine cmdLine;
    bool inShutdown() {
        return false;
    }

namespace {

    class UserManagementCommandsParserTest : public ::mongo::unittest::Test {
    public:
        scoped_ptr<AuthorizationManager> authzManager;
        AuthzManagerExternalStateMock* externalState;
        void setUp() {
            externalState = new AuthzManagerExternalStateMock();
            authzManager.reset(new AuthorizationManager(externalState));
        }
    };

    TEST_F(UserManagementCommandsParserTest, WriteConcernParsing) {
        BSONObj writeConcern;
        // Test no write concern provided
        ASSERT_OK(auth::extractWriteConcern(BSONObj(), &writeConcern));
        ASSERT(writeConcern.isEmpty());

        ASSERT_OK(auth::extractWriteConcern(BSON("writeConcern" << BSON("w" << "majority" <<
                                                                        "j" << true)),
                                            &writeConcern));

        ASSERT_EQUALS("majority", writeConcern["w"].str());
        ASSERT_EQUALS(true, writeConcern["j"].Bool());
    }

    TEST_F(UserManagementCommandsParserTest, CreateUserCommandParsing) {
        BSONArray emptyArray = BSONArrayBuilder().arr();
        BSONObj parsedUserObj;

        // Must have password
        ASSERT_NOT_OK(auth::parseAndValidateCreateUserCommand(BSON("createUser" << "spencer" <<
                                                                   "roles" << emptyArray),
                                                              "test",
                                                              authzManager.get(),
                                                              &parsedUserObj));

        // Must have roles array
        ASSERT_NOT_OK(auth::parseAndValidateCreateUserCommand(BSON("createUser" << "spencer" <<
                                                                   "pwd" << "password"),
                                                              "test",
                                                              authzManager.get(),
                                                              &parsedUserObj));

        // Cannot create users in the local db
        ASSERT_NOT_OK(auth::parseAndValidateCreateUserCommand(BSON("createUser" << "spencer" <<
                                                                   "pwd" << "password" <<
                                                                   "roles" << emptyArray),
                                                              "local",
                                                              authzManager.get(),
                                                              &parsedUserObj));

        // Cannot have extra fields
        ASSERT_NOT_OK(auth::parseAndValidateCreateUserCommand(BSON("createUser" << "spencer" <<
                                                                   "pwd" << "password" <<
                                                                   "roles" << emptyArray <<
                                                                   "anotherField" << "garbage"),
                                                              "test",
                                                              authzManager.get(),
                                                              &parsedUserObj));

        // Role must exist (string role)
        ASSERT_NOT_OK(auth::parseAndValidateCreateUserCommand(
                BSON("createUser" << "spencer" <<
                     "pwd" << "password" <<
                     "roles" << BSON_ARRAY("fakeRole")),
                "test",
                authzManager.get(),
                &parsedUserObj));

        // Role must exist (object role)
        ASSERT_NOT_OK(auth::parseAndValidateCreateUserCommand(
                BSON("createUser" << "spencer" <<
                     "pwd" << "password" <<
                     "roles" << BSON_ARRAY(BSON("name" << "fakeRole" <<
                                                "source" << "test" <<
                                                "hasRole" << true <<
                                                "canDelegate" << true))),
                "test",
                authzManager.get(),
                &parsedUserObj));

        // Role must have name
        ASSERT_NOT_OK(auth::parseAndValidateCreateUserCommand(
                BSON("createUser" << "spencer" <<
                     "pwd" << "password" <<
                     "roles" << BSON_ARRAY(BSON("source" << "test" <<
                                                "hasRole" << true <<
                                                "canDelegate" << true))),
                "test",
                authzManager.get(),
                &parsedUserObj));

        // Role must have source
        ASSERT_NOT_OK(auth::parseAndValidateCreateUserCommand(
                BSON("createUser" << "spencer" <<
                     "pwd" << "password" <<
                     "roles" << BSON_ARRAY(BSON("name" << "read" <<
                                                "hasRole" << true <<
                                                "canDelegate" << true))),
                "test",
                authzManager.get(),
                &parsedUserObj));

        // Role must have hasRole
        ASSERT_NOT_OK(auth::parseAndValidateCreateUserCommand(
                BSON("createUser" << "spencer" <<
                     "pwd" << "password" <<
                     "roles" << BSON_ARRAY(BSON("name" << "read" <<
                                                "source" << "test" <<
                                                "canDelegate" << true))),
                "test",
                authzManager.get(),
                &parsedUserObj));

        // Role must have canDelegate
        ASSERT_NOT_OK(auth::parseAndValidateCreateUserCommand(
                BSON("createUser" << "spencer" <<
                     "pwd" << "password" <<
                     "roles" << BSON_ARRAY(BSON("name" << "read" <<
                                                "source" << "test" <<
                                                "hasRole" << true))),
                "test",
                authzManager.get(),
                &parsedUserObj));

        // canDelegate and hasRole can't both be false
        ASSERT_NOT_OK(auth::parseAndValidateCreateUserCommand(
                BSON("createUser" << "spencer" <<
                     "pwd" << "password" <<
                     "roles" << BSON_ARRAY(BSON("name" << "read" <<
                                                "source" << "test" <<
                                                "hasRole" << false <<
                                                "canDelegate" << false))),
                "test",
                authzManager.get(),
                &parsedUserObj));

        // Empty roles array OK
        ASSERT_OK(auth::parseAndValidateCreateUserCommand(BSON("createUser" << "spencer" <<
                                                               "pwd" << "password" <<
                                                               "roles" << emptyArray),
                                                          "test",
                                                          authzManager.get(),
                                                          &parsedUserObj));


        // Missing password OK if source is $external
        ASSERT_OK(auth::parseAndValidateCreateUserCommand(BSON("createUser" << "spencer" <<
                                                               "roles" << emptyArray),
                                                          "$external",
                                                          authzManager.get(),
                                                          &parsedUserObj));

        // String role names OK
        ASSERT_OK(auth::parseAndValidateCreateUserCommand(
                BSON("createUser" << "spencer" <<
                     "pwd" << "password" <<
                     "roles" << BSON_ARRAY("read" << "dbAdmin")),
                "test",
                authzManager.get(),
                &parsedUserObj));

        ASSERT_EQUALS("spencer", parsedUserObj["name"].String());
        ASSERT_EQUALS("test", parsedUserObj["source"].String());
        std::string hashedPassword = auth::createPasswordDigest("spencer", "password");
        ASSERT_EQUALS(hashedPassword, parsedUserObj["credentials"].Obj()["MONGODB-CR"].String());
        std::vector<BSONElement> rolesArray = parsedUserObj["roles"].Array();
        ASSERT_EQUALS((size_t)2, rolesArray.size());
        ASSERT_EQUALS("read", rolesArray[0].Obj()["name"].String());
        ASSERT_EQUALS("test", rolesArray[0].Obj()["source"].String());
        ASSERT_EQUALS(true, rolesArray[0].Obj()["hasRole"].Bool());
        ASSERT_EQUALS(false, rolesArray[0].Obj()["canDelegate"].Bool());
        ASSERT_EQUALS("dbAdmin", rolesArray[1].Obj()["name"].String());
        ASSERT_EQUALS("test", rolesArray[1].Obj()["source"].String());
        ASSERT_EQUALS(true, rolesArray[1].Obj()["hasRole"].Bool());
        ASSERT_EQUALS(false, rolesArray[1].Obj()["canDelegate"].Bool());


        // Basic valid createUser command OK
        ASSERT_OK(auth::parseAndValidateCreateUserCommand(
                BSON("createUser" << "spencer" <<
                     "pwd" << "password" <<
                     "roles" << BSON_ARRAY(BSON("name" << "read" <<
                                                "source" << "test" <<
                                                "hasRole" << true <<
                                                "canDelegate" << false) <<
                                           BSON("name" << "dbAdminAnyDatabase" <<
                                                "source" << "admin" <<
                                                "hasRole" << false <<
                                                "canDelegate" << true)) <<
                     "customData" << BSON("foo" << "bar")),
                "test",
                authzManager.get(),
                &parsedUserObj));

        ASSERT_EQUALS("spencer", parsedUserObj["name"].String());
        ASSERT_EQUALS("test", parsedUserObj["source"].String());
        hashedPassword = auth::createPasswordDigest("spencer", "password");
        ASSERT_EQUALS(hashedPassword, parsedUserObj["credentials"].Obj()["MONGODB-CR"].String());
        ASSERT_EQUALS("bar", parsedUserObj["customData"].Obj()["foo"].String());
        rolesArray = parsedUserObj["roles"].Array();
        ASSERT_EQUALS((size_t)2, rolesArray.size());
        ASSERT_EQUALS("read", rolesArray[0].Obj()["name"].String());
        ASSERT_EQUALS("test", rolesArray[0].Obj()["source"].String());
        ASSERT_EQUALS(true, rolesArray[0].Obj()["hasRole"].Bool());
        ASSERT_EQUALS(false, rolesArray[0].Obj()["canDelegate"].Bool());
        ASSERT_EQUALS("dbAdminAnyDatabase", rolesArray[1].Obj()["name"].String());
        ASSERT_EQUALS("admin", rolesArray[1].Obj()["source"].String());
        ASSERT_EQUALS(false, rolesArray[1].Obj()["hasRole"].Bool());
        ASSERT_EQUALS(true, rolesArray[1].Obj()["canDelegate"].Bool());

    }


    TEST_F(UserManagementCommandsParserTest, UpdateUserCommandParsing) {
        BSONArray emptyArray = BSONArrayBuilder().arr();
        BSONObj parsedUpdateObj;
        UserName parsedUserName;

        // Cannot have extra fields
        ASSERT_NOT_OK(auth::parseAndValidateUpdateUserCommand(BSON("updateUser" << "spencer" <<
                                                                   "pwd" << "password" <<
                                                                   "roles" << emptyArray <<
                                                                   "anotherField" << "garbage"),
                                                              "test",
                                                              authzManager.get(),
                                                              &parsedUpdateObj,
                                                              &parsedUserName));

        // Role must exist (string role)
        ASSERT_NOT_OK(auth::parseAndValidateUpdateUserCommand(
                BSON("updateUser" << "spencer" <<
                     "pwd" << "password" <<
                     "roles" << BSON_ARRAY("fakeRole")),
                "test",
                authzManager.get(),
                &parsedUpdateObj,
                &parsedUserName));

        // Role must exist (object role)
        ASSERT_NOT_OK(auth::parseAndValidateUpdateUserCommand(
                BSON("updateUser" << "spencer" <<
                     "pwd" << "password" <<
                     "roles" << BSON_ARRAY(BSON("name" << "fakeRole" <<
                                                "source" << "test" <<
                                                "hasRole" << true <<
                                                "canDelegate" << true))),
                "test",
                authzManager.get(),
                &parsedUpdateObj,
                &parsedUserName));

        // Role must have name
        ASSERT_NOT_OK(auth::parseAndValidateUpdateUserCommand(
                BSON("updateUser" << "spencer" <<
                     "pwd" << "password" <<
                     "roles" << BSON_ARRAY(BSON("source" << "test" <<
                                                "hasRole" << true <<
                                                "canDelegate" << true))),
                "test",
                authzManager.get(),
                &parsedUpdateObj,
                &parsedUserName));

        // Role must have source
        ASSERT_NOT_OK(auth::parseAndValidateUpdateUserCommand(
                BSON("updateUser" << "spencer" <<
                     "pwd" << "password" <<
                     "roles" << BSON_ARRAY(BSON("name" << "read" <<
                                                "hasRole" << true <<
                                                "canDelegate" << true))),
                "test",
                authzManager.get(),
                &parsedUpdateObj,
                &parsedUserName));

        // Role must have hasRole
        ASSERT_NOT_OK(auth::parseAndValidateUpdateUserCommand(
                BSON("updateUser" << "spencer" <<
                     "pwd" << "password" <<
                     "roles" << BSON_ARRAY(BSON("name" << "read" <<
                                                "source" << "test" <<
                                                "canDelegate" << true))),
                "test",
                authzManager.get(),
                &parsedUpdateObj,
                &parsedUserName));

        // Role must have canDelegate
        ASSERT_NOT_OK(auth::parseAndValidateUpdateUserCommand(
                BSON("updateUser" << "spencer" <<
                     "pwd" << "password" <<
                     "roles" << BSON_ARRAY(BSON("name" << "read" <<
                                                "source" << "test" <<
                                                "hasRole" << true))),
                "test",
                authzManager.get(),
                &parsedUpdateObj,
                &parsedUserName));

        // canDelegate and hasRole can't both be false
        ASSERT_NOT_OK(auth::parseAndValidateUpdateUserCommand(
                BSON("updateUser" << "spencer" <<
                     "pwd" << "password" <<
                     "roles" << BSON_ARRAY(BSON("name" << "read" <<
                                                "source" << "test" <<
                                                "hasRole" << false <<
                                                "canDelegate" << false))),
                "test",
                authzManager.get(),
                &parsedUpdateObj,
                &parsedUserName));

        // Empty roles array OK
        ASSERT_OK(auth::parseAndValidateUpdateUserCommand(BSON("updateUser" << "spencer" <<
                                                               "pwd" << "password" <<
                                                               "roles" << emptyArray),
                                                          "test",
                                                          authzManager.get(),
                                                          &parsedUpdateObj,
                                                          &parsedUserName));

        // String role names OK
        ASSERT_OK(auth::parseAndValidateUpdateUserCommand(
                BSON("updateUser" << "spencer" <<
                     "pwd" << "password" <<
                     "roles" << BSON_ARRAY("read" << "dbAdmin")),
                "test",
                authzManager.get(),
                &parsedUpdateObj,
                &parsedUserName));

        BSONObj setObj = parsedUpdateObj["$set"].Obj();

        ASSERT_EQUALS(UserName("spencer", "test"), parsedUserName);
        std::string hashedPassword = auth::createPasswordDigest("spencer", "password");
        ASSERT_EQUALS(hashedPassword, setObj["credentials.MONGODB-CR"].String());
        std::vector<BSONElement> rolesArray = setObj["roles"].Array();
        ASSERT_EQUALS((size_t)2, rolesArray.size());
        ASSERT_EQUALS("read", rolesArray[0].Obj()["name"].String());
        ASSERT_EQUALS("test", rolesArray[0].Obj()["source"].String());
        ASSERT_EQUALS(true, rolesArray[0].Obj()["hasRole"].Bool());
        ASSERT_EQUALS(false, rolesArray[0].Obj()["canDelegate"].Bool());
        ASSERT_EQUALS("dbAdmin", rolesArray[1].Obj()["name"].String());
        ASSERT_EQUALS("test", rolesArray[1].Obj()["source"].String());
        ASSERT_EQUALS(true, rolesArray[1].Obj()["hasRole"].Bool());
        ASSERT_EQUALS(false, rolesArray[1].Obj()["canDelegate"].Bool());


        // Basic valid updateUser command OK
        ASSERT_OK(auth::parseAndValidateUpdateUserCommand(
                BSON("updateUser" << "spencer" <<
                     "pwd" << "password" <<
                     "roles" << BSON_ARRAY(BSON("name" << "read" <<
                                                "source" << "test" <<
                                                "hasRole" << true <<
                                                "canDelegate" << false) <<
                                           BSON("name" << "dbAdminAnyDatabase" <<
                                                "source" << "admin" <<
                                                "hasRole" << false <<
                                                "canDelegate" << true)) <<
                     "customData" << BSON("foo" << "bar")),
                "test",
                authzManager.get(),
                &parsedUpdateObj,
                &parsedUserName));

        setObj = parsedUpdateObj["$set"].Obj();

        ASSERT_EQUALS(UserName("spencer", "test"), parsedUserName);
        hashedPassword = auth::createPasswordDigest("spencer", "password");
        ASSERT_EQUALS(hashedPassword, setObj["credentials.MONGODB-CR"].String());
        ASSERT_EQUALS("bar", setObj["customData"].Obj()["foo"].String());
        rolesArray = setObj["roles"].Array();
        ASSERT_EQUALS((size_t)2, rolesArray.size());
        ASSERT_EQUALS("read", rolesArray[0].Obj()["name"].String());
        ASSERT_EQUALS("test", rolesArray[0].Obj()["source"].String());
        ASSERT_EQUALS(true, rolesArray[0].Obj()["hasRole"].Bool());
        ASSERT_EQUALS(false, rolesArray[0].Obj()["canDelegate"].Bool());
        ASSERT_EQUALS("dbAdminAnyDatabase", rolesArray[1].Obj()["name"].String());
        ASSERT_EQUALS("admin", rolesArray[1].Obj()["source"].String());
        ASSERT_EQUALS(false, rolesArray[1].Obj()["hasRole"].Bool());
        ASSERT_EQUALS(true, rolesArray[1].Obj()["canDelegate"].Bool());
    }

    TEST_F(UserManagementCommandsParserTest, UserRoleManipulationCommandsParsing) {
        UserName userName;
        std::vector<RoleName> roles;
        BSONObj writeConcern;

        // Command name must match
        ASSERT_NOT_OK(auth::parseUserRoleManipulationCommand(
                BSON("grantRolesToUser" << "spencer" <<
                     "roles" << BSON_ARRAY("read")),
                "revokeRolesFromUser",
                "test",
                authzManager.get(),
                &userName,
                &roles,
                &writeConcern));

        // Roles array can't be empty
        ASSERT_NOT_OK(auth::parseUserRoleManipulationCommand(
                BSON("grantRolesToUser" << "spencer" <<
                     "roles" << BSONArray()),
                "grantRolesToUser",
                "test",
                authzManager.get(),
                &userName,
                &roles,
                &writeConcern));

        // Roles must exist
        ASSERT_NOT_OK(auth::parseUserRoleManipulationCommand(
                BSON("grantRolesToUser" << "spencer" <<
                     "roles" << BSON_ARRAY("fakeRole")),
                "grantRolesToUser",
                "test",
                authzManager.get(),
                &userName,
                &roles,
                &writeConcern));

        ASSERT_OK(auth::parseUserRoleManipulationCommand(
                BSON("grantRolesToUser" << "spencer" <<
                     "roles" << BSON_ARRAY("readWrite" << BSON("name" << "dbAdmin" <<
                                                               "source" << "test2")) <<
                     "writeConcern" << BSON("w" << 1)),
                "grantRolesToUser",
                "test",
                authzManager.get(),
                &userName,
                &roles,
                &writeConcern));

        ASSERT_EQUALS(UserName("spencer", "test"), userName);
        ASSERT_EQUALS(1, writeConcern["w"].numberInt());
        ASSERT_EQUALS(2U, roles.size());
        ASSERT_EQUALS(RoleName("readWrite", "test"), roles[0]);
        ASSERT_EQUALS(RoleName("dbAdmin", "test2"), roles[1]);
    }

    TEST_F(UserManagementCommandsParserTest, CreateRoleCommandParsing) {
        BSONObj parsedRoleObj;
        BSONObj parsedWriteConcern;


        // Must have roles array
        ASSERT_NOT_OK(auth::parseAndValidateCreateRoleCommand(BSON("createRole" << "myRole" <<
                                                                   "privileges" << BSONArray()),
                                                              "test",
                                                              authzManager.get(),
                                                              &parsedRoleObj,
                                                              &parsedWriteConcern));

        // Must have privileges array
        ASSERT_NOT_OK(auth::parseAndValidateCreateRoleCommand(BSON("createRole" << "myRole" <<
                                                                   "roles" << BSONArray()),
                                                              "test",
                                                              authzManager.get(),
                                                              &parsedRoleObj,
                                                              &parsedWriteConcern));

        // Cannot create roles in the local db
        ASSERT_NOT_OK(auth::parseAndValidateCreateRoleCommand(BSON("createRole" << "myRole" <<
                                                                   "privileges" << BSONArray() <<
                                                                   "roles" << BSONArray()),
                                                              "local",
                                                              authzManager.get(),
                                                              &parsedRoleObj,
                                                              &parsedWriteConcern));

        // Cannot have extra fields
        ASSERT_NOT_OK(auth::parseAndValidateCreateRoleCommand(BSON("createRole" << "myRole" <<
                                                                   "privileges" << BSONArray() <<
                                                                   "roles" << BSONArray() <<
                                                                   "anotherField" << true),
                                                              "test",
                                                              authzManager.get(),
                                                              &parsedRoleObj,
                                                              &parsedWriteConcern));

        // Role must exist (string role)
        ASSERT_NOT_OK(auth::parseAndValidateCreateRoleCommand(
                BSON("createRole" << "myRole" <<
                     "privileges" << BSONArray() <<
                     "roles" << BSON_ARRAY("fakeRole")),
                "test",
                authzManager.get(),
                &parsedRoleObj,
                &parsedWriteConcern));

        // Role must exist (object role)
        ASSERT_NOT_OK(auth::parseAndValidateCreateRoleCommand(
                BSON("createRole" << "myRole" <<
                     "privileges" << BSONArray() <<
                     "roles" << BSON_ARRAY("name" << "fakeRole" << "source" << "test")),
                "test",
                authzManager.get(),
                &parsedRoleObj,
                &parsedWriteConcern));

        // Role must have name
        ASSERT_NOT_OK(auth::parseAndValidateCreateRoleCommand(
                BSON("createRole" << "myRole" <<
                     "privileges" << BSONArray() <<
                     "roles" << BSON_ARRAY("source" << "test")),
                "test",
                authzManager.get(),
                &parsedRoleObj,
                &parsedWriteConcern));

        // Role must have source
        ASSERT_NOT_OK(auth::parseAndValidateCreateRoleCommand(
                BSON("createRole" << "myRole" <<
                     "privileges" << BSONArray() <<
                     "roles" << BSON_ARRAY("name" << "readWrite")),
                "test",
                authzManager.get(),
                &parsedRoleObj,
                &parsedWriteConcern));

        // Role must not have have canDelegate or hasRole
        ASSERT_NOT_OK(auth::parseAndValidateCreateRoleCommand(
                BSON("createRole" << "myRole" <<
                     "privileges" << BSONArray() <<
                     "roles" << BSON_ARRAY("name" << "readWrite" <<
                                           "source" << "test" <<
                                           "hasRole" << true <<
                                           "canDelegate" << false)),
                "test",
                authzManager.get(),
                &parsedRoleObj,
                &parsedWriteConcern));

        // Privilege must have resource
        ASSERT_NOT_OK(auth::parseAndValidateCreateRoleCommand(
                BSON("createRole" << "myRole" <<
                     "privileges" << BSON_ARRAY(BSON("actions" << BSON_ARRAY("find"))) <<
                     "roles" << BSONArray()),
                "test",
                authzManager.get(),
                &parsedRoleObj,
                &parsedWriteConcern));

        // Privilege must have actions
        ASSERT_NOT_OK(auth::parseAndValidateCreateRoleCommand(
                BSON("createRole" << "myRole" <<
                     "privileges" << BSON_ARRAY(BSON("resource" << BSON("cluster" << true))) <<
                     "roles" << BSONArray()),
                "test",
                authzManager.get(),
                &parsedRoleObj,
                &parsedWriteConcern));

        // Action must exist
        ASSERT_NOT_OK(auth::parseAndValidateCreateRoleCommand(
                BSON("createRole" << "myRole" <<
                     "privileges" << BSON_ARRAY(BSON("resource" << BSON("cluster" << true) <<
                                                     "actions" << BSON_ARRAY("fakeAction"))) <<
                     "roles" << BSONArray()),
                "test",
                authzManager.get(),
                &parsedRoleObj,
                &parsedWriteConcern));

        // Resource can't be on cluster AND normal resources
        ASSERT_NOT_OK(auth::parseAndValidateCreateRoleCommand(
                BSON("createRole" << "myRole" <<
                     "privileges" << BSON_ARRAY(BSON("resource" << BSON("cluster" << true <<
                                                                        "db" << "" <<
                                                                        "collection" << "") <<
                                                     "actions" << BSONArray())) <<
                     "roles" << BSONArray()),
                "test",
                authzManager.get(),
                &parsedRoleObj,
                &parsedWriteConcern));

        // Resource can't have db without collection
        ASSERT_NOT_OK(auth::parseAndValidateCreateRoleCommand(
                BSON("createRole" << "myRole" <<
                     "privileges" << BSON_ARRAY(BSON("resource" << BSON("db" << "" ) <<
                                                     "actions" << BSONArray())) <<
                     "roles" << BSONArray()),
                "test",
                authzManager.get(),
                &parsedRoleObj,
                &parsedWriteConcern));

        // Resource can't have collection without db
        ASSERT_NOT_OK(auth::parseAndValidateCreateRoleCommand(
                BSON("createRole" << "myRole" <<
                     "privileges" << BSON_ARRAY(BSON("resource" << BSON("collection" << "" ) <<
                                                     "actions" << BSONArray())) <<
                     "roles" << BSONArray()),
                "test",
                authzManager.get(),
                &parsedRoleObj,
                &parsedWriteConcern));

        // Empty roles and privileges arrays OK
        ASSERT_OK(auth::parseAndValidateCreateRoleCommand(
                BSON("createRole" << "myRole" <<
                     "privileges" << BSONArray() <<
                     "roles" << BSONArray()),
                "test",
                authzManager.get(),
                &parsedRoleObj,
                &parsedWriteConcern));

        ASSERT_EQUALS("myRole", parsedRoleObj["name"].String());
        ASSERT_EQUALS("test", parsedRoleObj["source"].String());
        ASSERT_EQUALS(0U, parsedRoleObj["privileges"].Array().size());
        ASSERT_EQUALS(0U, parsedRoleObj["roles"].Array().size());

        // Empty db and collection for privilege is OK.
        ASSERT_OK(auth::parseAndValidateCreateRoleCommand(
                BSON("createRole" << "myRole" <<
                     "privileges" << BSON_ARRAY(BSON("resource" << BSON("db" << "" <<
                                                                        "collection" << "" ) <<
                                                     "actions" << BSON_ARRAY("find"))) <<
                     "roles" << BSONArray()),
                "test",
                authzManager.get(),
                &parsedRoleObj,
                &parsedWriteConcern));

        std::vector<BSONElement> privileges = parsedRoleObj["privileges"].Array();
        ASSERT_EQUALS(1U, privileges.size());
        ASSERT_EQUALS("", privileges[0].Obj()["resource"].Obj()["db"].String());
        ASSERT_EQUALS("", privileges[0].Obj()["resource"].Obj()["collection"].String());
        ASSERT_EQUALS(1U, privileges[0].Obj()["actions"].Array().size());
        ASSERT_EQUALS("find", privileges[0].Obj()["actions"].Array()[0].String());

        // Explicit collection name resource OK
        ASSERT_OK(auth::parseAndValidateCreateRoleCommand(
                BSON("createRole" << "myRole" <<
                     "privileges" << BSON_ARRAY(BSON("resource" << BSON("db" << "test" <<
                                                                        "collection" << "foo" ) <<
                                                     "actions" << BSON_ARRAY("find"))) <<
                     "roles" << BSONArray()),
                "test",
                authzManager.get(),
                &parsedRoleObj,
                &parsedWriteConcern));

        privileges = parsedRoleObj["privileges"].Array();
        ASSERT_EQUALS(1U, privileges.size());
        ASSERT_EQUALS("test", privileges[0].Obj()["resource"].Obj()["db"].String());
        ASSERT_EQUALS("foo", privileges[0].Obj()["resource"].Obj()["collection"].String());

        // Cluster resource OK
        ASSERT_OK(auth::parseAndValidateCreateRoleCommand(
                BSON("createRole" << "myRole" <<
                     "privileges" << BSON_ARRAY(BSON("resource" << BSON("cluster" << true) <<
                                                     "actions" << BSON_ARRAY("find"))) <<
                     "roles" << BSONArray()),
                "test",
                authzManager.get(),
                &parsedRoleObj,
                &parsedWriteConcern));

        privileges = parsedRoleObj["privileges"].Array();
        ASSERT_EQUALS(1U, privileges.size());
        ASSERT_EQUALS(true, privileges[0].Obj()["resource"].Obj()["cluster"].Bool());

        // String role names OK
        ASSERT_OK(auth::parseAndValidateCreateRoleCommand(
                BSON("createRole" << "myRole" <<
                     "privileges" << BSONArray() <<
                     "roles" << BSON_ARRAY("read" << "dbAdmin")),
                "test",
                authzManager.get(),
                &parsedRoleObj,
                &parsedWriteConcern));

        std::vector<BSONElement> rolesArray = parsedRoleObj["roles"].Array();
        ASSERT_EQUALS((size_t)2, rolesArray.size());
        ASSERT_EQUALS("read", rolesArray[0].Obj()["name"].String());
        ASSERT_EQUALS("test", rolesArray[0].Obj()["source"].String());
        ASSERT_EQUALS("dbAdmin", rolesArray[1].Obj()["name"].String());
        ASSERT_EQUALS("test", rolesArray[1].Obj()["source"].String());

        // Object roles OK
        ASSERT_OK(auth::parseAndValidateCreateRoleCommand(
                BSON("createRole" << "myRole" <<
                     "privileges" << BSONArray() <<
                     "roles" << BSON_ARRAY(BSON("name" << "read" << "source" << "test") <<
                                           BSON("name" << "dbAdmin" << "source" << "test"))),
                "test",
                authzManager.get(),
                &parsedRoleObj,
                &parsedWriteConcern));

        rolesArray = parsedRoleObj["roles"].Array();
        ASSERT_EQUALS((size_t)2, rolesArray.size());
        ASSERT_EQUALS("read", rolesArray[0].Obj()["name"].String());
        ASSERT_EQUALS("test", rolesArray[0].Obj()["source"].String());
        ASSERT_EQUALS("dbAdmin", rolesArray[1].Obj()["name"].String());
        ASSERT_EQUALS("test", rolesArray[1].Obj()["source"].String());
    }

}  // namespace
}  // namespace mongo
