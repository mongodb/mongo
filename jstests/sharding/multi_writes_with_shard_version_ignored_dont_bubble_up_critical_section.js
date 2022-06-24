/*
 * Tests that multi-writes where the router attaches 'shardVersion: IGNORED' (i.e. if they need to
 * target several shards AND are not part of a txn) do not bubble up StaleConfig errors due to
 * ongoing critical sections. Instead, the shard yields and waits for the critical section to finish
 * and then continues the write plan.
 *
 * @tags: [
 *   requires_fcv_61,
 * ]
 */

(function() {
"use strict";

load('jstests/libs/parallel_shell_helpers.js');
load("jstests/libs/fail_point_util.js");

// Configure 'internalQueryExecYieldIterations' on both shards such that operations will yield on
// each 10th PlanExecuter iteration.
var st = new ShardingTest({
    shards: 2,
    rs: {setParameter: {internalQueryExecYieldIterations: 10}},
    other: {enableBalancer: false}
});

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;
const numDocs = 100;
let coll = st.s.getCollection(ns);

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

function setupTest() {
    coll.drop();
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));

    // Create three chunks:
    // - [MinKey, 0) initially on shard0 and has no documents. This chunk will be migrated during
    // the test execution.
    // - [0, numDocs) on shard 0. Contains 'numDocs' documents.
    // - [numDocs, MaxKey) shard 1. Contains no documents.
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 0}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: numDocs}}));
    assert.commandWorked(st.s.adminCommand(
        {moveChunk: ns, find: {x: numDocs}, to: st.shard1.shardName, waitForDelete: true}));

    jsTest.log("Inserting initial data.");
    const bulkOp = coll.initializeOrderedBulkOp();
    for (let i = 0; i < numDocs; ++i) {
        bulkOp.insert({x: i, c: 0});
    }
    assert.commandWorked(bulkOp.execute());
    jsTest.log("Inserted initial data.");
}

function runMigration() {
    const awaitResult = startParallelShell(
        funWithArgs(function(ns, toShard) {
            jsTest.log("Starting migration.");
            assert.commandWorked(db.adminCommand({moveChunk: ns, find: {x: -1}, to: toShard}));
            jsTest.log("Completed migration.");
        }, ns, st.shard1.shardName), st.s.port);

    return awaitResult;
}

function updateOperationFn(shardColl, numInitialDocsOnShard0) {
    load('jstests/sharding/libs/shard_versioning_util.js');  // For kIgnoredShardVersion

    jsTest.log("Begin multi-update.");

    // Send a multi-update with 'shardVersion: IGNORED' directly to the shard, as if we were a
    // router.
    const result = assert.commandWorked(shardColl.runCommand({
        update: shardColl.getName(),
        updates: [{q: {}, u: {$inc: {c: 1}}, multi: true}],
        shardVersion: ShardVersioningUtil.kIgnoredShardVersion
    }));

    jsTest.log("End multi-update. Result: " + tojson(result));

    // Check that all documents got updates. Despite the weak guarantees of {multi: true} writes
    // concurrent with migrations, this has to be the case in this test because the migrated chunk
    // does not contain any document.
    assert.eq(numInitialDocsOnShard0, shardColl.find({c: 1}).itcount());
}

function deleteOperationFn(shardColl, numInitialDocsOnShard0) {
    load('jstests/sharding/libs/shard_versioning_util.js');  // For kIgnoredShardVersion

    jsTest.log("Begin multi-delete");

    // Send a multi-delete with 'shardVersion: IGNORED' directly to the shard, as if we were a
    // router.
    const result = assert.commandWorked(shardColl.runCommand({
        delete: shardColl.getName(),
        deletes: [{q: {}, limit: 0}],
        shardVersion: ShardVersioningUtil.kIgnoredShardVersion
    }));

    jsTest.log("End multi-delete. Result: " + tojson(result));

    // Check that all documents got deleted. Despite the weak guarantees of {multi: true} writes
    // concurrent with migrations, this has to be the case in this test because the migrated chunk
    // does not contain any document.
    assert.eq(0, shardColl.find().itcount());
}

function runTest(writeOpFn) {
    setupTest();

    let fp1 = configureFailPoint(
        st.rs0.getPrimary(), 'setYieldAllLocksHang', {namespace: coll.getFullName()});

    const awaitWriteResult = startParallelShell(
        funWithArgs(function(writeOpFn, dbName, collName, numDocs) {
            const shardColl = db.getSiblingDB(dbName)[collName];
            writeOpFn(shardColl, numDocs);
        }, writeOpFn, coll.getDB().getName(), coll.getName(), numDocs), st.rs0.getPrimary().port);

    // Wait for the write op to yield.
    fp1.wait();
    jsTest.log("Multi-write yielded");

    // Start chunk migration and wait for it to enter the critical section.
    let failpointHangMigrationWhileInCriticalSection =
        configureFailPoint(st.rs0.getPrimary(), 'moveChunkHangAtStep5');
    const awaitMigration = runMigration();
    failpointHangMigrationWhileInCriticalSection.wait();

    // Let the multi-write resume from the yield.
    jsTest.log("Resuming yielded multi-write");
    fp1.off();

    // Let the multi-write run for a bit after the resuming from yield. It will encounter the
    // critical section.
    sleep(1000);

    // Let the migration continue and release the critical section.
    jsTest.log("Letting migration exit its critical section and complete");
    failpointHangMigrationWhileInCriticalSection.off();
    awaitMigration();

    // Wait for the write op to finish. It should succeed.
    awaitWriteResult();
}

runTest(updateOperationFn);
runTest(deleteOperationFn);

st.stop();
})();
