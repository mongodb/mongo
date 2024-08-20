/**
 * Verifies that TTL indexes and capped collections can coexist, then tests them.
 * Explicitly tests the replication of TTL indexes to secondaries in a replica set.
 * @tags: [
 *   requires_fcv_71,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {TTLUtil} from "jstests/libs/ttl_util.js";

jsTest.log("Starting TTL + capped collection tests");

function hasTTLIndex(collection) {
    return collection.getIndexes().some((index) => index.expireAfterSeconds);
}

// Run TTL monitor constantly to speed up this test.
const rst = new ReplSetTest({nodes: 2, nodeOptions: {setParameter: "ttlMonitorSleepSecs=1"}});

rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const primaryDb = primary.getDB(jsTestName());
const secondary = rst.getSecondary();
const secondaryDb = secondary.getDB(jsTestName());

// Create timestamps for now, and an old, expired date.
const fresh = new Date();
const expired = new Date("2000-01-01T00:00:00Z");

// Ensure normal behavior of capped collection and TTL index on same collection.
let primaryColl = primaryDb.ttl_coll_1;
let secondaryColl = secondaryDb[primaryColl.getName()];
assert.commandWorked(
    primaryDb.createCollection(primaryColl.getName(), {capped: true, size: 100000, max: 3}));
assert.commandWorked(primaryColl.createIndex({timestamp: 1}, {expireAfterSeconds: 3600}));
// Disable the TTL monitor briefly so it can't clean our entries before the test is set up.
// Insert 3 expired documents and 1 fresh one. With the cap, doc1 should be overwritten.
primaryDb.adminCommand({setParameter: 1, ttlMonitorEnabled: false});
assert.commandWorked(primaryColl.insert([
    {name: "doc1", timestamp: expired},
    {name: "doc2", timestamp: expired},
    {name: "doc3", timestamp: expired},
    {name: "doc4", timestamp: fresh},
]));

assert.eq(primaryColl.find({name: "doc1"}).itcount(), 0, "Capped collection didn't overwrite");

// Re-enabled, then wait for TTL to run. The two expired documents should remain.
primaryDb.adminCommand({setParameter: 1, ttlMonitorEnabled: true});
TTLUtil.waitForPass(primaryDb);
assert.eq(primaryColl.find().itcount(), 1, "TTL index on timestamp didn't delete");

// Verify the secondary replicates this, and that the index exists on the secondary
assert.eq(secondaryColl.find().itcount(), 1, "Data not replicated to secondaries");
assert(hasTTLIndex(secondaryColl), "TTL index 1 did not replicate");

// Ensure that dropping the TTL index on a capped collection disables it.
primaryColl = primaryDb.ttl_coll_2;
secondaryColl = secondaryDb[primaryColl.getName()];
assert.commandWorked(
    primaryDb.createCollection(primaryColl.getName(), {capped: true, size: 100000, max: 3}));
assert.commandWorked(primaryColl.createIndex({timestamp: 1}, {expireAfterSeconds: 3600}));
rst.awaitReplication();
assert(hasTTLIndex(secondaryColl), "TTL index 2 did not replicate");

// Disable the TTL monitor briefly so it can't clean our entries before the test is set up.
primaryDb.adminCommand({setParameter: 1, ttlMonitorEnabled: false});
assert.commandWorked(primaryColl.insert([
    {name: "doc1", timestamp: expired},
    {name: "doc2", timestamp: expired},
    {name: "doc3", timestamp: expired},
    {name: "doc4", timestamp: fresh},
]));

// Ensure FIFO behavior of capped collection.
assert.eq(primaryColl.find({name: "doc1"}).itcount(), 0, "Capped collection didn't overwrite");

// Drop the TTL index, which should cause the next pass to ignore the documents.
assert.commandWorked(primaryColl.dropIndex("timestamp_1"));

// Re-enable TTL monitor and let it run.
primaryDb.adminCommand({setParameter: 1, ttlMonitorEnabled: true});
TTLUtil.waitForPass(primaryDb);

assert.eq(primaryColl.find().itcount(), 3, "TTL index remains active when it shouldn't");

// Verify the secondaries also drop their index
assert(!hasTTLIndex(secondaryColl), "Secondary node did not replicate index drop.");

// Ensure user deletes still work (as opposed to those caused by TTL).
primaryColl = primaryDb.ttl_coll_3;
secondaryColl = secondaryDb[primaryColl.getName()];
assert.commandWorked(
    primaryDb.createCollection(primaryColl.getName(), {capped: true, size: 100000, max: 3}));
assert.commandWorked(primaryColl.createIndex({timestamp: 1}, {expireAfterSeconds: 3600}));
primaryDb.adminCommand({setParameter: 1, ttlMonitorEnabled: false});
rst.awaitReplication();
assert(hasTTLIndex(secondaryColl), "The collection is not a TTL collection.");
assert.commandWorked(primaryColl.insert([
    {name: "doc1", timestamp: expired},
    {name: "doc2", timestamp: expired},
    {name: "doc3", timestamp: expired},
    {name: "doc4", timestamp: fresh},
]));

// Ensure FIFO behavior of capped collection.
assert.eq(primaryColl.find({name: "doc1"}).itcount(), 0, "Capped collection didn't overwrite");

// Let the TTL run to clean 2 documents.
primaryDb.adminCommand({setParameter: 1, ttlMonitorEnabled: true});
TTLUtil.waitForPass(primaryDb);
assert.eq(primaryColl.find().itcount(), 1, "TTL index on timestmap didn't delete");

// Try deleting the last one, and verify the secondary replicated this.
assert.commandWorked(primaryColl.deleteOne({}));
rst.awaitReplication();
assert.eq(primaryColl.find().itcount(), 0, "Drop didn't delete");
assert.eq(secondaryColl.find().itcount(), 0, "Drop didn't delete on secondary");

rst.stopSet();
