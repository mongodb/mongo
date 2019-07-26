/**
 * Test that when a sharded aggregation errors on just one shard, cursors on all other shards are
 * cleaned up correctly.
 *
 * Must be banned from suites that use a sharding fixture, since this test starts its own sharded
 * cluster. Must be banned in the $facet passthrough, since that suite changes the pipeline
 * splitting and merging behavior expected by this test.
 * @tags: [requires_sharding,do_not_wrap_aggregations_in_facets]
 */
(function() {
"use strict";

// For assertMergeFailsForAllModesWithCode.
load("jstests/aggregation/extras/merge_helpers.js");
load("jstests/aggregation/extras/utils.js");  // For assertErrorCode.

const kFailPointName = "waitAfterPinningCursorBeforeGetMoreBatch";
const kFailpointOptions = {
    shouldCheckForInterrupt: true
};

const st = new ShardingTest({shards: 2});
const kDBName = "test";
const kDivideByZeroErrCode = 16608;
const mongosDB = st.s.getDB(kDBName);
const shard0DB = st.shard0.getDB(kDBName);
const shard1DB = st.shard1.getDB(kDBName);

let coll = mongosDB.sharded_agg_cleanup_on_error;

for (let i = 0; i < 10; i++) {
    assert.writeOK(coll.insert({_id: i}));
}

st.shardColl(coll, {_id: 1}, {_id: 5}, {_id: 6}, kDBName, false);
st.ensurePrimaryShard(kDBName, st.shard0.name);

function assertFailsAndCleansUpCursors({pipeline, errCode}) {
    let cmdRes = mongosDB.runCommand(
        {aggregate: coll.getName(), pipeline: pipeline, cursor: {batchSize: 0}});
    assert.commandWorked(cmdRes);
    assert.neq(0, cmdRes.cursor.id);
    assert.eq(coll.getFullName(), cmdRes.cursor.ns);
    assert.eq(0, cmdRes.cursor.firstBatch.length);

    cmdRes = mongosDB.runCommand({getMore: cmdRes.cursor.id, collection: coll.getName()});
    assert.commandFailedWithCode(cmdRes, errCode);

    // Neither mongos or the shards should leave cursors open. By the time we get here, the
    // cursor which was hanging on shard 1 will have been marked interrupted, but isn't
    // guaranteed to be deleted yet. Thus, we use an assert.soon().
    assert.eq(mongosDB.serverStatus().metrics.cursor.open.total, 0);
    assert.eq(shard0DB.serverStatus().metrics.cursor.open.total, 0);
    assert.soon(() => shard1DB.serverStatus().metrics.cursor.open.pinned == 0);
}

try {
    // Set up a fail point which causes getMore to hang on shard 1.
    assert.commandWorked(shard1DB.adminCommand(
        {configureFailPoint: kFailPointName, mode: "alwaysOn", data: kFailpointOptions}));

    // Issue an aggregregation that will fail during a getMore on shard 0, and make sure that
    // this correctly kills the hanging cursor on shard 1. Use $_internalSplitPipeline to ensure
    // that this pipeline merges on mongos.
    assertFailsAndCleansUpCursors({
        pipeline: [
            {$project: {out: {$divide: ["$_id", 0]}}},
            {$_internalSplitPipeline: {mergeType: "mongos"}}
        ],
        errCode: kDivideByZeroErrCode
    });

    // Repeat the test above, but this time use $_internalSplitPipeline to force the merge to
    // take place on a shard 0.
    assertFailsAndCleansUpCursors({
        pipeline: [
            {$project: {out: {$divide: ["$_id", 0]}}},
            {$_internalSplitPipeline: {mergeType: "primaryShard"}}
        ],
        errCode: kDivideByZeroErrCode
    });
} finally {
    assert.commandWorked(shard0DB.adminCommand({configureFailPoint: kFailPointName, mode: "off"}));
}

// Test that aggregations which fail to establish a merging shard cursor also cleanup the open
// shard cursors.
try {
    // Enable the failpoint to fail on establishing a merging shard cursor.
    assert.commandWorked(mongosDB.adminCommand({
        configureFailPoint: "clusterAggregateFailToEstablishMergingShardCursor",
        mode: "alwaysOn"
    }));

    // Run an aggregation which requires a merging shard pipeline. This should fail because of
    // the failpoint.
    assertErrorCode(coll, [{$out: "target"}], ErrorCodes.FailPointEnabled);

    // Neither mongos or the shards should leave cursors open.
    assert.eq(mongosDB.serverStatus().metrics.cursor.open.total, 0);
    assert.soon(() => shard0DB.serverStatus().metrics.cursor.open.total == 0);
    assert.soon(() => shard1DB.serverStatus().metrics.cursor.open.total == 0);

} finally {
    assert.commandWorked(mongosDB.adminCommand(
        {configureFailPoint: "clusterAggregateFailToEstablishMergingShardCursor", mode: "off"}));
}

// Test that aggregations involving $exchange correctly clean up the producer cursors.
try {
    assert.commandWorked(mongosDB.adminCommand({
        configureFailPoint: "clusterAggregateFailToDispatchExchangeConsumerPipeline",
        mode: "alwaysOn"
    }));

    // Run an aggregation which is eligible for $exchange. This should assert because of
    // the failpoint. Add a $group stage to force an exchange-eligible split of the pipeline
    // before the $merge. Without the $group we won't use the exchange optimization and instead
    // will send the $merge to each shard.
    st.shardColl(mongosDB.target, {_id: 1}, {_id: 0}, {_id: 1}, kDBName, false);

    assertMergeFailsForAllModesWithCode({
        source: coll,
        target: mongosDB.target,
        prevStages: [{$group: {_id: "$fakeShardKey"}}],
        errorCodes: ErrorCodes.FailPointEnabled
    });

    // Neither mongos or the shards should leave cursors open.
    assert.eq(mongosDB.serverStatus().metrics.cursor.open.total, 0);
    assert.soon(() => shard0DB.serverStatus().metrics.cursor.open.total == 0);
    assert.soon(() => shard1DB.serverStatus().metrics.cursor.open.total == 0);

} finally {
    assert.commandWorked(mongosDB.adminCommand({
        configureFailPoint: "clusterAggregateFailToDispatchExchangeConsumerPipeline",
        mode: "off"
    }));
}

st.stop();
})();
