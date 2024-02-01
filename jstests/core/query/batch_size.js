// @tags: [
//   assumes_balancer_off,
//   requires_getmore
// ]

// Test subtleties of batchSize and limit.

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const coll = db.jstests_batch_size;
coll.drop();

let docsToInsert = [];
for (let i = 0; i < 4; i++) {
    docsToInsert.push({_id: i, a: i});
}
assert.commandWorked(coll.insert(docsToInsert));

function runIndexedTests() {
    // With limit, indexed.
    assert.eq(2, coll.find().limit(2).itcount());
    assert.eq(2, coll.find().sort({a: 1}).limit(2).itcount());

    // With batchSize, indexed.
    // SERVER-12438: If there is an index that provides the sort, then a plan with an unindexed
    // sort should never be used.  Consequently, batchSize will NOT be a hard limit in this
    // case.  WARNING: the behavior described above may change in the future.
    assert.eq(4, coll.find().batchSize(2).itcount());
    assert.eq(4, coll.find().sort({a: 1}).batchSize(2).itcount());
}

// Without batch size or limit, unindexed.
assert.eq(4, coll.find().itcount());
assert.eq(4, coll.find().sort({a: 1}).itcount());

// With limit, unindexed.
assert.eq(2, coll.find().limit(2).itcount());
assert.eq(2, coll.find().sort({a: 1}).limit(2).itcount());

assert.eq(4, coll.find().batchSize(2).itcount());
assert.eq(4, coll.find().sort({a: 1}).batchSize(2).itcount());

// With negative batchSize. A negative batchSize value instructs the server
// to return just a single batch of results.
assert.eq(1, coll.find().batchSize(-1).itcount());
assert.eq(2, coll.find().batchSize(-2).itcount());

// Run the tests with the index twice in order to double check plan caching.
assert.commandWorked(coll.createIndex({a: 1}));
for (let i = 0; i < 2; i++) {
    runIndexedTests();
}

// The next tests make sure that we obey limit and batchSize properly when the sort could be
// either indexed or unindexed.
assert(coll.drop());
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));

docsToInsert = [];
for (let i = 0; i < 100; i++) {
    docsToInsert.push({_id: i, a: i, b: 1});
}
assert.commandWorked(coll.insert(docsToInsert));

// Without a hint. Do it twice to make sure caching is ok.
for (let i = 0; i < 2; i++) {
    assert.eq(15, coll.find({a: {$gte: 85}}).sort({b: 1}).batchSize(2).itcount());
    assert.eq(6, coll.find({a: {$gte: 85}}).sort({b: 1}).limit(6).itcount());
}

// Hinting 'a'.
assert.eq(15, coll.find({a: {$gte: 85}}).sort({b: 1}).hint({a: 1}).batchSize(2).itcount());
assert.eq(6, coll.find({a: {$gte: 85}}).sort({b: 1}).hint({a: 1}).limit(6).itcount());

// Hinting 'b'.
assert.eq(15, coll.find({a: {$gte: 85}}).sort({b: 1}).hint({b: 1}).batchSize(2).itcount());
assert.eq(6, coll.find({a: {$gte: 85}}).sort({b: 1}).hint({b: 1}).limit(6).itcount());

// With explain.
let explain = coll.find({a: {$gte: 85}}).sort({b: 1}).batchSize(2).explain("executionStats");
assert.eq(15, explain.executionStats.nReturned);
explain = coll.find({a: {$gte: 85}}).sort({b: 1}).limit(6).explain("executionStats");
if (FixtureHelpers.isMongos(db)) {
    // If we're talking to a mongos, we expect at most one batch from each shard.
    assert.gte(FixtureHelpers.numberOfShardsForCollection(coll) * 6,
               explain.executionStats.nReturned);
} else {
    assert.eq(6, explain.executionStats.nReturned);
}

// Double check that we're not scanning more stuff than we have to. In order to get the sort
// using index 'a', we should need to scan about 50 keys and 50 documents.
explain = coll.find({a: {$gte: 50}}).sort({b: 1}).hint({a: 1}).limit(6).explain("executionStats");
assert.lte(explain.executionStats.totalKeysExamined, 60);
assert.lte(explain.executionStats.totalDocsExamined, 60);
if (FixtureHelpers.isMongos(db)) {
    // If we're talking to a mongos, we expect at most one batch from each shard.
    assert.gte(FixtureHelpers.numberOfShardsForCollection(coll) * 6,
               explain.executionStats.nReturned);
} else {
    assert.eq(6, explain.executionStats.nReturned);
}
assert(coll.drop());

// Generate big string to use in the object - 1MB+ String.
let bigStr = "ABCDEFGHIJKLMNOPQRSTUVWXYZ01234567890";
while (bigStr.length < 1000000) {
    bigStr = bigStr + "::" + bigStr;
}

// Insert enough documents to exceed the 32 MB in-memory sort limit.
const nDocs = 40 * FixtureHelpers.numberOfShardsForCollection(coll);
docsToInsert = [];
for (let i = 0; i < nDocs; i++) {
    docsToInsert.push({x: 1, y: 1, z: i, big: bigStr});
}
assert.commandWorked(coll.insert(docsToInsert));

// Two indices needed in order to trigger plan ranking. Neither index provides the sort order.
assert.commandWorked(coll.createIndex({x: 1}));
assert.commandWorked(coll.createIndex({y: 1}));

// We should only buffer 3 docs in memory.
const cursor = coll.find({x: 1, y: 1}).sort({z: -1}).limit(3);
assert.eq(nDocs - 1, cursor.next().z);
assert.eq(nDocs - 2, cursor.next().z);
assert.eq(nDocs - 3, cursor.next().z);
assert(!cursor.hasNext());
