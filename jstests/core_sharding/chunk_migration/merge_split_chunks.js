/**
 * Tests that merge, split and move chunks via mongos works/doesn't work with different chunk
 * configurations
 *
 * @tags: [
 *  requires_getmore,
 *  assumes_balancer_off,
 *  does_not_support_stepdowns,
 * ]
 */

import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

const dbName = db.getName();
const admin = db.getSiblingDB("admin");
const config = db.getSiblingDB("config");
const collName = jsTestName();
const ns = dbName + '.' + collName;
const coll = db.getCollection(collName);

const shardNames = db.adminCommand({listShards: 1}).shards.map(shard => shard._id);

if (shardNames.length < 2) {
    print(jsTestName() + " will not run; at least 2 shards are required.");
    quit();
}

print(jsTestName() + " is running on " + shardNames.length + " shards.");

assert.commandWorked(admin.runCommand({enableSharding: dbName}));
assert.commandWorked(admin.runCommand({shardCollection: ns, key: {_id: 1}}));

// Make sure split is correctly disabled for unsharded collection
jsTest.log("Trying to split an unsharded collection ...");
const collNameUnsplittable = collName + "_unsplittable";
const nsUnsplittable = dbName + '.' + collNameUnsplittable;
assert.commandWorked(db.runCommand({create: collNameUnsplittable}));
assert.commandFailedWithCode(admin.runCommand({split: nsUnsplittable, middle: {_id: 0}}),
                             ErrorCodes.NamespaceNotSharded);
jsTest.log("Trying to merge an unsharded collection ...");
assert.commandFailedWithCode(
    admin.runCommand({mergeChunks: nsUnsplittable, bounds: [{_id: 90}, {_id: MaxKey}]}),
    ErrorCodes.NamespaceNotSharded);
db.getCollection(collNameUnsplittable).drop();

// Create ranges MIN->0,0->10,(hole),20->40,40->50,50->90,(hole),100->110,110->MAX on first
// shard
jsTest.log("Creating ranges...");

assert.commandWorked(admin.runCommand({split: ns, middle: {_id: 0}}));
assert.commandWorked(admin.runCommand({split: ns, middle: {_id: 10}}));
assert.commandWorked(admin.runCommand({split: ns, middle: {_id: 20}}));
assert.commandWorked(admin.runCommand({split: ns, middle: {_id: 40}}));
assert.commandWorked(admin.runCommand({split: ns, middle: {_id: 50}}));
assert.commandWorked(admin.runCommand({split: ns, middle: {_id: 90}}));
assert.commandWorked(admin.runCommand({split: ns, middle: {_id: 100}}));
assert.commandWorked(admin.runCommand({split: ns, middle: {_id: 110}}));

assert.commandWorked(admin.runCommand({moveChunk: ns, find: {_id: 10}, to: shardNames[1]}));
assert.commandWorked(admin.runCommand({moveChunk: ns, find: {_id: 90}, to: shardNames[1]}));

// Insert some data into each of the consolidated ranges
let numDocs = 0;
const bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i <= 120; i++) {
    bulk.insert({_id: i});
    numDocs++;
}
assert.commandWorked(bulk.execute({w: "majority"}));

// S0: min->0, 0->10, 20->40, 40->50, 50->90, 100->110, 110->max
// S1: 10->20, 90->100
assert.eq(9, findChunksUtil.findChunksByNs(config, ns).itcount());

jsTest.log("Trying merges that should succeed...");

// Make sure merge including the MinKey works
assert.commandWorked(admin.runCommand({mergeChunks: ns, bounds: [{_id: MinKey}, {_id: 10}]}));
assert.eq(8, findChunksUtil.findChunksByNs(config, ns).itcount());
// S0: min->10, 20->40, 40->50, 50->90, 100->110, 110->max
// S1: 10->20, 90->100

// Make sure merging three chunks in the middle works
assert.commandWorked(admin.runCommand({mergeChunks: ns, bounds: [{_id: 20}, {_id: 90}]}));
assert.eq(6, findChunksUtil.findChunksByNs(config, ns).itcount());
assert.eq(numDocs, coll.find().itcount());
// S0: min->10, 20->90, 100->110, 110->max
// S1: 10->20, 90->100

// Make sure splitting chunks after merging works
assert.commandWorked(admin.runCommand({split: ns, middle: {_id: 55}}));
assert.eq(7, findChunksUtil.findChunksByNs(config, ns).itcount());
assert.eq(numDocs, coll.find().itcount());
// S0: min->10, 20->55, 55->90, 100->110, 110->max
// S1: 10->20, 90->100

// make sure moving the new chunk works
assert.commandWorked(admin.runCommand({moveChunk: ns, find: {_id: 20}, to: shardNames[1]}));
assert.commandWorked(admin.runCommand({moveChunk: ns, find: {_id: 55}, to: shardNames[1]}));
assert.eq(7, findChunksUtil.findChunksByNs(config, ns).itcount());
assert.eq(numDocs, coll.find().itcount());
// S0: min->10, 100->110, 110->max
// S1: 10->20, 20->55, 55->90, 90->100

// Make sure merge including the MaxKey works
assert.commandWorked(admin.runCommand({mergeChunks: ns, bounds: [{_id: 100}, {_id: MaxKey}]}));
assert.eq(6, findChunksUtil.findChunksByNs(config, ns).itcount());
// S0: min->10, 100->max
// S1: 10->20, 20->55, 55->90, 90->100

// Make sure merging chunks after a chunk has been moved out of a shard succeeds
assert.commandWorked(admin.runCommand({moveChunk: ns, find: {_id: 110}, to: shardNames[1]}));
assert.commandWorked(admin.runCommand({moveChunk: ns, find: {_id: 10}, to: shardNames[0]}));
assert.eq(numDocs, coll.find().itcount());
assert.eq(6, findChunksUtil.findChunksByNs(config, ns).itcount());
// S0: min->10, 10->20
// S1: 20->55, 55->90, 90->100, 100->max

assert.commandWorked(admin.runCommand({mergeChunks: ns, bounds: [{_id: 90}, {_id: MaxKey}]}));
assert.commandWorked(admin.runCommand({mergeChunks: ns, bounds: [{_id: 20}, {_id: 90}]}));
assert.eq(numDocs, coll.find().itcount());
// S0: min->10, 10->20
// S1: 20->90, 90->max

assert.commandWorked(admin.runCommand({split: ns, middle: {_id: 15}}));
assert.commandWorked(admin.runCommand({split: ns, middle: {_id: 30}}));
assert.commandWorked(admin.runCommand({moveChunk: ns, find: {_id: 30}, to: shardNames[0]}));
assert.eq(numDocs, coll.find().itcount());
// S0: min->10, 10->15, 15->20, 30->90
// S1: 20->30, 90->max

// range has a hole on shard 0
assert.commandFailed(admin.runCommand({mergeChunks: ns, bounds: [{_id: MinKey}, {_id: 90}]}));

assert.eq(6, findChunksUtil.findChunksByNs(config, ns).itcount());

coll.drop();
