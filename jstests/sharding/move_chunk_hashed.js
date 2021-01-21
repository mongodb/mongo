/*
 * Test that moveChunk moves the right chunks and documents and that deleted
 * rangeDeleter delete the right documents from the donor shard.
 */
(function() {
'use strict';

load('jstests/libs/chunk_manipulation_util.js');
load("jstests/sharding/libs/chunk_bounds_util.js");
load("jstests/sharding/libs/find_chunks_util.js");

/*
 * Returns the shard with the given shard name.
 */
function getShard(st, shardName) {
    for (let i = 0; i < st._connections.length; i++) {
        if (st._connections[i].shardName == shardName) {
            return st._connections[i];
        }
    }
}

/*
 * Returns true if docs contains targetDoc.
 */
function contains(docs, targetDoc) {
    for (let doc of docs) {
        if (bsonWoCompare(doc, targetDoc) == 0) {
            return true;
        }
    }
    return false;
}

let st = new ShardingTest({shards: 3});
let dbName = "test";
let collName = "user";
let ns = dbName + "." + collName;
let configDB = st.s.getDB('config');
let testDB = st.s.getDB(dbName);

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard1.shardName);
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 'hashed'}}));

// Use docs that are expected to go to multiple different shards.
let docs = [{x: -10}, {x: -1}, {x: 0}, {x: 1}, {x: 10}];
assert.commandWorked(testDB.user.insert(docs));

// Find the chunks with docs.
let chunkDocs = findChunksUtil.findChunksByNs(configDB, ns).toArray();
let chunksWithDocs = [];
let numChunksWithMultipleDocs = 0;

for (let chunkDoc of chunkDocs) {
    let docsInChunk = docs.filter((doc) => {
        let hash = convertShardKeyToHashed(doc.x);
        return chunkBoundsUtil.containsKey({x: hash}, chunkDoc.min, chunkDoc.max);
    });
    if (docsInChunk.length > 0) {
        chunksWithDocs.push({
            id: chunkDoc._id,
            shard: getShard(st, chunkDoc.shard),
            bounds: [chunkDoc.min, chunkDoc.max],
            docs: docsInChunk
        });
        numChunksWithMultipleDocs += docsInChunk.length > 1;
    }
}

// Check that we are moving chunks with one doc and multiple docs.
assert.lt(numChunksWithMultipleDocs, chunksWithDocs.length);

// Move the chunks one at a time.
for (let chunk of chunksWithDocs) {
    let docsOnFromShard = chunk.shard.getCollection(ns).find({}, {_id: 0}).toArray();
    let toShard = st.getOther(chunk.shard);
    assert.commandWorked(st.s.adminCommand(
        {moveChunk: ns, bounds: chunk.bounds, to: toShard.shardName, _waitForDelete: true}));

    // Check that the config database is updated correctly.
    assert.eq(0,
              findChunksUtil.countChunksForNs(
                  configDB, ns, {_id: chunk.id, shard: chunk.shard.shardName}));
    assert.eq(
        1,
        findChunksUtil.countChunksForNs(configDB, ns, {_id: chunk.id, shard: toShard.shardName}));

    // Check that the docs in the donated chunk are transferred to the recipient, and the
    // other docs remain on the donor.
    for (let doc of docsOnFromShard) {
        if (contains(chunk.docs, doc)) {
            assert.eq(0, chunk.shard.getCollection(ns).count(doc));
            assert.eq(1, toShard.getCollection(ns).count(doc));
        } else {
            assert.eq(1, chunk.shard.getCollection(ns).count(doc));
        }
    }
}

st.stop();
})();
