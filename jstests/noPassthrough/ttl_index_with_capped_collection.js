/**
 * Verifies that TTL indexes and capped collections can coexist, then tests them.
 */

(function() {
"use strict";
load("jstests/libs/ttl_util.js");

// Run TTL monitor constantly to speed up this test.
const conn = MongoRunner.runMongod({setParameter: 'ttlMonitorSleepSecs=1'});
const db = conn.getDB(jsTestName());

// Create timestamps for now, and an old, expired date
const fresh = new Date();
const expired = new Date("2000-01-01T00:00:00Z");

// Ensure normal behavior of capped collection and TTL index on same collection.
let coll = db.ttl_coll_1;
assert.commandWorked(db.createCollection(coll.getName(), {capped: true, size: 100000, max: 3}));
assert.commandWorked(coll.createIndex({timestamp: 1}, {expireAfterSeconds: 3600}));
// Insert 3 expired documents and 1 fresh one. With the cap, doc1 should be overwritten.
assert.commandWorked(coll.insert([
    {name: "doc1", timestamp: expired},
    {name: "doc2", timestamp: expired},
    {name: "doc3", timestamp: expired},
    {name: "doc4", timestamp: fresh},
]));

// Ensure FIFO behavior of capped collection.
assert.eq(coll.find({name: "doc1"}).itcount(), 0, "Capped collection didn't overwrite");

// Wait for TTL to run. The two expired documents should remain.
TTLUtil.waitForPass(db);
assert.eq(coll.find().itcount(), 1, "TTL index on timestamp didn't delete");

// Ensure that dropping the TTL index on a capped collection disables it.
coll = db.ttl_coll_2;
assert.commandWorked(db.createCollection(coll.getName(), {capped: true, size: 100000, max: 3}));
assert.commandWorked(coll.createIndex({timestamp: 1}, {expireAfterSeconds: 3600}));

// Disable the TTL monitor briefly to prevent race condition.
db.adminCommand({setParameter: 1, ttlMonitorEnabled: false});
assert.commandWorked(coll.insert([
    {name: "doc1", timestamp: expired},
    {name: "doc2", timestamp: expired},
    {name: "doc3", timestamp: expired},
    {name: "doc4", timestamp: fresh},
]));

// Ensure FIFO behavior of capped collection.
assert.eq(coll.find({name: "doc1"}).itcount(), 0, "Capped collection didn't overwrite");

// Drop the TTL index, which should cause the next pass to ignore the documents.
assert.commandWorked(coll.dropIndex("timestamp_1"));

// Re-enable TTL monitor and let it run.
db.adminCommand({setParameter: 1, ttlMonitorEnabled: true});
TTLUtil.waitForPass(db);

assert.eq(coll.find().itcount(), 3, "TTL index remains active when it shouldn't");

// Ensure manual deletes still work (as opposed to those caused by TTL).
coll = db.ttl_coll_3;
assert.commandWorked(db.createCollection(coll.getName(), {capped: true, size: 100000, max: 3}));
assert.commandWorked(coll.createIndex({timestamp: 1}, {expireAfterSeconds: 3600}));
assert.commandWorked(coll.insert([
    {name: "doc1", timestamp: expired},
    {name: "doc2", timestamp: expired},
    {name: "doc3", timestamp: expired},
    {name: "doc4", timestamp: fresh},
]));

// Ensure FIFO behavior of capped collection.
assert.eq(coll.find({name: "doc1"}).itcount(), 0, "Capped collection didn't overwrite");

// Let the TTL run to clean 2 documents.
TTLUtil.waitForPass(db);
assert.eq(coll.find().itcount(), 1, "TTL index on timestmap didn't delete");

// Try deleting the last one.
assert.commandWorked(coll.deleteOne({}));
assert.eq(coll.find().itcount(), 0, "Drop didn't delete");

MongoRunner.stopMongod(conn);
})();
