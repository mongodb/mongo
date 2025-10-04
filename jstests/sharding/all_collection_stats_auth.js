/*
 * Test to validate the privileges of using $_internalAllCollectionStats stage.
 *
 * @tags: [
 *   requires_fcv_62,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

// Test privileges
function testPrivileges() {
    // Create new role with the exact privileges to execute $allCollectionStats
    assert.commandWorked(
        adminDb.runCommand({
            createRole: "role_ok_priv",
            roles: [],
            privileges: [{resource: {cluster: true}, actions: ["allCollectionStats"]}],
        }),
    );

    // Creates users with privileges and no privileges
    assert.commandWorked(adminDb.runCommand({createUser: "user_no_priv", pwd: "pwd", roles: []}));

    assert.commandWorked(
        adminDb.runCommand({createUser: "user_priv1", pwd: "pwd", roles: [{role: "role_ok_priv", db: "admin"}]}),
    );

    assert.commandWorked(
        adminDb.runCommand({createUser: "user_priv2", pwd: "pwd", roles: [{role: "clusterMonitor", db: "admin"}]}),
    );

    assert(adminDb.logout());

    // User is in a role with privileges to execute the stage
    assert(adminDb.auth("user_priv1", "pwd"));
    assert.commandWorked(
        adminDb.runCommand({
            aggregate: 1,
            pipeline: [{$_internalAllCollectionStats: {stats: {storageStats: {}}}}],
            cursor: {},
        }),
    );
    assert(adminDb.logout());

    // User is in a role with privileges to execute the stage
    assert(adminDb.auth("user_priv2", "pwd"));
    assert.commandWorked(
        adminDb.runCommand({
            aggregate: 1,
            pipeline: [{$_internalAllCollectionStats: {stats: {storageStats: {}}}}],
            cursor: {},
        }),
    );
    assert(adminDb.logout());

    // User has no privileges to execute the stage
    assert(adminDb.auth("user_no_priv", "pwd"));
    assert.commandFailedWithCode(
        adminDb.runCommand({
            aggregate: 1,
            pipeline: [{$_internalAllCollectionStats: {stats: {storageStats: {}}}}],
            cursor: {},
        }),
        ErrorCodes.Unauthorized,
        "user should no longer have privileges to execute $_internalAllCollectionStats stage.",
    );
    assert(adminDb.logout());
}

// Configure initial sharding cluster
const st = new ShardingTest({shards: 1, keyFile: "jstests/libs/key1"});
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
