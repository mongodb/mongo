/**
 * Tests for the moveCollection/movePrimary feature on a collection with replicated record IDs.
 *
 * @tags: [
 *   featureFlagRecordIdsReplicated,
 *   # Replicated record IDs are incompatible with clustered collections.
 *   expects_explicit_underscore_id_index,
 * ]
 */

import {getRidForDoc, mapFieldToMatchingDocRid} from "jstests/libs/replicated_record_ids_utils.js";
import {
    moveDatabaseAndUnshardedColls
} from "jstests/sharding/libs/move_database_and_unsharded_coll_helper.js";

const st = new ShardingTest({mongos: 1, shards: 2});

const dbName = 'test';
const collName = jsTestName();
const collNS = dbName + '.' + collName;
let mongos = st.s0;
let shard0 = st.shard0;
let shard1 = st.shard1;

const testDB = mongos.getDB(dbName);
const coll = testDB[collName];

assert.commandWorked(testDB.adminCommand({enableSharding: dbName, primaryShard: shard0.shardName}));
assert.commandWorked(testDB.createCollection(collName, {recordIdsReplicated: true}));

let collInfo = coll.exists();
assert(collInfo.options.recordIdsReplicated, tojson(collInfo));

// Remove some of the initial documents on the collection with replicated record
// IDs to create gaps in the record IDs.
assert.commandWorked(coll.insert([
    {name: 'Alice'},  // record ID: 1
    {name: 'Bob'},    // record ID: 2
    {name: 'Bart'},   // record ID: 3
    {name: 'Lisa'},   // record ID: 4
]));
assert.commandWorked(coll.remove({name: {$in: ['Alice', 'Bob']}}));
let docs = coll.find().showRecordId().toArray();
assert.eq(2,
          docs.length,
          `Unexpected content in unsharded collection with replicated record IDs ${
              coll.getFullName()}: ${tojson(docs)}`);

// Generate map of name to record ID to check collection after moveCollection.
const collRecordIdsBeforeMoveCollection = mapFieldToMatchingDocRid(docs, 'name');
for (const [name, recordId] of Object.entries(collRecordIdsBeforeMoveCollection)) {
    assert(name === 'Bart' || name === 'Lisa',
           `Unexpected document (${name}, ${
               recordId}) in unsharded collection with replicated record IDs ${
               coll.getFullName()}: ${tojson(docs)}`);
    // We previously removed documents with record IDs 1 and 2.
    assert.between(3,
                   recordId,
                   4,
                   `Unexpected value for record ID in (${name}, ${recordId}): ${tojson(docs)}`,
                   /*inclusive=*/ true);
}

assert.eq(2, shard0.getCollection(collNS).countDocuments({}));
assert.eq(0, shard1.getCollection(collNS).countDocuments({}));

// Move to non-primary shard.
moveDatabaseAndUnshardedColls(testDB, shard1.shardName);

// Since the collection was moved, the record IDs should have been rewritten to start from 1.
assert.eq(0, shard0.getCollection(collNS).countDocuments({}));
assert.eq(2, shard1.getCollection(collNS).countDocuments({}));

// Check record IDs on shard.
assert.neq(collRecordIdsBeforeMoveCollection['Bart'],
           getRidForDoc(shard1.getDB(dbName), collName, {name: 'Bart'}),
           `Unexpected value for record ID: Bart: ${
               tojson(shard1.getCollection(collNS).find().showRecordId().toArray())}`);
assert.neq(collRecordIdsBeforeMoveCollection['Lisa'],
           getRidForDoc(shard1.getDB(dbName), collName, {name: 'Lisa'}),
           `Unexpected value for record ID: Lisa: ${
               tojson(shard1.getCollection(collNS).find().showRecordId().toArray())}`);

// Check record IDs through mongos.
assert.eq(getRidForDoc(shard1.getDB(dbName), collName, {name: 'Bart'}),
          getRidForDoc(mongos.getDB(dbName), collName, {name: 'Bart'}),
          `Unexpected value for record ID: Bart: ${
              tojson(shard1.getCollection(collNS).find().showRecordId().toArray())}`);
assert.eq(getRidForDoc(shard1.getDB(dbName), collName, {name: 'Lisa'}),
          getRidForDoc(mongos.getDB(dbName), collName, {name: 'Lisa'}),
          `Unexpected value for record ID: Lisa: ${
              tojson(shard1.getCollection(collNS).find().showRecordId().toArray())}`);

// Ensure that 'recordIdsReplicated` collection option is still present and set to true.
// Check collection options on shard.
collInfo = shard1.getCollection(collNS).exists();
assert(collInfo.options.recordIdsReplicated, tojson(collInfo));

// Check collection options through mongos.
collInfo = mongos.getCollection(collNS).exists();
assert(collInfo.options.recordIdsReplicated, tojson(collInfo));

st.stop();
