/*
 * Test that the server is able to roll back a 'dropDatabase' entry correctly.  This test creates
 * a collection, then executes a 'dropDatabase' command, partitioning the primary such that the
 * final 'dropDatabase' oplog entry is not replicated. The test then forces rollback of that entry.
 *
 * The 'dropDatabase' command drops each collection, ensures that the last drop is majority
 * committed, and only then logs a 'dropDatabase' oplog entry. This is therefore the only entry that
 * could get rolled back.
 *
 * Additionally test handling of an incompletely dropped database across a replica set. If a primary
 * writes a dropDatabase oplog entry and clears in-memory database state, but subsequently rolls
 * back the dropDatabase oplog entry, then the replica set secondaries will still have the in-memory
 * state. If the original primary is re-elected, it will allow a subsequent createCollection with a
 * database name conflicting with the original database. The secondaries should close the original
 * empty database and open the new database on receipt of the createCollection.
 *
 * @tags: [
 *   multiversion_incompatible,
 * ]
 */

(function() {

load("jstests/replsets/libs/rollback_test.js");

const testName = "rollback_drop_database";

// MongoDB does not allow multiple databases to exist that differ only in letter case. These
// database names will differ only in letter case, to test that secondaries will safely close
// conflicting empty databases.
const dbName = "olddatabase";
const conflictingDbName = "OLDDATABASE";

let rollbackTest = new RollbackTest(testName);
let rollbackNode = rollbackTest.getPrimary();
let syncSourceNode = rollbackTest.getSecondary();

// Perform initial insert (common operation).
assert.commandWorked(rollbackNode.getDB(dbName)["beforeRollback"].insert({"num": 1}));

// Set a failpoint on the original primary, so that it blocks after it commits the last
// 'dropCollection' entry but before the 'dropDatabase' entry is logged.
assert.commandWorked(rollbackNode.adminCommand(
    {configureFailPoint: "dropDatabaseHangBeforeInMemoryDrop", mode: "alwaysOn"}));

// Issue a 'dropDatabase' command.
let dropDatabaseFn = function() {
    const rollbackDb = "olddatabase";
    var primary = db.getMongo();
    jsTestLog("Dropping database " + rollbackDb + " on primary node " + primary.host);
    var dbToDrop = db.getSiblingDB(rollbackDb);
    assert.commandWorked(dbToDrop.dropDatabase({w: 1}));
};
let waitForDropDatabaseToFinish = startParallelShell(dropDatabaseFn, rollbackNode.port);

// Ensure that we've hit the failpoint before moving on.
checkLog.contains(rollbackNode,
                  "dropDatabase - fail point dropDatabaseHangBeforeInMemoryDrop enabled");

// Wait for the secondary to finish dropping the collection (the last replicated entry).
// We use the default 10-minute timeout for this.
assert.soon(function() {
    let res = syncSourceNode.getDB(dbName).getCollectionNames().includes("beforeRollback");
    return !res;
}, "Sync source did not finish dropping collection beforeRollback", 10 * 60 * 1000);

rollbackTest.transitionToRollbackOperations();

// Check that the dropDatabase oplog entry has not been written.
assert(!checkLog.checkContainsOnceJson(rollbackNode, 7360105));

// Allow the final 'dropDatabase' entry to be logged on the now isolated primary.
// This is the rollback node's divergent oplog entry.
assert.commandWorked(rollbackNode.adminCommand(
    {configureFailPoint: "dropDatabaseHangBeforeInMemoryDrop", mode: "off"}));
waitForDropDatabaseToFinish();

// Check that the dropDatabase oplog entry has now been written.
assert(checkLog.checkContainsOnceJson(rollbackNode, 7360105));

assert.eq(false, rollbackNode.getDB(dbName).getCollectionNames().includes("beforeRollback"));
jsTestLog("Database " + dbName + " successfully dropped on primary node " + rollbackNode.host);

rollbackTest.transitionToSyncSourceOperationsBeforeRollback();

// Perform an insert on another database while interfacing with the new primary.
// This is the sync source's divergent oplog entry.
assert.commandWorked(syncSourceNode.getDB("someDB")["afterRollback"].insert({"num": 2}));

rollbackTest.transitionToSyncSourceOperationsDuringRollback();
rollbackTest.transitionToSteadyStateOperations();
jsTestLog("Transitioned to steady state, going to run test operations");

// Check that replication rollback occurred on the old primary.
assert(checkLog.checkContainsOnceJson(rollbackNode, 21612));

// The syncSourceNode never received the dropDatabase oplog entry from the rollbackNode. Therefore,
// syncSourceNode never cleared the in-memory database state for that database. Check that
// syncSourceNode will safely clear the original empty database when applying a createCollection
// with a new database name that conflicts with the original.
rollbackTest.stepUpNode(rollbackNode);
// Using only w:2 because the third node is frozen / not replicating.
assert.commandWorked(rollbackNode.getDB(conflictingDbName)["afterRollback"].insert(
    {"num": 2}, {writeConcern: {w: 2}}));

rollbackTest.stop();
})();
