/**
 * Test $out and exchange with explain.
 *
 * @tags: [requires_sharding]
 */
(function() {
    "use strict";

    const st = new ShardingTest({shards: 2, rs: {nodes: 1}});

    const mongosDB = st.s.getDB("test_db");

    const inColl = mongosDB["inColl"];
    const outColl = mongosDB["outColl"];

    // Shard the input collection.
    st.shardColl(inColl, {a: 1}, {a: 500}, {a: 500}, mongosDB.getName());

    // Insert some data to the input collection.
    let bulk = inColl.initializeUnorderedBulkOp();
    for (let i = 0; i < 1000; i++) {
        bulk.insert({a: i});
    }
    assert.commandWorked(bulk.execute());

    // Shard the output collection.
    st.shardColl(outColl, {_id: 1}, {_id: 500}, {_id: 500}, mongosDB.getName());

    // Run the explain.
    let explain = inColl.explain("allPlansExecution").aggregate([
        {$group: {_id: "$a", a: {$avg: "$a"}}},
        {$out: {to: outColl.getName(), db: outColl.getDB().getName(), mode: "replaceDocuments"}}
    ]);

    // Make sure we see the exchange in the explain output.
    assert.eq(explain.mergeType, "exchange", tojson(explain));
    assert(explain.hasOwnProperty("splitPipeline"), tojson(explain));
    assert(explain.splitPipeline.hasOwnProperty("exchange"), tojson(explain));

    // Turn off the exchange and rerun the query.
    assert.commandWorked(mongosDB.adminCommand({setParameter: 1, internalQueryDisableExchange: 1}));
    explain = inColl.explain("allPlansExecution").aggregate([
        {$group: {_id: "$a", a: {$avg: "$a"}}},
        {$out: {to: outColl.getName(), db: outColl.getDB().getName(), mode: "replaceDocuments"}}
    ]);

    // Make sure there is no exchange.
    assert.eq(explain.mergeType, "primaryShard", tojson(explain));
    assert(explain.hasOwnProperty("splitPipeline"), tojson(explain));
    assert(!explain.splitPipeline.hasOwnProperty("exchange"), tojson(explain));

    st.stop();
}());
