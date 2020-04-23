/**
 * Test that unfinishedMigrationFromPreviousPrimary field in shardingStatistics reports the
 * expected number.
 */
(function() {
"use strict";

load('./jstests/libs/chunk_manipulation_util.js');

// Test calls step down on primaries.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

// For startParallelOps to write its state
var staticMongod = MongoRunner.runMongod({});

let st = new ShardingTest({shards: 2, rs: {nodes: 2}});
assert.commandWorked(st.s.adminCommand({enableSharding: 'test'}));
st.ensurePrimaryShard('test', st.shard0.shardName);
assert.commandWorked(st.s.adminCommand({shardCollection: 'test.user', key: {x: 1}}));

let priConn = st.rs0.getPrimary();
let serverStatusRes = priConn.adminCommand({serverStatus: 1});
assert.eq(0,
          serverStatusRes.shardingStatistics.unfinishedMigrationFromPreviousPrimary,
          tojson(serverStatusRes));

pauseMoveChunkAtStep(priConn, moveChunkStepNames.reachedSteadyState);
var joinMoveChunk =
    moveChunkParallel(staticMongod, st.s0.host, {x: 0}, null, 'test.user', st.shard1.shardName);

waitForMoveChunkStep(st.shard0, moveChunkStepNames.reachedSteadyState);

try {
    priConn.adminCommand({replSetStepDown: 60});
} catch (ex) {
    print(`Caught expected exception calling step down: ${tojson(ex)}`);
}

unpauseMoveChunkAtStep(priConn, moveChunkStepNames.reachedSteadyState);
st.rs0.awaitNodesAgreeOnPrimary();

// Wait for migration coordinator recovery to complete before checking server status.
priConn = st.rs0.getPrimary();
assert.soon(() => {
    return priConn.getDB('config').migrationCoordinators.count() == 0;
});

serverStatusRes = st.rs0.getPrimary().adminCommand({serverStatus: 1});
assert.eq(1,
          serverStatusRes.shardingStatistics.unfinishedMigrationFromPreviousPrimary,
          tojson(serverStatusRes));

try {
    joinMoveChunk();
} catch (ex) {
    print(`Caught expected exception due to step down during migration: ${tojson(ex)}`);
}

st.stop();

MongoRunner.stopMongod(staticMongod);
})();
