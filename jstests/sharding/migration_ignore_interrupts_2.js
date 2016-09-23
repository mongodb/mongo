// When a migration between shard0 and shard1 is about to enter the commit phase, a commit command
// with different migration session ID is rejected.

load('./jstests/libs/chunk_manipulation_util.js');

(function() {
    "use strict";

    var staticMongod = MongoRunner.runMongod({});  // For startParallelOps.

    var st = new ShardingTest({shards: 2});

    var mongos = st.s0, admin = mongos.getDB('admin'), dbName = "testDB", ns1 = dbName + ".foo",
        coll1 = mongos.getCollection(ns1), shard0 = st.shard0, shard1 = st.shard1,
        shard0Coll1 = shard0.getCollection(ns1), shard1Coll1 = shard1.getCollection(ns1);

    assert.commandWorked(admin.runCommand({enableSharding: dbName}));
    st.ensurePrimaryShard(dbName, st.shard0.shardName);
    assert.commandWorked(admin.runCommand({shardCollection: ns1, key: {a: 1}}));
    assert.writeOK(coll1.insert({a: 0}));
    assert.eq(1, shard0Coll1.find().itcount());
    assert.eq(0, shard1Coll1.find().itcount());
    assert.eq(1, coll1.find().itcount());

    // Shard0:
    //      coll1:     [-inf, +inf)
    // Shard1:

    jsTest.log("Set up complete, now proceeding to test that migration interruption fails.");

    // Start a migration between shard0 and shard1 on coll1, pause in steady state before commit.
    pauseMoveChunkAtStep(shard0, moveChunkStepNames.reachedSteadyState);
    var joinMoveChunk = moveChunkParallel(
        staticMongod, st.s0.host, {a: 0}, null, coll1.getFullName(), st.shard1.shardName);
    waitForMoveChunkStep(shard0, moveChunkStepNames.reachedSteadyState);

    jsTest.log('Sending false commit command....');
    assert.commandFailed(
        shard1.adminCommand({'_recvChunkCommit': 1, 'sessionId': "fake-migration-session-id"}));

    jsTest.log("Checking migration recipient is still in steady state, waiting for commit....");
    var res = shard1.adminCommand('_recvChunkStatus');
    assert.commandWorked(res);
    assert.eq(true, res.state === "steady", "False commit command succeeded.");

    // Finish migration.
    unpauseMoveChunkAtStep(shard0, moveChunkStepNames.reachedSteadyState);
    assert.doesNotThrow(function() {
        joinMoveChunk();
    });

    assert.eq(0, shard0Coll1.find().itcount());
    assert.eq(1, shard1Coll1.find().itcount());

    st.stop();
})();
