/**
 * Kills the OperationContext used by the donor shard to send
 * _configsvrEnsureChunkVersionIsGreaterThan and to force a filtering metadata refresh.
 *
 * Depends on the checkOrphansAreDeleted hook at the end of ShardingTest to verify that the orphans,
 * range deletion tasks, and migration coordinator state are deleted despite the killOps.
 */

(function() {
'use strict';

load('jstests/libs/parallel_shell_helpers.js');
load('jstests/libs/fail_point_util.js');

function getNewNs(dbName) {
    if (typeof getNewNs.counter == 'undefined') {
        getNewNs.counter = 0;
    }
    getNewNs.counter++;
    const collName = "ns" + getNewNs.counter;
    return [collName, dbName + "." + collName];
}

const dbName = "test";

let st = new ShardingTest({shards: 2});

const donorShard = st.shard0;
const recipientShard = st.shard1;
const numDocs = 1000;
const middle = numDocs / 2;

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: donorShard.shardName}));

function testKillOpAfterFailPoint(failPointName, opToKillThreadName) {
    const [collName, ns] = getNewNs(dbName);
    jsTest.log("Testing with " + tojson(arguments) + " using ns " + ns);

    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: middle}}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: donorShard.shardName}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {_id: middle}, to: donorShard.shardName}));

    // Insert some docs into the collection.
    var bulk = st.s.getDB(dbName).getCollection(collName).initializeUnorderedBulkOp();
    for (var i = 0; i < numDocs; i++) {
        bulk.insert({_id: i});
    }
    assert.commandWorked(bulk.execute());

    // Simulate a network error on sending commit to the config server, so that the donor tries to
    // recover the commit decision.
    let commitFailpoint = configureFailPoint(donorShard, "migrationCommitNetworkError");

    // Set the requested failpoint and launch the moveChunk asynchronously.
    let failPoint = configureFailPoint(donorShard, failPointName);
    const awaitResult = startParallelShell(
        funWithArgs(function(ns, toShardName, middle) {
            let ret = assert.commandWorked(
                db.adminCommand({moveChunk: ns, find: {_id: middle}, to: toShardName}));
            jsTest.log('moveChunk res: ' + tojson(ret));
        }, ns, recipientShard.shardName, middle), st.s.port);

    jsTest.log("Waiting for moveChunk to reach " + failPointName + " failpoint");
    failPoint.wait();

    let matchingOps;
    assert.soon(() => {
        matchingOps = donorShard.getDB("admin")
                          .aggregate([
                              {$currentOp: {'allUsers': true, 'idleConnections': true}},
                              {$match: {desc: {$regex: opToKillThreadName}}}
                          ])
                          .toArray();
        // Wait for the opid to be present, since it's possible for currentOp to run after the
        // Client has been created but before it has been associated with a new
        // OperationContext.
        return 1 === matchingOps.length && matchingOps[0].opid != null;
    }, "Failed to find op with desc " + opToKillThreadName);
    jsTest.log("Killing OperationContext for " + opToKillThreadName);
    donorShard.getDB("admin").killOp(matchingOps[0].opid);

    failPoint.off();

    awaitResult();

    // Allow the moveChunk to finish:
    commitFailpoint.off();
    jsTest.log("Make sure the recovery is executed");
    assert.eq(st.s0.getDB(dbName).getCollection(collName).countDocuments({}), 1000);
}

// After SERVER-47982 all the failpoints are hit on the migration recovery, which is performed on
// another thread which operation context is RecoverRefreshThread. To run this test on a
// multiversion suite we have to also search for the previous name.
//
// TODO (SERVER-47265): operation context name should be RecoverRefreshThread once SERVER-32198 is
// backported to 4.4
testKillOpAfterFailPoint("hangInEnsureChunkVersionIsGreaterThanInterruptible",
                         "(ensureChunkVersionIsGreaterThan)|(RecoverRefreshThread)");
testKillOpAfterFailPoint("hangInRefreshFilteringMetadataUntilSuccessInterruptible",
                         "(refreshFilteringMetadataUntilSuccess)|(RecoverRefreshThread)");
testKillOpAfterFailPoint("hangInPersistMigrateCommitDecisionInterruptible",
                         "(persist migrate commit decision)|(RecoverRefreshThread)");
testKillOpAfterFailPoint("hangInDeleteRangeDeletionOnRecipientInterruptible",
                         "(cancel range deletion on recipient)|(RecoverRefreshThread)");
testKillOpAfterFailPoint("hangInReadyRangeDeletionLocallyInterruptible",
                         "(ready local range deletion)|(RecoverRefreshThread)");
testKillOpAfterFailPoint("hangInAdvanceTxnNumInterruptible",
                         "(advance migration txn number)|(RecoverRefreshThread)");

st.stop();
})();
