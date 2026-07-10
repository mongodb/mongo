/**
 * Tests that the 'allowPartialResults' option to find and aggregate is respected: when a shard is
 * unreachable, the command returns partial results (with 'partialResultsReturned' set) if the option
 * is enabled, and fails otherwise.
 *
 * @tags: [
 *   # The 'allowPartialResults' option was added to the aggregate command in 9.0. In a
 *   # mixed-version cluster, an older binary that parses the command rejects it as an unknown
 *   # field, so restrict the test to clusters where every binary understands the option.
 *   requires_fcv_90,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

// This test shuts down a shard.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckingIndexesConsistentAcrossCluster = true;
TestData.skipCheckShardFilteringMetadata = true;

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

const st = new ShardingTest({shards: 2});

jsTest.log.info("Create a sharded collection with one chunk on each of the two shards.");
assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard1.shardName}),
);
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard0.shardName}));

jsTest.log.info("Insert some data.");
const nDocs = 100;
const coll = st.s0.getDB(dbName)[collName];
let bulk = coll.initializeUnorderedBulkOp();
for (let i = -50; i < 50; i++) {
    bulk.insert({_id: i});
}
assert.commandWorked(bulk.execute());

let findRes;
let aggRes;

jsTest.log.info("Without 'allowPartialResults', if all shards are up, find returns all docs.");
findRes = coll.runCommand({find: collName});
assert.commandWorked(findRes);
assert.eq(nDocs, findRes.cursor.firstBatch.length);
assert.eq(undefined, findRes.cursor.partialResultsReturned);

jsTest.log.info("With 'allowPartialResults: false', if all shards are up, find returns all docs.");
findRes = coll.runCommand({find: collName, allowPartialResults: false});
assert.commandWorked(findRes);
assert.eq(nDocs, findRes.cursor.firstBatch.length);
assert.eq(undefined, findRes.cursor.partialResultsReturned);

// Find with batch size less than the number of documents on each shard so getMore can be run.
let nRemainingDocs = nDocs;
const batchSize = 10;

jsTest.log.info("With 'allowPartialResults: true', if all shards are up, find returns all docs.");
findRes = coll.runCommand({find: collName, allowPartialResults: true, batchSize: batchSize});
assert.commandWorked(findRes);
assert.eq(batchSize, findRes.cursor.firstBatch.length);
assert.eq(undefined, findRes.cursor.partialResultsReturned);
nRemainingDocs -= batchSize;

jsTest.log.info(
    "With 'allowPartialResults: true', if all shards are up, aggregate returns all docs.",
);
aggRes = coll.runCommand({
    aggregate: collName,
    pipeline: [{$project: {_id: 1}}],
    cursor: {},
    allowPartialResults: true,
});
assert.commandWorked(aggRes);
assert.eq(nDocs, aggRes.cursor.firstBatch.length);
assert.eq(undefined, aggRes.cursor.partialResultsReturned);

jsTest.log.info(
    "With 'allowPartialResults: true' and the merge forced onto a shard (via " +
        "internalQueryProhibitMergingOnMongoS), the flag flows through the $mergeCursors pipeline " +
        "run by a mongod and the aggregate still returns all docs when all shards are up.",
);
assert.commandWorked(
    st.s.adminCommand({setParameter: 1, internalQueryProhibitMergingOnMongoS: true}),
);
aggRes = coll.runCommand({
    aggregate: collName,
    // A $sort forces a merging pipeline; with merging prohibited on mongos it runs on a shard.
    pipeline: [{$sort: {_id: 1}}],
    cursor: {},
    allowPartialResults: true,
});
assert.commandWorked(aggRes);
assert.eq(nDocs, aggRes.cursor.firstBatch.length);
assert.eq(undefined, aggRes.cursor.partialResultsReturned);
assert.commandWorked(
    st.s.adminCommand({setParameter: 1, internalQueryProhibitMergingOnMongoS: false}),
);

jsTest.log.info("Stopping " + st.shard1.shardName);
st.rs1.stopSet();
nRemainingDocs -= nDocs / 2 - batchSize;

// Do getMore with the returned cursor.
jsTest.log.info(
    "When no getMores are issued to the unreachable shard because mongos has loaded 'batchSize' " +
        "docs from each shard in the initial find, getMore does not return partialResultsReturned.",
);
let getMoreRes = coll.runCommand({
    getMore: findRes.cursor.id,
    collection: collName,
    batchSize: batchSize,
});
assert.commandWorked(getMoreRes);
assert.eq(batchSize, getMoreRes.cursor.nextBatch.length);
assert.eq(undefined, getMoreRes.cursor.partialResultsReturned);
nRemainingDocs -= batchSize;

jsTest.log.info(
    "When getMores are issued to the unreachable shard, getMore returns partialResultsReturned: 1",
);
// Use batch size of nRemainingDocs + 1 so that the getMore will wait for the scheduled getMores to
// all the shards.
getMoreRes = coll.runCommand({
    getMore: findRes.cursor.id,
    collection: collName,
    batchSize: nRemainingDocs + 1,
});
assert.commandWorked(getMoreRes);
assert.eq(nRemainingDocs, getMoreRes.cursor.nextBatch.length);
assert.eq(true, getMoreRes.cursor.partialResultsReturned);

jsTest.log.info("Without 'allowPartialResults', if some shards are down, find fails.");
assert.commandFailed(coll.runCommand({find: collName}));

jsTest.log.info("With 'allowPartialResults: false', if some shards are down, find fails.");
assert.commandFailed(coll.runCommand({find: collName, allowPartialResults: false}));

nRemainingDocs = nDocs / 2;

jsTest.log.info(
    "With 'allowPartialResults: true', if some shards are down, find returns partial results",
);
findRes = coll.runCommand({find: collName, allowPartialResults: true, batchSize: batchSize});
assert.commandWorked(findRes);
assert.eq(batchSize, findRes.cursor.firstBatch.length);
assert.eq(true, findRes.cursor.partialResultsReturned);
nRemainingDocs -= batchSize;

jsTest.log.info(
    "getMore after a find that returns partial results returns partialResultsReturned: true",
);
getMoreRes = coll.runCommand({
    getMore: findRes.cursor.id,
    collection: collName,
    batchSize: batchSize,
});
assert.commandWorked(getMoreRes);
assert.eq(batchSize, getMoreRes.cursor.nextBatch.length);
assert.eq(true, getMoreRes.cursor.partialResultsReturned);
nRemainingDocs -= batchSize;

jsTest.log.info(
    "Subsequent getMores should return partialResultsReturned: true regardless of the batch size.",
);
getMoreRes = coll.runCommand({getMore: findRes.cursor.id, collection: collName});
assert.commandWorked(getMoreRes);
assert.eq(nRemainingDocs, getMoreRes.cursor.nextBatch.length);
assert.eq(true, getMoreRes.cursor.partialResultsReturned);

jsTest.log.info("Without 'allowPartialResults', if some shards are down, aggregate fails.");
assert.commandFailed(
    coll.runCommand({aggregate: collName, pipeline: [{$project: {_id: 1}}], cursor: {}}),
);

jsTest.log.info("With 'allowPartialResults: false', if some shards are down, aggregate fails.");
assert.commandFailed(
    coll.runCommand({
        aggregate: collName,
        pipeline: [{$project: {_id: 1}}],
        cursor: {},
        allowPartialResults: false,
    }),
);

nRemainingDocs = nDocs / 2;

jsTest.log.info(
    "With 'allowPartialResults: true', if some shards are down, aggregate returns partial results.",
);
aggRes = coll.runCommand({
    aggregate: collName,
    pipeline: [{$project: {_id: 1}}],
    cursor: {batchSize: batchSize},
    allowPartialResults: true,
});
assert.commandWorked(aggRes);
assert.eq(batchSize, aggRes.cursor.firstBatch.length);
assert.eq(true, aggRes.cursor.partialResultsReturned);
nRemainingDocs -= batchSize;

jsTest.log.info(
    "getMore after an aggregate that returns partial results also reports partialResultsReturned: " +
        "true.",
);
let aggGetMoreRes = coll.runCommand({getMore: aggRes.cursor.id, collection: collName});
assert.commandWorked(aggGetMoreRes);
assert.eq(nRemainingDocs, aggGetMoreRes.cursor.nextBatch.length);
assert.eq(true, aggGetMoreRes.cursor.partialResultsReturned);

st.stop();
