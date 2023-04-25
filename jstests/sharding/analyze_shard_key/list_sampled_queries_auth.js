/**
 * Test to validate the privileges required by the listSampledQuery aggregation stage.
 *
 * @tags: [requires_fcv_70]
 */

(function() {

'use strict';

load("jstests/sharding/analyze_shard_key/libs/query_sampling_util.js");

function runTest(conn) {
    const dbName = "testDb";
    const collName0 = "testColl0";
    const collName1 = "testColl1";

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
    assert.commandWorked(testDb.createCollection(collName0));
    assert.commandWorked(testDb.createCollection(collName1));
    assert(adminDb.logout());

    // Set up a user without any role or privilege.
    assert(adminDb.auth("super", "super"));
    assert.commandWorked(adminDb.runCommand({createUser: "user_no_priv", pwd: "pwd", roles: []}));
    assert(adminDb.logout());
    // Verify that the user is not authorized to run the listSampledQueries aggregation stage.
    assert(adminDb.auth("user_no_priv", "pwd"));
    assert.commandFailedWithCode(
        adminDb.runCommand({aggregate: 1, pipeline: [{$listSampledQueries: {}}], cursor: {}}),
        ErrorCodes.Unauthorized);
    assert(adminDb.logout());

    // Set up a role with 'listSampledQueries' privilege.
    assert(adminDb.auth("super", "super"));
    assert.commandWorked(adminDb.runCommand({
        createRole: "role_priv",
        roles: [],
        privileges: [{resource: {cluster: true}, actions: ["listSampledQueries"]}]
    }));
    // Set up a user with the privileged role.
    assert.commandWorked(adminDb.runCommand({
        createUser: "user_with_explicit_priv",
        pwd: "pwd",
        roles: [{role: "role_priv", db: "admin"}]
    }));
    assert(adminDb.logout());
    // Verify that the user is authorized to run the listSampledQueries aggregation stage.
    assert(adminDb.auth("user_with_explicit_priv", "pwd"));
    assert.commandWorked(
        adminDb.runCommand({aggregate: 1, pipeline: [{$listSampledQueries: {}}], cursor: {}}),
    );
    assert(adminDb.logout());

    // Set up a user as a clusterMonitor with the 'listSampledQueries' privilege.
    assert(adminDb.auth("super", "super"));
    assert.commandWorked(adminDb.runCommand({
        createUser: "user_with_clusterMonitor",
        pwd: "pwd",
        roles: [{role: "clusterMonitor", db: "admin"}]
    }));
    assert(adminDb.logout());
    // Verify that the user is authorized to run the listSampledQueries aggregation stage.
    assert(adminDb.auth("user_with_clusterMonitor", "pwd"));
    assert.commandWorked(
        adminDb.runCommand({aggregate: 1, pipeline: [{$listSampledQueries: {}}], cursor: {}}),
    );
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
