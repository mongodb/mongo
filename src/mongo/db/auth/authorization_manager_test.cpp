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
 * Unit tests of the AuthorizationManager type.
 */

#include "mongo/base/status.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authz_session_external_state_mock.h"
#include "mongo/db/auth/authz_manager_external_state_mock.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/map_util.h"

#define ASSERT_NULL(EXPR) ASSERT_FALSE(EXPR)
#define ASSERT_NON_NULL(EXPR) ASSERT_TRUE(EXPR)

namespace mongo {
namespace {

    class AuthorizationManagerTest : public ::mongo::unittest::Test {
    public:
        virtual ~AuthorizationManagerTest() {
            authzManager->invalidateUserCache();
        }

        void setUp() {
            externalState = new AuthzManagerExternalStateMock();
            authzManager.reset(new AuthorizationManager(externalState));
            // This duplicates the behavior from the server that adds the internal user at process
            // startup via a MONGO_INITIALIZER
            authzManager->addInternalUser(internalSecurity.user);
        }

        scoped_ptr<AuthorizationManager> authzManager;
        AuthzManagerExternalStateMock* externalState;
    };

    class V1PrivilegeDocumentParsing : public AuthorizationManagerTest {
    public:
        V1PrivilegeDocumentParsing() {}

        scoped_ptr<User> user;
        scoped_ptr<User> adminUser;

        void setUp() {
            AuthorizationManagerTest::setUp();
            user.reset(new User(UserName("spencer", "test")));
            adminUser.reset(new User(UserName("admin", "admin")));
            authzManager->setAuthorizationVersion(1);
        }
    };

    TEST_F(V1PrivilegeDocumentParsing, testParsingV0PrivilegeDocuments) {
        User user(UserName("Spencer", "test"));
        User adminUser(UserName("Spencer", "admin"));
        BSONObj invalid;
        BSONObj readWrite = BSON("user" << "Spencer" << "pwd" << "passwordHash");
        BSONObj readOnly = BSON("user" << "Spencer" << "pwd" << "passwordHash" <<
                                "readOnly" << true);

        ASSERT_NOT_OK(authzManager->_initializeUserFromPrivilegeDocument(&user, invalid));

        ASSERT_OK(authzManager->_initializeUserFromPrivilegeDocument(&user, readOnly));
        ASSERT(user.getActionsForResource("test").contains(ActionType::find));
        ASSERT(!user.getActionsForResource("test").contains(ActionType::insert));

        ASSERT_OK(authzManager->_initializeUserFromPrivilegeDocument(&user, readWrite));
        ASSERT(user.getActionsForResource("test").contains(ActionType::find));
        ASSERT(user.getActionsForResource("test").contains(ActionType::insert));
        ASSERT(user.getActionsForResource("test").contains(ActionType::userAdmin));
        ASSERT(user.getActionsForResource("test").contains(ActionType::compact));
        ASSERT(!user.getActionsForResource("test").contains(ActionType::shutdown));
        ASSERT(!user.getActionsForResource("test").contains(ActionType::addShard));
        ASSERT(!user.getActionsForResource("admin").contains(ActionType::find));
        ASSERT(!user.getActionsForResource("*").contains(ActionType::find));

        ASSERT_OK(authzManager->_initializeUserFromPrivilegeDocument(&adminUser, readOnly));
        ASSERT(adminUser.getActionsForResource("*").contains(ActionType::find));
        ASSERT(!adminUser.getActionsForResource("admin").contains(ActionType::insert));
        ASSERT(!adminUser.getActionsForResource("*").contains(ActionType::insert));

        ASSERT_OK(authzManager->_initializeUserFromPrivilegeDocument(&adminUser, readWrite));
        ASSERT(adminUser.getActionsForResource("*").contains(ActionType::find));
        ASSERT(adminUser.getActionsForResource("*").contains(ActionType::insert));
    }

    TEST_F(V1PrivilegeDocumentParsing, VerifyRolesFieldMustBeAnArray) {
        ASSERT_NOT_OK(authzManager->_initializeUserFromPrivilegeDocument(
                user.get(),
                BSON("user" << "spencer" << "pwd" << "" << "roles" << "read")));
        ASSERT(user->getActionsForResource("test").empty());
    }

    TEST_F(V1PrivilegeDocumentParsing, VerifyInvalidRoleGrantsNoPrivileges) {
        ASSERT_OK(authzManager->_initializeUserFromPrivilegeDocument(
                user.get(),
                BSON("user" << "spencer" << "pwd" << "" << "roles" << BSON_ARRAY("frim"))));
        ASSERT(user->getActionsForResource("test").empty());
    }

    TEST_F(V1PrivilegeDocumentParsing, VerifyInvalidRoleStillAllowsOtherRoles) {
        ASSERT_OK(authzManager->_initializeUserFromPrivilegeDocument(
                user.get(),
                BSON("user" << "spencer" <<
                     "pwd" << "" <<
                     "roles" << BSON_ARRAY("read" << "frim"))));
        ASSERT(user->getActionsForResource("test").contains(ActionType::find));
    }

    TEST_F(V1PrivilegeDocumentParsing, VerifyCannotGrantClusterAdminRoleFromNonAdminDatabase) {
        ASSERT_OK(authzManager->_initializeUserFromPrivilegeDocument(
                user.get(),
                BSON("user" << "spencer" <<
                     "pwd" << "" <<
                     "roles" << BSON_ARRAY("read" << "clusterAdmin"))));
        ASSERT(user->getActionsForResource("test").contains(ActionType::find));
        ASSERT(!user->getActionsForResource("test").contains(ActionType::shutdown));
        ASSERT(!user->getActionsForResource("test").contains(ActionType::dropDatabase));
    }

    TEST_F(V1PrivilegeDocumentParsing, VerifyCannotGrantClusterReadFromNonAdminDatabase) {
        ASSERT_OK(authzManager->_initializeUserFromPrivilegeDocument(
                user.get(),
                BSON("user" << "spencer" <<
                     "pwd" << "" <<
                     "roles" << BSON_ARRAY("read" << "readAnyDatabase"))));
        ASSERT(user->getActionsForResource("test").contains(ActionType::find));
        ASSERT(!user->getActionsForResource("test2").contains(ActionType::find));
    }

    TEST_F(V1PrivilegeDocumentParsing, VerifyCannotGrantClusterReadWriteFromNonAdminDatabase) {
        ASSERT_OK(authzManager->_initializeUserFromPrivilegeDocument(
                user.get(),
                BSON("user" << "spencer" <<
                     "pwd" << "" <<
                     "roles" << BSON_ARRAY("read" << "readWriteAnyDatabase"))));
        ASSERT(user->getActionsForResource("test").contains(ActionType::find));
        ASSERT(!user->getActionsForResource("test").contains(ActionType::insert));
        ASSERT(!user->getActionsForResource("test2").contains(ActionType::insert));
    }

    TEST_F(V1PrivilegeDocumentParsing, VerifyCannotGrantClusterUserAdminFromNonAdminDatabase) {
        ASSERT_OK(authzManager->_initializeUserFromPrivilegeDocument(
                user.get(),
                BSON("user" << "spencer" <<
                     "pwd" << "" <<
                     "roles" << BSON_ARRAY("read" << "userAdminAnyDatabase"))));
        ASSERT(user->getActionsForResource("test").contains(ActionType::find));
        ASSERT(!user->getActionsForResource("test").contains(ActionType::userAdmin));
        ASSERT(!user->getActionsForResource("test2").contains(ActionType::userAdmin));
    }

    TEST_F(V1PrivilegeDocumentParsing, VerifyCannotGrantClusterDBAdminFromNonAdminDatabase) {
        ASSERT_OK(authzManager->_initializeUserFromPrivilegeDocument(
                user.get(),
                BSON("user" << "spencer" <<
                     "pwd" << "" <<
                     "roles" << BSON_ARRAY("read" << "dbAdminAnyDatabase"))));
        ASSERT(user->getActionsForResource("test").contains(ActionType::find));
        ASSERT(!user->getActionsForResource("test").contains(ActionType::clean));
        ASSERT(!user->getActionsForResource("test2").contains(ActionType::clean));
    }

    TEST_F(V1PrivilegeDocumentParsing, VerifyOtherDBRolesMustBeAnObjectOfArraysOfStrings) {
        ASSERT_NOT_OK(authzManager->_initializeUserFromPrivilegeDocument(
                adminUser.get(),
                BSON("user" << "admin" <<
                     "pwd" << "" <<
                     "roles" << BSON_ARRAY("read") <<
                     "otherDBRoles" << BSON_ARRAY("read"))));
        ASSERT(!adminUser->getActionsForResource("test").contains(ActionType::find));
        ASSERT(!adminUser->getActionsForResource("test2").contains(ActionType::find));
        ASSERT(!adminUser->getActionsForResource("admin").contains(ActionType::find));

        ASSERT_NOT_OK(authzManager->_initializeUserFromPrivilegeDocument(
                adminUser.get(),
                BSON("user" << "admin" <<
                     "pwd" << "" <<
                     "roles" << BSON_ARRAY("read") <<
                     "otherDBRoles" << BSON("test2" << "read"))));
        ASSERT(!adminUser->getActionsForResource("test").contains(ActionType::find));
        ASSERT(!adminUser->getActionsForResource("test2").contains(ActionType::find));
        ASSERT(!adminUser->getActionsForResource("admin").contains(ActionType::find));
    }

    TEST_F(V1PrivilegeDocumentParsing, VerifyCannotGrantPrivilegesOnOtherDatabasesNormally) {
        // Cannot grant privileges on other databases, except from admin database.
        ASSERT_NOT_OK(authzManager->_initializeUserFromPrivilegeDocument(
                user.get(),
                BSON("user" << "spencer" <<
                     "pwd" << "" <<
                     "roles" << BSON_ARRAY("read") <<
                     "otherDBRoles" << BSON("test2" << BSON_ARRAY("read")))));
        ASSERT(!user->getActionsForResource("test").contains(ActionType::find));
        ASSERT(!user->getActionsForResource("test2").contains(ActionType::find));
        ASSERT(!user->getActionsForResource("admin").contains(ActionType::find));
    }

    TEST_F(V1PrivilegeDocumentParsing, SuccessfulSimpleReadGrant) {
        // Grant read on test.
        ASSERT_OK(authzManager->_initializeUserFromPrivilegeDocument(
                user.get(),
                BSON("user" << "spencer" <<
                     "pwd" << "" <<
                     "roles" << BSON_ARRAY("read"))));
        ASSERT(user->getActionsForResource("test").contains(ActionType::find));
        ASSERT(!user->getActionsForResource("test2").contains(ActionType::find));
        ASSERT(!user->getActionsForResource("admin").contains(ActionType::find));
    }

    TEST_F(V1PrivilegeDocumentParsing, SuccessfulSimpleUserAdminTest) {
        // Grant userAdmin on "test" database.
        ASSERT_OK(authzManager->_initializeUserFromPrivilegeDocument(
                user.get(),
                BSON("user" << "spencer" <<
                     "pwd" << "" <<
                     "roles" << BSON_ARRAY("userAdmin"))));
        ASSERT(user->getActionsForResource("test").contains(ActionType::userAdmin));
        ASSERT(!user->getActionsForResource("test2").contains(ActionType::userAdmin));
        ASSERT(!user->getActionsForResource("admin").contains(ActionType::userAdmin));
    }

    TEST_F(V1PrivilegeDocumentParsing, GrantUserAdminOnAdmin) {
        // Grant userAdmin on admin.
        ASSERT_OK(authzManager->_initializeUserFromPrivilegeDocument(
                adminUser.get(),
                BSON("user" << "admin" <<
                     "pwd" << "" <<
                     "roles" << BSON_ARRAY("userAdmin"))));
        ASSERT(!adminUser->getActionsForResource("test").contains(ActionType::userAdmin));
        ASSERT(!adminUser->getActionsForResource("test2").contains(ActionType::userAdmin));
        ASSERT(adminUser->getActionsForResource("admin").contains(ActionType::userAdmin));
    }

    TEST_F(V1PrivilegeDocumentParsing, GrantUserAdminOnTestViaAdmin) {
        // Grant userAdmin on test via admin.
        ASSERT_OK(authzManager->_initializeUserFromPrivilegeDocument(
                adminUser.get(),
                BSON("user" << "admin" <<
                     "pwd" << "" <<
                     "roles" << BSONArrayBuilder().arr() <<
                     "otherDBRoles" << BSON("test" << BSON_ARRAY("userAdmin")))));
        ASSERT(adminUser->getActionsForResource("test").contains(ActionType::userAdmin));
        ASSERT(!adminUser->getActionsForResource("test2").contains(ActionType::userAdmin));
        ASSERT(!adminUser->getActionsForResource("admin").contains(ActionType::userAdmin));
    }

    TEST_F(V1PrivilegeDocumentParsing, SuccessfulClusterAdminTest) {
        // Grant userAdminAnyDatabase.
        ASSERT_OK(authzManager->_initializeUserFromPrivilegeDocument(
                adminUser.get(),
                BSON("user" << "admin" <<
                     "pwd" << "" <<
                     "roles" << BSON_ARRAY("userAdminAnyDatabase"))));
        ASSERT(adminUser->getActionsForResource("*").contains(ActionType::userAdmin));
    }


    TEST_F(V1PrivilegeDocumentParsing, GrantClusterReadWrite) {
        // Grant readWrite on everything via the admin database.
        ASSERT_OK(authzManager->_initializeUserFromPrivilegeDocument(
                adminUser.get(),
                BSON("user" << "admin" <<
                     "pwd" << "" <<
                     "roles" << BSON_ARRAY("readWriteAnyDatabase"))));
        ASSERT(adminUser->getActionsForResource("*").contains(ActionType::find));
        ASSERT(adminUser->getActionsForResource("*").contains(ActionType::insert));
    }

    TEST_F(V1PrivilegeDocumentParsing, ProhibitGrantOnWildcard) {
        // Cannot grant readWrite to everything using "otherDBRoles".
        ASSERT_NOT_OK(authzManager->_initializeUserFromPrivilegeDocument(
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

    TEST_F(V1PrivilegeDocumentParsing, GrantClusterAdmin) {
        // Grant cluster admin
        ASSERT_OK(authzManager->_initializeUserFromPrivilegeDocument(
                adminUser.get(),
                BSON("user" << "admin" <<
                     "pwd" << "" <<
                     "roles" << BSON_ARRAY("clusterAdmin"))));
        ASSERT(adminUser->getActionsForResource("*").contains(ActionType::dropDatabase));
        ASSERT(adminUser->getActionsForResource("*").contains(ActionType::shutdown));
        ASSERT(adminUser->getActionsForResource("*").contains(ActionType::moveChunk));
    }

    TEST_F(V1PrivilegeDocumentParsing, GetPrivilegesFromPrivilegeDocumentInvalid) {
        // Try to mix fields from V0 and V1 privilege documents and make sure it fails.
        ASSERT_NOT_OK(authzManager->_initializeUserFromPrivilegeDocument(
                user.get(),
                BSON("user" << "spencer" <<
                     "pwd" << "passwordHash" <<
                     "readOnly" << false <<
                     "roles" << BSON_ARRAY("read"))));
        ASSERT(!adminUser->getActionsForResource("test").contains(ActionType::find));
    }



    TEST_F(AuthorizationManagerTest, testAquireV0User) {
        authzManager->setAuthorizationVersion(1);

        ASSERT_OK(externalState->insert(NamespaceString("test.system.users"),
                                        BSON("user" << "v0RW" << "pwd" << "password"),
                                        BSONObj()));
        ASSERT_OK(externalState->insert(NamespaceString("admin.system.users"),
                                        BSON("user" << "v0AdminRO" <<
                                             "pwd" << "password" <<
                                             "readOnly" << true),
                                        BSONObj()));

        User* v0RW;
        ASSERT_OK(authzManager->acquireUser(UserName("v0RW", "test"), &v0RW));
        ASSERT(UserName("v0RW", "test") == v0RW->getName());
        ASSERT(v0RW->isValid());
        ASSERT_EQUALS((uint32_t)1, v0RW->getRefCount());
        const User::RoleDataMap& roles = v0RW->getRoles();
        ASSERT_EQUALS(1U, roles.size());
        User::RoleData role = roles.begin()->second;
        ASSERT_EQUALS("test", role.name.getDB());
        ASSERT_EQUALS("oldReadWrite", role.name.getRole());
        ASSERT(role.hasRole);
        ASSERT(!role.canDelegate);
        ActionSet actions = v0RW->getActionsForResource("test");
        ASSERT(actions.contains(ActionType::find));
        ASSERT(actions.contains(ActionType::insert));
        ASSERT_FALSE(actions.contains(ActionType::shutdown));
        actions = v0RW->getActionsForResource("test2");
        ASSERT(actions.empty());
        actions = v0RW->getActionsForResource("admin");
        ASSERT(actions.empty());
        // Make sure user's refCount is 0 at the end of the test to avoid an assertion failure
        authzManager->releaseUser(v0RW);

        User* v0AdminRO;
        ASSERT_OK(authzManager->acquireUser(UserName("v0AdminRO", "admin"), &v0AdminRO));
        ASSERT(UserName("v0AdminRO", "admin") == v0AdminRO->getName());
        ASSERT(v0AdminRO->isValid());
        ASSERT_EQUALS((uint32_t)1, v0AdminRO->getRefCount());
        const User::RoleDataMap& adminRoles = v0AdminRO->getRoles();
        ASSERT_EQUALS(1U, adminRoles.size());
        role = adminRoles.begin()->second;
        ASSERT_EQUALS("admin", role.name.getDB());
        ASSERT_EQUALS("oldAdminRead", role.name.getRole());
        ASSERT(role.hasRole);
        ASSERT(!role.canDelegate);
        actions = v0AdminRO->getActionsForResource("*");
        ASSERT(actions.contains(ActionType::find));
        ASSERT(actions.contains(ActionType::listDatabases));
        ASSERT_FALSE(actions.contains(ActionType::insert));
        ASSERT_FALSE(actions.contains(ActionType::dropDatabase));
        // Make sure user's refCount is 0 at the end of the test to avoid an assertion failure
        authzManager->releaseUser(v0AdminRO);
    }

    TEST_F(AuthorizationManagerTest, testAquireV1User) {
        authzManager->setAuthorizationVersion(1);

        ASSERT_OK(externalState->insert(NamespaceString("test.system.users"),
                                        BSON("user" << "v1read" <<
                                             "pwd" << "password" <<
                                             "roles" << BSON_ARRAY("read")),
                                        BSONObj()));
        ASSERT_OK(externalState->insert(NamespaceString("admin.system.users"),
                                        BSON("user" << "v1cluster" <<
                                             "pwd" << "password" <<
                                             "roles" << BSON_ARRAY("clusterAdmin")),
                                        BSONObj()));

        User* v1read;
        ASSERT_OK(authzManager->acquireUser(UserName("v1read", "test"), &v1read));
        ASSERT(UserName("v1read", "test") == v1read->getName());
        ASSERT(v1read->isValid());
        ASSERT_EQUALS((uint32_t)1, v1read->getRefCount());

        const User::RoleDataMap& roles = v1read->getRoles();
        ASSERT_EQUALS(1U, roles.size());
        User::RoleData role = roles.begin()->second;
        ASSERT_EQUALS("test", role.name.getDB());
        ASSERT_EQUALS("read", role.name.getRole());
        ASSERT(role.hasRole);
        ASSERT(!role.canDelegate);
        ActionSet actions = v1read->getActionsForResource("test");
        ASSERT(actions.contains(ActionType::find));
        ASSERT_FALSE(actions.contains(ActionType::insert));
        actions = v1read->getActionsForResource("test2");
        ASSERT(actions.empty());
        actions = v1read->getActionsForResource("admin");
        ASSERT(actions.empty());
        // Make sure user's refCount is 0 at the end of the test to avoid an assertion failure
        authzManager->releaseUser(v1read);

        User* v1cluster;
        ASSERT_OK(authzManager->acquireUser(UserName("v1cluster", "admin"), &v1cluster));
        ASSERT(UserName("v1cluster", "admin") == v1cluster->getName());
        ASSERT(v1cluster->isValid());
        ASSERT_EQUALS((uint32_t)1, v1cluster->getRefCount());
        const User::RoleDataMap& clusterRoles = v1cluster->getRoles();
        ASSERT_EQUALS(1U, clusterRoles.size());
        role = clusterRoles.begin()->second;
        ASSERT_EQUALS("admin", role.name.getDB());
        ASSERT_EQUALS("clusterAdmin", role.name.getRole());
        ASSERT(role.hasRole);
        ASSERT(!role.canDelegate);
        actions = v1cluster->getActionsForResource("*");
        ASSERT(actions.contains(ActionType::listDatabases));
        ASSERT(actions.contains(ActionType::dropDatabase));
        ASSERT_FALSE(actions.contains(ActionType::find));
        // Make sure user's refCount is 0 at the end of the test to avoid an assertion failure
        authzManager->releaseUser(v1cluster);
    }

    TEST_F(AuthorizationManagerTest, initializeAllV1UserData) {
        authzManager->setAuthorizationVersion(1);

        ASSERT_OK(externalState->insert(NamespaceString("test.system.users"),
                                        BSON("user" << "readOnly" <<
                                             "pwd" << "password" <<
                                             "roles" << BSON_ARRAY("read")),
                                        BSONObj()));
        ASSERT_OK(externalState->insert(NamespaceString("admin.system.users"),
                                        BSON("user" << "clusterAdmin" <<
                                             "userSource" << "$external" <<
                                             "roles" << BSON_ARRAY("clusterAdmin")),
                                        BSONObj()));
        ASSERT_OK(externalState->insert(NamespaceString("test.system.users"),
                                        BSON("user" << "readWriteMultiDB" <<
                                             "pwd" << "password" <<
                                             "roles" << BSON_ARRAY("readWrite")),
                                        BSONObj()));
        ASSERT_OK(externalState->insert(NamespaceString("test2.system.users"),
                                        BSON("user" << "readWriteMultiDB" <<
                                             "userSource" << "test" <<
                                             "roles" << BSON_ARRAY("readWrite")),
                                        BSONObj()));

        Status status = authzManager->initialize();
        ASSERT_OK(status);

        User* readOnly;
        ASSERT_OK(authzManager->acquireUser(UserName("readOnly", "test"), &readOnly));
        ASSERT(UserName("readOnly", "test") == readOnly->getName());
        ASSERT(readOnly->isValid());
        ASSERT_EQUALS((uint32_t)2, readOnly->getRefCount());
        const User::RoleDataMap& roles = readOnly->getRoles();
        ASSERT_EQUALS(1U, roles.size());
        User::RoleData role = roles.begin()->second;
        ASSERT_EQUALS("test", role.name.getDB());
        ASSERT_EQUALS("read", role.name.getRole());
        ASSERT(role.hasRole);
        ASSERT(!role.canDelegate);
        ActionSet actions = readOnly->getActionsForResource("test");
        ASSERT(actions.contains(ActionType::find));
        ASSERT_FALSE(actions.contains(ActionType::insert));
        actions = readOnly->getActionsForResource("test2");
        ASSERT(actions.empty());
        actions = readOnly->getActionsForResource("admin");
        ASSERT(actions.empty());
        // Make sure user's refCount is 0 at the end of the test to avoid an assertion failure
        authzManager->releaseUser(readOnly);

        User* clusterAdmin;
        ASSERT_OK(authzManager->acquireUser(UserName("clusterAdmin", "$external"), &clusterAdmin));
        ASSERT(UserName("clusterAdmin", "$external") == clusterAdmin->getName());
        ASSERT(clusterAdmin->isValid());
        ASSERT_EQUALS((uint32_t)2, clusterAdmin->getRefCount());
        const User::RoleDataMap& clusterRoles = clusterAdmin->getRoles();
        ASSERT_EQUALS(1U, clusterRoles.size());
        role = clusterRoles.begin()->second;
        ASSERT_EQUALS("admin", role.name.getDB());
        ASSERT_EQUALS("clusterAdmin", role.name.getRole());
        ASSERT(role.hasRole);
        ASSERT(!role.canDelegate);
        actions = clusterAdmin->getActionsForResource("*");
        ASSERT(actions.contains(ActionType::listDatabases));
        ASSERT(actions.contains(ActionType::dropDatabase));
        ASSERT_FALSE(actions.contains(ActionType::find));
        // Make sure user's refCount is 0 at the end of the test to avoid an assertion failure
        authzManager->releaseUser(clusterAdmin);

        User* multiDB;
        status = authzManager->acquireUser(UserName("readWriteMultiDB", "test2"), &multiDB);
        ASSERT_NOT_OK(status);
        ASSERT(status.code() == ErrorCodes::UserNotFound);

        ASSERT_OK(authzManager->acquireUser(UserName("readWriteMultiDB", "test"), &multiDB));
        ASSERT(UserName("readWriteMultiDB", "test") == multiDB->getName());
        ASSERT(multiDB->isValid());
        ASSERT_EQUALS((uint32_t)2, multiDB->getRefCount());

        const User::RoleDataMap& multiDBRoles = multiDB->getRoles();
        ASSERT_EQUALS(2U, multiDBRoles.size());
        role = multiDBRoles.find(RoleName("readWrite", "test"))->second;
        ASSERT_EQUALS("test", role.name.getDB());
        ASSERT_EQUALS("readWrite", role.name.getRole());
        ASSERT(role.hasRole);
        ASSERT(!role.canDelegate);
        role = multiDBRoles.find(RoleName("readWrite", "test2"))->second;
        ASSERT_EQUALS("test2", role.name.getDB());
        ASSERT_EQUALS("readWrite", role.name.getRole());
        ASSERT(role.hasRole);
        ASSERT(!role.canDelegate);

        actions = multiDB->getActionsForResource("test");
        ASSERT(actions.contains(ActionType::find));
        ASSERT(actions.contains(ActionType::insert));
        ASSERT_FALSE(actions.contains(ActionType::shutdown));
        actions = multiDB->getActionsForResource("test2");
        ASSERT(actions.contains(ActionType::find));
        ASSERT(actions.contains(ActionType::insert));
        ASSERT_FALSE(actions.contains(ActionType::shutdown));
        actions = multiDB->getActionsForResource("admin");
        ASSERT(actions.empty());
        // Make sure user's refCount is 0 at the end of the test to avoid an assertion failure
        authzManager->releaseUser(multiDB);


        // initializeAllV1UserData() pins the users by adding 1 to their refCount, so need to
        // release each user an extra time to bring their refCounts to 0.
        authzManager->releaseUser(readOnly);
        authzManager->releaseUser(clusterAdmin);
        authzManager->releaseUser(multiDB);
    }


    TEST_F(AuthorizationManagerTest, testAquireV2User) {
        authzManager->setAuthorizationVersion(2);

        ASSERT_OK(externalState->insertPrivilegeDocument(
                "admin",
                BSON("name" << "v2read" <<
                     "source" << "test" <<
                     "credentials" << BSON("MONGODB-CR" << "password") <<
                     "roles" << BSON_ARRAY(BSON("name" << "read" <<
                                                "source" << "test" <<
                                                "canDelegate" << false <<
                                                "hasRole" << true))),
                BSONObj()));
        ASSERT_OK(externalState->insertPrivilegeDocument(
                "admin",
                BSON("name" << "v2cluster" <<
                     "source" << "admin" <<
                     "credentials" << BSON("MONGODB-CR" << "password") <<
                     "roles" << BSON_ARRAY(BSON("name" << "clusterAdmin" <<
                                                "source" << "admin" <<
                                                "canDelegate" << true <<
                                                "hasRole" << true))),
                BSONObj()));

        User* v2read;
        ASSERT_OK(authzManager->acquireUser(UserName("v2read", "test"), &v2read));
        ASSERT(UserName("v2read", "test") == v2read->getName());
        ASSERT(v2read->isValid());
        ASSERT_EQUALS((uint32_t)1, v2read->getRefCount());
        const User::RoleDataMap& roles = v2read->getRoles();
        ASSERT_EQUALS(1U, roles.size());
        User::RoleData role = roles.begin()->second;
        ASSERT_EQUALS("test", role.name.getDB());
        ASSERT_EQUALS("read", role.name.getRole());
        ASSERT(role.hasRole);
        ASSERT(!role.canDelegate);
        ActionSet actions = v2read->getActionsForResource("test");
        ASSERT(actions.contains(ActionType::find));
        ASSERT_FALSE(actions.contains(ActionType::insert));
        actions = v2read->getActionsForResource("test2");
        ASSERT(actions.empty());
        actions = v2read->getActionsForResource("admin");
        ASSERT(actions.empty());
        // Make sure user's refCount is 0 at the end of the test to avoid an assertion failure
        authzManager->releaseUser(v2read);

        User* v2cluster;
        ASSERT_OK(authzManager->acquireUser(UserName("v2cluster", "admin"), &v2cluster));
        ASSERT(UserName("v2cluster", "admin") == v2cluster->getName());
        ASSERT(v2cluster->isValid());
        ASSERT_EQUALS((uint32_t)1, v2cluster->getRefCount());
        const User::RoleDataMap& clusterRoles = v2cluster->getRoles();
        ASSERT_EQUALS(1U, clusterRoles.size());
        role = clusterRoles.begin()->second;
        ASSERT_EQUALS("admin", role.name.getDB());
        ASSERT_EQUALS("clusterAdmin", role.name.getRole());
        ASSERT(role.hasRole);
        ASSERT(role.canDelegate);
        actions = v2cluster->getActionsForResource("*");
        ASSERT(actions.contains(ActionType::listDatabases));
        ASSERT(actions.contains(ActionType::dropDatabase));
        ASSERT_FALSE(actions.contains(ActionType::find));
        // Make sure user's refCount is 0 at the end of the test to avoid an assertion failure
        authzManager->releaseUser(v2cluster);
    }

    class AuthzUpgradeTest : public AuthorizationManagerTest {
    public:
        static const NamespaceString versionCollectionName;
        static const NamespaceString usersCollectionName;
        static const NamespaceString backupUsersCollectionName;
        static const NamespaceString newUsersCollectioName;

        void setUpV1UserData() {
            ASSERT_OK(externalState->insert(NamespaceString("test.system.users"),
                              BSON("user" << "readOnly" <<
                                   "pwd" << "password" <<
                                   "roles" << BSON_ARRAY("read")),
                              BSONObj()));
            ASSERT_OK(externalState->insert(NamespaceString("admin.system.users"),
                              BSON("user" << "clusterAdmin" <<
                                   "userSource" << "$external" <<
                                   "roles" << BSON_ARRAY("clusterAdmin")),
                              BSONObj()));
            ASSERT_OK(externalState->insert(NamespaceString("test.system.users"),
                              BSON("user" << "readWriteMultiDB" <<
                                   "pwd" << "password" <<
                                   "roles" << BSON_ARRAY("readWrite")),
                              BSONObj()));
            ASSERT_OK(externalState->insert(NamespaceString("test2.system.users"),
                              BSON("user" << "readWriteMultiDB" <<
                                   "userSource" << "test" <<
                                   "roles" << BSON_ARRAY("readWrite")),
                              BSONObj()));

            ASSERT_OK(authzManager->initialize());
        }

        void validateV1AdminUserData(const NamespaceString& collectionName) {
            BSONObj doc;

            // Verify that the expected users are present.
            ASSERT_EQUALS(1U, externalState->getCollectionContents(collectionName).size());
            ASSERT_OK(externalState->findOne(collectionName,
                                             BSON("user" << "clusterAdmin" <<
                                                  "userSource" << "$external"),
                                             &doc));
            ASSERT_EQUALS("clusterAdmin", doc["user"].str());
            ASSERT_EQUALS("$external", doc["userSource"].str());
            ASSERT_TRUE(doc["pwd"].eoo());
            ASSERT_EQUALS(1U, doc["roles"].Array().size());
            ASSERT_EQUALS("clusterAdmin", doc["roles"].Array()[0].str());
        }

        void validateV2UserData() {
            BSONObj doc;

            // Verify that the admin.system.version document reflects correct upgrade.
            ASSERT_OK(externalState->findOne(versionCollectionName,
                                             BSON("_id" << 1 << "currentVersion" << 2),
                                             &doc));
            ASSERT_EQUALS(2, doc.nFields());
            ASSERT_EQUALS(1U, externalState->getCollectionContents(versionCollectionName).size());

            // Verify that the admin._newusers collection was dropped.
            ASSERT_EQUALS(0U, externalState->getCollectionContents(newUsersCollectioName).size());

            // Verify that the expected users are present.
            ASSERT_EQUALS(3U, externalState->getCollectionContents(usersCollectionName).size());
            ASSERT_OK(externalState->findOne(usersCollectionName,
                                             BSON("name" << "readOnly" << "source" << "test"),
                                             &doc));
            ASSERT_EQUALS("readOnly", doc["name"].str());
            ASSERT_EQUALS("test", doc["source"].str());
            ASSERT_EQUALS("password", doc["credentials"]["MONGODB-CR"].str());
            ASSERT_EQUALS(1U, doc["roles"].Array().size());

            ASSERT_OK(externalState->findOne(
                              usersCollectionName,
                              BSON("name" << "clusterAdmin" << "source" << "$external"),
                              &doc));
            ASSERT_EQUALS("clusterAdmin", doc["name"].str());
            ASSERT_EQUALS("$external", doc["source"].str());
            ASSERT_EQUALS(1U, doc["roles"].Array().size());

            ASSERT_OK(externalState->findOne(
                              usersCollectionName,
                              BSON("name" << "readWriteMultiDB" << "source" << "test"),
                              &doc));
            ASSERT_EQUALS("readWriteMultiDB", doc["name"].str());
            ASSERT_EQUALS("test", doc["source"].str());
            ASSERT_EQUALS("password", doc["credentials"]["MONGODB-CR"].str());
            ASSERT_EQUALS(2U, doc["roles"].Array().size());
        }
    };

    const NamespaceString AuthzUpgradeTest::versionCollectionName("admin.system.version");
    const NamespaceString AuthzUpgradeTest::usersCollectionName("admin.system.users");
    const NamespaceString AuthzUpgradeTest::backupUsersCollectionName("admin.backup.users");
    const NamespaceString AuthzUpgradeTest::newUsersCollectioName("admin._newusers");

    TEST_F(AuthzUpgradeTest, upgradeUserDataFromV1ToV2Clean) {
        authzManager->setAuthorizationVersion(1);
        setUpV1UserData();
        ASSERT_OK(authzManager->upgradeAuthCollections());

        validateV2UserData();
        validateV1AdminUserData(backupUsersCollectionName);
    }

    TEST_F(AuthzUpgradeTest, upgradeUserDataFromV1ToV2WithSysVerDoc) {
        authzManager->setAuthorizationVersion(1);
        setUpV1UserData();
        ASSERT_OK(externalState->insert(versionCollectionName,
                                        BSON("_id" << 1 << "currentVersion" << 1),
                                        BSONObj()));
        ASSERT_OK(authzManager->upgradeAuthCollections());

        validateV1AdminUserData(backupUsersCollectionName);
        validateV2UserData();
    }

    TEST_F(AuthzUpgradeTest, upgradeUserDataFromV1ToV2FailsWithBadInitialVersionDoc) {
        authzManager->setAuthorizationVersion(1);
        setUpV1UserData();
        ASSERT_OK(externalState->insert(versionCollectionName,
                                        BSON("_id" << 1 << "currentVersion" << 3),
                                        BSONObj()));
        ASSERT_NOT_OK(authzManager->upgradeAuthCollections());
        validateV1AdminUserData(usersCollectionName);
        ASSERT_OK(externalState->remove(versionCollectionName, BSONObj(), BSONObj()));
        ASSERT_OK(authzManager->upgradeAuthCollections());
        validateV1AdminUserData(backupUsersCollectionName);
        validateV2UserData();
    }

    TEST_F(AuthzUpgradeTest, upgradeUserDataFromV1ToV2FailsWithVersionDocMispatch) {
        authzManager->setAuthorizationVersion(1);
        setUpV1UserData();
        ASSERT_OK(externalState->insert(versionCollectionName,
                                        BSON("_id" << 1 << "currentVersion" << 2),
                                        BSONObj()));
        ASSERT_NOT_OK(authzManager->upgradeAuthCollections());
        validateV1AdminUserData(usersCollectionName);
    }

}  // namespace
}  // namespace mongo
