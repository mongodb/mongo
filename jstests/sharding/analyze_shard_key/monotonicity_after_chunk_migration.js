/**
 * Tests that the analyzeShardKey command returns the correct monotonicity in the case where the
 * collection has gone through a chunk migration.
 *
 * @tags: [requires_fcv_63, featureFlagAnalyzeShardKey]
 */
(function() {
"use strict";

load("jstests/sharding/analyze_shard_key/libs/analyze_shard_key_util.js");

const st = new ShardingTest({
    shards: 2,
    rs: {
        nodes: 2,
        setParameter: {
            "failpoint.analyzeShardKeySkipCalcalutingReadWriteDistributionMetrics":
                tojson({mode: "alwaysOn"})
        }
    }
});

const dbName = "testDb";
const collName = "testColl";
const ns = dbName + "." + collName;

const db = st.s.getDB(dbName);
const coll = db.getCollection(collName);

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);

// Make the collection have the following chunks:
// shard0: [MinKey, 0] (-11000 documents)
// shard1: [0, MaxKey] (2000 documents)
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 0}}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: 0}, to: st.shard1.shardName}));

const minVal = -11000;
const maxVal = 2000;
const docs = [];
for (let i = minVal; i < maxVal; i++) {
    docs.push({x: i});
}
assert.commandWorked(db.runCommand({insert: collName, documents: docs, ordered: true}));

const listCollectionRes =
    assert.commandWorked(db.runCommand({listCollections: 1, filter: {name: collName}}));
const isClusteredColl =
    listCollectionRes.cursor.firstBatch[0].options.hasOwnProperty("clusteredIndex");
const expectedType = isClusteredColl ? "unknown" : "monotonic";

const res0 = assert.commandWorked(st.s.adminCommand({analyzeShardKey: ns, key: {x: 1}}));
assert.eq(res0.monotonicity.type, expectedType, res0);

// Make the collection have the following chunks:
// shard0: [MinKey, -1000] (10000 documents)
// shard1: [0, MaxKey] [-1000, 0] (3000 documents)
assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: -1000}}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: -1000}, to: st.shard1.shardName}));

// If mongos forwards the command to shard1 instead of shard0 (primary shard), the monotonicity
// check will find that the documents [0, 2000] were inserted before the documents [-1000, 0] and
// return that the shard key is not monotonically changing.
const res1 = assert.commandWorked(st.s.adminCommand({analyzeShardKey: ns, key: {x: 1}}));
assert.eq(res1.monotonicity.type, expectedType, res1);

st.stop();
})();
