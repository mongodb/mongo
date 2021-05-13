/**
 * Tests that range deletion tasks are eventually deleted even if collection is dropped
 * before migration coordination is resumed.
 *
 * @tags: [
 *     # (SERVER-56915) this test is artificially blocking refreshes, potentially resulting in a
 *     # deadlock on shutdown in case of failure
 *     does_not_support_stepdowns
 * ]
 */

(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");

/*
 * Runs moveChunk on the host to move the chunk to the given shard.
 */
function runMoveChunk(host, ns, findCriteria, toShard) {
    const mongos = new Mongo(host);
    return mongos.adminCommand({moveChunk: ns, find: findCriteria, to: toShard});
}

/*
 * Returns the number of migration coordinator docs for the given namespace on the host.
 */
function getNumMigrationCoordinatorDocs(conn, ns) {
    return conn.getDB("config").getCollection("migrationCoordinators").count({nss: ns});
}

/*
 * Returns the number of range deletion task docs for the given namespace on the host.
 */
function getNumRangeDeletionDocs(conn, ns) {
    return conn.getDB("config").getCollection("rangeDeletions").count({nss: ns});
}

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

let st = new ShardingTest({shards: 2});
let testColl = st.s.getDB(dbName).getCollection(collName);

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

let donorShard = st.shard0;
let recipientShard = st.shard1;

let donorReplSetTest = st.rs0;
donorReplSetTest.awaitNodesAgreeOnPrimary();
let donorPrimary = donorReplSetTest.getPrimary();

let moveChunkHangAtStep5FailPoint = configureFailPoint(donorPrimary, "moveChunkHangAtStep5");
let metadataRefreshFailPoint =
    configureFailPoint(donorPrimary, "hangBeforeFilteringMetadataRefresh");
let moveChunkThread =
    new Thread(runMoveChunk, st.s.host, ns, {_id: MinKey}, recipientShard.shardName);

moveChunkThread.start();

moveChunkHangAtStep5FailPoint.wait();
assert.commandWorked(
    donorPrimary.adminCommand({replSetStepDown: 5 /* stepDownSecs */, force: true}));

moveChunkHangAtStep5FailPoint.off();
moveChunkThread.join();

metadataRefreshFailPoint.wait();

jsTest.log("Verify that the donor has the migration coordinator doc and range deletion task doc");
assert.eq(1, getNumMigrationCoordinatorDocs(donorShard, ns));
assert.eq(1, getNumRangeDeletionDocs(donorShard, ns));

jsTest.log("Verify that the recipient has the range deletion task doc");
assert.eq(1, getNumRangeDeletionDocs(recipientShard, ns));

testColl.drop();
metadataRefreshFailPoint.off();

jsTest.log("Wait for the recipient to delete the range deletion task doc");
assert.soon(() => {
    return 0 == getNumRangeDeletionDocs(recipientShard, ns);
});

jsTest.log("Wait for the donor to delete the range deletion task doc");
assert.soon(() => {
    return 0 == getNumRangeDeletionDocs(donorShard, ns);
});

jsTest.log("Wait for the donor to delete the migration coordinator doc");
assert.soon(() => {
    return 0 === getNumMigrationCoordinatorDocs(donorShard, ns);
});

st.stop();
})();
