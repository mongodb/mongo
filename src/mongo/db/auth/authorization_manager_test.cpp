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
        scoped_ptr<AuthorizationManager> authzManager;
        AuthzManagerExternalStateMock* externalState;
        void setUp() {
            externalState = new AuthzManagerExternalStateMock();
            authzManager.reset(new AuthorizationManager(externalState));
            // This duplicates the behavior from the server that adds the internal user at process
            // startup via a MONGO_INITIALIZER
            authzManager->addInternalUser(internalSecurity.user);
        }
    };

    TEST_F(AuthorizationManagerTest, testAquireV0User) {
        externalState->insertPrivilegeDocument("test",
                                               BSON("user" << "v0RW" <<
                                                    "pwd" << "password"));
        externalState->insertPrivilegeDocument("admin",
                                               BSON("user" << "v0AdminRO" <<
                                                    "pwd" << "password" <<
                                                    "readOnly" << true));

        User* v0RW;
        ASSERT_OK(authzManager->acquireUser(UserName("v0RW", "test"), &v0RW));
        ASSERT(UserName("v0RW", "test") == v0RW->getName());
        ASSERT(v0RW->isValid());
        ASSERT_EQUALS((uint32_t)1, v0RW->getRefCount());
        RoleNameIterator it = v0RW->getRoles();
        ASSERT(it.more());
        RoleName roleName = it.next();
        ASSERT_EQUALS("test", roleName.getDB());
        ASSERT_EQUALS("oldReadWrite", roleName.getRole());
        ASSERT_FALSE(it.more());
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
        it = v0AdminRO->getRoles();
        ASSERT(it.more());
        roleName = it.next();
        ASSERT_EQUALS("admin", roleName.getDB());
        ASSERT_EQUALS("oldAdminRead", roleName.getRole());
        ASSERT_FALSE(it.more());
        actions = v0AdminRO->getActionsForResource("*");
        ASSERT(actions.contains(ActionType::find));
        ASSERT(actions.contains(ActionType::listDatabases));
        ASSERT_FALSE(actions.contains(ActionType::insert));
        ASSERT_FALSE(actions.contains(ActionType::dropDatabase));
        // Make sure user's refCount is 0 at the end of the test to avoid an assertion failure
        authzManager->releaseUser(v0AdminRO);
    }

    TEST_F(AuthorizationManagerTest, testAquireV1User) {
        externalState->insertPrivilegeDocument("test",
                                               BSON("user" << "v1read" <<
                                                    "pwd" << "password" <<
                                                    "roles" << BSON_ARRAY("read")));
        externalState->insertPrivilegeDocument("admin",
                                               BSON("user" << "v1cluster" <<
                                                    "pwd" << "password" <<
                                                    "roles" << BSON_ARRAY("clusterAdmin")));

        User* v1read;
        ASSERT_OK(authzManager->acquireUser(UserName("v1read", "test"), &v1read));
        ASSERT(UserName("v1read", "test") == v1read->getName());
        ASSERT(v1read->isValid());
        ASSERT_EQUALS((uint32_t)1, v1read->getRefCount());
        RoleNameIterator it = v1read->getRoles();
        ASSERT(it.more());
        RoleName roleName = it.next();
        ASSERT_EQUALS("test", roleName.getDB());
        ASSERT_EQUALS("read", roleName.getRole());
        ASSERT_FALSE(it.more());
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
        it = v1cluster->getRoles();
        ASSERT(it.more());
        roleName = it.next();
        ASSERT_EQUALS("admin", roleName.getDB());
        ASSERT_EQUALS("clusterAdmin", roleName.getRole());
        ASSERT_FALSE(it.more());
        actions = v1cluster->getActionsForResource("*");
        ASSERT(actions.contains(ActionType::listDatabases));
        ASSERT(actions.contains(ActionType::dropDatabase));
        ASSERT_FALSE(actions.contains(ActionType::find));
        // Make sure user's refCount is 0 at the end of the test to avoid an assertion failure
        authzManager->releaseUser(v1cluster);
    }

    TEST_F(AuthorizationManagerTest, initializeAllV1UserData) {
        externalState->insertPrivilegeDocument("test",
                                               BSON("user" << "readOnly" <<
                                                    "pwd" << "password" <<
                                                    "roles" << BSON_ARRAY("read")));
        externalState->insertPrivilegeDocument("admin",
                                               BSON("user" << "clusterAdmin" <<
                                                    "userSource" << "$external" <<
                                                    "roles" << BSON_ARRAY("clusterAdmin")));
        externalState->insertPrivilegeDocument("test",
                                               BSON("user" << "readWriteMultiDB" <<
                                                    "pwd" << "password" <<
                                                    "roles" << BSON_ARRAY("readWrite")));
        externalState->insertPrivilegeDocument("test2",
                                               BSON("user" << "readWriteMultiDB" <<
                                                    "userSource" << "test" <<
                                                    "roles" << BSON_ARRAY("readWrite")));

        Status status = authzManager->initializeAllV1UserData();
        ASSERT_OK(status);

        User* readOnly;
        ASSERT_OK(authzManager->acquireUser(UserName("readOnly", "test"), &readOnly));
        ASSERT(UserName("readOnly", "test") == readOnly->getName());
        ASSERT(readOnly->isValid());
        ASSERT_EQUALS((uint32_t)2, readOnly->getRefCount());
        RoleNameIterator it = readOnly->getRoles();
        ASSERT(it.more());
        RoleName roleName = it.next();
        ASSERT_EQUALS("test", roleName.getDB());
        ASSERT_EQUALS("read", roleName.getRole());
        ASSERT_FALSE(it.more());
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
        it = clusterAdmin->getRoles();
        ASSERT(it.more());
        roleName = it.next();
        ASSERT_EQUALS("admin", roleName.getDB());
        ASSERT_EQUALS("clusterAdmin", roleName.getRole());
        ASSERT_FALSE(it.more());
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
        it = multiDB->getRoles();

        bool hasRoleOnTest = false;
        bool hasRoleOnTest2 = false;
        int numRoles = 0;
        while(it.more()) {
            ++numRoles;
            RoleName roleName = it.next();
            ASSERT_EQUALS("readWrite", roleName.getRole());
            if (roleName.getDB() == "test") {
                hasRoleOnTest = true;
            } else {
                ASSERT_EQUALS("test2", roleName.getDB());
                hasRoleOnTest2 = true;
            }
        }
        ASSERT_EQUALS(2, numRoles);
        ASSERT(hasRoleOnTest);
        ASSERT(hasRoleOnTest2);

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
}  // namespace
}  // namespace mongo
