/**
 * Tests that direct shard connections are correctly allowed and disallowed using authentication.
 *
 * @tags: [requires_fcv_70]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {removeShard} from "jstests/sharding/libs/remove_shard_util.js";

// Create a new sharded cluster for testing and enable auth.
const st = new ShardingTest({name: jsTestName(), keyFile: "jstests/libs/key1", shards: 1});

const shardConn = st.rs0.getPrimary();
const shardAdminDB = shardConn.getDB("admin");
const shardAdminTestDB = shardConn.getDB("test");
const userConn = new Mongo(st.shard0.host);
const userTestDB = userConn.getDB("test");

shardAdminDB.createUser({user: "admin", pwd: 'x', roles: ["root"]});
assert(shardAdminDB.auth("admin", 'x'), "Authentication failed");

// directConnectionChecksWithSingleShard server parameter should always exist and it is set to true
// by default.
let singleShardClusterWarnings =
    assert
        .commandWorked(
            shardAdminDB.runCommand({getParameter: 1, directConnectionChecksWithSingleShard: 1}))
        .directConnectionChecksWithSingleShard;
assert(singleShardClusterWarnings);

assert.commandWorked(shardAdminDB.runCommand(
    {setParameter: 1, logComponentVerbosity: {sharding: {verbosity: 2}, assert: {verbosity: 1}}}));

const resetClusterCardinalityOnRemoveShard =
    FeatureFlagUtil.isPresentAndEnabled(shardAdminDB, "ReplicaSetEndpoint");

function getUnauthorizedDirectWritesCount() {
    return assert.commandWorked(shardAdminDB.runCommand({serverStatus: 1}))
        .shardingStatistics.unauthorizedDirectShardOps;
}

function runTests(shouldBlockDirectConnections, directWriteCount) {
    // Direct writes with root privileges should always be authorized.
    assert.commandWorked(shardAdminTestDB.getCollection("coll").update(
        {x: {$exists: true}}, {$inc: {x: 1}}, {upsert: true}));
    assert.eq(getUnauthorizedDirectWritesCount(), directWriteCount);

    // Test direct writes with only read/write privileges. This should always warn (excluding
    // RSEndpoint) but the operation should not fail with only one shard in the cluster.
    shardAdminTestDB.createUser({user: "user", pwd: "y", roles: ["readWrite"]});
    assert(userTestDB.auth("user", "y"), "Authentication failed");
    // Run the command and check warnings. The command should fail in a 2+ shard cluster and succeed
    // in a 1 shard cluster but we should always emit a warning (excluding RSEndpoint).
    if (!shouldBlockDirectConnections) {
        assert.commandWorked(userTestDB.getCollection("coll").update(
            {x: {$exists: true}}, {$inc: {x: 1}}, {upsert: true}));
        // No warning will be emitted even if the parameter is set if RSEndpoint is enabled.
        if (st.isReplicaSetEndpointActive()) {
            assert.eq(getUnauthorizedDirectWritesCount(), directWriteCount);
        } else {
            assert.eq(getUnauthorizedDirectWritesCount(), ++directWriteCount);
        }
    } else {
        assert.commandFailedWithCode(userTestDB.getCollection("coll").update(
                                         {x: {$exists: true}}, {$inc: {x: 1}}, {upsert: true}),
                                     ErrorCodes.Unauthorized);
        assert.eq(getUnauthorizedDirectWritesCount(), ++directWriteCount);
    }

    // Test direct writes with only read/write privileges where
    // directConnectionChecksWithSingleShard is set to false. This should not emit a warning if
    // there is only one shard in the cluster.
    assert.commandWorked(
        shardAdminDB.runCommand({setParameter: 1, directConnectionChecksWithSingleShard: false}));
    // Run the command and check warnings. The command should fail in a 2+ shard cluster and succeed
    // in a 1 shard cluster and we should only emit a warning in the 2+ shard scenario.
    if (!shouldBlockDirectConnections) {
        assert.commandWorked(userTestDB.getCollection("coll").update(
            {x: {$exists: true}}, {$inc: {x: 1}}, {upsert: true}));
        assert.eq(getUnauthorizedDirectWritesCount(), directWriteCount);
    } else {
        assert.commandFailedWithCode(userTestDB.getCollection("coll").update(
                                         {x: {$exists: true}}, {$inc: {x: 1}}, {upsert: true}),
                                     ErrorCodes.Unauthorized);
        assert.eq(getUnauthorizedDirectWritesCount(), ++directWriteCount);
    }
    // Reset the parameter for future tests.
    assert.commandWorked(
        shardAdminDB.runCommand({setParameter: 1, directConnectionChecksWithSingleShard: true}));
    userTestDB.logout();
    assert.eq(getUnauthorizedDirectWritesCount(), directWriteCount);

    // Direct writes with read/write plus the direct shard operations privileges should
    // always be authorized.
    shardAdminDB.createUser(
        {user: "user2", pwd: "z", roles: ["readWriteAnyDatabase", "directShardOperations"]});
    let shardUserWithDirectWritesAdminDB = userConn.getDB("admin");
    let shardUserWithDirectWritesTestDB = userConn.getDB("test");
    assert(shardUserWithDirectWritesAdminDB.auth("user2", "z"), "Authentication failed");
    assert.commandWorked(shardUserWithDirectWritesTestDB.getCollection("coll").update(
        {x: {$exists: true}}, {$inc: {x: 1}}, {upsert: true}));
    assert.eq(getUnauthorizedDirectWritesCount(), directWriteCount);
    shardUserWithDirectWritesAdminDB.logout();
    assert.eq(getUnauthorizedDirectWritesCount(), directWriteCount);

    // Reset users and logout for next set of tests.
    shardAdminTestDB.dropUser("user");
    shardAdminDB.dropUser("user2");
    assert.eq(getUnauthorizedDirectWritesCount(), directWriteCount);
    return directWriteCount;
}

// With only one shard, direct shard operations should be allowed.
jsTest.log("Running tests with only one shard.");
let directWriteCount = runTests(false /* shouldBlockDirectConnections */, 0);

// Adding the second shard will trigger the check for direct shard ops.
var newShard = new ReplSetTest({name: "additionalShard", nodes: 1});
newShard.startSet({keyFile: "jstests/libs/key1", shardsvr: ""});
newShard.initiate();
let mongosAdminUser = st.s.getDB('admin');
if (!TestData.configShard) {
    mongosAdminUser.createUser({user: "globalAdmin", pwd: 'a', roles: ["root"]});
    assert(mongosAdminUser.auth("globalAdmin", "a"), "Authentication failed");
} else {
    assert(mongosAdminUser.auth("admin", "x"), "Authentication failed");
}
assert.commandWorked(mongosAdminUser.runCommand({addShard: newShard.getURL()}));

// With two shards, direct shard operations should be prevented.
jsTest.log("Running tests with two shards.");
directWriteCount = runTests(true /* shouldBlockDirectConnections */, directWriteCount);

// Remove the second shard, this shouldn't affect the direct shard op checks.
removeShard(mongosAdminUser, newShard.getURL());

// With one shard again, direct shard operations should still be prevented assuming that
// featureFlagReplicaSetEndpoint is disabled.
jsTest.log("Running tests with one shard again.");
directWriteCount = runTests(
    !resetClusterCardinalityOnRemoveShard /* shouldBlockDirectConnections */, directWriteCount);

// Logout of final users
mongosAdminUser.logout();
shardAdminDB.logout();

// Stop the sharding test before the additional shard to ensure the test hooks run successfully.
st.stop();
newShard.stopSet();
