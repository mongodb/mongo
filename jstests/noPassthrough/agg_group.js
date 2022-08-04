// Tests that $group pushdown to SBE feature works in a sharded environment for some special
// scenarios.
//
// Notes:
// - In a sharded environment, the mongos splits a $group stage into two different stages. One is a
// merge $group stage at the mongos-side which does the global aggregation and the other is a $group
// stage at the shard-side which does the partial aggregation.
// - All aggregation features are tested by aggregation test suites under a sharded environment
// through passthrough tests. So, this test suite focuses on some special scenarios like for
// example, $group is pushed down to SBE at the shard-side and some accumulators may return the
// partial aggregation results in a special format to the mongos.
//
// @tags: [requires_sharding]
(function() {
'use strict';

load("jstests/libs/analyze_plan.js");

const st = new ShardingTest({config: 1, shards: 1});

// This database name can provide multiple similar test cases with a good separate namespace and
// each test case may create a separate collection for its own dataset.
const db = st.getDB(jsTestName());
const dbAtShard = st.shard0.getDB(jsTestName());

// Makes sure that the test db is sharded and the data is stored into the only shard.
assert.commandWorked(st.s0.adminCommand({enableSharding: db.getName()}));
st.ensurePrimaryShard(db.getName(), st.shard0.shardName);

let assertShardedGroupResultsMatch = (coll, pipeline) => {
    // Turns to the classic engine at the shard before figuring out its result.
    assert.commandWorked(
        dbAtShard.adminCommand({setParameter: 1, internalQueryForceClassicEngine: true}));

    // Collects the classic engine's result as the expected result, executing the pipeline at the
    // mongos.
    const classicalRes =
        coll.runCommand({aggregate: coll.getName(), pipeline: pipeline, cursor: {}})
            .cursor.firstBatch;

    // Turns to the SBE engine at the shard.
    assert.commandWorked(
        dbAtShard.adminCommand({setParameter: 1, internalQueryForceClassicEngine: false}));

    // Verifies that the SBE engine's results are same as the expected results, executing the
    // pipeline at the mongos.
    const sbeRes = coll.runCommand({aggregate: coll.getName(), pipeline: pipeline, cursor: {}})
                       .cursor.firstBatch;

    assert.sameMembers(sbeRes, classicalRes);
};

let prepareCollection = coll => {
    coll.drop();

    // Makes sure that the collection is sharded.
    assert.commandWorked(st.s0.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));

    return coll;
};

// A test case for a sharded $sum

let coll = prepareCollection(db.partial_sum);

// Prepares data for the 'NumberLong' sum result to overflow, when the shard sends back the partial
// sum as a doc with 'subTotal' and 'subTotalError' fields to the mongos. All data go to the only
// shard and so overflow will happen.
assert.commandWorked(
    coll.insert([{a: 1, b: NumberLong("9223372036854775807")}, {a: 2, b: NumberLong("10")}]));

assertShardedGroupResultsMatch(coll, [{$group: {_id: "$a", s: {$sum: "$b"}}}]);
assertShardedGroupResultsMatch(coll, [{$group: {_id: null, s: {$sum: "$b"}}}]);

// Test cases for a sharded $stdDevPop and $stdDevSamp

coll = prepareCollection(db.partial_std_dev);
assert.commandWorked(coll.insert([
    {"item": "a", "price": 10},
    {"item": "b", "price": 20},
    {"item": "a", "price": 5},
    {"item": "b", "price": 10},
    {"item": "c", "price": 5},
]));
assertShardedGroupResultsMatch(coll, [{$group: {_id: "$item", sd: {$stdDevSamp: "$price"}}}]);
assertShardedGroupResultsMatch(coll, [{$group: {_id: "$item", sd: {$stdDevSamp: "$missing"}}}]);
assertShardedGroupResultsMatch(coll, [{$group: {_id: "$item", sd: {$stdDevPop: "$price"}}}]);
assertShardedGroupResultsMatch(coll, [{$group: {_id: "$item", sd: {$stdDevPop: "$missing"}}}]);

// A test case for a sharded $avg

coll = prepareCollection(db.partial_avg);

// Prepares dataset so that each group has different numeric data types for price field which
// will excercise different code paths in generated SBE plan stages.
// Prices for group "a" are all decimals.
assert.commandWorked(coll.insert(
    [{item: "a", price: NumberDecimal("10.7")}, {item: "a", price: NumberDecimal("20.3")}]));
// Prices for group "b" are one decimal and one non-decimal.
assert.commandWorked(
    coll.insert([{item: "b", price: NumberDecimal("3.7")}, {item: "b", price: 2.3}]));
// Prices for group "c" are all non-decimals.
assert.commandWorked(coll.insert([{item: "c", price: 3}, {item: "b", price: 1}]));

// Verifies that SBE group pushdown with $avg works in a sharded environment.
assertShardedGroupResultsMatch(coll, [{$group: {_id: "$item", a: {$avg: "$price"}}}]);

// Verifies that SBE group pushdown with sharded $avg works for missing data.
assertShardedGroupResultsMatch(coll, [{$group: {_id: "$item", a: {$avg: "$missing"}}}]);

st.stop();
}());
