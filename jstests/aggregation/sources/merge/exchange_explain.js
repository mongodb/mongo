/**
 * Test $merge and exchange with explain.
 *
 * @tags: [requires_sharding]
 */
load('jstests/aggregation/extras/utils.js');

(function() {
    "use strict";

    const st = new ShardingTest({shards: 2, rs: {nodes: 1}});

    const mongosDB = st.s.getDB("test_db");

    const inColl = mongosDB["inColl"];
    const targetCollRange = mongosDB["targetCollRange"];
    const targetCollRangeOtherField = mongosDB["targetCollRangeOtherField"];
    const targetCollHash = mongosDB["targetCollHash"];

    const numDocs = 1000;

    function runExplainQuery(targetColl) {
        return inColl.explain("allPlansExecution").aggregate([
            {$group: {_id: "$a", a: {$avg: "$a"}}},
            {
              $merge: {
                  into: {
                      db: targetColl.getDB().getName(),
                      coll: targetColl.getName(),
                  },
                  whenMatched: "replaceWithNew",
                  whenNotMatched: "insert"
              }
            }
        ]);
    }

    function runRealQuery(targetColl) {
        return inColl.aggregate([
            {$group: {_id: "$a", a: {$avg: "$a"}}},
            {
              $merge: {
                  into: {
                      db: targetColl.getDB().getName(),
                      coll: targetColl.getName(),
                  },
                  whenMatched: "replaceWithNew",
                  whenNotMatched: "insert"
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
    st.shardColl(targetCollRange, {_id: 1}, {_id: 500}, {_id: 500}, mongosDB.getName());
    st.shardColl(targetCollRangeOtherField, {b: 1}, {b: 500}, {b: 500}, mongosDB.getName());
    st.shardColl(targetCollHash, {_id: "hashed"}, false, false, mongosDB.getName());

    // Run the explain. We expect to see the range based exchange here.
    let explain = runExplainQuery(targetCollRange);

    // Make sure we see the exchange in the explain output.
    assert.eq(explain.mergeType, "exchange", tojson(explain));
    let exchangeSpec = getExchangeSpec(explain);
    assert.eq(exchangeSpec.policy, "keyRange");
    assert.eq(exchangeSpec.key, {_id: 1});

    // Run the real query.
    runRealQuery(targetCollRange);
    let results = targetCollRange.aggregate([{'$count': "count"}]).next().count;
    assert.eq(results, numDocs);

    // Rerun the same query with the hash based exchange.
    explain = runExplainQuery(targetCollHash);

    // Make sure we see the exchange in the explain output.
    assert.eq(explain.mergeType, "exchange", tojson(explain));
    exchangeSpec = getExchangeSpec(explain);
    assert.eq(exchangeSpec.policy, "keyRange");
    assert.eq(exchangeSpec.key, {_id: "hashed"});

    // Run the real query.
    runRealQuery(targetCollHash);
    results = targetCollHash.aggregate([{'$count': "count"}]).next().count;
    assert.eq(results, numDocs);

    // This should fail because the "on" field ('b' in this case, the shard key of the target
    // collection) cannot be an array.
    assertErrorCode(inColl,
                    [{
                       $merge: {
                           into: {
                               db: targetCollRangeOtherField.getDB().getName(),
                               coll: targetCollRangeOtherField.getName(),
                           },
                           whenMatched: "replaceWithNew",
                           whenNotMatched: "insert"
                       }
                    }],
                    51132);

    // Turn off the exchange and rerun the query.
    assert.commandWorked(mongosDB.adminCommand({setParameter: 1, internalQueryDisableExchange: 1}));
    explain = runExplainQuery(targetCollRange);

    // Make sure there is no exchange.
    assert.eq(explain.mergeType, "anyShard", tojson(explain));
    assert(explain.hasOwnProperty("splitPipeline"), tojson(explain));
    assert(!explain.splitPipeline.hasOwnProperty("exchange"), tojson(explain));

    // This should fail similar to before even if we are not running the exchange.
    assertErrorCode(inColl,
                    [{
                       $merge: {
                           into: {
                               db: targetCollRangeOtherField.getDB().getName(),
                               coll: targetCollRangeOtherField.getName(),
                           },
                           whenMatched: "replaceWithNew",
                           whenNotMatched: "insert"
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
        pipeline: [{
            $merge: {
                into: targetCollRange.getName(),
                whenMatched: "replaceWithNew",
                whenNotMatched: "insert"
            }
        }],
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
