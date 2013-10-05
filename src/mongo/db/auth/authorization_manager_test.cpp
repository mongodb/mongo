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
#include "mongo/bson/mutable/document.h"
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

    TEST(RoleParsingTest, BuildRoleBSON) {
        RoleGraph graph;
        RoleName roleA("roleA", "dbA");
        RoleName roleB("roleB", "dbB");
        RoleName roleC("roleC", "dbC");
        ActionSet actions;
        actions.addAction(ActionType::find);
        actions.addAction(ActionType::insert);

        ASSERT_OK(graph.createRole(roleA));
        ASSERT_OK(graph.createRole(roleB));
        ASSERT_OK(graph.createRole(roleC));

        ASSERT_OK(graph.addRoleToRole(roleA, roleC));
        ASSERT_OK(graph.addRoleToRole(roleA, roleB));
        ASSERT_OK(graph.addRoleToRole(roleB, roleC));

        ASSERT_OK(graph.addPrivilegeToRole(
                roleA, Privilege(ResourcePattern::forAnyNormalResource(), actions)));
        ASSERT_OK(graph.addPrivilegeToRole(
                roleB, Privilege(ResourcePattern::forExactNamespace(NamespaceString("dbB.foo")),
                                 actions)));
        ASSERT_OK(graph.addPrivilegeToRole(
                roleC, Privilege(ResourcePattern::forClusterResource(), actions)));
        ASSERT_OK(graph.recomputePrivilegeData());


        // Role A
        mutablebson::Document doc;
        ASSERT_OK(AuthorizationManager::getBSONForRole(&graph, roleA, doc.root()));
        BSONObj roleDoc = doc.getObject();

        ASSERT_EQUALS("dbA.roleA", roleDoc["_id"].String());
        ASSERT_EQUALS("roleA", roleDoc["name"].String());
        ASSERT_EQUALS("dbA", roleDoc["db"].String());

        vector<BSONElement> privs = roleDoc["privileges"].Array();
        ASSERT_EQUALS(1U, privs.size());
        ASSERT_EQUALS("", privs[0].Obj()["resource"].Obj()["db"].String());
        ASSERT_EQUALS("", privs[0].Obj()["resource"].Obj()["collection"].String());
        ASSERT(privs[0].Obj()["resource"].Obj()["cluster"].eoo());
        vector<BSONElement> actionElements = privs[0].Obj()["actions"].Array();
        ASSERT_EQUALS(2U, actionElements.size());
        ASSERT_EQUALS("find", actionElements[0].String());
        ASSERT_EQUALS("insert", actionElements[1].String());

        vector<BSONElement> roles = roleDoc["roles"].Array();
        ASSERT_EQUALS(2U, roles.size());
        ASSERT_EQUALS("roleC", roles[0].Obj()["name"].String());
        ASSERT_EQUALS("dbC", roles[0].Obj()["db"].String());
        ASSERT_EQUALS("roleB", roles[1].Obj()["name"].String());
        ASSERT_EQUALS("dbB", roles[1].Obj()["db"].String());

        // Role B
        doc.reset();
        ASSERT_OK(AuthorizationManager::getBSONForRole(&graph, roleB, doc.root()));
        roleDoc = doc.getObject();

        ASSERT_EQUALS("dbB.roleB", roleDoc["_id"].String());
        ASSERT_EQUALS("roleB", roleDoc["name"].String());
        ASSERT_EQUALS("dbB", roleDoc["db"].String());

        privs = roleDoc["privileges"].Array();
        ASSERT_EQUALS(1U, privs.size());
        ASSERT_EQUALS("dbB", privs[0].Obj()["resource"].Obj()["db"].String());
        ASSERT_EQUALS("foo", privs[0].Obj()["resource"].Obj()["collection"].String());
        ASSERT(privs[0].Obj()["resource"].Obj()["cluster"].eoo());
        actionElements = privs[0].Obj()["actions"].Array();
        ASSERT_EQUALS(2U, actionElements.size());
        ASSERT_EQUALS("find", actionElements[0].String());
        ASSERT_EQUALS("insert", actionElements[1].String());

        roles = roleDoc["roles"].Array();
        ASSERT_EQUALS(1U, roles.size());
        ASSERT_EQUALS("roleC", roles[0].Obj()["name"].String());
        ASSERT_EQUALS("dbC", roles[0].Obj()["db"].String());

        // Role C
        doc.reset();
        ASSERT_OK(AuthorizationManager::getBSONForRole(&graph, roleC, doc.root()));
        roleDoc = doc.getObject();

        ASSERT_EQUALS("dbC.roleC", roleDoc["_id"].String());
        ASSERT_EQUALS("roleC", roleDoc["name"].String());
        ASSERT_EQUALS("dbC", roleDoc["db"].String());

        privs = roleDoc["privileges"].Array();
        ASSERT_EQUALS(1U, privs.size());
        ASSERT(privs[0].Obj()["resource"].Obj()["cluster"].Bool());
        ASSERT(privs[0].Obj()["resource"].Obj()["db"].eoo());
        ASSERT(privs[0].Obj()["resource"].Obj()["collection"].eoo());
        actionElements = privs[0].Obj()["actions"].Array();
        ASSERT_EQUALS(2U, actionElements.size());
        ASSERT_EQUALS("find", actionElements[0].String());
        ASSERT_EQUALS("insert", actionElements[1].String());

        roles = roleDoc["roles"].Array();
        ASSERT_EQUALS(0U, roles.size());
    }

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

    TEST_F(AuthorizationManagerTest, testAcquireV0User) {
        authzManager->setAuthorizationVersion(1);

        ASSERT_OK(externalState->insert(NamespaceString("test.system.users"),
                                        BSON("user" << "v0RW" << "pwd" << "password"),
                                        BSONObj()));
        ASSERT_OK(externalState->insert(NamespaceString("admin.system.users"),
                                        BSON("user" << "v0AdminRO" <<
                                             "pwd" << "password" <<
                                             "readOnly" << true),
                                        BSONObj()));

        ASSERT_OK(authzManager->initialize());
        User* v0RW;
        ASSERT_OK(authzManager->acquireUser(UserName("v0RW", "test"), &v0RW));
        ASSERT_EQUALS(UserName("v0RW", "test"), v0RW->getName());
        ASSERT(v0RW->isValid());
        ASSERT_EQUALS((uint32_t)1, v0RW->getRefCount());
        const User::RoleDataMap& roles = v0RW->getRoles();
        ASSERT_EQUALS(1U, roles.size());
        User::RoleData role = roles.begin()->second;
        ASSERT_EQUALS(RoleName("dbOwner", "test"), role.name);
        ASSERT(role.hasRole);
        ASSERT(!role.canDelegate);
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
        ASSERT_EQUALS(RoleName("readAnyDatabase", "admin"), role.name);
        ASSERT(role.hasRole);
        ASSERT(!role.canDelegate);
        // Make sure user's refCount is 0 at the end of the test to avoid an assertion failure
        authzManager->releaseUser(v0AdminRO);
    }

    TEST_F(AuthorizationManagerTest, testAcquireV1User) {
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
        ASSERT_EQUALS(UserName("v1read", "test"), v1read->getName());
        ASSERT(v1read->isValid());
        ASSERT_EQUALS((uint32_t)1, v1read->getRefCount());

        const User::RoleDataMap& roles = v1read->getRoles();
        ASSERT_EQUALS(1U, roles.size());
        User::RoleData role = roles.begin()->second;
        ASSERT_EQUALS(RoleName("read", "test"), role.name);
        ASSERT(role.hasRole);
        ASSERT(!role.canDelegate);
        // Make sure user's refCount is 0 at the end of the test to avoid an assertion failure
        authzManager->releaseUser(v1read);

        User* v1cluster;
        ASSERT_OK(authzManager->acquireUser(UserName("v1cluster", "admin"), &v1cluster));
        ASSERT_EQUALS(UserName("v1cluster", "admin"), v1cluster->getName());
        ASSERT(v1cluster->isValid());
        ASSERT_EQUALS((uint32_t)1, v1cluster->getRefCount());
        const User::RoleDataMap& clusterRoles = v1cluster->getRoles();
        ASSERT_EQUALS(1U, clusterRoles.size());
        role = clusterRoles.begin()->second;
        ASSERT_EQUALS(RoleName("clusterAdmin", "admin"), role.name);
        ASSERT(role.hasRole);
        ASSERT(!role.canDelegate);
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
        ASSERT_EQUALS(UserName("readOnly", "test"), readOnly->getName());
        ASSERT(readOnly->isValid());
        ASSERT_EQUALS((uint32_t)2, readOnly->getRefCount());
        const User::RoleDataMap& roles = readOnly->getRoles();
        ASSERT_EQUALS(1U, roles.size());
        User::RoleData role = roles.begin()->second;
        ASSERT_EQUALS(RoleName("read", "test"), role.name);
        ASSERT(role.hasRole);
        ASSERT(!role.canDelegate);
        // Make sure user's refCount is 0 at the end of the test to avoid an assertion failure
        authzManager->releaseUser(readOnly);

        User* clusterAdmin;
        ASSERT_OK(authzManager->acquireUser(UserName("clusterAdmin", "$external"), &clusterAdmin));
        ASSERT_EQUALS(UserName("clusterAdmin", "$external"), clusterAdmin->getName());
        ASSERT(clusterAdmin->isValid());
        ASSERT_EQUALS((uint32_t)2, clusterAdmin->getRefCount());
        const User::RoleDataMap& clusterRoles = clusterAdmin->getRoles();
        ASSERT_EQUALS(1U, clusterRoles.size());
        role = clusterRoles.begin()->second;
        ASSERT_EQUALS(RoleName("clusterAdmin", "admin"), role.name);
        ASSERT(role.hasRole);
        ASSERT(!role.canDelegate);
        // Make sure user's refCount is 0 at the end of the test to avoid an assertion failure
        authzManager->releaseUser(clusterAdmin);

        User* multiDB;
        status = authzManager->acquireUser(UserName("readWriteMultiDB", "test2"), &multiDB);
        ASSERT_NOT_OK(status);
        ASSERT(status.code() == ErrorCodes::UserNotFound);

        ASSERT_OK(authzManager->acquireUser(UserName("readWriteMultiDB", "test"), &multiDB));
        ASSERT_EQUALS(UserName("readWriteMultiDB", "test"), multiDB->getName());
        ASSERT(multiDB->isValid());
        ASSERT_EQUALS((uint32_t)2, multiDB->getRefCount());

        const User::RoleDataMap& multiDBRoles = multiDB->getRoles();
        ASSERT_EQUALS(2U, multiDBRoles.size());
        role = mapFindWithDefault(multiDBRoles, RoleName("readWrite", "test"), User::RoleData());
        ASSERT_EQUALS(RoleName("readWrite", "test"), role.name);
        ASSERT(role.hasRole);
        ASSERT(!role.canDelegate);
        role = mapFindWithDefault(multiDBRoles, RoleName("readWrite", "test2"), User::RoleData());
        ASSERT_EQUALS(RoleName("readWrite", "test2"), role.name);
        ASSERT(role.hasRole);
        ASSERT(!role.canDelegate);

        // Make sure user's refCount is 0 at the end of the test to avoid an assertion failure
        authzManager->releaseUser(multiDB);


        // initializeAllV1UserData() pins the users by adding 1 to their refCount, so need to
        // release each user an extra time to bring their refCounts to 0.
        authzManager->releaseUser(readOnly);
        authzManager->releaseUser(clusterAdmin);
        authzManager->releaseUser(multiDB);
    }


    TEST_F(AuthorizationManagerTest, testAcquireV2User) {
        authzManager->setAuthorizationVersion(2);

        ASSERT_OK(externalState->insertPrivilegeDocument(
                "admin",
                BSON("name" << "v2read" <<
                     "db" << "test" <<
                     "credentials" << BSON("MONGODB-CR" << "password") <<
                     "roles" << BSON_ARRAY(BSON("name" << "read" <<
                                                "db" << "test" <<
                                                "canDelegate" << false <<
                                                "hasRole" << true))),
                BSONObj()));
        ASSERT_OK(externalState->insertPrivilegeDocument(
                "admin",
                BSON("name" << "v2cluster" <<
                     "db" << "admin" <<
                     "credentials" << BSON("MONGODB-CR" << "password") <<
                     "roles" << BSON_ARRAY(BSON("name" << "clusterAdmin" <<
                                                "db" << "admin" <<
                                                "canDelegate" << true <<
                                                "hasRole" << true))),
                BSONObj()));

        User* v2read;
        ASSERT_OK(authzManager->acquireUser(UserName("v2read", "test"), &v2read));
        ASSERT_EQUALS(UserName("v2read", "test"), v2read->getName());
        ASSERT(v2read->isValid());
        ASSERT_EQUALS((uint32_t)1, v2read->getRefCount());
        const User::RoleDataMap& roles = v2read->getRoles();
        ASSERT_EQUALS(1U, roles.size());
        User::RoleData role = roles.begin()->second;
        ASSERT_EQUALS(RoleName("read", "test"), role.name);
        ASSERT(role.hasRole);
        ASSERT(!role.canDelegate);
        // Make sure user's refCount is 0 at the end of the test to avoid an assertion failure
        authzManager->releaseUser(v2read);

        User* v2cluster;
        ASSERT_OK(authzManager->acquireUser(UserName("v2cluster", "admin"), &v2cluster));
        ASSERT_EQUALS(UserName("v2cluster", "admin"), v2cluster->getName());
        ASSERT(v2cluster->isValid());
        ASSERT_EQUALS((uint32_t)1, v2cluster->getRefCount());
        const User::RoleDataMap& clusterRoles = v2cluster->getRoles();
        ASSERT_EQUALS(1U, clusterRoles.size());
        role = clusterRoles.begin()->second;
        ASSERT_EQUALS(RoleName("clusterAdmin", "admin"), role.name);
        ASSERT(role.hasRole);
        ASSERT(role.canDelegate);
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
                                             BSON("name" << "readOnly" << "db" << "test"),
                                             &doc));
            ASSERT_EQUALS("readOnly", doc["name"].str());
            ASSERT_EQUALS("test", doc["db"].str());
            ASSERT_EQUALS("password", doc["credentials"]["MONGODB-CR"].str());
            ASSERT_EQUALS(1U, doc["roles"].Array().size());

            ASSERT_OK(externalState->findOne(
                              usersCollectionName,
                              BSON("name" << "clusterAdmin" << "db" << "$external"),
                              &doc));
            ASSERT_EQUALS("clusterAdmin", doc["name"].str());
            ASSERT_EQUALS("$external", doc["db"].str());
            ASSERT_EQUALS(1U, doc["roles"].Array().size());

            ASSERT_OK(externalState->findOne(
                              usersCollectionName,
                              BSON("name" << "readWriteMultiDB" << "db" << "test"),
                              &doc));
            ASSERT_EQUALS("readWriteMultiDB", doc["name"].str());
            ASSERT_EQUALS("test", doc["db"].str());
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
        int numRemoved;
        ASSERT_OK(externalState->remove(versionCollectionName, BSONObj(), BSONObj(), &numRemoved));
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
