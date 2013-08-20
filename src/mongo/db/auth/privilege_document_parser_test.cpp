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
                     "roles" << emptyArray)));

        // Need userSource field
        ASSERT_NOT_OK(v2parser.checkValidPrivilegeDocument("test",
                BSON("user" << "spencer" <<
                     "credentials" << BSON("MONGODB-CR" << "a") <<
                     "roles" << emptyArray)));

        // Need credentials field
        ASSERT_NOT_OK(v2parser.checkValidPrivilegeDocument("test",
                BSON("user" << "spencer" <<
                     "userSource" << "test" <<
                     "roles" << emptyArray)));

        // Need roles field
        ASSERT_NOT_OK(v2parser.checkValidPrivilegeDocument("test",
                BSON("user" << "spencer" <<
                     "userSource" << "test" <<
                     "credentials" << BSON("MONGODB-CR" << "a"))));


        // db the user command is run on must match the userSource
        ASSERT_NOT_OK(v2parser.checkValidPrivilegeDocument("test",
                BSON("user" << "spencer" <<
                     "userSource" << "test2" <<
                     "credentials" << BSON("MONGODB-CR" << "a") <<
                     "roles" << emptyArray)));

        // Don't need credentials field if userSource is $external
        ASSERT_OK(v2parser.checkValidPrivilegeDocument("$external",
                BSON("user" << "spencer" <<
                     "userSource" << "$external" <<
                     "roles" << emptyArray)));

        // Empty roles arrays are OK
        ASSERT_OK(v2parser.checkValidPrivilegeDocument("test",
                BSON("user" << "spencer" <<
                     "userSource" << "test" <<
                     "credentials" << BSON("MONGODB-CR" << "a") <<
                     "roles" << emptyArray)));

        // Roles must be objects
        ASSERT_NOT_OK(v2parser.checkValidPrivilegeDocument("test",
                BSON("user" << "spencer" <<
                     "userSource" << "test" <<
                     "credentials" << BSON("MONGODB-CR" << "a") <<
                     "roles" << BSON_ARRAY("read"))));

        // Role needs name
        ASSERT_NOT_OK(v2parser.checkValidPrivilegeDocument("test",
                BSON("user" << "spencer" <<
                     "userSource" << "test" <<
                     "credentials" << BSON("MONGODB-CR" << "a") <<
                     "roles" << BSON_ARRAY(BSON("source" << "dbA" <<
                                                "canDelegate" << true <<
                                                "hasRole" << true)))));

        // Role needs source
        ASSERT_NOT_OK(v2parser.checkValidPrivilegeDocument("test",
                BSON("user" << "spencer" <<
                     "userSource" << "test" <<
                     "credentials" << BSON("MONGODB-CR" << "a") <<
                     "roles" << BSON_ARRAY(BSON("name" << "roleA" <<
                                                "canDelegate" << true <<
                                                "hasRole" << true)))));

        // Role needs canDelegate
        ASSERT_NOT_OK(v2parser.checkValidPrivilegeDocument("test",
                BSON("user" << "spencer" <<
                     "userSource" << "test" <<
                     "credentials" << BSON("MONGODB-CR" << "a") <<
                     "roles" << BSON_ARRAY(BSON("name" << "roleA" <<
                                                "source" << "dbA" <<
                                                "hasRole" << true)))));

        // Role needs hasRole
        ASSERT_NOT_OK(v2parser.checkValidPrivilegeDocument("test",
                BSON("user" << "spencer" <<
                     "userSource" << "test" <<
                     "credentials" << BSON("MONGODB-CR" << "a") <<
                     "roles" << BSON_ARRAY(BSON("name" << "roleA" <<
                                                "source" << "dbA" <<
                                                "canDelegate" << true)))));


        // Basic valid privilege document
        ASSERT_OK(v2parser.checkValidPrivilegeDocument("test",
                BSON("user" << "spencer" <<
                     "userSource" << "test" <<
                     "credentials" << BSON("MONGODB-CR" << "a") <<
                     "roles" << BSON_ARRAY(BSON("name" << "roleA" <<
                                                "source" << "dbA" <<
                                                "canDelegate" << true <<
                                                "hasRole" << true)))));

        // Multiple roles OK
        ASSERT_OK(v2parser.checkValidPrivilegeDocument("test",
                BSON("user" << "spencer" <<
                     "userSource" << "test" <<
                     "credentials" << BSON("MONGODB-CR" << "a") <<
                     "roles" << BSON_ARRAY(BSON("name" << "roleA" <<
                                                "source" << "dbA" <<
                                                "canDelegate" << true <<
                                                "hasRole" << true) <<
                                           BSON("name" << "roleB" <<
                                                "source" << "dbB" <<
                                                "canDelegate" << true <<
                                                "hasRole" << true)))));

        // Optional extraData field OK
        ASSERT_OK(v2parser.checkValidPrivilegeDocument("test",
                BSON("user" << "spencer" <<
                     "userSource" << "test" <<
                     "credentials" << BSON("MONGODB-CR" << "a") <<
                     "extraData" << BSON("foo" << "bar") <<
                     "roles" << BSON_ARRAY(BSON("name" << "roleA" <<
                                                "source" << "dbA" <<
                                                "canDelegate" << true <<
                                                "hasRole" << true)))));
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

    TEST_F(PrivilegeDocumentParsing, V2RoleExtraction) {
        // "roles" field must be provided
        ASSERT_NOT_OK(v2parser.initializeUserRolesFromPrivilegeDocument(
                user.get(),
                BSON("user" << "spencer"),
                "test"));

        // V1-style roles arrays no longer work
        ASSERT_NOT_OK(v2parser.initializeUserRolesFromPrivilegeDocument(
                user.get(),
                BSON("user" << "spencer" <<
                     "roles" << BSON_ARRAY("read")),
                "test"));

        // Roles must have "name", "source", "canDelegate", and "hasRole" fields
        ASSERT_NOT_OK(v2parser.initializeUserRolesFromPrivilegeDocument(
                user.get(),
                BSON("user" << "spencer" <<
                     "roles" << BSON_ARRAY(BSONObj())),
                "test"));

        ASSERT_NOT_OK(v2parser.initializeUserRolesFromPrivilegeDocument(
                user.get(),
                BSON("user" << "spencer" <<
                     "roles" << BSON_ARRAY(BSON("name" << "roleA"))),
                "test"));

        ASSERT_NOT_OK(v2parser.initializeUserRolesFromPrivilegeDocument(
                user.get(),
                BSON("user" << "spencer" <<
                     "roles" << BSON_ARRAY(BSON("name" << "roleA" << "source" << "dbA"))),
                "test"));
        ASSERT_NOT_OK(v2parser.initializeUserRolesFromPrivilegeDocument(
                user.get(),
                BSON("user" << "spencer" <<
                     "roles" << BSON_ARRAY(BSON("name" << "roleA" <<
                                                "source" << "dbA" <<
                                                "canDelegate" << true))),
                "test"));

        // Role doesn't get added if hasRole is false
        ASSERT_OK(v2parser.initializeUserRolesFromPrivilegeDocument(
                user.get(),
                BSON("user" << "spencer" <<
                     "roles" << BSON_ARRAY(BSON("name" << "roleA" <<
                                                "source" << "dbA" <<
                                                "canDelegate" << true <<
                                                "hasRole" << false))),
                "test"));
        RoleNameIterator nameIt = user->getRoles();
        ASSERT(!nameIt.more());

        // Valid role names are extracted successfully
        ASSERT_OK(v2parser.initializeUserRolesFromPrivilegeDocument(
                user.get(),
                BSON("user" << "spencer" <<
                     "roles" << BSON_ARRAY(BSON("name" << "roleA" <<
                                                "source" << "dbA" <<
                                                "canDelegate" << true <<
                                                "hasRole" << true))),
                "test"));
        nameIt = user->getRoles();
        ASSERT(nameIt.more());
        ASSERT(nameIt.next() == RoleName("roleA", "dbA"));
        ASSERT(!nameIt.more());

        // Multiple roles OK
        ASSERT_OK(v2parser.initializeUserRolesFromPrivilegeDocument(
                user.get(),
                BSON("user" << "spencer" <<
                     "roles" << BSON_ARRAY(BSON("name" << "roleA" <<
                                                "source" << "dbA" <<
                                                "canDelegate" << true <<
                                                "hasRole" << true) <<
                                           BSON("name" << "roleB" <<
                                                "source" << "dbB" <<
                                                "canDelegate" << true <<
                                                "hasRole" << true))),
                "test"));
        nameIt = user->getRoles();
        ASSERT(nameIt.more());
        RoleName firstRole = nameIt.next();
        if (firstRole == RoleName("roleA", "dbA")) {
            ASSERT(nameIt.more());
            ASSERT(nameIt.next() == RoleName("roleB", "dbB"));
        } else if (firstRole == RoleName("roleB", "dbB")) {
            ASSERT(nameIt.more());
            ASSERT(nameIt.next() == RoleName("roleA", "dbA"));
        } else {
            ASSERT(false);
        }
        ASSERT(!nameIt.more());
    }

}  // namespace
}  // namespace mongo
