/**
 * Confirms that a sharded $sample which employs the DSSampleFromRandomCursor optimization is
 * capable of yielding.
 *
 * @tags: [assumes_read_concern_unchanged, do_not_wrap_aggregations_in_facets, requires_journaling,
 * requires_sharding, requires_wiredtiger]
 */
(function() {
    "use strict";

    load("jstests/concurrency/fsm_workload_helpers/server_types.js");  // For isWiredTiger.
    load("jstests/libs/profiler.js");  // For profilerHasSingleMatchingEntryOrThrow.

    // $sample cannot be optimized on a storage engine which does not support random cursors.
    if (!isWiredTiger(db)) {
        return;
    }

    // Set up a 2-shard cluster. Configure 'internalQueryExecYieldIterations' on both shards such
    // that operations will yield on each PlanExecuter iteration.
    const st = new ShardingTest({
        name: jsTestName(),
        shards: 2,
        rs: {nodes: 1, setParameter: {internalQueryExecYieldIterations: 1}}
    });

    const mongosDB = st.s.getDB(jsTestName());
    const mongosColl = mongosDB.test;

    const shard0DB = st.rs0.getPrimary().getDB(jsTestName());
    const shard1DB = st.rs1.getPrimary().getDB(jsTestName());

    // Shard the test collection, split it at {_id: 0}, and move the upper chunk to shard1.
    st.shardColl(mongosColl, {_id: 1}, {_id: 0}, {_id: 0});

    // Insert enough documents on each shard to induce the $sample random-cursor optimization.
    for (let i = (-150); i < 150; ++i) {
        assert.writeOK(mongosColl.insert({_id: i}));
    }

    // Enable the profiler on both shards.
    for (let shardDB of[shard0DB, shard1DB]) {
        shardDB.setProfilingLevel(2);
    }

    // Run the initial aggregate for the $sample stage.
    const cmdRes = assert.commandWorked(mongosDB.runCommand({
        aggregate: mongosColl.getName(),
        pipeline: [{$sample: {size: 3}}],
        cursor: {batchSize: 0}
    }));
    assert.eq(cmdRes.cursor.firstBatch.length, 0);

    // Retrieve the remaining results for the $sample aggregation.
    const sampleCursor = new DBCommandCursor(mongosDB.getMongo(), cmdRes);
    assert.eq(sampleCursor.toArray().length, 3);

    // Confirm that the $sample getMore yields on both shards.
    for (let shardDB of[shard0DB, shard1DB]) {
        profilerHasSingleMatchingEntryOrThrow(shardDB, {
            planSummary: "MULTI_ITERATOR",
            ns: mongosColl.getFullName(),
            numYield: {$gt: 0},
            op: "getmore"
        });
    }

    // Gracefully shut down the cluster.
    st.stop();
})();