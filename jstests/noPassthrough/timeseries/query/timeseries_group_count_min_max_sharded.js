/**
 * Tests that sharded time-series collections correctly handle $group optimizations with $count and
 * $min/max. Verifies that grouping by different keys and combinations of accumulators works, and
 * that the previously failing case in SERVER-104401, where the needsMerge value wasn't propagated
 * from the parent expCtx to the _willBeMerging value in the group processor in the shards pipeline,
 * is fixed.
 *
 * @tags: [
 *   requires_fcv_82,
 *   requires_sharding,
 *   requires_timeseries,
 * ]
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// Start a 2-shard cluster with a small chunkSize so buckets get split over both shards quickly.
const st = new ShardingTest({shards: 2, other: {chunkSize: 1}});
const sDB = st.s.getDB('test');
assert(sDB.coll.drop());
assert.commandWorked(sDB.adminCommand({enableSharding: 'test', primaryShard: st.shard0.shardName}));
assert.commandWorked(sDB.adminCommand(
    {shardCollection: `test.coll`, key: {m: 1}, timeseries: {timeField: 't', metaField: 'm'}}));

const coll = sDB.coll;
st.startBalancer();

// Insert 100 docs with varied meta and a large payload to force bucket creation.
const alphabet = 'abcdefghijklmnop';
const docs = [];
for (let i = 0; i < 100; i++) {
    docs.push({t: new Date(i * 1000), m: i % 100, otherField: alphabet.repeat(1000)});
}
assert.commandWorked(coll.insert(docs));

// Let the balancer move documents so we have data on both shards.
st.awaitBalancerRound();

// Group by the large string field.
var results = assert.doesNotThrow(() => {
    return coll
        .aggregate(
            [{$group: {_id: "$otherField", document_count: {$count: {}}, max_time: {$max: "$t"}}}])
        .toArray();
});

assertArrayEq({
    actual: results,
    expected: [{
        _id: alphabet.repeat(1000),
        document_count: 100,
        max_time: new ISODate("1970-01-01T00:01:39Z")
    }]
});

// Group by the meta field 'm'.
results = assert.doesNotThrow(() => {
    return coll
        .aggregate([{$group: {_id: "$m", document_count: {$count: {}}, max_time: {$max: "$t"}}}])
        .toArray();
});

assert.eq(results.length, 100);

// Simple count with _id: null.
results = assert.doesNotThrow(() => {
    return coll.aggregate([{$group: {_id: null, document_count: {$count: {}}}}]).toArray();
});

assertArrayEq({actual: results, expected: [{_id: null, document_count: 100}]});

// Simple count with _id: null and min_time.
results = assert.doesNotThrow(() => {
    return coll
        .aggregate([{$group: {_id: null, document_count: {$count: {}}, min_time: {$min: "$t"}}}])
        .toArray();
});

assertArrayEq({
    actual: results,
    expected: [{_id: null, document_count: 100, min_time: new ISODate("1970-01-01T00:00:00Z")}]
});

// Simple count with _id: null and max_time.
// Verify that this no longer throws with assertion 9961600 (SERVER-104401).
results = assert.doesNotThrow(() => {
    return coll
        .aggregate([{$group: {_id: null, document_count: {$count: {}}, max_time: {$max: "$t"}}}])
        .toArray();
});

assertArrayEq({
    actual: results,
    expected: [{_id: null, document_count: 100, max_time: new ISODate("1970-01-01T00:01:39Z")}]
});

// Simple $sum (what $count desugars into) with _id: null and max_time.
results = assert.doesNotThrow(() => {
    return coll
        .aggregate([{$group: {_id: null, document_count: {$sum: 1}, max_time: {$max: "$t"}}}])
        .toArray();
});

assertArrayEq({
    actual: results,
    expected: [{_id: null, document_count: 100, max_time: new ISODate("1970-01-01T00:01:39Z")}]
});

st.stop();
