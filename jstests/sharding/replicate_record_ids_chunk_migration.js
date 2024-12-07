/**
 * Tests that replicated RecordIds aren't preserved across moveChunk.
 *
 * @tags: [
 *  featureFlagRecordIdsReplicated,
 *  assumes_balancer_off,
 *  # Replicated record IDs are incompatible with clustered collections.
 *  expects_explicit_underscore_id_index,
 * ]
 */

import {getRidForDoc, mapFieldToMatchingDocRid} from "jstests/libs/replicated_record_ids_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    findChunksUtil,
} from "jstests/sharding/libs/find_chunks_util.js";

/**
 * Moves a single chunk from 'shard0' to 'shard1' either by specifying bounds or using find
 * query.
 */
function moveSingleChunk(chunk, ns, keyDoc, useBounds) {
    // Confirm current location of chunk.
    assert.eq(shard0.shardName, configDB.chunks.findOne({_id: chunk._id}).shard);

    // Move singleton chunk using specified method.
    if (useBounds) {
        assert.commandWorked(mongos.adminCommand(
            {moveChunk: ns, bounds: [chunk.min, chunk.max], to: shard1.shardName}));
    } else {
        // Select chunk using an equality match on the shard key using the 'find' option.
        const isHashed = !keyDoc;
        assert(!isHashed);
        assert.commandWorked(
            mongos.adminCommand({moveChunk: ns, find: keyDoc, to: shard1.shardName}));
    }

    // Ensure that the chunk was successfully moved from the original shard 'shard0' to
    // the new location at shard 'shard1'.
    const chunkAfterMove = configDB.chunks.findOne({_id: chunk._id});
    jsTestLog(`${ns}: Chunk after move to ${shard1.shardName}: ${tojson(chunkAfterMove)}`);
    assert.eq(shard1.shardName, chunkAfterMove.shard);
    assert.eq(0, configDB.chunks.count({_id: chunk._id, shard: shard0.shardName}));
    assert.eq(1, configDB.chunks.count({_id: chunk._id, shard: shard1.shardName}));
}

/**
 * Runs a single test case for moveChunk.
 * Shards collection by index keyed on 'keyDoc' if provided. Otherwise, uses hashed sharding.
 * If 'useBounds' is true, moveChunk locates the chunk using 'bounds' rather than by equality.
 * If 'splitChunk' is true, we will split the chunk at the origin shard in two before moving
 * the chunks to the destination shard one by one. Also, enabling this option implies 'useBounds'.
 */
function runMoveChunkReplicaRecordIDsTest(collName, keyDoc, useBounds, splitChunk) {
    const isHashed = !keyDoc;

    const coll = testDB[collName];
    const ns = coll.getFullName();

    assert.commandWorked(
        coll.getDB().createCollection(coll.getName(), {recordIdsReplicated: true}));
    const keyPattern = isHashed ? {_id: 'hashed'} : keyDoc;
    assert.commandWorked(coll.createIndex(keyPattern));

    // Remove some of the initial documents on the collection with replicated record
    // IDs to create gaps in the record IDs.
    assert.commandWorked(coll.insert([
        {name: 'Alice', b: 100},  // record ID: 1
        {name: 'Bob', b: 200},    // record ID: 2
        {name: 'Bart', b: 300},   // record ID: 3
        {name: 'Lisa', b: 400},   // record ID: 4
    ]));
    jsTestLog(
        `All documents in ${ns} before deletion: ${tojson(coll.find().showRecordId().toArray())}`);
    jsTestLog(`Number of chunks for ${ns}: ${findChunksUtil.countChunksForNs(configDB, ns)}`);
    assert.commandWorked(coll.remove({name: {$in: ['Alice', 'Bob']}}));
    const docs = coll.find().showRecordId().toArray();
    assert.eq(2,
              docs.length,
              `Unexpected content in unsharded collection with replicated record IDs ${ns}: ${
                  tojson(docs)}`);

    // Generate map of name to record ID to check collection after moveCollection.
    const collRecordIdsBeforeMoveChunk = mapFieldToMatchingDocRid(docs, 'name');
    for (const [name, recordId] of Object.entries(collRecordIdsBeforeMoveChunk)) {
        assert(name === 'Bart' || name === 'Lisa',
               `Unexpected document (${name}, ${
                   recordId}) in unsharded collection with replicated record IDs ${ns}: ${
                   tojson(docs)}`);
        // We previously removed documents with record IDs 1 and 2.
        assert.between(3,
                       recordId,
                       4,
                       `Unexpected value for record ID in (${name}, ${recordId}): ${tojson(docs)}`,
                       /*inclusive=*/ true);
    }

    // The index backing the shard key 'keyPattern' was created at the beginning of this
    // test function immediately after creating the collection.
    // Depending on 'isHashed', the shard key 'keyPattern' can be either hashed or range-based.
    assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: keyPattern}));

    // Ensure that all the documents are on a single chunk initially.
    assert.eq(1, findChunksUtil.countChunksForNs(configDB, ns));
    const initialChunk = findChunksUtil.findOneChunkByNs(configDB, ns, {shard: shard0.shardName});
    jsTestLog(`${ns}: Initial chunk on ${shard0.shardName}: ${tojson(initialChunk)}`);

    if (splitChunk) {
        assert(useBounds);

        assert.commandWorked(
            mongos.adminCommand({split: ns, bounds: [initialChunk.min, initialChunk.max]}));
        assert.eq(2, findChunksUtil.countChunksForNs(configDB, ns));
        const chunks =
            findChunksUtil.findChunksByNs(configDB, ns, {shard: shard0.shardName}).toArray();
        jsTestLog(`${ns}: Chunks after split on ${shard0.shardName}: ${tojson(chunks)}`);

        chunks.forEach((chunk) => {
            moveSingleChunk(chunk, ns, keyDoc, useBounds);
        });
    } else {
        moveSingleChunk(initialChunk, ns, keyDoc, useBounds);
    }

    // Since the collection was moved, the record IDs should have been rewritten to start from 1.
    const docsAfterMoveChunk = shard1.getCollection(ns).find().showRecordId().toArray();
    const collRecordIdsAfterMoveChunk = mapFieldToMatchingDocRid(docsAfterMoveChunk, 'name');
    assert.neq(collRecordIdsBeforeMoveChunk['Bart'],
               collRecordIdsAfterMoveChunk['Bart'],
               `Unexpected value for record ID: Bart: ${tojson(docsAfterMoveChunk)}`);
    const lisaRid = getRidForDoc(shard1.getDB(dbName), collName, {name: 'Lisa'});
    assert.neq(collRecordIdsBeforeMoveChunk['Lisa'],
               collRecordIdsAfterMoveChunk['Lisa'],
               `Unexpected value for record ID: Lisa: ${tojson(docsAfterMoveChunk)}`);
    assert.neq(collRecordIdsAfterMoveChunk['Bart'],
               collRecordIdsAfterMoveChunk['Lisa'],
               `Documents on destination shard have conflicting record IDs: ${
                   tojson(docsAfterMoveChunk)}`);

    // Ensure collection option 'recordIdsReplicated' is preserved on destination shard.
    const collInfo = shard1.getCollection(ns).exists();
    assert(collInfo.options.recordIdsReplicated, tojson(collInfo));
}

const st = new ShardingTest({mongos: 1, shards: 2});
const dbName = 'test';

const mongos = st.s0;
const shard0 = st.shard0;
const shard1 = st.shard1;
const configDB = mongos.getDB('config');
const testDB = mongos.getDB(dbName);

assert.commandWorked(mongos.adminCommand({enableSharding: dbName, primaryShard: shard0.shardName}));

runMoveChunkReplicaRecordIDsTest(jsTestName() + '_hashed_bounds',
                                 /*keyDoc=*/ undefined,
                                 /*useBounds=*/ true,
                                 /*splitChunk=*/ false);

runMoveChunkReplicaRecordIDsTest(jsTestName() + '_hashed_split',
                                 /*keyDoc=*/ undefined,
                                 /*useBounds=*/ true,
                                 /*splitChunk=*/ true);

runMoveChunkReplicaRecordIDsTest(jsTestName() + '_not_hashed_name_bounds',
                                 {name: 1},
                                 /*useBounds=*/ true,
                                 /*splitChunk=*/ false);

runMoveChunkReplicaRecordIDsTest(jsTestName() + '_not_hashed_name_find',
                                 {name: 1},
                                 /*useBounds=*/ false,
                                 /*splitChunk=*/ false);

runMoveChunkReplicaRecordIDsTest(jsTestName() + '_not_hashed_name_split',
                                 {name: 1},
                                 /*useBounds=*/ true,
                                 /*splitChunk=*/ true);

runMoveChunkReplicaRecordIDsTest(jsTestName() + '_not_hashed_name_b_bounds',
                                 {name: 1, b: 1},
                                 /*useBounds=*/ true,
                                 /*splitChunk=*/ false);

runMoveChunkReplicaRecordIDsTest(jsTestName() + '_not_hashed_name_b_find',
                                 {name: 1, b: 1},
                                 /*useBounds=*/ false,
                                 /*splitChunk=*/ false);

runMoveChunkReplicaRecordIDsTest(jsTestName() + '_not_hashed_name_b_split',
                                 {name: 1, b: 1},
                                 /*useBounds=*/ true,
                                 /*splitChunk=*/ true);

st.stop();
