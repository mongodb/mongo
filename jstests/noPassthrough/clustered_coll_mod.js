/**
 * Test collMod command on a clustered collection.
 *
 * @tags: [
 *   requires_fcv_51,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/clustered_collection_util.js");

// Run TTL monitor constantly to speed up this test.
const conn = MongoRunner.runMongod({setParameter: 'ttlMonitorSleepSecs=1'});

const clusteredIndexesEnabled =
    assert.commandWorked(conn.adminCommand({getParameter: 1, featureFlagClusteredIndexes: 1}))
        .featureFlagClusteredIndexes.value;

if (!clusteredIndexesEnabled) {
    jsTestLog('Skipping test because the clustered indexes feature flag is disabled');
    MongoRunner.stopMongod(conn);
    return;
}

const dbName = jsTestName();
const testDB = conn.getDB(dbName);
const collName = "coll";

// Set the original expireAfterSeconds to a day.
const expireAfterSeconds = 60 * 60 * 24;

assert.commandWorked(testDB.createCollection(
    collName, {clusteredIndex: {key: {_id: 1}, unique: true}, expireAfterSeconds}));

// Insert documents less than a day old so they don't automatically expire.
const batchSize = 10;
const now = new Date();
let docs = [];
for (let i = 0; i < batchSize; i++) {
    // Make them 5 minutes expired.
    const fiveMinutesPastMS = 5 * 60 * 1000;
    const recentDate = new Date(now - fiveMinutesPastMS - i);
    docs.push({
        _id: recentDate,
        info: "unexpired",
    });
}
const coll = testDB[collName];
assert.commandWorked(coll.insertMany(docs, {ordered: false}));
assert.eq(coll.find().itcount(), batchSize);

ClusteredCollectionUtil.waitForTTL(testDB);
assert.eq(coll.find().itcount(), batchSize);

// Shorten the expireAfterSeconds so all the documents in the collection are expired.
assert.commandWorked(testDB.runCommand({collMod: collName, expireAfterSeconds: 1}));

ClusteredCollectionUtil.waitForTTL(testDB);

// Confirm all documents were deleted once the expireAfterSeconds was shortened.
assert.eq(coll.find().itcount(), 0);

// Turn TTL off.
assert.commandWorked(testDB.runCommand({collMod: collName, expireAfterSeconds: "off"}));

// Ensure there is no outstanding TTL pass in progress that will still remove entries.
ClusteredCollectionUtil.waitForTTL(testDB);

assert.commandWorked(coll.insert({_id: now, info: "unexpired"}));

ClusteredCollectionUtil.waitForTTL(testDB);

assert.eq(coll.find().itcount(), 1);

assert.commandFailedWithCode(
    testDB.runCommand({collMod: collName, index: {keyPattern: {_id: 1}, hidden: true}}), 6011800);

assert.commandFailedWithCode(
    testDB.runCommand({collMod: collName, index: {name: "_id_", hidden: true}}), 6011800);

MongoRunner.stopMongod(conn);
})();
