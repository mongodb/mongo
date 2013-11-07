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
        ASSERT_EQUALS("roleA", roleDoc["role"].String());
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
        ASSERT_EQUALS("roleC", roles[0].Obj()["role"].String());
        ASSERT_EQUALS("dbC", roles[0].Obj()["db"].String());
        ASSERT_EQUALS("roleB", roles[1].Obj()["role"].String());
        ASSERT_EQUALS("dbB", roles[1].Obj()["db"].String());

        // Role B
        doc.reset();
        ASSERT_OK(AuthorizationManager::getBSONForRole(&graph, roleB, doc.root()));
        roleDoc = doc.getObject();

        ASSERT_EQUALS("dbB.roleB", roleDoc["_id"].String());
        ASSERT_EQUALS("roleB", roleDoc["role"].String());
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
        ASSERT_EQUALS("roleC", roles[0].Obj()["role"].String());
        ASSERT_EQUALS("dbC", roles[0].Obj()["db"].String());

        // Role C
        doc.reset();
        ASSERT_OK(AuthorizationManager::getBSONForRole(&graph, roleC, doc.root()));
        roleDoc = doc.getObject();

        ASSERT_EQUALS("dbC.roleC", roleDoc["_id"].String());
        ASSERT_EQUALS("roleC", roleDoc["role"].String());
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
            externalState->setAuthzVersion(AuthorizationManager::schemaVersion26Final);
            authzManager.reset(new AuthorizationManager(externalState));
            externalState->setAuthorizationManager(authzManager.get());
            authzManager->setAuthEnabled(true);
            // This duplicates the behavior from the server that adds the internal user at process
            // startup via a MONGO_INITIALIZER
            authzManager->addInternalUser(internalSecurity.user);
        }

        scoped_ptr<AuthorizationManager> authzManager;
        AuthzManagerExternalStateMock* externalState;
    };

    TEST_F(AuthorizationManagerTest, testAcquireV0User) {
        externalState->setAuthzVersion(AuthorizationManager::schemaVersion24);

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
        ASSERT_EQUALS(1U, v0RW->getRefCount());
        RoleNameIterator roles = v0RW->getRoles();
        ASSERT_EQUALS(RoleName("dbOwner", "test"), roles.next());
        ASSERT_FALSE(roles.more());
        // Make sure user's refCount is 0 at the end of the test to avoid an assertion failure
        authzManager->releaseUser(v0RW);

        User* v0AdminRO;
        ASSERT_OK(authzManager->acquireUser(UserName("v0AdminRO", "admin"), &v0AdminRO));
        ASSERT(UserName("v0AdminRO", "admin") == v0AdminRO->getName());
        ASSERT(v0AdminRO->isValid());
        ASSERT_EQUALS((uint32_t)1, v0AdminRO->getRefCount());
        RoleNameIterator adminRoles = v0AdminRO->getRoles();
        ASSERT_EQUALS(RoleName("readAnyDatabase", "admin"), adminRoles.next());
        ASSERT_FALSE(adminRoles.more());
        // Make sure user's refCount is 0 at the end of the test to avoid an assertion failure
        authzManager->releaseUser(v0AdminRO);
    }

    TEST_F(AuthorizationManagerTest, testAcquireV1User) {
        externalState->setAuthzVersion(AuthorizationManager::schemaVersion24);

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

        RoleNameIterator roles = v1read->getRoles();
        ASSERT_EQUALS(RoleName("read", "test"), roles.next());
        ASSERT_FALSE(roles.more());
        // Make sure user's refCount is 0 at the end of the test to avoid an assertion failure
        authzManager->releaseUser(v1read);

        User* v1cluster;
        ASSERT_OK(authzManager->acquireUser(UserName("v1cluster", "admin"), &v1cluster));
        ASSERT_EQUALS(UserName("v1cluster", "admin"), v1cluster->getName());
        ASSERT(v1cluster->isValid());
        ASSERT_EQUALS((uint32_t)1, v1cluster->getRefCount());
        RoleNameIterator clusterRoles = v1cluster->getRoles();
        ASSERT_EQUALS(RoleName("clusterAdmin", "admin"), clusterRoles.next());
        ASSERT_FALSE(clusterRoles.more());
        // Make sure user's refCount is 0 at the end of the test to avoid an assertion failure
        authzManager->releaseUser(v1cluster);
    }

    TEST_F(AuthorizationManagerTest, initializeAllV1UserData) {
        externalState->setAuthzVersion(AuthorizationManager::schemaVersion24);

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
        ASSERT_EQUALS(1U, readOnly->getRefCount());
        RoleNameIterator roles = readOnly->getRoles();
        ASSERT_EQUALS(RoleName("read", "test"), roles.next());
        ASSERT_FALSE(roles.more());
        // Make sure user's refCount is 0 at the end of the test to avoid an assertion failure
        authzManager->releaseUser(readOnly);

        User* clusterAdmin;
        ASSERT_OK(authzManager->acquireUser(UserName("clusterAdmin", "$external"), &clusterAdmin));
        ASSERT_EQUALS(UserName("clusterAdmin", "$external"), clusterAdmin->getName());
        ASSERT(clusterAdmin->isValid());
        ASSERT_EQUALS(1U, clusterAdmin->getRefCount());
        RoleNameIterator clusterRoles = clusterAdmin->getRoles();
        ASSERT_EQUALS(RoleName("clusterAdmin", "admin"), clusterRoles.next());
        ASSERT_FALSE(clusterRoles.more());
        // Make sure user's refCount is 0 at the end of the test to avoid an assertion failure
        authzManager->releaseUser(clusterAdmin);

        User* multiDB;
        status = authzManager->acquireUser(UserName("readWriteMultiDB", "test2"), &multiDB);
        ASSERT_NOT_OK(status);
        ASSERT(status.code() == ErrorCodes::UserNotFound);

        ASSERT_OK(authzManager->acquireUser(UserName("readWriteMultiDB", "test"), &multiDB));
        ASSERT_EQUALS(UserName("readWriteMultiDB", "test"), multiDB->getName());
        ASSERT(multiDB->isValid());
        ASSERT_EQUALS(1U, multiDB->getRefCount());
        User* multiDBProbed;
        ASSERT_OK(authzManager->acquireV1UserProbedForDb(
                          UserName("readWriteMultiDB", "test"),
                          "test2",
                          &multiDBProbed));
        authzManager->releaseUser(multiDB);
        multiDB = multiDBProbed;
        ASSERT_EQUALS(UserName("readWriteMultiDB", "test"), multiDB->getName());
        ASSERT(multiDB->isValid());
        ASSERT_EQUALS(1U, multiDB->getRefCount());

        RoleNameIterator multiDBRoles = multiDB->getRoles();
        ASSERT(multiDBRoles.more());
        RoleName role = multiDBRoles.next();
        if (role == RoleName("readWrite", "test")) {
            ASSERT(multiDBRoles.more());
            ASSERT_EQUALS(RoleName("readWrite", "test2"), multiDBRoles.next());
        } else {
            ASSERT_EQUALS(RoleName("readWrite", "test2"), role);
            ASSERT(multiDBRoles.more());
            ASSERT_EQUALS(RoleName("readWrite", "test"), multiDBRoles.next());
        }
        ASSERT_FALSE(multiDBRoles.more());

        // Make sure user's refCount is 0 at the end of the test to avoid an assertion failure
        authzManager->releaseUser(multiDB);


        // initializeAllV1UserData() pins the users by adding 1 to their refCount, so need to
        // release each user an extra time to bring their refCounts to 0.
        authzManager->releaseUser(readOnly);
        authzManager->releaseUser(clusterAdmin);
        authzManager->releaseUser(multiDB);
    }


    TEST_F(AuthorizationManagerTest, testAcquireV2User) {
        externalState->setAuthzVersion(AuthorizationManager::schemaVersion26Final);

        ASSERT_OK(externalState->insertPrivilegeDocument(
                "admin",
                BSON("user" << "v2read" <<
                     "db" << "test" <<
                     "credentials" << BSON("MONGODB-CR" << "password") <<
                     "roles" << BSON_ARRAY(BSON("role" << "read" <<
                                                "db" << "test" <<
                                                "canDelegate" << false <<
                                                "hasRole" << true))),
                BSONObj()));
        ASSERT_OK(externalState->insertPrivilegeDocument(
                "admin",
                BSON("user" << "v2cluster" <<
                     "db" << "admin" <<
                     "credentials" << BSON("MONGODB-CR" << "password") <<
                     "roles" << BSON_ARRAY(BSON("role" << "clusterAdmin" <<
                                                "db" << "admin" <<
                                                "canDelegate" << true <<
                                                "hasRole" << true))),
                BSONObj()));

        User* v2read;
        ASSERT_OK(authzManager->acquireUser(UserName("v2read", "test"), &v2read));
        ASSERT_EQUALS(UserName("v2read", "test"), v2read->getName());
        ASSERT(v2read->isValid());
        ASSERT_EQUALS(1U, v2read->getRefCount());
        RoleNameIterator roles = v2read->getRoles();
        ASSERT_EQUALS(RoleName("read", "test"), roles.next());
        ASSERT_FALSE(roles.more());
        // Make sure user's refCount is 0 at the end of the test to avoid an assertion failure
        authzManager->releaseUser(v2read);

        User* v2cluster;
        ASSERT_OK(authzManager->acquireUser(UserName("v2cluster", "admin"), &v2cluster));
        ASSERT_EQUALS(UserName("v2cluster", "admin"), v2cluster->getName());
        ASSERT(v2cluster->isValid());
        ASSERT_EQUALS(1U, v2cluster->getRefCount());
        RoleNameIterator clusterRoles = v2cluster->getRoles();
        ASSERT_EQUALS(RoleName("clusterAdmin", "admin"), clusterRoles.next());
        ASSERT_FALSE(clusterRoles.more());
        // Make sure user's refCount is 0 at the end of the test to avoid an assertion failure
        authzManager->releaseUser(v2cluster);
    }

    class AuthzUpgradeTest : public AuthorizationManagerTest {
    public:
        void setUpV1UserData() {

            // Docs for "readOnly@test"
            ASSERT_OK(externalState->insert(NamespaceString("test.system.users"),
                              BSON("user" << "readOnly" <<
                                   "pwd" << "password" <<
                                   "roles" << BSON_ARRAY("read")),
                              BSONObj()));

            // Docs for "clusterAdmin@$external"
            ASSERT_OK(externalState->insert(NamespaceString("admin.system.users"),
                              BSON("user" << "clusterAdmin" <<
                                   "userSource" << "$external" <<
                                   "roles" << BSON_ARRAY("clusterAdmin")),
                              BSONObj()));

            // Docs for "readWriteMultiDB@test"
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

            // Docs for otherdbroles@test
            ASSERT_OK(externalState->insert(NamespaceString("test.system.users"),
                              BSON("user" << "otherdbroles" <<
                                   "pwd" << "password" <<
                                   "roles" << BSON_ARRAY("readWrite")),
                              BSONObj()));
            ASSERT_OK(externalState->insert(NamespaceString("admin.system.users"),
                              BSON("user" << "otherdbroles" <<
                                   "userSource" << "test" <<
                                   "roles" << BSONArray() <<
                                   "otherDBRoles" << BSON("test3" << BSON_ARRAY("readWrite"))),
                              BSONObj()));

            // Docs for mixedroles@test
            ASSERT_OK(externalState->insert(NamespaceString("test.system.users"),
                              BSON("user" << "mixedroles" <<
                                   "pwd" << "password" <<
                                   "roles" << BSON_ARRAY("readWrite")),
                              BSONObj()));
            ASSERT_OK(externalState->insert(NamespaceString("admin.system.users"),
                              BSON("user" << "mixedroles" <<
                                   "userSource" << "test" <<
                                   "roles" << BSONArray() <<
                                   "otherDBRoles" << BSON("test3" << BSON_ARRAY("readWrite" <<
                                                                                "dbAdmin") <<
                                                          "test2" << BSON_ARRAY("readWrite"))),
                              BSONObj()));
            ASSERT_OK(externalState->insert(NamespaceString("test2.system.users"),
                              BSON("user" << "mixedroles" <<
                                   "userSource" << "test" <<
                                   "roles" << BSON_ARRAY("readWrite")),
                              BSONObj()));

            ASSERT_OK(authzManager->initialize());
        }

        void validateV1AdminUserData(const NamespaceString& collectionName) {
            BSONObj doc;

            // Verify that the expected users are present.
            ASSERT_EQUALS(3U, externalState->getCollectionContents(collectionName).size());
            ASSERT_OK(externalState->findOne(collectionName,
                                             BSON("user" << "clusterAdmin" <<
                                                  "userSource" << "$external"),
                                             &doc));
            ASSERT_EQUALS("clusterAdmin", doc["user"].str());
            ASSERT_EQUALS("$external", doc["userSource"].str());
            ASSERT_TRUE(doc["pwd"].eoo());
            ASSERT_EQUALS(1U, doc["roles"].Array().size());
            ASSERT_EQUALS("clusterAdmin", doc["roles"].Array()[0].str());

            ASSERT_OK(externalState->findOne(collectionName,
                                             BSON("user" << "otherdbroles" <<
                                                  "userSource" << "test"),
                                             &doc));
            ASSERT_TRUE(doc["pwd"].eoo());
            ASSERT_EQUALS(0U, doc["roles"].Array().size());

            ASSERT_OK(externalState->findOne(collectionName,
                                             BSON("user" << "mixedroles" <<
                                                  "userSource" << "test"),
                                             &doc));
            ASSERT_TRUE(doc["pwd"].eoo());
            ASSERT_EQUALS(0U, doc["roles"].Array().size());
        }

        void validateV2UserData() {
            BSONObj doc;

            // Verify that the admin.system.version document reflects correct upgrade.
            ASSERT_OK(externalState->findOne(
                              AuthorizationManager::versionCollectionNamespace,
                              BSON("_id" << "authSchema" <<
                                   AuthorizationManager::schemaVersionFieldName <<
                                   AuthorizationManager::schemaVersion26Final),
                              &doc));
            ASSERT_EQUALS(2, doc.nFields());
            ASSERT_EQUALS(1U, externalState->getCollectionContents(
                                  AuthorizationManager::versionCollectionNamespace).size());

            // Verify that the expected users are present.
            ASSERT_EQUALS(5U, externalState->getCollectionContents(
                                  AuthorizationManager::usersAltCollectionNamespace).size());
            ASSERT_EQUALS(5U, externalState->getCollectionContents(
                                  AuthorizationManager::usersCollectionNamespace).size());

            // "readOnly@test" user
            ASSERT_OK(externalState->findOne(AuthorizationManager::usersCollectionNamespace,
                                             BSON("user" << "readOnly" << "db" << "test"),
                                             &doc));
            ASSERT_EQUALS("readOnly", doc["user"].str());
            ASSERT_EQUALS("test", doc["db"].str());
            ASSERT_EQUALS("password", doc["credentials"]["MONGODB-CR"].str());
            ASSERT_EQUALS(1U, doc["roles"].Array().size());

            // "clusterAdmin@$external" user
            ASSERT_OK(externalState->findOne(
                              AuthorizationManager::usersCollectionNamespace,
                              BSON("user" << "clusterAdmin" << "db" << "$external"),
                              &doc));
            ASSERT_EQUALS("clusterAdmin", doc["user"].str());
            ASSERT_EQUALS("$external", doc["db"].str());
            ASSERT_EQUALS(1U, doc["roles"].Array().size());

            // "readWriteMultiDB@test" user
            ASSERT_OK(externalState->findOne(
                              AuthorizationManager::usersCollectionNamespace,
                              BSON("user" << "readWriteMultiDB" << "db" << "test"),
                              &doc));
            ASSERT_EQUALS("readWriteMultiDB", doc["user"].str());
            ASSERT_EQUALS("test", doc["db"].str());
            ASSERT_EQUALS("password", doc["credentials"]["MONGODB-CR"].str());
            ASSERT_EQUALS(2U, doc["roles"].Array().size());

            // "otherdbroles@test" user
            ASSERT_OK(externalState->findOne(
                              AuthorizationManager::usersCollectionNamespace,
                              BSON("user" << "otherdbroles" << "db" << "test"),
                              &doc));
            ASSERT_EQUALS("test.otherdbroles", doc["_id"].str());
            ASSERT_EQUALS("password", doc["credentials"]["MONGODB-CR"].str());
            std::vector<BSONElement> roles = doc["roles"].Array();
            std::set<std::pair<std::string, std::string> > rolePairs;
            for (size_t i = 0; i < roles.size(); ++i) {
                BSONElement roleElement = roles[i];
                rolePairs.insert(make_pair(roleElement["role"].str(), roleElement["db"].str()));
            }
            ASSERT_EQUALS(2U, rolePairs.size());
            ASSERT_EQUALS(1U, rolePairs.count(make_pair("readWrite", "test")));
            ASSERT_EQUALS(1U, rolePairs.count(make_pair("readWrite", "test3")));

            // "mixedroles@test" user
            ASSERT_OK(externalState->findOne(
                              AuthorizationManager::usersCollectionNamespace,
                              BSON("user" << "mixedroles" << "db" << "test"),
                              &doc));
            ASSERT_EQUALS("test.mixedroles", doc["_id"].str());
            ASSERT_EQUALS("password", doc["credentials"]["MONGODB-CR"].str());
            rolePairs.clear();
            roles = doc["roles"].Array();
            for (size_t i = 0; i < roles.size(); ++i) {
                BSONElement roleElement = roles[i];
                rolePairs.insert(make_pair(roleElement["role"].str(), roleElement["db"].str()));
            }
            ASSERT_EQUALS(4U, rolePairs.size());
            ASSERT_EQUALS(1U, rolePairs.count(make_pair("readWrite", "test")));
            ASSERT_EQUALS(1U, rolePairs.count(make_pair("readWrite", "test2")));
            ASSERT_EQUALS(1U, rolePairs.count(make_pair("readWrite", "test3")));
            ASSERT_EQUALS(1U, rolePairs.count(make_pair("dbAdmin", "test3")));
        }

        void upgradeAuthCollections() {
            bool done = false;
            int iters = 0;
            while (!done) {
                ASSERT_OK(authzManager->upgradeSchemaStep(BSONObj(), &done));
                ASSERT_LESS_THAN(iters++, 10);
            }
        }
    };

    TEST_F(AuthzUpgradeTest, upgradeUserDataFromV1ToV2Clean) {
        externalState->setAuthzVersion(AuthorizationManager::schemaVersion24);
        setUpV1UserData();
        upgradeAuthCollections();

        validateV2UserData();
        validateV1AdminUserData(AuthorizationManager::usersBackupCollectionNamespace);
    }

    TEST_F(AuthzUpgradeTest, upgradeUserDataFromV1ToV2WithSysVerDoc) {
        externalState->setAuthzVersion(AuthorizationManager::schemaVersion24);
        setUpV1UserData();
        upgradeAuthCollections();

        validateV1AdminUserData(AuthorizationManager::usersBackupCollectionNamespace);
        validateV2UserData();
    }

    TEST_F(AuthzUpgradeTest, upgradeUserDataFromV1ToV2FailsWithBadInitialVersionDoc) {
        externalState->setAuthzVersion(AuthorizationManager::schemaVersion24);
        setUpV1UserData();
        externalState->setAuthzVersion(AuthorizationManager::schemaVersion26Final);
        bool done;
        ASSERT_OK(authzManager->upgradeSchemaStep(BSONObj(), &done));
        ASSERT_TRUE(done);
        validateV1AdminUserData(AuthorizationManager::usersCollectionNamespace);
        int numRemoved;
        ASSERT_OK(externalState->remove(AuthorizationManager::versionCollectionNamespace,
                                        BSONObj(),
                                        BSONObj(),
                                        &numRemoved));
        upgradeAuthCollections();
        validateV1AdminUserData(AuthorizationManager::usersBackupCollectionNamespace);
        validateV2UserData();
    }

}  // namespace
}  // namespace mongo
