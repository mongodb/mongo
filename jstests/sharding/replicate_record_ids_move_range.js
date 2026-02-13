/**
 * Tests that replicated RecordIds aren't preserved across moveRange.
 *
 * @tags: [
 *  featureFlagRecordIdsReplicated,
 *  assumes_balancer_off,
 *  # Replicated record IDs are incompatible with clustered collections.
 *  expects_explicit_underscore_id_index,
 * ]
 */

import {mapFieldToMatchingDocRid} from "jstests/libs/collection_write_path/replicated_record_ids_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

/**
 * Moves the chunk using moveRange either for hashed or range-based shard key.
 */
function moveRange(chunk, ns, fromShard, toShard) {
    // Confirm current location of the chunk.
    assert.eq(fromShard.shardName, configDB.chunks.findOne({_id: chunk._id}).shard);

    assert.commandWorked(
        mongos.adminCommand({moveRange: ns, min: chunk.min, max: chunk.max, toShard: toShard.shardName}),
    );

    // Ensure that the chunk was successfully moved from the original shard to the new location.
    const chunkAfterMove = configDB.chunks.findOne({_id: chunk._id});
    jsTest.log.info(`${ns}: Chunk after move to ${toShard.shardName}: ${tojson(chunkAfterMove)}`);
    assert.eq(toShard.shardName, chunkAfterMove.shard);
    assert.eq(0, configDB.chunks.count({_id: chunk._id, shard: fromShard.shardName}));
    assert.eq(1, configDB.chunks.count({_id: chunk._id, shard: toShard.shardName}));
}

function assertRecordIdsNEQ(recordIds1, recordIds2, docs) {
    assert.neq(recordIds1["Bing"], recordIds2["Bing"], `Unexpected value for record ID: Bing: ${tojson(docs)}`);
    assert.neq(recordIds1["Louie"], recordIds2["Louie"], `Unexpected value for record ID: Louie: ${tojson(docs)}`);
    assert.neq(
        recordIds2["Bing"],
        recordIds2["Louie"],
        `Documents on destination shard have conflicting record IDs: ${tojson(docs)}`,
    );
}

/**
 * Runs a single test case for moveRange.
 * Shards collection by index keyed on 'keyDoc' if provided. Otherwise, uses hashed sharding.
 */
function runMoveRangeReplicaRecordIDsTest(collName, keyDoc) {
    const isHashed = !keyDoc;

    const coll = testDB[collName];
    const ns = coll.getFullName();

    assert.commandWorked(coll.getDB().createCollection(coll.getName()));
    const keyPattern = isHashed ? {_id: "hashed"} : keyDoc;
    assert.commandWorked(coll.createIndex(keyPattern));

    // Remove some of the initial documents on the collection with replicated record IDs to create gaps in the record IDs.
    assert.commandWorked(
        coll.insert([
            {name: "Ale", b: 100}, // record ID: 1
            {name: "Benny", b: 200}, // record ID: 2
            {name: "Bing", b: 300}, // record ID: 3
            {name: "Louie", b: 400}, // record ID: 4
        ]),
    );
    jsTest.log.info(`All documents in ${ns} before deletion: ${tojson(coll.find().showRecordId().toArray())}`);
    assert.commandWorked(coll.remove({name: {$in: ["Ale", "Benny"]}}));
    const docs = coll.find().showRecordId().toArray();
    assert.eq(
        2,
        docs.length,
        `Unexpected content in unsharded collection with replicated record IDs ${ns}: ${tojson(docs)}`,
    );

    // Generate map of name to record ID to check collection after moveCollection.
    const collRecordIdsBeforeMoveRange = mapFieldToMatchingDocRid(docs, "name");
    for (const [name, recordId] of Object.entries(collRecordIdsBeforeMoveRange)) {
        assert(
            name === "Bing" || name === "Louie",
            `Unexpected document (${name}, ${
                recordId
            }) in unsharded collection with replicated record IDs ${ns}: ${tojson(docs)}`,
        );
        // We previously removed documents with record IDs 1 and 2.
        assert.between(
            3,
            recordId,
            4,
            `Unexpected value for record ID in (${name}, ${recordId}): ${tojson(docs)}`,
            /*inclusive=*/ true,
        );
    }

    // The index backing the shard key 'keyPattern' was created at the beginning of this test function immediately after creating the collection.
    // Depending on 'isHashed', the shard key 'keyPattern' can be either hashed or range-based.
    assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: keyPattern}));

    // First move: shard0 to shard1.

    // Ensure that all the documents are on a single chunk initially.
    assert.eq(1, findChunksUtil.countChunksForNs(configDB, ns));
    const initialChunk = findChunksUtil.findOneChunkByNs(configDB, ns, {shard: shard0.shardName});
    jsTest.log.info(`${ns}: Initial chunk on ${shard0.shardName}: ${tojson(initialChunk)}`);

    moveRange(initialChunk, ns, shard0, shard1);

    // Since the collection was moved, the record IDs should have been rewritten to start from 1.
    const docsAfterMoveRange = shard1.getCollection(ns).find().showRecordId().toArray();
    const collRecordIdsAfterMoveRange = mapFieldToMatchingDocRid(docsAfterMoveRange, "name");
    assertRecordIdsNEQ(collRecordIdsBeforeMoveRange, collRecordIdsAfterMoveRange, docsAfterMoveRange);

    // Ensure collection option 'recordIdsReplicated' is preserved on destination shard.
    const collInfoShard1 = shard1.getCollection(ns).exists();
    assert(collInfoShard1.options.recordIdsReplicated, tojson(collInfoShard1));

    // Second move: shard1 to shard0.

    // Ensure that all the documents are on a single chunk on shard1.
    assert.eq(1, findChunksUtil.countChunksForNs(configDB, ns));
    const movedChunk = findChunksUtil.findOneChunkByNs(configDB, ns, {shard: shard1.shardName});
    jsTest.log.info(`${ns}: Moved chunk on ${shard1.shardName}: ${tojson(movedChunk)}`);

    moveRange(movedChunk, ns, shard1, shard0);

    // Even though the collection was moved back, the record IDs should have been rewritten.
    const docsMovedBack = shard0.getCollection(ns).find().showRecordId().toArray();
    const collRecordIdsMovedBack = mapFieldToMatchingDocRid(docsMovedBack, "name");
    assertRecordIdsNEQ(collRecordIdsBeforeMoveRange, collRecordIdsMovedBack, docsMovedBack);

    // Ensure collection option 'recordIdsReplicated' is still present.
    const collInfoShard0 = shard0.getCollection(ns).exists();
    assert(collInfoShard0.options.recordIdsReplicated, tojson(collInfoShard0));
}

const st = new ShardingTest({mongos: 1, shards: 2});
const dbName = jsTestName();

const mongos = st.s0;
const shard0 = st.shard0;
const shard1 = st.shard1;
const configDB = mongos.getDB("config");
const testDB = mongos.getDB(dbName);

assert.commandWorked(mongos.adminCommand({enableSharding: dbName, primaryShard: shard0.shardName}));

runMoveRangeReplicaRecordIDsTest(jsTestName() + "_hashed", /*keyDoc=*/ undefined);

runMoveRangeReplicaRecordIDsTest(jsTestName() + "_name", {name: 1});

runMoveRangeReplicaRecordIDsTest(jsTestName() + "_name_b", {name: 1, b: 1});

st.stop();
