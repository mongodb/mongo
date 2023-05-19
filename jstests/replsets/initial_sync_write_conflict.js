/*
 *  Tests that initial sync is successfully able to clone the collection and build
 *  index without any orphan index entries even if a WriteConflictException is
 *  thrown while inserting documents into collections.
 */

const testName = "write_conflict_exception";

// Start a 2 node replica set.
const replSet = new ReplSetTest({name: testName, nodes: [{}, {rsConfig: {priority: 0}}]});

jsTest.log("Starting test");
replSet.startSet();
replSet.initiate();

var secondary = replSet.getSecondary();

// Start and restart secondary with fail point that throws exception enabled.
jsTest.log("Stopping secondary");
replSet.stop(secondary);
jsTest.log("Re-starting secondary ");
secondary = replSet.start(secondary, {
    startClean: true,
    setParameter: {"failpoint.failAfterBulkLoadDocInsert": "{'mode': {'times': 1}}"}
});

// Wait for everything to be synced.
jsTest.log("Waiting for initial sync to succeed");
replSet.awaitSecondaryNodes();

// If the index table contains any entries pointing to invalid document(RecordID), then
// validateCollections called during replica stopSet will capture the index corruption and throw
// error.
replSet.stopSet();
