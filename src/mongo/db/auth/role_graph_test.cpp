/**
 *    Copyright (C) 2013 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

/**
 * Unit tests of the RoleGraph type.
 */

#include <algorithm>

#include "mongo/db/auth/role_graph.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {

    // Tests adding and removing roles from other roles, the RoleNameIterator, and the
    // getDirectMembers and getDirectSubordinates methods
    TEST(RoleGraphTest, AddRemoveRoles) {
        RoleName roleA("roleA", "dbA");
        RoleName roleB("roleB", "dbB");
        RoleName roleC("roleC", "dbC");
        RoleName roleD("readWrite", "dbD"); // built-in role

        RoleGraph graph;
        ASSERT_OK(graph.createRole(roleA));
        ASSERT_OK(graph.createRole(roleB));
        ASSERT_OK(graph.createRole(roleC));

        RoleNameIterator it;
        it = graph.getDirectSubordinates(roleA);
        ASSERT_FALSE(it.more());
        it = graph.getDirectMembers(roleA);
        ASSERT_FALSE(it.more());

        ASSERT_OK(graph.addRoleToRole(roleA, roleB));

        // A -> B
        it = graph.getDirectSubordinates(roleA);
        ASSERT_TRUE(it.more());
        // should not advance the iterator
        ASSERT_EQUALS(it.get().getFullName(), roleB.getFullName());
        ASSERT_EQUALS(it.get().getFullName(), roleB.getFullName());
        ASSERT_EQUALS(it.next().getFullName(), roleB.getFullName());
        ASSERT_FALSE(it.more());

        it = graph.getDirectMembers(roleA);
        ASSERT_FALSE(it.more());

        it = graph.getDirectMembers(roleB);
        ASSERT_EQUALS(it.next().getFullName(), roleA.getFullName());
        ASSERT_FALSE(it.more());

        it = graph.getDirectSubordinates(roleB);
        ASSERT_FALSE(it.more());

        ASSERT_OK(graph.addRoleToRole(roleA, roleC));
        ASSERT_OK(graph.addRoleToRole(roleB, roleC));
        ASSERT_OK(graph.addRoleToRole(roleB, roleD));

        /*
         * Graph now looks like:
         *   A
         *  / \
         * v   v
         * B -> C
         * |
         * v
         * D
        */


        it = graph.getDirectSubordinates(roleA); // should be roleB and roleC, order doesn't matter
        RoleName cur = it.next();
        if (cur == roleB) {
            ASSERT_EQUALS(it.next().getFullName(), roleC.getFullName());
        } else if (cur == roleC) {
            ASSERT_EQUALS(it.next().getFullName(), roleB.getFullName());
        } else {
            FAIL(mongoutils::str::stream() << "unexpected role returned: " << cur.getFullName());
        }
        ASSERT_FALSE(it.more());

        it = graph.getDirectSubordinates(roleB); // should be roleC and roleD, order doesn't matter
        cur = it.next();
        if (cur == roleC) {
            ASSERT_EQUALS(it.next().getFullName(), roleD.getFullName());
        } else if (cur == roleD) {
            ASSERT_EQUALS(it.next().getFullName(), roleC.getFullName());
        } else {
            FAIL(mongoutils::str::stream() << "unexpected role returned: " << cur.getFullName());
        }
        ASSERT_FALSE(it.more());

        it = graph.getDirectSubordinates(roleC);
        ASSERT_FALSE(it.more());

        it = graph.getDirectMembers(roleA);
        ASSERT_FALSE(it.more());

        it = graph.getDirectMembers(roleB);
        ASSERT_EQUALS(it.next().getFullName(), roleA.getFullName());
        ASSERT_FALSE(it.more());

        it = graph.getDirectMembers(roleC); // should be role A and role B, order doesn't matter
        cur = it.next();
        if (cur == roleA) {
            ASSERT_EQUALS(it.next().getFullName(), roleB.getFullName());
        } else if (cur == roleB) {
            ASSERT_EQUALS(it.next().getFullName(), roleA.getFullName());
        } else {
            FAIL(mongoutils::str::stream() << "unexpected role returned: " << cur.getFullName());
        }
        ASSERT_FALSE(it.more());

        // Now remove roleD from roleB and make sure graph is update correctly
        ASSERT_OK(graph.removeRoleFromRole(roleB, roleD));

        /*
         * Graph now looks like:
         *   A
         *  / \
         * v   v
         * B -> C
         */
        it = graph.getDirectSubordinates(roleB); // should be just roleC
        ASSERT_EQUALS(it.next().getFullName(), roleC.getFullName());
        ASSERT_FALSE(it.more());

        it = graph.getDirectSubordinates(roleD); // should be empty
        ASSERT_FALSE(it.more());


        // Now delete roleB entirely and make sure that the other roles are updated properly
        ASSERT_OK(graph.deleteRole(roleB));
        ASSERT_NOT_OK(graph.deleteRole(roleB));
        it = graph.getDirectSubordinates(roleA);
        ASSERT_EQUALS(it.next().getFullName(), roleC.getFullName());
        ASSERT_FALSE(it.more());
        it = graph.getDirectMembers(roleC);
        ASSERT_EQUALS(it.next().getFullName(), roleA.getFullName());
        ASSERT_FALSE(it.more());
    }

    // Tests that adding multiple privileges on the same resource correctly collapses those to one
    // privilege
    TEST(RoleGraphTest, AddPrivileges) {
        RoleName roleA("roleA", "dbA");

        RoleGraph graph;
        ASSERT_OK(graph.createRole(roleA));

        // Test adding a single privilege
        ActionSet actions;
        actions.addAction(ActionType::find);
        ASSERT_OK(graph.addPrivilegeToRole(roleA, Privilege("dbA", actions)));

        PrivilegeVector privileges = graph.getDirectPrivileges(roleA);
        ASSERT_EQUALS(static_cast<size_t>(1), privileges.size());
        ASSERT_EQUALS("dbA", privileges[0].getResource());
        ASSERT_EQUALS(actions.toString(), privileges[0].getActions().toString());

        // Add a privilege on a different resource
        ASSERT_OK(graph.addPrivilegeToRole(roleA, Privilege("dbA.foo", actions)));
        privileges = graph.getDirectPrivileges(roleA);
        ASSERT_EQUALS(static_cast<size_t>(2), privileges.size());
        ASSERT_EQUALS("dbA", privileges[0].getResource());
        ASSERT_EQUALS(actions.toString(), privileges[0].getActions().toString());
        ASSERT_EQUALS("dbA.foo", privileges[1].getResource());
        ASSERT_EQUALS(actions.toString(), privileges[1].getActions().toString());


        // Add different privileges on an existing resource and make sure they get de-duped
        actions.removeAllActions();
        actions.addAction(ActionType::insert);

        PrivilegeVector privilegesToAdd;
        privilegesToAdd.push_back(Privilege("dbA", actions));

        actions.removeAllActions();
        actions.addAction(ActionType::update);
        privilegesToAdd.push_back(Privilege("dbA", actions));

        ASSERT_OK(graph.addPrivilegesToRole(roleA, privilegesToAdd));

        privileges = graph.getDirectPrivileges(roleA);
        ASSERT_EQUALS(static_cast<size_t>(2), privileges.size());
        ASSERT_EQUALS("dbA", privileges[0].getResource());
        ASSERT_NOT_EQUALS(actions.toString(), privileges[0].getActions().toString());
        actions.addAction(ActionType::find);
        actions.addAction(ActionType::insert);
        ASSERT_EQUALS(actions.toString(), privileges[0].getActions().toString());
        actions.removeAction(ActionType::insert);
        actions.removeAction(ActionType::update);
        ASSERT_EQUALS("dbA.foo", privileges[1].getResource());
        ASSERT_EQUALS(actions.toString(), privileges[1].getActions().toString());
    }

    // Tests that recomputePrivilegeData correctly detects cycles in the graph.
    TEST(RoleGraphTest, DetectCycles) {
        RoleName roleA("roleA", "dbA");
        RoleName roleB("roleB", "dbB");
        RoleName roleC("roleC", "dbC");
        RoleName roleD("roleD", "dbD");

        RoleGraph graph;
        ASSERT_OK(graph.createRole(roleA));
        ASSERT_OK(graph.createRole(roleB));
        ASSERT_OK(graph.createRole(roleC));
        ASSERT_OK(graph.createRole(roleD));

        ASSERT_OK(graph.recomputePrivilegeData());
        ASSERT_OK(graph.addRoleToRole(roleA, roleB));
        ASSERT_OK(graph.recomputePrivilegeData());
        ASSERT_OK(graph.addRoleToRole(roleA, roleC));
        ASSERT_OK(graph.addRoleToRole(roleB, roleC));
        ASSERT_OK(graph.recomputePrivilegeData());
        /*
         * Graph now looks like:
         *    A
         *   / \
         *  v   v
         *  B -> C
         */
        ASSERT_OK(graph.addRoleToRole(roleC, roleD));
        ASSERT_OK(graph.addRoleToRole(roleD, roleB)); // Add a cycle
        /*
         * Graph now looks like:
         *    A
         *   / \
         *  v   v
         *  B -> C
         *  ^   /
         *   \ v
         *    D
         */
        ASSERT_NOT_OK(graph.recomputePrivilegeData());
        ASSERT_OK(graph.removeRoleFromRole(roleD, roleB));
        ASSERT_OK(graph.recomputePrivilegeData());
    }

    // Tests that recomputePrivilegeData correctly updates transitive privilege data for all roles.
    TEST(RoleGraphTest, RecomputePrivilegeData) {
        // We create 4 roles and give each of them a unique privilege.  After that the direct
        // privileges for all the roles are not touched.  The only thing that is changed is the
        // role membership graph, and we test how that affects the set of all transitive privileges
        // for each role.
        RoleName roleA("roleA", "dbA");
        RoleName roleB("roleB", "dbB");
        RoleName roleC("roleC", "dbC");
        RoleName roleD("readWrite", "dbD"); // built-in role

        ActionSet actions;
        actions.addAllActions();

        RoleGraph graph;
        ASSERT_OK(graph.createRole(roleA));
        ASSERT_OK(graph.createRole(roleB));
        ASSERT_OK(graph.createRole(roleC));

        ASSERT_OK(graph.addPrivilegeToRole(roleA, Privilege("dbA", actions)));
        ASSERT_OK(graph.addPrivilegeToRole(roleB, Privilege("dbB", actions)));
        ASSERT_OK(graph.addPrivilegeToRole(roleC, Privilege("dbC", actions)));

        ASSERT_OK(graph.recomputePrivilegeData());

        PrivilegeVector privileges = graph.getAllPrivileges(roleA);
        ASSERT_EQUALS(static_cast<size_t>(1), privileges.size());
        ASSERT_EQUALS("dbA", privileges[0].getResource());

        // At this point we have all 4 roles set up, each with their own privilege, but no
        // roles have been granted to each other.

        ASSERT_OK(graph.addRoleToRole(roleA, roleB));
        // Role graph: A->B
        ASSERT_OK(graph.recomputePrivilegeData());
        privileges = graph.getAllPrivileges(roleA);
        ASSERT_EQUALS(static_cast<size_t>(2), privileges.size());
        ASSERT_EQUALS("dbA", privileges[0].getResource());
        ASSERT_EQUALS("dbB", privileges[1].getResource());

        // Add's roleC's privileges to roleB and make sure roleA gets them as well.
        ASSERT_OK(graph.addRoleToRole(roleB, roleC));
        // Role graph: A->B->C
        ASSERT_OK(graph.recomputePrivilegeData());
        privileges = graph.getAllPrivileges(roleA);
        ASSERT_EQUALS(static_cast<size_t>(3), privileges.size());
        ASSERT_EQUALS("dbA", privileges[0].getResource());
        ASSERT_EQUALS("dbB", privileges[1].getResource());
        ASSERT_EQUALS("dbC", privileges[2].getResource());
        privileges = graph.getAllPrivileges(roleB);
        ASSERT_EQUALS(static_cast<size_t>(2), privileges.size());
        ASSERT_EQUALS("dbB", privileges[0].getResource());
        ASSERT_EQUALS("dbC", privileges[1].getResource());

        // Add's roleD's privileges to roleC and make sure that roleA and roleB get them as well.
        ASSERT_OK(graph.addRoleToRole(roleC, roleD));
        // Role graph: A->B->C->D
        ASSERT_OK(graph.recomputePrivilegeData());
        privileges = graph.getAllPrivileges(roleA);
        ASSERT_EQUALS(static_cast<size_t>(4), privileges.size());
        ASSERT_EQUALS("dbA", privileges[0].getResource());
        ASSERT_EQUALS("dbB", privileges[1].getResource());
        ASSERT_EQUALS("dbC", privileges[2].getResource());
        ASSERT_EQUALS("dbD", privileges[3].getResource());
        privileges = graph.getAllPrivileges(roleB);
        ASSERT_EQUALS(static_cast<size_t>(3), privileges.size());
        ASSERT_EQUALS("dbB", privileges[0].getResource());
        ASSERT_EQUALS("dbC", privileges[1].getResource());
        ASSERT_EQUALS("dbD", privileges[2].getResource());
        privileges = graph.getAllPrivileges(roleC);
        ASSERT_EQUALS(static_cast<size_t>(2), privileges.size());
        ASSERT_EQUALS("dbC", privileges[0].getResource());
        ASSERT_EQUALS("dbD", privileges[1].getResource());
        privileges = graph.getAllPrivileges(roleD);
        ASSERT_EQUALS(static_cast<size_t>(1), privileges.size());
        ASSERT_EQUALS("dbD", privileges[0].getResource());

        // Remove roleC from roleB, make sure that roleA then loses both roleC's and roleD's
        // privileges
        ASSERT_OK(graph.removeRoleFromRole(roleB, roleC));
        // Role graph: A->B     C->D
        ASSERT_OK(graph.recomputePrivilegeData());
        privileges = graph.getAllPrivileges(roleA);
        ASSERT_EQUALS(static_cast<size_t>(2), privileges.size());
        ASSERT_EQUALS("dbA", privileges[0].getResource());
        ASSERT_EQUALS("dbB", privileges[1].getResource());
        privileges = graph.getAllPrivileges(roleB);
        ASSERT_EQUALS(static_cast<size_t>(1), privileges.size());
        ASSERT_EQUALS("dbB", privileges[0].getResource());
        privileges = graph.getAllPrivileges(roleC);
        ASSERT_EQUALS(static_cast<size_t>(2), privileges.size());
        ASSERT_EQUALS("dbC", privileges[0].getResource());
        ASSERT_EQUALS("dbD", privileges[1].getResource());
        privileges = graph.getAllPrivileges(roleD);
        ASSERT_EQUALS(static_cast<size_t>(1), privileges.size());
        ASSERT_EQUALS("dbD", privileges[0].getResource());

        // Make sure direct privileges were untouched
        privileges = graph.getDirectPrivileges(roleA);
        ASSERT_EQUALS(static_cast<size_t>(1), privileges.size());
        ASSERT_EQUALS("dbA", privileges[0].getResource());
        privileges = graph.getDirectPrivileges(roleB);
        ASSERT_EQUALS(static_cast<size_t>(1), privileges.size());
        ASSERT_EQUALS("dbB", privileges[0].getResource());
        privileges = graph.getDirectPrivileges(roleC);
        ASSERT_EQUALS(static_cast<size_t>(1), privileges.size());
        ASSERT_EQUALS("dbC", privileges[0].getResource());
        privileges = graph.getDirectPrivileges(roleD);
        ASSERT_EQUALS(static_cast<size_t>(1), privileges.size());
        ASSERT_EQUALS("dbD", privileges[0].getResource());
    }

    // Test that if you grant 1 role to another, then remove it and change it's privileges, then
    // re-grant it, the receiving role sees the new privileges and not the old ones.
    TEST(RoleGraphTest, ReAddRole) {
        RoleName roleA("roleA", "dbA");
        RoleName roleB("roleB", "dbB");
        RoleName roleC("roleC", "dbC");

        ActionSet actionsA, actionsB, actionsC;
        actionsA.addAction(ActionType::find);
        actionsB.addAction(ActionType::insert);
        actionsC.addAction(ActionType::update);

        RoleGraph graph;
        ASSERT_OK(graph.createRole(roleA));
        ASSERT_OK(graph.createRole(roleB));
        ASSERT_OK(graph.createRole(roleC));

        ASSERT_OK(graph.addPrivilegeToRole(roleA, Privilege("db", actionsA)));
        ASSERT_OK(graph.addPrivilegeToRole(roleB, Privilege("db", actionsB)));
        ASSERT_OK(graph.addPrivilegeToRole(roleC, Privilege("db", actionsC)));

        ASSERT_OK(graph.addRoleToRole(roleA, roleB));
        ASSERT_OK(graph.addRoleToRole(roleB, roleC)); // graph: A <- B <- C

        ASSERT_OK(graph.recomputePrivilegeData());

        // roleA should have privileges from roleB and roleC
        PrivilegeVector privileges = graph.getAllPrivileges(roleA);
        ASSERT_EQUALS(static_cast<size_t>(1), privileges.size());
        ASSERT_TRUE(privileges[0].getActions().contains(ActionType::find));
        ASSERT_TRUE(privileges[0].getActions().contains(ActionType::insert));
        ASSERT_TRUE(privileges[0].getActions().contains(ActionType::update));

        // Now remove roleB from roleA.  B still is a member of C, but A no longer should have
        // privileges from B or C.
        ASSERT_OK(graph.removeRoleFromRole(roleA, roleB));
        ASSERT_OK(graph.recomputePrivilegeData());

        // roleA should no longer have the privileges from roleB or roleC
        privileges = graph.getAllPrivileges(roleA);
        ASSERT_EQUALS(static_cast<size_t>(1), privileges.size());
        ASSERT_TRUE(privileges[0].getActions().contains(ActionType::find));
        ASSERT_FALSE(privileges[0].getActions().contains(ActionType::insert));
        ASSERT_FALSE(privileges[0].getActions().contains(ActionType::update));

        // Change the privileges that roleB grants
        ASSERT_OK(graph.removeAllPrivilegesFromRole(roleB));
        ActionSet newActionsB;
        newActionsB.addAction(ActionType::remove);
        ASSERT_OK(graph.addPrivilegeToRole(roleB, Privilege("db", newActionsB)));

        // Grant roleB back to roleA, make sure roleA has roleB's new privilege but not its old one.
        ASSERT_OK(graph.addRoleToRole(roleA, roleB));
        ASSERT_OK(graph.recomputePrivilegeData());

        privileges = graph.getAllPrivileges(roleA);
        ASSERT_EQUALS(static_cast<size_t>(1), privileges.size());
        ASSERT_TRUE(privileges[0].getActions().contains(ActionType::find));
        ASSERT_TRUE(privileges[0].getActions().contains(ActionType::update)); // should get roleC's actions again
        ASSERT_TRUE(privileges[0].getActions().contains(ActionType::remove)); // roleB should grant this to roleA
        ASSERT_FALSE(privileges[0].getActions().contains(ActionType::insert)); // no roles have this action anymore

        // Now delete roleB completely.  A should once again lose the privileges from both B and C.
        ASSERT_OK(graph.deleteRole(roleB));
        ASSERT_OK(graph.recomputePrivilegeData());

        // roleA should no longer have the privileges from roleB or roleC
        privileges = graph.getAllPrivileges(roleA);
        ASSERT_EQUALS(static_cast<size_t>(1), privileges.size());
        ASSERT_TRUE(privileges[0].getActions().contains(ActionType::find));
        ASSERT_FALSE(privileges[0].getActions().contains(ActionType::update));
        ASSERT_FALSE(privileges[0].getActions().contains(ActionType::remove));
        ASSERT_FALSE(privileges[0].getActions().contains(ActionType::insert));

        // Now re-create roleB and give it a new privilege, then grant it back to roleA.
        // RoleA should get its new privilege but not roleC's privilege this time nor either of
        // roleB's old privileges.
        ASSERT_OK(graph.createRole(roleB));
        actionsB.removeAllActions();
        actionsB.addAction(ActionType::shutdown);
        ASSERT_OK(graph.addPrivilegeToRole(roleB, Privilege("db", actionsB)));
        ASSERT_OK(graph.addRoleToRole(roleA, roleB));
        ASSERT_OK(graph.recomputePrivilegeData());

        privileges = graph.getAllPrivileges(roleA);
        ASSERT_EQUALS(static_cast<size_t>(1), privileges.size());
        ASSERT_TRUE(privileges[0].getActions().contains(ActionType::find));
        ASSERT_TRUE(privileges[0].getActions().contains(ActionType::shutdown));
        ASSERT_FALSE(privileges[0].getActions().contains(ActionType::update));
        ASSERT_FALSE(privileges[0].getActions().contains(ActionType::remove));
        ASSERT_FALSE(privileges[0].getActions().contains(ActionType::insert));
    }

    // Tests copy constructor and swap functionality.
    TEST(RoleGraphTest, CopySwap) {
        RoleName roleA("roleA", "dbA");
        RoleName roleB("roleB", "dbB");
        RoleName roleC("roleC", "dbC");

        RoleGraph graph;
        ASSERT_OK(graph.createRole(roleA));
        ASSERT_OK(graph.createRole(roleB));
        ASSERT_OK(graph.createRole(roleC));

        ActionSet actions;
        actions.addAction(ActionType::find);
        ASSERT_OK(graph.addPrivilegeToRole(roleA, Privilege("dbA", actions)));
        ASSERT_OK(graph.addPrivilegeToRole(roleB, Privilege("dbB", actions)));
        ASSERT_OK(graph.addPrivilegeToRole(roleC, Privilege("dbC", actions)));

        ASSERT_OK(graph.addRoleToRole(roleA, roleB));

        // Make a copy of the graph to do further modifications on.
        RoleGraph tempGraph(graph);
        ASSERT_OK(tempGraph.addRoleToRole(roleB, roleC));
        tempGraph.recomputePrivilegeData();

        // Now swap the copy back with the original graph and make sure the original was updated
        // properly.
        swap(tempGraph, graph);

        RoleNameIterator it = graph.getDirectSubordinates(roleB);
        ASSERT_TRUE(it.more());
        ASSERT_EQUALS(it.next().getFullName(), roleC.getFullName());
        ASSERT_FALSE(it.more());

        graph.getAllPrivileges(roleA); // should have privileges from roleB *and* role C
        PrivilegeVector privileges = graph.getAllPrivileges(roleA);
        ASSERT_EQUALS(static_cast<size_t>(3), privileges.size());
        ASSERT_EQUALS("dbA", privileges[0].getResource());
        ASSERT_EQUALS("dbB", privileges[1].getResource());
        ASSERT_EQUALS("dbC", privileges[2].getResource());
    }

    // Tests error handling
    TEST(RoleGraphTest, ErrorHandling) {
        RoleName roleA("roleA", "dbA");
        RoleName roleB("roleB", "dbB");
        RoleName roleC("roleC", "dbC");

        ActionSet actions;
        actions.addAction(ActionType::find);
        Privilege privilege1("db1", actions);
        Privilege privilege2("db2", actions);
        PrivilegeVector privileges;
        privileges.push_back(privilege1);
        privileges.push_back(privilege2);

        RoleGraph graph;
        // None of the roles exist yet.
        ASSERT_NOT_OK(graph.addPrivilegeToRole(roleA, privilege1));
        ASSERT_NOT_OK(graph.addPrivilegesToRole(roleA, privileges));
        ASSERT_NOT_OK(graph.removePrivilegeFromRole(roleA, privilege1));
        ASSERT_NOT_OK(graph.removePrivilegesFromRole(roleA, privileges));
        ASSERT_NOT_OK(graph.removeAllPrivilegesFromRole(roleA));
        ASSERT_NOT_OK(graph.addRoleToRole(roleA, roleB));
        ASSERT_NOT_OK(graph.removeRoleFromRole(roleA, roleB));

        // One of the roles exists
        ASSERT_OK(graph.createRole(roleA));
        ASSERT_NOT_OK(graph.createRole(roleA)); // Can't create same role twice
        ASSERT_NOT_OK(graph.addRoleToRole(roleA, roleB));
        ASSERT_NOT_OK(graph.addRoleToRole(roleB, roleA));
        ASSERT_NOT_OK(graph.removeRoleFromRole(roleA, roleB));
        ASSERT_NOT_OK(graph.removeRoleFromRole(roleB, roleA));

        // Should work now that both exist.
        ASSERT_OK(graph.createRole(roleB));
        ASSERT_OK(graph.addRoleToRole(roleA, roleB));
        ASSERT_OK(graph.removeRoleFromRole(roleA, roleB));
        ASSERT_NOT_OK(graph.removeRoleFromRole(roleA, roleB)); // roleA isn't actually a member of roleB

        // Can't remove a privilege from a role that doesn't have it.
        ASSERT_NOT_OK(graph.removePrivilegeFromRole(roleA, privilege1));
        ASSERT_OK(graph.addPrivilegeToRole(roleA, privilege1));
        ASSERT_OK(graph.removePrivilegeFromRole(roleA, privilege1)); // now should work

        // Test that removing a vector of privileges fails if *any* of the privileges are missing.
        ASSERT_OK(graph.addPrivilegeToRole(roleA, privilege1));
        ASSERT_OK(graph.addPrivilegeToRole(roleA, privilege2));
        // Removing both privileges should work since it has both
        ASSERT_OK(graph.removePrivilegesFromRole(roleA, privileges));
        // Now add only 1 back and this time removing both should fail
        ASSERT_OK(graph.addPrivilegeToRole(roleA, privilege1));
        ASSERT_NOT_OK(graph.removePrivilegesFromRole(roleA, privileges));
    }


    TEST(RoleGraphTest, BuiltinRoles) {
        RoleName userRole("userDefined", "dbA");
        RoleName builtinRole("read", "dbA");

        ActionSet actions;
        actions.addAction(ActionType::insert);
        Privilege privilege("dbA", actions);

        RoleGraph graph;

        ASSERT(graph.roleExists(builtinRole));
        ASSERT_NOT_OK(graph.createRole(builtinRole));
        ASSERT_NOT_OK(graph.deleteRole(builtinRole));
        ASSERT(graph.roleExists(builtinRole));
        ASSERT(!graph.roleExists(userRole));
        ASSERT_OK(graph.createRole(userRole));
        ASSERT(graph.roleExists(userRole));

        ASSERT_NOT_OK(graph.addPrivilegeToRole(builtinRole, privilege));
        ASSERT_NOT_OK(graph.removePrivilegeFromRole(builtinRole, privilege));
        ASSERT_NOT_OK(graph.addRoleToRole(builtinRole, userRole));
        ASSERT_NOT_OK(graph.removeRoleFromRole(builtinRole, userRole));

        ASSERT_OK(graph.addPrivilegeToRole(userRole, privilege));
        ASSERT_OK(graph.addRoleToRole(userRole, builtinRole));
        ASSERT_OK(graph.recomputePrivilegeData());

        PrivilegeVector privileges = graph.getDirectPrivileges(userRole);
        ASSERT_EQUALS(1U, privileges.size());
        ASSERT(privileges[0].getActions().equals(actions));
        ASSERT(!privileges[0].getActions().contains(ActionType::find));
        ASSERT_EQUALS("dbA", privileges[0].getResource());

        privileges = graph.getAllPrivileges(userRole);
        ASSERT_EQUALS(1U, privileges.size());
        ASSERT(privileges[0].getActions().isSupersetOf(actions));
        ASSERT(privileges[0].getActions().contains(ActionType::insert));
        ASSERT(privileges[0].getActions().contains(ActionType::find));
        ASSERT_EQUALS("dbA", privileges[0].getResource());

        ASSERT_OK(graph.deleteRole(userRole));
        ASSERT(!graph.roleExists(userRole));
    }

}  // namespace
}  // namespace mongo
