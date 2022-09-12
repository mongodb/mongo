/*
 * Test to validate the privileges of using $shardedDataDistribution stage.
 *
 * @tags: [
 *   requires_fcv_62,
 * ]
 */

(function() {
'use strict';

if (!TestData.auth) {
    jsTestLog("Skipping testing authorization since auth is not enabled");
    return;
}

// Test privileges
function testPrivileges() {
    // Create new role with the exact privileges to execute $shardedDataDistribution
    assert.commandWorked(adminDb.runCommand({
        createRole: "role_ok_priv",
        roles: [],
        privileges: [{resource: {cluster: true}, actions: ["shardedDataDistribution"]}]
    }));

    // Creates users with privileges and no privileges
    assert.commandWorked(adminDb.runCommand({createUser: "user_no_priv", pwd: "pwd", roles: []}));

    assert.commandWorked(adminDb.runCommand(
        {createUser: "user_priv1", pwd: "pwd", roles: [{role: "role_ok_priv", db: 'admin'}]}));

    assert.commandWorked(adminDb.runCommand(
        {createUser: "user_priv2", pwd: "pwd", roles: [{role: "clusterMonitor", db: 'admin'}]}));

    assert(adminDb.logout());

    // User is in a role with privileges to execute the stage
    assert(adminDb.auth("user_priv1", "pwd"));
    assert.commandWorked(
        adminDb.runCommand({aggregate: 1, pipeline: [{$shardedDataDistribution: {}}], cursor: {}}));
    assert(adminDb.logout());

    // User is in a role with privileges to execute the stage
    assert(adminDb.auth("user_priv2", "pwd"));
    assert.commandWorked(
        adminDb.runCommand({aggregate: 1, pipeline: [{$shardedDataDistribution: {}}], cursor: {}}));
    assert(adminDb.logout());

    // User has no privileges to execute the stage
    assert(adminDb.auth("user_no_priv", "pwd"));
    assert.commandFailedWithCode(
        adminDb.runCommand({aggregate: 1, pipeline: [{$shardedDataDistribution: {}}], cursor: {}}),
        ErrorCodes.Unauthorized,
        "user should no longer have privileges to execute $shardedDataDistribution stage.");
    assert(adminDb.logout());
}

// Configure initial sharding cluster
const st = new ShardingTest({shards: 1});
const mongos = st.s;

const ns1 = "test.foo";
const adminDb = mongos.getDB("admin");
const testDb = mongos.getDB("test");

// Create a super user with __system role.
assert.commandWorked(adminDb.runCommand({createUser: "super", pwd: "super", roles: ["__system"]}));
assert(adminDb.logout());
assert(adminDb.auth("super", "super"));

st.adminCommand({shardcollection: ns1, key: {skey: 1}});

// Insert data to validate the aggregation stage
for (let i = 0; i < 6; i++) {
    assert.commandWorked(testDb.getCollection("foo").insert({skey: i}));
}

testPrivileges();

st.stop();
})();
