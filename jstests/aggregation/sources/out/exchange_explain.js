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
    const outCollRange = mongosDB["outCollRange"];
    const outCollHash = mongosDB["outCollHash"];

    const numDocs = 1000;

    function runExplainQuery(outColl) {
        return inColl.explain("allPlansExecution").aggregate([
            {$group: {_id: "$a", a: {$avg: "$a"}}},
            {
              $out: {
                  to: outColl.getName(),
                  db: outColl.getDB().getName(),
                  mode: "replaceDocuments"
              }
            }
        ]);
    }

    function runRealQuery(outColl) {
        return inColl.aggregate([
            {$group: {_id: "$a", a: {$avg: "$a"}}},
            {
              $out: {
                  to: outColl.getName(),
                  db: outColl.getDB().getName(),
                  mode: "replaceDocuments"
              }
            }
        ]);
    }

    function getExchangeSpec(explain) {
        assert(explain.hasOwnProperty("splitPipeline"), tojson(explain));
        assert(explain.splitPipeline.hasOwnProperty("exchange"), tojson(explain));

        return explain.splitPipeline.exchange;
    }

    // Shard the input collection.
    st.shardColl(inColl, {a: 1}, {a: 500}, {a: 500}, mongosDB.getName());

    // Insert some data to the input collection.
    let bulk = inColl.initializeUnorderedBulkOp();
    for (let i = 0; i < numDocs; i++) {
        bulk.insert({a: i});
    }
    assert.commandWorked(bulk.execute());

    // Shard the output collections.
    st.shardColl(outCollRange, {_id: 1}, {_id: 500}, {_id: 500}, mongosDB.getName());
    st.shardColl(outCollHash, {_id: "hashed"}, false, false, mongosDB.getName());

    // Run the explain. We expect to see the range based exchange here.
    let explain = runExplainQuery(outCollRange);

    // Make sure we see the exchange in the explain output.
    assert.eq(explain.mergeType, "exchange", tojson(explain));
    let exchangeSpec = getExchangeSpec(explain);
    assert.eq(exchangeSpec.policy, "keyRange");
    assert.eq(exchangeSpec.key, {_id: 1});

    // Run the real query.
    runRealQuery(outCollRange);
    let results = outCollRange.aggregate([{'$count': "count"}]).next().count;
    assert.eq(results, numDocs);

    // Rerun the same query with the hash based exchange.
    explain = runExplainQuery(outCollHash);

    // Make sure we see the exchange in the explain output.
    assert.eq(explain.mergeType, "exchange", tojson(explain));
    exchangeSpec = getExchangeSpec(explain);
    assert.eq(exchangeSpec.policy, "keyRange");
    assert.eq(exchangeSpec.key, {_id: "hashed"});

    // Run the real query.
    runRealQuery(outCollHash);
    results = outCollHash.aggregate([{'$count': "count"}]).next().count;
    assert.eq(results, numDocs);

    // Turn off the exchange and rerun the query.
    assert.commandWorked(mongosDB.adminCommand({setParameter: 1, internalQueryDisableExchange: 1}));
    explain = runExplainQuery(outCollRange);

    // Make sure there is no exchange.
    assert.eq(explain.mergeType, "primaryShard", tojson(explain));
    assert(explain.hasOwnProperty("splitPipeline"), tojson(explain));
    assert(!explain.splitPipeline.hasOwnProperty("exchange"), tojson(explain));

    st.stop();
}());
