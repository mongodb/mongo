/**
 * Test to validate the privileges required by the analyzeShardKey and configureQueryAnalyzer
 * commands and _refreshQueryAnalyzerConfiguration internal command.
 *
 * @tags: [requires_fcv_70, featureFlagAnalyzeShardKey]
 */

(function() {

'use strict';

function runTest(conn) {
    const dbName = "testDb";
    const collName0 = "testColl0";
    const collName1 = "testColl1";
    const ns0 = dbName + "." + collName0;
    const ns1 = dbName + "." + collName1;

    const adminDb = conn.getDB("admin");
    assert.commandWorked(
        adminDb.runCommand({createUser: "super", pwd: "super", roles: ["__system"]}));
    assert(adminDb.auth("super", "super"));
    const testDb = adminDb.getSiblingDB(dbName);
    const docs = [];
    const numDocs = 1000;
    for (let i = 0; i < numDocs; i++) {
        docs.push({x: i});
    }
    assert.commandWorked(testDb.getCollection(collName0).insert(docs));
    assert.commandWorked(testDb.getCollection(collName1).insert(docs));
    assert(adminDb.logout());

    // Set up a user without any role or privilege.
    assert(adminDb.auth("super", "super"));
    assert.commandWorked(adminDb.runCommand({createUser: "user_no_priv", pwd: "pwd", roles: []}));
    assert(adminDb.logout());
    // Verify that the user is not authorized to run the analyzeShardKey command against ns0 or
    // ns1.
    assert(adminDb.auth("user_no_priv", "pwd"));
    assert.commandFailedWithCode(adminDb.runCommand({"analyzeShardKey": ns0, key: {_id: 1}}),
                                 ErrorCodes.Unauthorized);
    assert.commandFailedWithCode(adminDb.runCommand({"analyzeShardKey": ns1, key: {_id: 1}}),
                                 ErrorCodes.Unauthorized);
    assert(adminDb.logout());

    // Set up a user with the 'analyzeShardKey' privilege against ns0.
    assert(adminDb.auth("super", "super"));
    assert.commandWorked(adminDb.runCommand({
        createRole: "role_ns0_priv",
        roles: [],
        privileges: [{resource: {db: dbName, collection: collName0}, actions: ["analyzeShardKey"]}]
    }));
    assert.commandWorked(adminDb.runCommand({
        createUser: "user_with_explicit_ns0_priv",
        pwd: "pwd",
        roles: [{role: "role_ns0_priv", db: "admin"}]
    }));
    assert(adminDb.logout());
    // Verify that the user is authorized to run the analyzeShardKey command against ns0 but not
    // ns1.
    assert(adminDb.auth("user_with_explicit_ns0_priv", "pwd"));
    assert.commandWorked(adminDb.runCommand({"analyzeShardKey": ns0, key: {_id: 1}}));
    assert.commandFailedWithCode(adminDb.runCommand({"analyzeShardKey": ns1, key: {_id: 1}}),
                                 ErrorCodes.Unauthorized);
    assert(adminDb.logout());

    // Set up a user with the 'clusterManager' role.
    assert(adminDb.auth("super", "super"));
    assert.commandWorked(adminDb.runCommand({
        createUser: "user_cluster_mgr",
        pwd: "pwd",
        roles: [{role: "clusterManager", db: "admin"}]
    }));
    assert(adminDb.logout());
    // Verify that the user is authorized to run the analyzeShardKey command against both ns0
    // and ns1.
    assert(adminDb.auth("user_cluster_mgr", "pwd"));
    assert.commandWorked(adminDb.runCommand({"analyzeShardKey": ns0, key: {_id: 1}}));
    assert.commandWorked(adminDb.runCommand({"analyzeShardKey": ns1, key: {_id: 1}}));
    assert(adminDb.logout());

    // Set up a user with the 'enableSharding' role.
    assert(adminDb.auth("super", "super"));
    assert.commandWorked(adminDb.runCommand({
        createUser: "user_enable_sharding",
        pwd: "pwd",
        roles: [{role: "enableSharding", db: "admin"}]
    }));
    assert(adminDb.logout());
    // Verify that the user is authorized to run the analyzeShardKey command against both ns0
    // and ns1.
    assert(adminDb.auth("user_enable_sharding", "pwd"));
    assert.commandWorked(adminDb.runCommand({"analyzeShardKey": ns0, key: {_id: 1}}));
    assert.commandWorked(adminDb.runCommand({"analyzeShardKey": ns1, key: {_id: 1}}));
    assert(adminDb.logout());
}

{
    const st = new ShardingTest({shards: 1, keyFile: "jstests/libs/key1"});

    runTest(st.s);

    st.stop();
}

{
    const rst = new ReplSetTest({nodes: 1, keyFile: "jstests/libs/key1"});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    runTest(primary);

    rst.stopSet();
}
})();
