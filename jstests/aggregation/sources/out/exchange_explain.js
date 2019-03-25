/**
 * Test $out and exchange with explain.
 *
 * @tags: [requires_sharding]
 */
load('jstests/aggregation/extras/utils.js');

(function() {
    "use strict";

    const st = new ShardingTest({shards: 2, rs: {nodes: 1}});

    const mongosDB = st.s.getDB("test_db");

    const inColl = mongosDB["inColl"];
    const outCollRange = mongosDB["outCollRange"];
    const outCollRangeOtherField = mongosDB["outCollRangeOtherField"];
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
        bulk.insert({a: i}, {b: [0, 1, 2, 3, i]});
    }
    assert.commandWorked(bulk.execute());

    // Shard the output collections.
    st.shardColl(outCollRange, {_id: 1}, {_id: 500}, {_id: 500}, mongosDB.getName());
    st.shardColl(outCollRangeOtherField, {b: 1}, {b: 500}, {b: 500}, mongosDB.getName());
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

    // This should fail with the error '$out write error: uniqueKey field 'b' cannot be missing,
    // null, undefined or an array.' as we are trying to insert an array value.
    assertErrorCode(inColl,
                    [{
                       $out: {
                           to: outCollRangeOtherField.getName(),
                           db: outCollRangeOtherField.getDB().getName(),
                           mode: "replaceDocuments"
                       }
                    }],
                    51132);

    // Turn off the exchange and rerun the query.
    assert.commandWorked(mongosDB.adminCommand({setParameter: 1, internalQueryDisableExchange: 1}));
    explain = runExplainQuery(outCollRange);

    // Make sure there is no exchange.
    assert.eq(explain.mergeType, "anyShard", tojson(explain));
    assert(explain.hasOwnProperty("splitPipeline"), tojson(explain));
    assert(!explain.splitPipeline.hasOwnProperty("exchange"), tojson(explain));

    // This should fail with the same error '$out write error: uniqueKey field 'b' cannot be
    // missing, null, undefined or an array.' as before even if we are not running the exchange.
    assertErrorCode(inColl,
                    [{
                       $out: {
                           to: outCollRangeOtherField.getName(),
                           db: outCollRangeOtherField.getDB().getName(),
                           mode: "replaceDocuments"
                       }
                    }],
                    51132);

    // SERVER-38349 Make sure mongos rejects specifying exchange directly.
    assert.commandFailedWithCode(mongosDB.runCommand({
        aggregate: inColl.getName(),
        pipeline: [],
        cursor: {},
        exchange: {
            policy: "keyRange",
            bufferSize: NumberInt(1024),
            boundaries: [{_id: 0}],
            consumers: NumberInt(2),
            consumerIds: [NumberInt(0), NumberInt(1)]
        }
    }),
                                 51028);

    assert.commandFailedWithCode(mongosDB.runCommand({
        aggregate: inColl.getName(),
        pipeline: [{$out: {to: outCollRange.getName(), mode: "replaceDocuments"}}],
        cursor: {},
        exchange: {
            policy: "keyRange",
            bufferSize: NumberInt(1024),
            boundaries: [{_id: 0}],
            consumers: NumberInt(2),
            consumerIds: [NumberInt(0), NumberInt(1)]
        }
    }),
                                 51028);

    st.stop();
}());
