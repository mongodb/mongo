/**
 * Tests that a secondary that hasn't finished sharding initialization can successfully handle
 * a read request that uses afterClusterTime >= the optime of the insert of the shard identity
 * document on the primary.

 * @tags: [
 *     requires_fcv_52,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/replsets/rslib.js");
load("jstests/replsets/libs/sync_source.js");

let st = new ShardingTest({shards: 1});

// Set up replica set that we'll add as shard
let replTest = new ReplSetTest({nodes: 3, name: jsTest.name() + "-newReplSet"});
replTest.startSet({shardsvr: ''});
let nodeList = replTest.nodeList();

replTest.initiate({
    _id: replTest.name,
    members: [
        {_id: 0, host: nodeList[0], priority: 1},
        {_id: 1, host: nodeList[1], priority: 0, tags: {"tag": "hanging"}},
        {_id: 2, host: nodeList[2], priority: 0}
    ]
});

let primary = replTest.getPrimary();
let hangingSecondary = replTest.getSecondaries()[0];
let anotherSecondary = replTest.getSecondaries()[1];

// Set failpoint on one secondary to hang during sharding initialization
jsTest.log("Going to turn on the hangDuringShardingInitialization fail point.");
const fpHangDuringShardInit =
    configureFailPoint(hangingSecondary, "hangDuringShardingInitialization");

/**
 * Force the other secondary node to sync from the primary. If it syncs from the hanging secondary
 * node that has not inserted the sharding identity document, the sharding initialization will not
 * be triggered and the addShard command will fail
 */
jsTest.log("Going to set sync source of secondary node to be primary.");
const fpForceSyncSource = forceSyncSource(replTest, anotherSecondary, primary);

jsTest.log("Going to add replica set as shard: " + tojson(replTest.getReplSetConfig()));
const shardName = "newShard";
assert.commandWorked(
    st.s.getDB("admin").runCommand({addShard: replTest.getURL(), name: shardName}));
fpHangDuringShardInit.wait();

jsTest.log("Check and wait for the sharding state to be initialized on primary.");
assert.soon(function() {
    const shardingStatePrimary =
        replTest.getPrimary().getDB('admin').runCommand({shardingState: 1});
    return shardingStatePrimary.enabled == true;
});

const dbName = "testDB";
const sessionDb = st.s.startSession().getDatabase(dbName);

jsTest.log("Going to write a document to testDB.foo.");
// Make sure that the test db data is stored into the new shard.
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, shardName);
assert.commandWorked(sessionDb.foo.insert({x: 1}));

/**
 * Send a read request to the hanging secondary node. We expect it to fail as the sharding state is
 * not initialized. The read will block waiting for the afterClusterTime that is younger (greater)
 * than the opLog timestamp on hanging secondary node. A maxTimeMs is set to force the read time out
 * so that it will not block for too long.
 *
 * In this test case
 * -T1: insert of the shard identity doc (addShard)
 * -T2: write operation (insert)

 * The afterClusterTime we give is T2.  On hanging secondary node, the oplog is still at ts < T1.
 */
jsTest.log(
    "Going to send a read request with maxTimeMS 100000 to secondary that is hanging in setting up sharding initialization.");
const operationTime = sessionDb.getSession().getOperationTime();
const error = sessionDb.runCommand({
    find: "foo",
    maxTimeMS: 10000,
    $readPreference: {mode: "secondary", tags: [{"tag": "hanging"}]},
    readConcern: {level: "local", "afterClusterTime": operationTime}
});
assert.commandFailedWithCode(error, ErrorCodes.MaxTimeMSExpired);

jsTest.log("Going to turn off the hangDuringShardingInitialization.");
fpHangDuringShardInit.off();

jsTest.log("Check and wait for the sharding state to be initialized on hanging secondary.");
assert.soon(function() {
    return hangingSecondary.getDB('admin').runCommand({shardingState: 1}).enabled == true;
}, "Mongos did not update its sharding state after 10 seconds", 10 * 1000);

/**
 * Send the read request again. We expect it to succeed now as the sharding state is initialized and
 * the read won't block waiting for read concern.
 */
jsTest.log("Going to send the read request again.");
assert.commandWorked(sessionDb.runCommand({
    find: "foo",
    $readPreference: {mode: "secondary", tags: [{"tag": "hanging"}]},
    readConcern: {level: "local", "afterClusterTime": operationTime}
}));

fpForceSyncSource.off();
sessionDb.getSession().endSession();
replTest.stopSet();

st.stop();
})();
