/*
* Test that the server is able to roll back a 'dropDatabase' entry correctly.  This test creates
* a collection, then executes a 'dropDatabase' command, partitioning the primary such that the
* final 'dropDatabase' oplog entry is not replicated. The test then forces rollback of that entry.
*
* The 'dropDatabase' command drops each collection, ensures that the last drop is committed,
* and only then logs a 'dropDatabase' oplog entry. This is therefore the only entry that could
* get rolled back.
*/

(function() {

    load("jstests/replsets/libs/rollback_test.js");
    load("jstests/libs/check_log.js");

    const testName = "rollback_drop_database";
    const oldDbName = "oldDatabase";
    const newDbName = "newDatabase";

    let rollbackTest = new RollbackTest(testName);
    let rollbackNode = rollbackTest.getPrimary();
    let syncSourceNode = rollbackTest.getSecondary();

    // Perform initial insert (common operation).
    assert.writeOK(rollbackNode.getDB(oldDbName)["beforeRollback"].insert({"num": 1}));

    // Set a failpoint on the original primary, so that it blocks after it commits the last
    // 'dropCollection' entry but before the 'dropDatabase' entry is logged.
    assert.commandWorked(rollbackNode.adminCommand(
        {configureFailPoint: "dropDatabaseHangBeforeLog", mode: "alwaysOn"}));

    // Issue a 'dropDatabase' command.
    let dropDatabaseFn = function() {
        const rollbackDb = "oldDatabase";
        var primary = db.getMongo();
        jsTestLog("Dropping database " + rollbackDb + " on primary node " + primary.host);
        var dbToDrop = db.getSiblingDB(rollbackDb);
        assert.commandWorked(dbToDrop.dropDatabase());
    };
    let waitForDropDatabaseToFinish = startParallelShell(dropDatabaseFn, rollbackNode.port);

    // Ensure that we've hit the failpoint before moving on.
    checkLog.contains(rollbackNode, "dropDatabase - fail point dropDatabaseHangBeforeLog enabled");

    // Wait for the secondary to finish dropping the collection (the last replicated entry).
    // We use the default 10-minute timeout for this.
    assert.soon(function() {
        let res = syncSourceNode.getDB(oldDbName).getCollectionNames().includes("beforeRollback");
        return !res;
    }, "Sync source did not finish dropping collection beforeRollback", 10 * 60 * 1000);

    rollbackTest.transitionToRollbackOperations();

    // Allow the final 'dropDatabase' entry to be logged on the now isolated primary.
    // This is the rollback node's divergent oplog entry.
    assert.commandWorked(
        rollbackNode.adminCommand({configureFailPoint: "dropDatabaseHangBeforeLog", mode: "off"}));
    waitForDropDatabaseToFinish();
    assert.eq(false, rollbackNode.getDB(oldDbName).getCollectionNames().includes("beforeRollback"));
    jsTestLog("Database " + oldDbName + " successfully dropped on primary node " +
              rollbackNode.host);

    rollbackTest.transitionToSyncSourceOperationsBeforeRollback();

    // Perform an insert on another database while interfacing with the new primary.
    // This is the sync source's divergent oplog entry.
    assert.writeOK(syncSourceNode.getDB(newDbName)["afterRollback"].insert({"num": 2}));

    rollbackTest.transitionToSyncSourceOperationsDuringRollback();
    rollbackTest.transitionToSteadyStateOperations();

    rollbackTest.stop();
})();
