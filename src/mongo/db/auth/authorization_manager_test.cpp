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
        }

        scoped_ptr<AuthorizationManager> authzManager;
        AuthzManagerExternalStateMock* externalState;
    };

    TEST_F(AuthorizationManagerTest, testAcquireV2User) {
        externalState->setAuthzVersion(AuthorizationManager::schemaVersion26Final);

        ASSERT_OK(externalState->insertPrivilegeDocument(
                "admin",
                BSON("_id" << "admin.v2read" <<
                     "user" << "v2read" <<
                     "db" << "test" <<
                     "credentials" << BSON("MONGODB-CR" << "password") <<
                     "roles" << BSON_ARRAY(BSON("role" << "read" << "db" << "test"))),
                BSONObj()));
        ASSERT_OK(externalState->insertPrivilegeDocument(
                "admin",
                BSON("_id" << "admin.v2cluster" <<
                     "user" << "v2cluster" <<
                     "db" << "admin" <<
                     "credentials" << BSON("MONGODB-CR" << "password") <<
                     "roles" << BSON_ARRAY(BSON("role" << "clusterAdmin" << "db" << "admin"))),
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

}  // namespace
}  // namespace mongo
