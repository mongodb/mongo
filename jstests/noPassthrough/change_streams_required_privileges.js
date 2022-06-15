// Tests that a change stream requires the correct privileges to be run.
// This test uses the WiredTiger storage engine, which does not support running without journaling.
// @tags: [
//   requires_majority_read_concern,
//   requires_persistence,
//   requires_replication,
// ]
(function() {
"use strict";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();
const password = "test_password";
rst.getPrimary().getDB("admin").createUser(
    {user: "userAdmin", pwd: password, roles: [{db: "admin", role: "userAdminAnyDatabase"}]});
rst.restart(0, {auth: '', keyFile: 'jstests/libs/key1'});

const db = rst.getPrimary().getDB("test");
const coll = db.coll;
const adminDB = db.getSiblingDB("admin");

// Wrap different sections of the test in separate functions to make the scoping clear.
(function createRoles() {
    assert(adminDB.auth("userAdmin", password));
    // Create some collection-level roles.
    db.createRole({
        role: "write",
        roles: [],
        privileges: [{
            resource: {db: db.getName(), collection: coll.getName()},
            actions: ["insert", "update", "remove"]
        }]
    });
    db.createRole({
        role: "find_only",
        roles: [],
        privileges: [{resource: {db: db.getName(), collection: coll.getName()}, actions: ["find"]}]
    });
    db.createRole({
        role: "find_and_change_stream",
        roles: [],
        privileges: [{
            resource: {db: db.getName(), collection: coll.getName()},
            actions: ["find", "changeStream"]
        }]
    });
    db.createRole({
        role: "change_stream_only",
        roles: [],
        privileges:
            [{resource: {db: db.getName(), collection: coll.getName()}, actions: ["changeStream"]}]
    });

    // Create some privileges at the database level.
    db.createRole({
        role: "db_write",
        roles: [],
        privileges: [
            {resource: {db: db.getName(), collection: ""},
             actions: ["insert", "update", "remove"]}
        ]
    });
    db.createRole({
        role: "db_find_only",
        roles: [],
        privileges: [{resource: {db: db.getName(), collection: ""}, actions: ["find"]}]
    });
    db.createRole({
        role: "db_find_and_change_stream",
        roles: [],
        privileges:
            [{resource: {db: db.getName(), collection: ""}, actions: ["find", "changeStream"]}]
    });
    db.createRole({
        role: "db_change_stream_only",
        roles: [],
        privileges: [{resource: {db: db.getName(), collection: ""}, actions: ["changeStream"]}]
    });

    // Create some privileges at the admin database level.
    adminDB.createRole({
        role: "admin_db_write",
        roles: [],
        privileges: [
            {resource: {db: db.getName(), collection: ""},
             actions: ["insert", "update", "remove"]}
        ]
    });
    adminDB.createRole({
        role: "admin_db_find_only",
        roles: [],
        privileges: [{resource: {db: "admin", collection: ""}, actions: ["find"]}]
    });
    adminDB.createRole({
        role: "admin_db_find_and_change_stream",
        roles: [],
        privileges: [{resource: {db: "admin", collection: ""}, actions: ["find", "changeStream"]}]
    });
    adminDB.createRole({
        role: "admin_db_change_stream_only",
        roles: [],
        privileges: [{resource: {db: "admin", collection: ""}, actions: ["changeStream"]}]
    });

    // Create some roles at the any-db, any-collection level.
    adminDB.createRole({
        role: "any_db_find_only",
        roles: [],
        privileges: [{resource: {db: "", collection: ""}, actions: ["find"]}]
    });
    adminDB.createRole({
        role: "any_db_find_and_change_stream",
        roles: [],
        privileges: [{resource: {db: "", collection: ""}, actions: ["find", "changeStream"]}]
    });
    adminDB.createRole({
        role: "any_db_change_stream_only",
        roles: [],
        privileges: [{resource: {db: "", collection: ""}, actions: ["changeStream"]}]
    });

    // Create some roles at the cluster level.
    adminDB.createRole({
        role: "cluster_find_only",
        roles: [],
        privileges: [{resource: {cluster: true}, actions: ["find"]}]
    });
    adminDB.createRole({
        role: "cluster_find_and_change_stream",
        roles: [],
        privileges: [{resource: {cluster: true}, actions: ["find", "changeStream"]}]
    });
    adminDB.createRole({
        role: "cluster_change_stream_only",
        roles: [],
        privileges: [{resource: {cluster: true}, actions: ["changeStream"]}]
    });
    adminDB.logout();
}());

(function createUsers() {
    assert(adminDB.auth("userAdmin", password));

    // Create some users for a specific collection. Use the name of the role as the name of the
    // user.
    for (let role of ["write", "find_only", "find_and_change_stream", "change_stream_only"]) {
        db.createUser({user: role, pwd: password, roles: [role]});
    }

    // Create some users at the database level. Use the name of the role as the name of the
    // user, except for the built-in roles.
    for (let role of
             ["db_write", "db_find_only", "db_find_and_change_stream", "db_change_stream_only"]) {
        db.createUser({user: role, pwd: password, roles: [role]});
    }
    db.createUser({user: "db_read", pwd: password, roles: ["read"]});

    // Create some users on the admin database. Use the name of the role as the name of the
    // user, except for the built-in roles.
    for (let role of ["admin_db_write",
                      "admin_db_find_only",
                      "admin_db_find_and_change_stream",
                      "admin_db_change_stream_only"]) {
        adminDB.createUser({user: role, pwd: password, roles: [role]});
    }
    adminDB.createUser({user: "admin_db_read", pwd: password, roles: ["read"]});

    // Create some users with privileges on all databases. Use the name of the role as the name
    // of the user, except for the built-in roles.
    for (let role of ["any_db_find_only",
                      "any_db_find_and_change_stream",
                      "any_db_change_stream_only"]) {
        adminDB.createUser({user: role, pwd: password, roles: [role]});
    }

    // Create some users on the whole cluster. Use the name of the role as the name of the user.
    for (let role of ["cluster_find_only",
                      "cluster_find_and_change_stream",
                      "cluster_change_stream_only"]) {
        adminDB.createUser({user: role, pwd: password, roles: [role]});
    }

    adminDB.logout();
}());

(function testPrivilegesForSingleCollection() {
    // Test that users without the required privileges cannot open a change stream. A user
    // needs both the 'find' and 'changeStream' action on the collection. Note in particular
    // that the whole-cluster privileges (specified with {cluster: true}) is not enough to open
    // a change stream on any particular collection.
    for (let userWithoutPrivileges of [{db: db, name: "find_only"},
                                       {db: db, name: "change_stream_only"},
                                       {db: db, name: "write"},
                                       {db: db, name: "db_find_only"},
                                       {db: db, name: "db_change_stream_only"},
                                       {db: db, name: "db_write"},
                                       {db: adminDB, name: "admin_db_find_only"},
                                       {db: adminDB, name: "admin_db_find_and_change_stream"},
                                       {db: adminDB, name: "admin_db_change_stream_only"},
                                       {db: adminDB, name: "admin_db_read"},
                                       {db: adminDB, name: "any_db_find_only"},
                                       {db: adminDB, name: "any_db_change_stream_only"},
                                       {db: adminDB, name: "cluster_find_only"},
                                       {db: adminDB, name: "cluster_find_and_change_stream"},
                                       {db: adminDB, name: "cluster_change_stream_only"}]) {
        jsTestLog(`Testing user ${tojson(userWithoutPrivileges)} cannot open a change stream ` +
                  `on a collection`);
        const db = userWithoutPrivileges.db;
        assert(db.auth(userWithoutPrivileges.name, password));

        assert.commandFailedWithCode(
            coll.getDB().runCommand(
                {aggregate: coll.getName(), pipeline: [{$changeStream: {}}], cursor: {}}),
            ErrorCodes.Unauthorized);

        db.logout();
    }

    // Test that a user with the required privileges can open a change stream.
    for (let userWithPrivileges of [{db: db, name: "find_and_change_stream"},
                                    {db: db, name: "db_find_and_change_stream"},
                                    {db: db, name: "db_read"},
                                    {db: adminDB, name: "any_db_find_and_change_stream"}]) {
        jsTestLog(`Testing user ${tojson(userWithPrivileges)} _can_ open a change stream on a` +
                  ` collection`);
        const db = userWithPrivileges.db;
        assert(db.auth(userWithPrivileges.name, password));

        assert.doesNotThrow(() => coll.watch());

        db.logout();
    }
}());

(function testPrivilegesForWholeDB() {
    // Test that users without the required privileges cannot open a change stream. A user needs
    // both the 'find' and 'changeStream' action on the database. Note in particular that the
    // whole-cluster privileges (specified with {cluster: true}) is not enough to open a change
    // stream on the whole database.
    for (let userWithoutPrivileges of [{db: db, name: "find_only"},
                                       {db: db, name: "change_stream_only"},
                                       {db: db, name: "find_and_change_stream"},
                                       {db: db, name: "write"},
                                       {db: db, name: "db_find_only"},
                                       {db: db, name: "db_change_stream_only"},
                                       {db: db, name: "db_write"},
                                       {db: adminDB, name: "admin_db_find_only"},
                                       {db: adminDB, name: "admin_db_find_and_change_stream"},
                                       {db: adminDB, name: "admin_db_change_stream_only"},
                                       {db: adminDB, name: "admin_db_read"},
                                       {db: adminDB, name: "any_db_find_only"},
                                       {db: adminDB, name: "any_db_change_stream_only"},
                                       {db: adminDB, name: "cluster_find_only"},
                                       {db: adminDB, name: "cluster_find_and_change_stream"},
                                       {db: adminDB, name: "cluster_change_stream_only"}]) {
        jsTestLog(`Testing user ${tojson(userWithoutPrivileges)} cannot open a change stream` +
                  ` on the whole database`);
        const db = userWithoutPrivileges.db;
        assert(db.auth(userWithoutPrivileges.name, password));

        assert.commandFailedWithCode(
            coll.getDB().runCommand({aggregate: 1, pipeline: [{$changeStream: {}}], cursor: {}}),
            ErrorCodes.Unauthorized);

        db.logout();
    }

    // Test that a user with the required privileges can open a change stream.
    for (let userWithPrivileges of [{db: db, name: "db_find_and_change_stream"},
                                    {db: db, name: "db_read"},
                                    {db: adminDB, name: "any_db_find_and_change_stream"}]) {
        jsTestLog(`Testing user ${tojson(userWithPrivileges)} _can_ open a change stream on` +
                  ` the whole database`);
        const db = userWithPrivileges.db;
        assert(db.auth(userWithPrivileges.name, password));

        assert.doesNotThrow(() => coll.getDB().watch());

        db.logout();
    }
}());

(function testPrivilegesForWholeCluster() {
    // Test that users without the required privileges cannot open a change stream. A user needs
    // both the 'find' and 'changeStream' action on _any_ resource. Note in particular that the
    // whole-cluster privileges (specified with {cluster: true}) is not enough to open a change
    // stream on the whole cluster.
    for (let userWithoutPrivileges of [{db: db, name: "find_only"},
                                       {db: db, name: "change_stream_only"},
                                       {db: db, name: "find_and_change_stream"},
                                       {db: db, name: "write"},
                                       {db: db, name: "db_find_only"},
                                       {db: db, name: "db_find_and_change_stream"},
                                       {db: db, name: "db_change_stream_only"},
                                       {db: db, name: "db_read"},
                                       {db: db, name: "db_write"},
                                       {db: adminDB, name: "admin_db_find_only"},
                                       {db: adminDB, name: "admin_db_find_and_change_stream"},
                                       {db: adminDB, name: "admin_db_change_stream_only"},
                                       {db: adminDB, name: "admin_db_read"},
                                       {db: adminDB, name: "any_db_find_only"},
                                       {db: adminDB, name: "any_db_change_stream_only"},
                                       {db: adminDB, name: "cluster_find_only"},
                                       {db: adminDB, name: "cluster_change_stream_only"},
                                       {db: adminDB, name: "cluster_find_and_change_stream"}]) {
        jsTestLog(`Testing user ${tojson(userWithoutPrivileges)} cannot open a change stream` +
                  ` on the whole cluster`);
        const db = userWithoutPrivileges.db;
        assert(db.auth(userWithoutPrivileges.name, password));

        assert.commandFailedWithCode(adminDB.runCommand({
            aggregate: 1,
            pipeline: [{$changeStream: {allChangesForCluster: true}}],
            cursor: {}
        }),
                                     ErrorCodes.Unauthorized);

        db.logout();
    }

    // Test that a user with the required privileges can open a change stream.
    for (let userWithPrivileges of [{db: adminDB, name: "any_db_find_and_change_stream"}]) {
        jsTestLog(`Testing user ${tojson(userWithPrivileges)} _can_ open a change stream` +
                  ` on the whole cluster`);
        const db = userWithPrivileges.db;
        assert(db.auth(userWithPrivileges.name, password));

        assert.doesNotThrow(() => db.getMongo().watch());

        db.logout();
    }
}());
rst.stopSet();
}());
