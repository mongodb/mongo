/*
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
 * These tests verify that modifications to user-defined roles propagate to secondary nodes.
 *
 * First, it sets up a 1-node replicaset and adds some roles.
 * Then, it adds a second node to the replicaset, and verifies that the roles sync correctly.
 * Then, it runs a variety of operations on the primary, and ensures that they replicate correctly.
 */

(function() {

    var name = 'user_defined_roles_on_secondaries';
    var m0, m1;

    function assertListContainsRole(list, role, msg) {
        var i;
        for (i = 0; i < list.length; ++i) {
            if (list[i].role == role.role && list[i].db == role.db)
                return;
        }
        doassert("Could not find value " + tojson(val) + " in " +
                 tojson(list)(msg ? ": " + msg : ""));
    }

    //
    // Create a 1-node replicaset and add two roles, inheriting the built-in read role on db1.
    //
    //     read
    //    /    \
    //  r1      r2
    //
    var rstest = new ReplSetTest({name: name, nodes: 1, nodeOptions: {}});

    rstest.startSet();
    rstest.initiate();

    m0 = rstest.nodes[0];

    m0.getDB("db1").createRole({
        role: "r1",
        roles: ["read"],
        privileges: [{resource: {db: "db1", collection: "system.users"}, actions: ["find"]}]
    });

    m0.getDB("db1").createRole({
        role: "r2",
        roles: ["read"],
        privileges: [{resource: {db: "db1", collection: "log"}, actions: ["insert"]}]
    });

    //
    // Add a second node to the set, and add a third role, dependent on the first two.
    //
    //     read
    //    /    \
    //  r1      r2
    //    \    /
    //      r3
    //
    rstest.add();
    rstest.reInitiate();

    // This write will have to wait on the initial sync to complete before progressing.
    assert.soonNoExcept(() => {
        assert.writeOK(rstest.getPrimary().getDB("db1")["aCollection"].insert(
            {a: "afterSecondNodeAdded"}, {writeConcern: {w: 2, wtimeout: 60 * 1000}}));
        return true;
    });

    rstest.getPrimary().getDB("db1").createRole({
        role: "r3",
        roles: ["r1", "r2"],
        privileges: [{resource: {db: "db1", collection: "log"}, actions: ["update"]}]
    },
                                                {w: 2});

    // Verify that both members of the set see the same role graph.
    rstest.nodes.forEach(function(node) {
        var role = node.getDB("db1").getRole("r3");
        assert.eq(2, role.roles.length, node);
        assertListContainsRole(role.roles, {role: "r1", db: "db1"}, node);
        assertListContainsRole(role.roles, {role: "r2", db: "db1"}, node);
        assert.eq(3, role.inheritedRoles.length, node);
        assertListContainsRole(role.inheritedRoles, {role: "r1", db: "db1"}, node);
        assertListContainsRole(role.inheritedRoles, {role: "r2", db: "db1"}, node);
        assertListContainsRole(role.inheritedRoles, {role: "read", db: "db1"}, node);
    });

    // Verify that updating roles propagates.
    rstest.getPrimary().getDB("db1").revokeRolesFromRole("r1", ["read"], {w: 2});
    rstest.getPrimary().getDB("db1").grantRolesToRole("r1", ["dbAdmin"], {w: 2});
    rstest.nodes.forEach(function(node) {
        var role = node.getDB("db1").getRole("r1");
        assert.eq(1, role.roles.length, node);
        assertListContainsRole(role.roles, {role: "dbAdmin", db: "db1"});
    });

    // Verify that dropping roles propagates.
    rstest.getPrimary().getDB("db1").dropRole("r2", {w: 2});
    rstest.nodes.forEach(function(node) {
        assert.eq(null, node.getDB("db1").getRole("r2"));
        var role = node.getDB("db1").getRole("r3");
        assert.eq(1, role.roles.length, node);
        assertListContainsRole(role.roles, {role: "r1", db: "db1"}, node);
        assert.eq(2, role.inheritedRoles.length, node);
        assertListContainsRole(role.inheritedRoles, {role: "r1", db: "db1"}, node);
        assertListContainsRole(role.inheritedRoles, {role: "dbAdmin", db: "db1"}, node);
    });

    // Verify that dropping the admin database propagates.
    assert.commandWorked(rstest.getPrimary().getDB("admin").dropDatabase());
    assert.commandWorked(rstest.getPrimary().getDB("admin").getLastErrorObj(2));
    rstest.nodes.forEach(function(node) {
        var roles = node.getDB("db1").getRoles();
        assert.eq(0, roles.length, node);
    });

    // Verify that applyOps commands propagate.
    // NOTE: This section of the test depends on the oplog and roles schemas.
    assert.commandWorked(rstest.getPrimary().getDB("admin").runCommand({
        applyOps: [
            {op: "c", ns: "admin.$cmd", o: {create: "system.roles"}},
            {
              op: "i",
              ns: "admin.system.roles",
              o: {
                  _id: "db1.s1",
                  role: "s1",
                  db: "db1",
                  roles: [{role: "read", db: "db1"}],
                  privileges:
                      [{resource: {db: "db1", collection: "system.users"}, actions: ["find"]}]
              }
            },
            {
              op: "i",
              ns: "admin.system.roles",
              o: {
                  _id: "db1.s2",
                  role: "s2",
                  db: "db1",
                  roles: [{role: "read", db: "db1"}],
                  privileges: [{resource: {db: "db1", collection: "log"}, actions: ["insert"]}]
              }
            },
            {op: "c", ns: "admin.$cmd", o: {dropDatabase: 1}},
            {op: "c", ns: "admin.$cmd", o: {create: "system.roles"}},
            {
              op: "i",
              ns: "admin.system.roles",
              o: {
                  _id: "db1.t1",
                  role: "t1",
                  db: "db1",
                  roles: [{role: "read", db: "db1"}],
                  privileges:
                      [{resource: {db: "db1", collection: "system.users"}, actions: ["find"]}]
              }
            },
            {
              op: "i",
              ns: "admin.system.roles",
              o: {
                  _id: "db1.t2",
                  role: "t2",
                  db: "db1",
                  roles: [],
                  privileges: [{resource: {db: "db1", collection: "log"}, actions: ["insert"]}]
              }
            },
            {
              op: "u",
              ns: "admin.system.roles",
              o: {$set: {roles: [{role: "readWrite", db: "db1"}]}},
              o2: {_id: "db1.t2"}
            }
        ]
    }));

    assert.commandWorked(rstest.getPrimary().getDB("admin").getLastErrorObj(2));
    rstest.nodes.forEach(function(node) {
        var role = node.getDB("db1").getRole("t1");
        assert.eq(1, role.roles.length, node);
        assertListContainsRole(role.roles, {role: "read", db: "db1"}, node);

        var role = node.getDB("db1").getRole("t2");
        assert.eq(1, role.roles.length, node);
        assertListContainsRole(role.roles, {role: "readWrite", db: "db1"}, node);
    });

}());
