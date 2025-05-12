/*
 * Test that _getNextSessionMods filters out unrelated oplog entries.
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {chunkBoundsUtil} from "jstests/sharding/libs/chunk_bounds_util.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

/*
 * Returns the oplog entry on the shard that matches the query. Asserts
 * that there is only one such oplog entry.
 */
function findOplogEntry(shard, query) {
    let oplogDocs = shard.getDB('local').oplog.rs.find(query).toArray();
    assert.eq(1, oplogDocs.length);
    return oplogDocs[0];
}

/*
 * Returns the oplog operation, within an applyOps, on the shard that matches the query. Asserts
 * that there is only one oplog entry containing a matching operation.
 */
function findOplogOperationForBatchedVectoredInserts(shard, query) {
    let outerQuery = {op: 'c', txnNumber: query.txnNumber};
    delete query.txnNumber;
    query = Object.merge(outerQuery, {"o.applyOps": {$elemMatch: query}});
    let oplogDocs = shard.getDB('local').oplog.rs.find(query, {"o.applyOps.$": 1}).toArray();
    assert.eq(1, oplogDocs.length);
    assert.eq(1, oplogDocs[0].o.applyOps.length);
    return oplogDocs[0].o.applyOps[0];
}

/*
 * Moves chunks so that the documents in the test are on the same shard.
 */
function ensureChunkPlacement(mongosConn, nss, docPlacement, docChunks) {
    if (docPlacement[0].shardName == docPlacement[1].shardName) {
        // Chunks are placed correctly, nothing to do.
        return;
    }
    // Move chunk 1 to meet chunk 2
    assert.commandWorked(mongosConn.adminCommand(
        {moveChunk: nss, bounds: docChunks[0], to: docPlacement[1].shardName}));
}

let st = new ShardingTest({shards: 2});
let dbName = "test";
let collName = "user";
let ns = dbName + "." + collName;
let collName2 = "user2";
let ns2 = dbName + "." + collName2;
let configDB = st.s.getDB('config');
let testDB = st.s.getDB(dbName);

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard1.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 'hashed'}}));

// If the feature is enabled, we will test both the cases where the inserts are in separate
// oplog entries and where they are batched together.
const isBatchingVectoredInserts =
    FeatureFlagUtil.isPresentAndEnabled(testDB, "ReplicateVectoredInsertsTransactionally");

if (isBatchingVectoredInserts) {
    assert.commandWorked(st.s.adminCommand({shardCollection: ns2, key: {x: 'hashed'}}));
}

// TODO SERVER-81884: update once 8.0 becomes last LTS.
let oneChunkFeatureFlag = FeatureFlagUtil.isPresentAndEnabled(
    testDB, "OneChunkPerShardEmptyCollectionWithHashedShardKey");
if (oneChunkFeatureFlag) {
    // Docs are expected to go to the same shards but different chunks.
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: convertShardKeyToHashed(10)}}));
    if (isBatchingVectoredInserts) {
        assert.commandWorked(
            st.s.adminCommand({split: ns2, middle: {x: convertShardKeyToHashed(10)}}));
    }
}

let docs = [{x: -1000}, {x: 10}];

function getDocLocations(st, configDB, ns, docs) {
    let chunkDocs = findChunksUtil.findChunksByNs(configDB, ns).toArray();
    let shardChunkBounds = chunkBoundsUtil.findShardChunkBounds(chunkDocs);
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
    return {shards: shards, bounds: docChunkBounds};
}

let res = getDocLocations(st, configDB, ns, docs);
let shards = res.shards;
let docChunkBounds = res.bounds;

if (!oneChunkFeatureFlag) {
    // Since we now assign chunks in a round robin fashion, we may need to move chunks so that the
    // docs are on the same shard.
    ensureChunkPlacement(st.s, ns, shards, docChunkBounds);
    // Recalculate placement
    res = getDocLocations(st, configDB, ns, docs);
    shards = res.shards;
    docChunkBounds = res.bounds;
}
assert.eq(shards[0], shards[1]);
assert.neq(docChunkBounds[0], docChunkBounds[1]);

function testChunkMove(dbName, collName, findSourceOperation) {
    let ns = dbName + "." + collName;
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
    let donorOperation = findSourceOperation(fromShard, Object.assign({}, txnQuery, oplogQuery));
    let recipientOplogEntry = findOplogEntry(toShard, txnQuery);
    assert.eq(recipientOplogEntry.op, "n");
    assert.eq(0, bsonWoCompare(recipientOplogEntry.o2.o, donorOperation.o));

    // Check that docs are on the right shards.
    assert.eq(1, fromShard.getCollection(ns).find(docs[1]).count());
    assert.eq(1, toShard.getCollection(ns).find(docs[0]).count());
}

if (isBatchingVectoredInserts) {
    testChunkMove(dbName, collName, findOplogOperationForBatchedVectoredInserts);
    assert.commandWorked(shards[0].adminCommand({setParameter: 1, internalInsertMaxBatchSize: 1}));
    testChunkMove(dbName, collName2, findOplogEntry);
} else {
    testChunkMove(dbName, collName, findOplogEntry);
}

st.stop();
