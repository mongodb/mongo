/**
 * @tags: [
 *   featureFlagAuthoritativeShardsDDL,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

// Deliberately inserts an orphan directly on a shard.
TestData.skipCheckOrphans = true;
const st = new ShardingTest({shards: 2});
const dbName = "test";
const coll = st.s.getDB(dbName).distinct_gapped;
const ns = coll.getFullName();

// Shard a collection:
//    - dbPrimary: shard0
//    - shardkey: {x:1}
//    - doc inserted: {x:15}
assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}),
);
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
assert.commandWorked(coll.insert({x: 15}));

// Move range [10, 20) to shard1
assert.commandWorked(
    st.s.adminCommand({moveRange: ns, min: {x: 10}, max: {x: 20}, toShard: st.shard1.shardName}),
);

// Insert an orphan to shard1 lower than {x:15} (the previously inserted doc)
assert.commandWorked(st.shard1.getCollection(ns).insert({x: 5}));

// Guard that we are actually exercising the shard-filtering DISTINCT_SCAN path.
assert(
    JSON.stringify(coll.explain().distinct("x")).includes("DISTINCT_SCAN"),
    "expected a DISTINCT_SCAN plan",
);

const res = coll.distinct("x");
jsTestLog("Distinct result: " + tojson(res));
assert.eq([15], res);

// Remove the manually-inserted orphan so the post-test orphan check passes.
assert.commandWorked(st.shard1.getCollection(ns).deleteOne({x: 5}));

st.stop();
