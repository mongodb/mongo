/**
 * Technically this is not time series colleciton test; however, due to legacy behavior, a user
 * inserts into a collection in time series bucket namespace is required not to fail.  Please note
 * this behavior is likely going to be corrected in 6.3 or after. The presence of this test does
 * not imply such behavior is supported.
 *
 * As this tests code path relevant to time series, the requires_tiemseries flag is set to avoid
 * incompatible behavior related to multi statement transactions.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   requires_fcv_63
 * ]
 */
(function() {
"use strict";

var st = new ShardingTest({shards: 2});
assert.commandWorked(st.s0.adminCommand({enableSharding: 'test'}));
st.ensurePrimaryShard('test', st.shard1.shardName);
var testDB = st.getDB('test');

// shard the collection on x
assert.commandWorked(st.s0.adminCommand({shardcollection: "test.coll", key: {x: 1}}));

assert.commandWorked(testDB.system.buckets.coll.insert({a: 1}));
assert.commandWorked(testDB.coll.insert({b: 1}));

var docs = testDB.coll.find().toArray();
assert.eq(1, docs.length);

var docsSystemBuckets = testDB.system.buckets.coll.find().toArray();
assert.eq(1, docsSystemBuckets.length);

st.stop();
})();
