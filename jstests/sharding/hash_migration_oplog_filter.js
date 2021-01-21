/*
 * Test that _getNextSessionMods filters out unrelated oplog entries.
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
(function() {
'use strict';

load('jstests/libs/chunk_manipulation_util.js');
load("jstests/sharding/libs/chunk_bounds_util.js");
load("jstests/sharding/libs/find_chunks_util.js");

/*
 * Returns the oplog entry on the shard that matches the query. Asserts
 * that there is only one such oplog entry.
 */
function findOplogEntry(shard, query) {
    let oplogDocs = shard.getDB('local').oplog.rs.find(query).toArray();
    assert.eq(1, oplogDocs.length);
    return oplogDocs[0];
}

let st = new ShardingTest({shards: 2});
let dbName = "test";
let collName = "user";
let ns = dbName + "." + collName;
let configDB = st.s.getDB('config');
let testDB = st.s.getDB(dbName);

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard1.shardName);
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 'hashed'}}));

let chunkDocs = findChunksUtil.findChunksByNs(configDB, ns).toArray();
let shardChunkBounds = chunkBoundsUtil.findShardChunkBounds(chunkDocs);

// Use docs that are expected to go to the same shards but different chunks.
let docs = [{x: -1000}, {x: 10}];
let shards = [];
let docChunkBounds = [];

docs.forEach(function(doc) {
    let hashDoc = {x: convertShardKeyToHashed(doc.x)};

    // Check that chunks for the doc and its hash are different.
    let originalShardBoundsPair =
        chunkBoundsUtil.findShardAndChunkBoundsForShardKey(st, shardChunkBounds, doc);
    let hashShardBoundsPair =
        chunkBoundsUtil.findShardAndChunkBoundsForShardKey(st, shardChunkBounds, hashDoc);
    assert.neq(originalShardBoundsPair.bounds, hashShardBoundsPair.bounds);

    shards.push(hashShardBoundsPair.shard);
    docChunkBounds.push(hashShardBoundsPair.bounds);
});

assert.eq(shards[0], shards[1]);
assert.neq(docChunkBounds[0], docChunkBounds[1]);

// Insert the docs.
let cmd = {
    insert: collName,
    documents: docs,
    ordered: false,
    lsid: {id: UUID()},
    txnNumber: NumberLong(35)
};
assert.commandWorked(testDB.runCommand(cmd));
assert.eq(2, testDB.user.find({}).count());
assert.eq(2, shards[0].getCollection(ns).find({}).count());

// Move the chunk for docs[0].
let fromShard = shards[0];
let toShard = st.getOther(fromShard);
assert.commandWorked(st.s.adminCommand(
    {moveChunk: ns, bounds: docChunkBounds[0], to: toShard.shardName, _waitForDelete: true}));

// Check that only the oplog entries for docs[0] are copied onto the recipient shard.
let oplogQuery = {"op": "i", "o.x": docs[0].x};
let txnQuery = {ns: ns, txnNumber: cmd.txnNumber};
let donorOplogEntry = findOplogEntry(fromShard, Object.assign({}, txnQuery, oplogQuery));
let recipientOplogEntry = findOplogEntry(toShard, txnQuery);
assert.eq(recipientOplogEntry.op, "n");
assert.eq(0, bsonWoCompare(recipientOplogEntry.o2.o, donorOplogEntry.o));

// Check that docs are on the right shards.
assert.eq(1, fromShard.getCollection(ns).find(docs[1]).count());
assert.eq(1, toShard.getCollection(ns).find(docs[0]).count());

st.stop();
})();
