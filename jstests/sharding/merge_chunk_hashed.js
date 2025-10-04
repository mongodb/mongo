/*
 * Test that merging chunks for hashed sharding via mongos works/doesn't work with
 * different chunk configurations.
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {chunkBoundsUtil} from "jstests/sharding/libs/chunk_bounds_util.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

let st = new ShardingTest({shards: 2, mongos: 2});
// , configOptions: {verbose: 3}
let mongos = st.s0;
let staleMongos = st.s1;

let dbName = "test";
let collName = "user";
let ns = dbName + "." + collName;
let configDB = mongos.getDB("config");
let admin = mongos.getDB("admin");

assert.commandWorked(admin.runCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
let coll = mongos.getCollection(ns);

assert.commandWorked(admin.runCommand({shardCollection: ns, key: {x: "hashed"}}));

// Setup predictable chunk distribution:
// shard0: MIN                  -> -4611686018427387902,
//         -4611686018427387902 -> 0
// shard1: 0                    -> 4611686018427387902
//         4611686018427387902  -> MAX
assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: NumberLong("-4611686018427387902")}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: NumberLong("4611686018427387902")}}));

assert.commandWorked(
    admin.runCommand({
        moveChunk: ns,
        bounds: [{x: MinKey}, {x: NumberLong("-4611686018427387902")}],
        to: st.shard0.shardName,
        _waitForDelete: true,
    }),
);
assert.commandWorked(
    admin.runCommand({
        moveChunk: ns,
        bounds: [{x: NumberLong("-4611686018427387902")}, {x: 0}],
        to: st.shard0.shardName,
        _waitForDelete: true,
    }),
);
assert.commandWorked(
    admin.runCommand({
        moveChunk: ns,
        bounds: [{x: 0}, {x: NumberLong("4611686018427387902")}],
        to: st.shard1.shardName,
        _waitForDelete: true,
    }),
);
assert.commandWorked(
    admin.runCommand({
        moveChunk: ns,
        bounds: [{x: NumberLong("4611686018427387902")}, {x: MaxKey}],
        to: st.shard1.shardName,
        _waitForDelete: true,
    }),
);

// Get the chunk -4611686018427387902 -> 0 on shard0.
let chunkToSplit = findChunksUtil.findOneChunkByNs(configDB, ns, {min: {$ne: {x: MinKey}}, shard: st.shard0.shardName});

// Create chunks from that chunk and move some chunks to create holes.
// shard0: MIN                  -> chunkToSplit.min,
//         chunkToSplit.min     -> -4500000000000000000,
//         (hole),
//         -4000000000000000000 -> -3500000000000000000,
//         -3500000000000000000 -> -3000000000000000000,
//         -3000000000000000000 -> -2500000000000000000,
//         (hole),
//         -2000000000000000000 -> 0
// shard1: -4500000000000000000 ->  -4000000000000000000
//         -2500000000000000000 -> -2000000000000000000
//         0                    -> 4611686018427387902
//         4611686018427387902  -> MAX
let splitPoints = [
    {x: NumberLong(-4500000000000000000)},
    {x: NumberLong(-4000000000000000000)},
    {x: NumberLong(-3500000000000000000)},
    {x: NumberLong(-3000000000000000000)},
    {x: NumberLong(-2500000000000000000)},
    {x: NumberLong(-2000000000000000000)},
];
assert.gt(0, bsonWoCompare(chunkToSplit.min, splitPoints[0]));
assert.lt(0, bsonWoCompare(chunkToSplit.max, splitPoints[splitPoints.length - 1]));

jsTest.log("Creating additional chunks and holes...");
for (let splitPoint of splitPoints) {
    assert.commandWorked(admin.runCommand({split: ns, middle: splitPoint}));
}
assert.commandWorked(
    admin.runCommand({
        moveChunk: ns,
        bounds: [{x: NumberLong(-4500000000000000000)}, {x: NumberLong(-4000000000000000000)}],
        to: st.shard1.shardName,
        _waitForDelete: true,
    }),
);
assert.commandWorked(
    admin.runCommand({
        moveChunk: ns,
        bounds: [{x: NumberLong(-2500000000000000000)}, {x: NumberLong(-2000000000000000000)}],
        to: st.shard1.shardName,
        _waitForDelete: true,
    }),
);

let chunkDocs = findChunksUtil.findChunksByNs(configDB, ns).toArray();
let shardChunkBounds = chunkBoundsUtil.findShardChunkBounds(chunkDocs);

jsTest.log("Inserting docs...");
// Use docs that belong to different chunks and go on both shards.
let docs = [{x: -100}, {x: -60}, {x: -10}, {x: 10}];
let shards = [];
let docChunkBounds = [];
docs.forEach(function (doc) {
    let hash = convertShardKeyToHashed(doc.x);
    let {shard, bounds} = chunkBoundsUtil.findShardAndChunkBoundsForShardKey(st, shardChunkBounds, {x: hash});
    shards.push(shard);
    docChunkBounds.push(bounds);
});
assert.eq(2, new Set(shards).size);
assert.eq(docs.length, new Set(docChunkBounds).size);
assert.commandWorked(coll.insert(docs));

let staleCollection = staleMongos.getCollection(ns);

jsTest.log("Trying merges that should fail...");

// Make sure merging non-exact chunks is invalid.
assert.commandFailed(admin.runCommand({mergeChunks: ns, bounds: [{x: MinKey}, {x: NumberLong(-5000000000000000000)}]}));
assert.commandFailed(
    admin.runCommand({
        mergeChunks: ns,
        bounds: [{x: NumberLong(-5500000000000000000)}, {x: NumberLong(-4500000000000000000)}],
    }),
);
assert.commandFailed(
    admin.runCommand({
        mergeChunks: ns,
        bounds: [{x: NumberLong(4500000000000000000)}, {x: NumberLong(5500000000000000000)}],
    }),
);
assert.commandFailed(admin.runCommand({mergeChunks: ns, bounds: [{x: NumberLong(-1500000000000000000)}, {x: MaxKey}]}));

// Make sure merging over holes is invalid.
assert.commandFailed(admin.runCommand({mergeChunks: ns, bounds: [{x: MinKey}, {x: NumberLong(-3500000000000000000)}]}));
assert.commandFailed(
    admin.runCommand({mergeChunks: ns, bounds: [{x: NumberLong(-3500000000000000000)}, {x: NumberLong(0)}]}),
);
assert.commandFailed(
    admin.runCommand({mergeChunks: ns, bounds: [{x: NumberLong(-3000000000000000000)}, {x: NumberLong(0)}]}),
);

// Make sure merging between shards is invalid.
assert.commandFailed(admin.runCommand({mergeChunks: ns, bounds: [{x: MinKey}, {x: NumberLong(-4000000000000000000)}]}));
assert.commandFailed(
    admin.runCommand({
        mergeChunks: ns,
        bounds: [{x: NumberLong(-3000000000000000000)}, {x: NumberLong(-2000000000000000000)}],
    }),
);
assert.commandFailed(
    admin.runCommand({mergeChunks: ns, bounds: [{x: NumberLong(-2500000000000000000)}, {x: NumberLong(0)}]}),
);
assert.eq(4, staleCollection.find().itcount());

jsTest.log("Trying merges that should succeed...");

// Merging single chunks should be treated as a no-op
// (or fail because 'the range specifies one single chunk' in multiversion test environments)
try {
    assert.commandWorked(admin.runCommand({mergeChunks: ns, bounds: [{x: MinKey}, chunkToSplit.min]}));
} catch (e) {
    if (!e.message.match(/could not merge chunks, collection .* already contains chunk for/)) {
        throw e;
    }
}
try {
    assert.commandWorked(
        admin.runCommand({
            mergeChunks: ns,
            bounds: [{x: NumberLong(-4500000000000000000)}, {x: NumberLong(-4000000000000000000)}],
        }),
    );
} catch (e) {
    if (!e.message.match(/could not merge chunks, collection .* already contains chunk for/)) {
        throw e;
    }
}

// Make sure merge including the MinKey works.
assert.commandWorked(admin.runCommand({mergeChunks: ns, bounds: [{x: MinKey}, {x: NumberLong(-4500000000000000000)}]}));
assert.eq(4, staleCollection.find().itcount());
// shard0: MIN                  -> -4500000000000000000,
//         (hole),
//         -4000000000000000000 -> -3500000000000000000,
//         -3500000000000000000 -> -3000000000000000000,
//         -3000000000000000000 -> -2500000000000000000,
//         (hole),
//         -2000000000000000000 -> 0
// shard1: -4500000000000000000 ->  -4000000000000000000
//         -2500000000000000000 -> -2000000000000000000
//         0                    -> 4611686018427387902
//         4611686018427387902  -> MAX

// Make sure merging three chunks in the middle works.
assert.commandWorked(
    admin.runCommand({
        mergeChunks: ns,
        bounds: [{x: NumberLong(-4000000000000000000)}, {x: NumberLong(-2500000000000000000)}],
    }),
);
assert.eq(4, staleCollection.find().itcount());
// shard0: MIN                  -> -4500000000000000000,
//         (hole),
//         -4000000000000000000 -> -2500000000000000000,
//         (hole),
//         -2000000000000000000 -> 0
// shard1: -4500000000000000000 -> -4000000000000000000
//         -2500000000000000000 -> -2000000000000000000
//         0                    -> 4611686018427387902
//         4611686018427387902  -> MAX

// Make sure merge including the MaxKey works.
assert.commandWorked(admin.runCommand({mergeChunks: ns, bounds: [{x: NumberLong(0)}, {x: MaxKey}]}));
assert.eq(4, staleCollection.find().itcount());

// Make sure merging chunks after a chunk has been moved out of a shard succeeds
assert.commandWorked(
    admin.runCommand({
        moveChunk: ns,
        bounds: [{x: NumberLong(-2000000000000000000)}, {x: NumberLong(0)}],
        to: st.shard1.shardName,
        _waitForDelete: true,
    }),
);
assert.commandWorked(
    admin.runCommand({
        moveChunk: ns,
        bounds: [{x: NumberLong(-4500000000000000000)}, {x: NumberLong(-4000000000000000000)}],
        to: st.shard0.shardName,
        _waitForDelete: true,
    }),
);
assert.eq(4, staleCollection.find().itcount());
// shard0: MIN                  -> -4500000000000000000,
//         -4500000000000000000 -> -4000000000000000000
//         -4000000000000000000 -> -2500000000000000000,
// shard1: -2500000000000000000 -> -2000000000000000000
//         -2000000000000000000 -> 0
//         0                    -> 4611686018427387902
//         4611686018427387902  -> MAX

assert.commandWorked(admin.runCommand({mergeChunks: ns, bounds: [{x: NumberLong(-2500000000000000000)}, {x: MaxKey}]}));
assert.eq(4, staleCollection.find().itcount());
// shard0: MIN                  -> -4500000000000000000,
//         -4500000000000000000 -> -4000000000000000000
//         -4000000000000000000 -> -2500000000000000000,
// shard1: -2500000000000000000 -> MAX

// Make sure merge on the other shard after a chunk has been merged succeeds.
assert.commandWorked(admin.runCommand({mergeChunks: ns, bounds: [{x: MinKey}, {x: NumberLong(-2500000000000000000)}]}));
// shard0: MIN                  -> -2500000000000000000,
// shard1: -2500000000000000000 -> MAX

assert.eq(2, findChunksUtil.findChunksByNs(configDB, ns).itcount());
assert.eq(
    1,
    findChunksUtil
        .findChunksByNs(configDB, ns, {
            min: {x: MinKey},
            max: {x: NumberLong(-2500000000000000000)},
            shard: st.shard0.shardName,
        })
        .count(),
);
assert.eq(
    1,
    findChunksUtil
        .findChunksByNs(configDB, ns, {
            min: {x: NumberLong(-2500000000000000000)},
            max: {x: MaxKey},
            shard: st.shard1.shardName,
        })
        .count(),
);

st.stop();
