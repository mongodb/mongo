/**
 * Verifies that scaling is applied after summing the statistics together from individual shards.
 * @tags: [
 *   expects_explicit_underscore_id_index,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = "test";
const collName = "scaled_collection_stats";
const ns = dbName + "." + collName;

const st = new ShardingTest({shards: 2});

assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

jsTest.log("Insert some data.");
const coll = st.s0.getDB(dbName)[collName];
let bulk = coll.initializeUnorderedBulkOp();
for (let i = -50; i < 50; i++) {
    bulk.insert({_id: i});
}
assert.commandWorked(bulk.execute());

jsTest.log("Create a sharded collection with one chunk on each of the two shards.");
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard1.shardName, _waitForDelete: true}));

// Ensure that all the inserted docs are on disk so that stats() returns up to date info.
assert.commandWorked(st.s.adminCommand({fsync: 1}));

// Determine the unscaled index size.
let res = assert.commandWorked(coll.stats());
const totalIndexSize = res.totalIndexSize;

assert(res.sharded);
assert.eq(2, res.nchunks);

assert.commandFailed(coll.stats(-1));
assert.commandFailed(coll.stats(0));
assert.commandFailed(coll.stats(0.99));

// Verify that the scaling is applied after summing the statistics together from individual shards.
res = assert.commandWorked(coll.stats(totalIndexSize));

// Scaling was applied before summing the individual shard statistics together making it
// less accurate as the statistics are represented as integers using the floor() function. Now that
// the individual shard statistics are summed together before any scaling is applied, we see more
// accuracy.
assert.eq(1, res.totalIndexSize);

st.stop();
