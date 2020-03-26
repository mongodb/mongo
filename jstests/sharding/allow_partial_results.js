/**
 * Tests that the 'allowPartialResults' option to find is respected, and that aggregation does not
 * accept the 'allowPartialResults' option.
 */

// This test shuts down a shard.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckingIndexesConsistentAcrossCluster = true;

(function() {
"use strict";
const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

const st = new ShardingTest({shards: 2});

jsTest.log("Insert some data.");
const nDocs = 100;
const coll = st.s0.getDB(dbName)[collName];
let bulk = coll.initializeUnorderedBulkOp();
for (let i = -50; i < 50; i++) {
    bulk.insert({_id: i});
}
assert.commandWorked(bulk.execute());

jsTest.log("Create a sharded collection with one chunk on each of the two shards.");
st.ensurePrimaryShard(dbName, st.shard0.shardName);
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard1.shardName}));

let findRes;

jsTest.log("Without 'allowPartialResults', if all shards are up, find returns all docs.");
findRes = coll.runCommand({find: collName});
assert.commandWorked(findRes);
assert.eq(nDocs, findRes.cursor.firstBatch.length);
assert.eq(undefined, findRes.cursor.partialResultsReturned);

jsTest.log("With 'allowPartialResults: false', if all shards are up, find returns all docs.");
findRes = coll.runCommand({find: collName, allowPartialResults: false});
assert.commandWorked(findRes);
assert.eq(nDocs, findRes.cursor.firstBatch.length);
assert.eq(undefined, findRes.cursor.partialResultsReturned);

// Find with batch size less than the number of documents on each shard so getMore can be run.
let nRemainingDocs = nDocs;
const batchSize = 10;

jsTest.log("With 'allowPartialResults: true', if all shards are up, find returns all docs.");
findRes = coll.runCommand({find: collName, allowPartialResults: true, batchSize: batchSize});
assert.commandWorked(findRes);
assert.eq(batchSize, findRes.cursor.firstBatch.length);
assert.eq(undefined, findRes.cursor.partialResultsReturned);
nRemainingDocs -= batchSize;

jsTest.log("Stopping " + st.shard0.shardName);
st.rs0.stopSet();
nRemainingDocs -= nDocs / 2 - batchSize;

// Do getMore with the returned cursor.
jsTest.log(
    "When no getMores are issued to the unreachable shard because mongos has loaded 'batchSize' " +
    "docs from each shard in the initial find, getMore does not return partialResultsReturned.");
let getMoreRes =
    coll.runCommand({getMore: findRes.cursor.id, collection: collName, batchSize: batchSize});
assert.commandWorked(getMoreRes);
assert.eq(batchSize, getMoreRes.cursor.nextBatch.length);
assert.eq(undefined, getMoreRes.cursor.partialResultsReturned);
nRemainingDocs -= batchSize;

jsTest.log(
    "When getMores are issued to the unreachable shard, getMore returns partialResultsReturned: 1");
// Use batch size of nRemainingDocs + 1 so that the getMore will wait for the scheduled getMores to
// all the shards.
getMoreRes = coll.runCommand(
    {getMore: findRes.cursor.id, collection: collName, batchSize: nRemainingDocs + 1});
assert.commandWorked(getMoreRes);
assert.eq(nRemainingDocs, getMoreRes.cursor.nextBatch.length);
assert.eq(true, getMoreRes.cursor.partialResultsReturned);

jsTest.log("Without 'allowPartialResults', if some shards are down, find fails.");
assert.commandFailed(coll.runCommand({find: collName}));

jsTest.log("With 'allowPartialResults: false', if some shards are down, find fails.");
assert.commandFailed(coll.runCommand({find: collName, allowPartialResults: false}));

nRemainingDocs = nDocs / 2;

jsTest.log(
    "With 'allowPartialResults: true', if some shards are down, find returns partial results");
findRes = coll.runCommand({find: collName, allowPartialResults: true, batchSize: batchSize});
assert.commandWorked(findRes);
assert.eq(batchSize, findRes.cursor.firstBatch.length);
assert.eq(true, findRes.cursor.partialResultsReturned);
nRemainingDocs -= batchSize;

jsTest.log(
    "getMore after a find that returns partial results returns partialResultsReturned: true");
getMoreRes =
    coll.runCommand({getMore: findRes.cursor.id, collection: collName, batchSize: batchSize});
assert.commandWorked(getMoreRes);
assert.eq(batchSize, getMoreRes.cursor.nextBatch.length);
assert.eq(true, getMoreRes.cursor.partialResultsReturned);
nRemainingDocs -= batchSize;

jsTest.log(
    "Subsequent getMores should return partialResultsReturned: true regardless of the batch size.");
getMoreRes = coll.runCommand({getMore: findRes.cursor.id, collection: collName});
assert.commandWorked(getMoreRes);
assert.eq(nRemainingDocs, getMoreRes.cursor.nextBatch.length);
assert.eq(true, getMoreRes.cursor.partialResultsReturned);

jsTest.log("The allowPartialResults option does not currently apply to aggregation.");
assert.commandFailedWithCode(coll.runCommand({
    aggregate: collName,
    pipeline: [{$project: {_id: 1}}],
    cursor: {},
    allowPartialResults: true
}),
                             ErrorCodes.FailedToParse);

st.stop();
}());
