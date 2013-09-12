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
        V2PrivilegeDocumentParser v2parser;

        void setUp() {
            user.reset(new User(UserName("spencer", "test")));
            adminUser.reset(new User(UserName("admin", "admin")));
        }
    };


    TEST_F(PrivilegeDocumentParsing, V2DocumentValidation) {
        BSONArray emptyArray = BSONArrayBuilder().arr();

        // V1 documents don't work
        ASSERT_NOT_OK(v2parser.checkValidPrivilegeDocument(
                BSON("user" << "spencer" << "pwd" << "a" <<
                     "roles" << BSON_ARRAY("read"))));

        // Need name field
        ASSERT_NOT_OK(v2parser.checkValidPrivilegeDocument(
                BSON("source" << "test" <<
                     "credentials" << BSON("MONGODB-CR" << "a") <<
                     "roles" << emptyArray)));

        // Need source field
        ASSERT_NOT_OK(v2parser.checkValidPrivilegeDocument(
                BSON("name" << "spencer" <<
                     "credentials" << BSON("MONGODB-CR" << "a") <<
                     "roles" << emptyArray)));

        // Need credentials field
        ASSERT_NOT_OK(v2parser.checkValidPrivilegeDocument(
                BSON("name" << "spencer" <<
                     "source" << "test" <<
                     "roles" << emptyArray)));

        // Need roles field
        ASSERT_NOT_OK(v2parser.checkValidPrivilegeDocument(
                BSON("name" << "spencer" <<
                     "source" << "test" <<
                     "credentials" << BSON("MONGODB-CR" << "a"))));

        // Don't need credentials field if userSource is $external
        ASSERT_OK(v2parser.checkValidPrivilegeDocument(
                BSON("name" << "spencer" <<
                     "source" << "$external" <<
                     "roles" << emptyArray)));

        // Empty roles arrays are OK
        ASSERT_OK(v2parser.checkValidPrivilegeDocument(
                BSON("name" << "spencer" <<
                     "source" << "test" <<
                     "credentials" << BSON("MONGODB-CR" << "a") <<
                     "roles" << emptyArray)));

        // Roles must be objects
        ASSERT_NOT_OK(v2parser.checkValidPrivilegeDocument(
                BSON("name" << "spencer" <<
                     "source" << "test" <<
                     "credentials" << BSON("MONGODB-CR" << "a") <<
                     "roles" << BSON_ARRAY("read"))));

        // Role needs name
        ASSERT_NOT_OK(v2parser.checkValidPrivilegeDocument(
                BSON("name" << "spencer" <<
                     "source" << "test" <<
                     "credentials" << BSON("MONGODB-CR" << "a") <<
                     "roles" << BSON_ARRAY(BSON("source" << "dbA" <<
                                                "canDelegate" << true <<
                                                "hasRole" << true)))));

        // Role needs source
        ASSERT_NOT_OK(v2parser.checkValidPrivilegeDocument(
                BSON("name" << "spencer" <<
                     "source" << "test" <<
                     "credentials" << BSON("MONGODB-CR" << "a") <<
                     "roles" << BSON_ARRAY(BSON("name" << "roleA" <<
                                                "canDelegate" << true <<
                                                "hasRole" << true)))));

        // Role needs canDelegate
        ASSERT_NOT_OK(v2parser.checkValidPrivilegeDocument(
                BSON("name" << "spencer" <<
                     "source" << "test" <<
                     "credentials" << BSON("MONGODB-CR" << "a") <<
                     "roles" << BSON_ARRAY(BSON("name" << "roleA" <<
                                                "source" << "dbA" <<
                                                "hasRole" << true)))));

        // Role needs hasRole
        ASSERT_NOT_OK(v2parser.checkValidPrivilegeDocument(
                BSON("name" << "spencer" <<
                     "source" << "test" <<
                     "credentials" << BSON("MONGODB-CR" << "a") <<
                     "roles" << BSON_ARRAY(BSON("name" << "roleA" <<
                                                "source" << "dbA" <<
                                                "canDelegate" << true)))));


        // Basic valid privilege document
        ASSERT_OK(v2parser.checkValidPrivilegeDocument(
                BSON("name" << "spencer" <<
                     "source" << "test" <<
                     "credentials" << BSON("MONGODB-CR" << "a") <<
                     "roles" << BSON_ARRAY(BSON("name" << "roleA" <<
                                                "source" << "dbA" <<
                                                "canDelegate" << true <<
                                                "hasRole" << true)))));

        // Multiple roles OK
        ASSERT_OK(v2parser.checkValidPrivilegeDocument(
                BSON("name" << "spencer" <<
                     "source" << "test" <<
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
        ASSERT_OK(v2parser.checkValidPrivilegeDocument(
                BSON("name" << "spencer" <<
                     "source" << "test" <<
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
                BSON("name" << "spencer" <<
                     "source" << "test" <<
                     "pwd" << "")));

        // Credentials must be provided (so long as userSource is not $external)
        ASSERT_NOT_OK(v2parser.initializeUserCredentialsFromPrivilegeDocument(
                user.get(),
                BSON("name" << "spencer" <<
                     "source" << "test")));

        // Credentials must be object
        ASSERT_NOT_OK(v2parser.initializeUserCredentialsFromPrivilegeDocument(
                user.get(),
                BSON("name" << "spencer" <<
                     "source" << "test" <<
                     "credentials" << "a")));

        // Must specify credentials for MONGODB-CR
        ASSERT_NOT_OK(v2parser.initializeUserCredentialsFromPrivilegeDocument(
                user.get(),
                BSON("name" << "spencer" <<
                     "source" << "test" <<
                     "credentials" << BSON("foo" << "bar"))));

        // Make sure extracting valid credentials works
        ASSERT_OK(v2parser.initializeUserCredentialsFromPrivilegeDocument(
                user.get(),
                BSON("name" << "spencer" <<
                     "source" << "test" <<
                     "credentials" << BSON("MONGODB-CR" << "a"))));
        ASSERT(user->getCredentials().password == "a");
        ASSERT(!user->getCredentials().isExternal);

        // Leaving out 'credentials' field is OK so long as userSource is $external
        ASSERT_OK(v2parser.initializeUserCredentialsFromPrivilegeDocument(
                user.get(),
                BSON("name" << "spencer" <<
                     "source" << "$external")));
        ASSERT(user->getCredentials().password.empty());
        ASSERT(user->getCredentials().isExternal);

    }

    TEST_F(PrivilegeDocumentParsing, V2RoleExtraction) {
        // "roles" field must be provided
        ASSERT_NOT_OK(v2parser.initializeUserRolesFromPrivilegeDocument(
                user.get(),
                BSON("name" << "spencer"),
                "test"));

        // V1-style roles arrays no longer work
        ASSERT_NOT_OK(v2parser.initializeUserRolesFromPrivilegeDocument(
                user.get(),
                BSON("name" << "spencer" <<
                     "roles" << BSON_ARRAY("read")),
                "test"));

        // Roles must have "name", "source", "canDelegate", and "hasRole" fields
        ASSERT_NOT_OK(v2parser.initializeUserRolesFromPrivilegeDocument(
                user.get(),
                BSON("name" << "spencer" <<
                     "roles" << BSON_ARRAY(BSONObj())),
                "test"));

        ASSERT_NOT_OK(v2parser.initializeUserRolesFromPrivilegeDocument(
                user.get(),
                BSON("name" << "spencer" <<
                     "roles" << BSON_ARRAY(BSON("name" << "roleA"))),
                "test"));

        ASSERT_NOT_OK(v2parser.initializeUserRolesFromPrivilegeDocument(
                user.get(),
                BSON("name" << "spencer" <<
                     "roles" << BSON_ARRAY(BSON("name" << "roleA" << "source" << "dbA"))),
                "test"));
        ASSERT_NOT_OK(v2parser.initializeUserRolesFromPrivilegeDocument(
                user.get(),
                BSON("name" << "spencer" <<
                     "roles" << BSON_ARRAY(BSON("name" << "roleA" <<
                                                "source" << "dbA" <<
                                                "canDelegate" << true))),
                "test"));

        // Role doesn't get added if hasRole is false
        ASSERT_OK(v2parser.initializeUserRolesFromPrivilegeDocument(
                user.get(),
                BSON("name" << "spencer" <<
                     "roles" << BSON_ARRAY(BSON("name" << "roleA" <<
                                                "source" << "dbA" <<
                                                "canDelegate" << true <<
                                                "hasRole" << false))),
                "test"));
        const User::RoleDataMap& roles = user->getRoles();
        ASSERT_EQUALS(1U, roles.size());
        User::RoleData role = roles.begin()->second;
        ASSERT_EQUALS(RoleName("roleA", "dbA"), role.name);
        ASSERT(!role.hasRole);
        ASSERT(role.canDelegate);

        // Valid role names are extracted successfully
        ASSERT_OK(v2parser.initializeUserRolesFromPrivilegeDocument(
                user.get(),
                BSON("name" << "spencer" <<
                     "roles" << BSON_ARRAY(BSON("name" << "roleA" <<
                                                "source" << "dbA" <<
                                                "canDelegate" << true <<
                                                "hasRole" << true))),
                "test"));
        const User::RoleDataMap& roles2 = user->getRoles();
        ASSERT_EQUALS(1U, roles2.size());
        role = roles2.begin()->second;
        ASSERT_EQUALS(RoleName("roleA", "dbA"), role.name);
        ASSERT(role.hasRole);
        ASSERT(role.canDelegate);

        // Multiple roles OK
        ASSERT_OK(v2parser.initializeUserRolesFromPrivilegeDocument(
                user.get(),
                BSON("name" << "spencer" <<
                     "roles" << BSON_ARRAY(BSON("name" << "roleA" <<
                                                "source" << "dbA" <<
                                                "canDelegate" << true <<
                                                "hasRole" << true) <<
                                           BSON("name" << "roleB" <<
                                                "source" << "dbB" <<
                                                "canDelegate" << false <<
                                                "hasRole" << true))),
                "test"));
        const User::RoleDataMap& roles3 = user->getRoles();
        ASSERT_EQUALS(2U, roles3.size());
        role = roles3.find(RoleName("roleA", "dbA"))->second;
        ASSERT_EQUALS(RoleName("roleA", "dbA"), role.name);
        ASSERT(role.hasRole);
        ASSERT(role.canDelegate);
        role = roles3.find(RoleName("roleB", "dbB"))->second;
        ASSERT_EQUALS(RoleName("roleB", "dbB"), role.name);
        ASSERT(role.hasRole);
        ASSERT(!role.canDelegate);
    }

}  // namespace
}  // namespace mongo
