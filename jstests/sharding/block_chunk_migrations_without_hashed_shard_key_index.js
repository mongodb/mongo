/**
 * Tests that chunk migrations are blocked when there is no index on a hashed shard key.
 *
 * @tags: [
 *   requires_fcv_70,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

const st = new ShardingTest({});

const dbName = "testDb";
const collName = "testColl";
const nss = dbName + "." + collName;
const configDB = st.s.getDB('config');

const coll = st.getDB(dbName).getCollection(collName);
let docs = Array.from({length: 1000}, (x, i) => ({_id: i, field: "a".repeat(1024 * 1024)}));

assert.commandWorked(
    st.s.adminCommand({enablesharding: dbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(coll.createIndex({"_id": "hashed"}));
assert.commandWorked(st.s.adminCommand({shardCollection: nss, key: {_id: "hashed"}}));

// Move all chunks to a single shard so the balancer is triggered due to data imbalance.
let chunks = findChunksUtil.findChunksByNs(configDB, nss).toArray();
chunks.forEach(chunk => {
    st.s.adminCommand({moveChunk: nss, bounds: [chunk.min, chunk.max], to: st.shard0.shardName});
});

assert.eq(0, findChunksUtil.findChunksByNs(configDB, nss, {shard: st.shard1.shardName}).itcount());

assert.commandWorked(coll.insert(docs));
assert.commandWorked(coll.dropIndex({"_id": "hashed"}));

st.startBalancer();
st.awaitBalancerRound();

// During balancing, the balancer should catch the IndexNotFound and turn off the balancer for the
// collection by setting {noBalance : true}.
assert.soon(() => {
    return configDB.getCollection('collections')
               .findOne({_id: nss}, {}, {readConcern: "majority"})
               .noBalance === true;
});
st.awaitBalancerRound();

// Confirm all chunks remain on shard0.
assert.eq(0, findChunksUtil.findChunksByNs(configDB, nss, {shard: st.shard1.shardName}).itcount());

// Commands that trigger chunk migrations should fail with IndexNotFound.
assert.commandFailedWithCode(
    st.s.adminCommand(
        {moveChunk: nss, bounds: [chunks[0].min, chunks[0].max], to: st.shard1.shardName}),
    ErrorCodes.IndexNotFound);
assert.commandFailedWithCode(
    st.s.adminCommand(
        {moveRange: nss, toShard: st.shard1.shardName, min: chunks[0].min, max: chunks[0].max}),
    ErrorCodes.IndexNotFound);

// Recreate the index and verify that we can re-enable balancing.
assert.commandWorked(coll.createIndex({"_id": "hashed"}));
st.enableBalancing(nss);

assert.eq(false, configDB.getCollection('collections').findOne({_id: nss}).noBalance);
st.awaitBalancerRound();
assert.soon(() => {
    return findChunksUtil.findChunksByNs(configDB, nss, {shard: st.shard1.shardName}).itcount() > 0;
});

st.awaitMigrations();

st.stop();
