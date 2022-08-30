/**
 * Tests that transactions are able to write to the time-series buckets collection.
 *
 * @tags: [
 *   uses_transactions,
 *   uses_snapshot_read_concern
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");  // For 'TimeseriesTest'.

const session = db.getMongo().startSession();

// Use a custom database, to avoid conflict with other tests that use the system.buckets.foo
// collection.
const testDB = session.getDatabase("timeseries_buckets_writes_in_txn");
assert.commandWorked(testDB.dropDatabase());

// Access a collection prefixed with system.buckets, to mimic doing a
// transaction on a timeseries buckets collection.
const systemColl = testDB.getCollection(TimeseriesTest.getBucketsCollName("foo"));

// Ensure that a collection exists with at least one document.
assert.commandWorked(systemColl.insert({name: 0}, {writeConcern: {w: "majority"}}));

session.startTransaction({readConcern: {level: "snapshot"}});

jsTestLog("Test writing to the collection.");

jsTestLog("Testing findAndModify.");

// These findAndModify operations would throw if they failed.
systemColl.findAndModify({query: {}, update: {}});
systemColl.findAndModify({query: {}, remove: true});

jsTestLog("Testing insert.");
assert.commandWorked(systemColl.insert({name: "new"}));

jsTestLog("Testing update.");
assert.commandWorked(systemColl.update({name: 0}, {$set: {name: "foo"}}));
assert.commandWorked(
    systemColl.update({name: "nonexistent"}, {$set: {name: "foo"}}, {upsert: true}));

jsTestLog("Testing remove.");
assert.commandWorked(systemColl.remove({name: 0}));
assert.commandWorked(systemColl.remove({_id: {$exists: true}}));

// Insert a document to be read by the subsequent commands.
assert.commandWorked(systemColl.insert({name: "new"}));

jsTestLog("Test reading from the collection.");

jsTestLog("Testing find.");
assert.eq(systemColl.find().itcount(), 1);

jsTestLog("Testing aggregate.");
assert.eq(systemColl.aggregate([{$match: {}}]).itcount(), 1);

assert.commandWorked(session.commitTransaction_forTesting());
}());
