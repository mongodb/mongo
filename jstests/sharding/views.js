/**
 * Tests for expected behavior when querying a view that is based on a sharded collection.
 * @tags: [requires_find_command]
 */
(function() {
    "use strict";

    // For profilerHasSingleMatchingEntryOrThrow.
    load("jstests/libs/profiler.js");

    // Given sharded explain output in 'shardedExplain', verifies that the explain mode 'verbosity'
    // affected the output verbosity appropriately, and that the response has the expected format.
    function verifyExplainResult(shardedExplain, verbosity) {
        assert.commandWorked(shardedExplain);
        assert(shardedExplain.hasOwnProperty("shards"), tojson(shardedExplain));
        for (let elem in shardedExplain.shards) {
            let shard = shardedExplain.shards[elem];
            assert(shard.stages[0].hasOwnProperty("$cursor"), tojson(shardedExplain));
            assert(shard.stages[0].$cursor.hasOwnProperty("queryPlanner"), tojson(shardedExplain));
            if (verbosity === "queryPlanner") {
                assert(!shard.stages[0].$cursor.hasOwnProperty("executionStats"),
                       tojson(shardedExplain));
            } else if (verbosity === "executionStats") {
                assert(shard.stages[0].$cursor.hasOwnProperty("executionStats"),
                       tojson(shardedExplain));
                assert(!shard.stages[0].$cursor.executionStats.hasOwnProperty("allPlansExecution"),
                       tojson("shardedExplain"));
            } else {
                assert.eq(verbosity, "allPlansExecution", tojson(shardedExplain));
                assert(shard.stages[0].$cursor.hasOwnProperty("executionStats"),
                       tojson(shardedExplain));
                assert(shard.stages[0].$cursor.executionStats.hasOwnProperty("allPlansExecution"),
                       tojson(shardedExplain));
            }
        }
    }

    let st = new ShardingTest({name: "views_sharded", shards: 2, other: {enableBalancer: false}});

    let mongos = st.s;
    let config = mongos.getDB("config");
    let db = mongos.getDB(jsTestName());
    db.dropDatabase();

    let coll = db.getCollection("coll");

    assert.commandWorked(config.adminCommand({enableSharding: db.getName()}));
    st.ensurePrimaryShard(db.getName(), "shard0000");
    assert.commandWorked(config.adminCommand({shardCollection: coll.getFullName(), key: {a: 1}}));

    assert.commandWorked(mongos.adminCommand({split: coll.getFullName(), middle: {a: 6}}));
    assert.commandWorked(
        db.adminCommand({moveChunk: coll.getFullName(), find: {a: 25}, to: "shard0001"}));

    for (let i = 0; i < 10; ++i) {
        assert.writeOK(coll.insert({a: i}));
    }

    assert.commandWorked(db.createView("view", coll.getName(), [{$match: {a: {$gte: 4}}}]));
    let view = db.getCollection("view");

    const explainVerbosities = ["queryPlanner", "executionStats", "allPlansExecution"];

    //
    // find
    //
    assert.eq(5, view.find({a: {$lte: 8}}).itcount());

    let result = db.runCommand({explain: {find: "view", filter: {a: {$lte: 7}}}});
    verifyExplainResult(result, "allPlansExecution");
    for (let verbosity of explainVerbosities) {
        result =
            db.runCommand({explain: {find: "view", filter: {a: {$lte: 7}}}, verbosity: verbosity});
        verifyExplainResult(result, verbosity);
    }

    //
    // aggregate
    //
    assert.eq(5, view.aggregate([{$match: {a: {$lte: 8}}}]).itcount());

    // Test that the explain:true flag for the aggregate command results in queryPlanner verbosity.
    result =
        db.runCommand({aggregate: "view", pipeline: [{$match: {a: {$lte: 8}}}], explain: true});
    verifyExplainResult(result, "queryPlanner");

    result = db.runCommand(
        {explain: {aggregate: "view", pipeline: [{$match: {a: {$lte: 8}}}], cursor: {}}});
    verifyExplainResult(result, "allPlansExecution");
    for (let verbosity of explainVerbosities) {
        result = db.runCommand({
            explain: {aggregate: "view", pipeline: [{$match: {a: {$lte: 8}}}], cursor: {}},
            verbosity: verbosity
        });
        verifyExplainResult(result, verbosity);
    }

    //
    // count
    //
    assert.eq(5, view.count({a: {$lte: 8}}));

    result = db.runCommand({explain: {count: "view", query: {a: {$lte: 8}}}});
    verifyExplainResult(result, "allPlansExecution");
    for (let verbosity of explainVerbosities) {
        result =
            db.runCommand({explain: {count: "view", query: {a: {$lte: 8}}}, verbosity: verbosity});
        verifyExplainResult(result, verbosity);
    }

    //
    // distinct
    //
    result = db.runCommand({distinct: "view", key: "a", query: {a: {$lte: 8}}});
    assert.commandWorked(result);
    assert.eq([4, 5, 6, 7, 8], result.values.sort());

    result = db.runCommand({explain: {distinct: "view", key: "a", query: {a: {$lte: 8}}}});
    verifyExplainResult(result, "allPlansExecution");
    for (let verbosity of explainVerbosities) {
        result = db.runCommand(
            {explain: {distinct: "view", key: "a", query: {a: {$lte: 8}}}, verbosity: verbosity});
        verifyExplainResult(result, verbosity);
    }

    //
    // Confirm cleanupOrphaned command fails.
    //
    result = st.getPrimaryShard(db.getName()).getDB("admin").runCommand({
        cleanupOrphaned: view.getFullName()
    });
    assert.commandFailedWithCode(result, ErrorCodes.CommandNotSupportedOnView);

    //
    //  Confirm getShardVersion command fails.
    //
    assert.commandFailedWithCode(db.adminCommand({getShardVersion: view.getFullName()}),
                                 ErrorCodes.NamespaceNotSharded);

    //
    // Confirm that the comment parameter on a find command is retained when rewritten as an
    // expanded aggregation on the view.
    //
    let sdb = st.shard0.getDB(jsTestName());
    assert.commandWorked(sdb.setProfilingLevel(2));

    assert.eq(5, view.find({a: {$lte: 8}}).comment("agg_comment").itcount());

    profilerHasSingleMatchingEntryOrThrow(sdb, {
        "command.aggregate": coll.getName(),
        "command.fromRouter": true,
        "command.comment": "agg_comment"
    });

    st.stop();
})();
