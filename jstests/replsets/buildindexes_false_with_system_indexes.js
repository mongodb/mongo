/*
 * Tests that hidden nodes with buildIndexes: false behave correctly when system tables with
 * default indexes are created.
 *
 * @tags: [requires_persistence]
 */
(function() {
    'use strict';

    load("jstests/replsets/rslib.js");

    const testName = "buildindexes_false_with_system_indexes";

    let rst = new ReplSetTest({
        name: testName,
        nodes: [
            {},
            {rsConfig: {priority: 0}},
            {rsConfig: {priority: 0, hidden: true, buildIndexes: false}},
        ],
    });
    const nodes = rst.startSet();
    rst.initiate();

    let primary = rst.getPrimary();
    assert.eq(primary, nodes[0]);
    let secondary = nodes[1];
    const hidden = nodes[2];

    rst.awaitReplication();
    jsTestLog("Creating a role in the admin database");
    let adminDb = primary.getDB("admin");
    adminDb.createRole(
        {role: 'test_role', roles: [{role: 'readWrite', db: 'test'}], privileges: []});
    rst.awaitReplication();

    jsTestLog("Creating a user in the admin database");
    adminDb.createUser({user: 'test_user', pwd: 'test', roles: [{role: 'test_role', db: 'admin'}]});
    rst.awaitReplication();

    // Make sure the indexes we expect are present on all nodes.  The buildIndexes: false node
    // should have only the _id_ index.
    let secondaryAdminDb = secondary.getDB("admin");
    const hiddenAdminDb = hidden.getDB("admin");

    assert.eq(["_id_", "user_1_db_1"], adminDb.system.users.getIndexes().map(x => x.name).sort());
    assert.eq(["_id_", "role_1_db_1"], adminDb.system.roles.getIndexes().map(x => x.name).sort());
    assert.eq(["_id_", "user_1_db_1"],
              secondaryAdminDb.system.users.getIndexes().map(x => x.name).sort());
    assert.eq(["_id_", "role_1_db_1"],
              secondaryAdminDb.system.roles.getIndexes().map(x => x.name).sort());
    assert.eq(["_id_"], hiddenAdminDb.system.users.getIndexes().map(x => x.name).sort());
    assert.eq(["_id_"], hiddenAdminDb.system.roles.getIndexes().map(x => x.name).sort());

    // Drop the indexes and restart the secondary.  The indexes should not be re-created.
    jsTestLog("Dropping system indexes and restarting secondary.");
    adminDb.system.users.dropIndex("user_1_db_1");
    adminDb.system.roles.dropIndex("role_1_db_1");
    rst.awaitReplication();
    assert.eq(["_id_"], adminDb.system.users.getIndexes().map(x => x.name).sort());
    assert.eq(["_id_"], adminDb.system.roles.getIndexes().map(x => x.name).sort());
    assert.eq(["_id_"], secondaryAdminDb.system.users.getIndexes().map(x => x.name).sort());
    assert.eq(["_id_"], secondaryAdminDb.system.roles.getIndexes().map(x => x.name).sort());
    assert.eq(["_id_"], hiddenAdminDb.system.users.getIndexes().map(x => x.name).sort());
    assert.eq(["_id_"], hiddenAdminDb.system.roles.getIndexes().map(x => x.name).sort());

    secondary = rst.restart(secondary, {}, true /* wait for node to become healthy */);
    secondaryAdminDb = secondary.getDB("admin");
    assert.eq(["_id_"], secondaryAdminDb.system.users.getIndexes().map(x => x.name).sort());
    assert.eq(["_id_"], secondaryAdminDb.system.roles.getIndexes().map(x => x.name).sort());

    jsTestLog("Now restarting primary; indexes should be created.");
    rst.restart(primary);
    primary = rst.getPrimary();
    rst.awaitReplication();
    adminDb = primary.getDB("admin");
    assert.eq(["_id_", "user_1_db_1"], adminDb.system.users.getIndexes().map(x => x.name).sort());
    assert.eq(["_id_", "role_1_db_1"], adminDb.system.roles.getIndexes().map(x => x.name).sort());
    assert.eq(["_id_", "user_1_db_1"],
              secondaryAdminDb.system.users.getIndexes().map(x => x.name).sort());
    assert.eq(["_id_", "role_1_db_1"],
              secondaryAdminDb.system.roles.getIndexes().map(x => x.name).sort());
    assert.eq(["_id_"], hiddenAdminDb.system.users.getIndexes().map(x => x.name).sort());
    assert.eq(["_id_"], hiddenAdminDb.system.roles.getIndexes().map(x => x.name).sort());

    rst.stopSet();
}());
