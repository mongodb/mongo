/**
 * Test that rollback via refetch with no UUID support succeeds during downgrade. This runs various
 * ddl commands that are expected to cause different UUID conflicts among the two nodes.
 */

(function() {
    'use strict';

    load('jstests/replsets/libs/rollback_test.js');
    load("jstests/libs/check_log.js");

    const testName = 'rollback_during_downgrade';
    const dbName = testName;

    // Set up Rollback Test.
    let rollbackTest = new RollbackTest(testName);

    let rollbackNode = rollbackTest.getPrimary();
    let syncSourceNode = rollbackTest.getSecondary();

    // Create some collections.
    assert.writeOK(rollbackNode.getDB(dbName)['tToRemoveUUID'].insert({t: 1}));
    assert.writeOK(rollbackNode.getDB(dbName)['tToDrop'].insert({t: 2}));
    assert.writeOK(rollbackNode.getDB(dbName)['tToRename'].insert({t: 3}));
    assert.writeOK(rollbackNode.getDB(dbName)['tToDropIndexes'].insert({t: 4}));
    assert.commandWorked(rollbackNode.getDB(dbName)['tToDropIndexes'].createIndex({t: 1}));
    assert.writeOK(rollbackNode.getDB(dbName)['tToNotRemoveUUID'].insert({t: 5}));

    // Stop downgrade in the middle of the collMod section.
    assert.commandWorked(rollbackNode.adminCommand({
        configureFailPoint: 'hangBeforeDatabaseUpgrade',
        data: {database: dbName},
        mode: 'alwaysOn'
    }));

    // Downgrade the cluster in the background.
    let downgradeFn = function() {
        jsTestLog('Downgrading replica set');
        let adminDB = db.getSiblingDB('admin');
        try {
            adminDB.runCommand({setFeatureCompatibilityVersion: '3.4'});
        } catch (e) {
            if (!isNetworkError(e)) {
                throw e;
            }
            print("Downgrading set threw expected network error: " + tojson(e));
        }
    };
    let waitForDowngradeToFinish = startParallelShell(downgradeFn, rollbackNode.port);

    checkLog.contains(rollbackNode, 'collMod - hangBeforeDatabaseUpgrade fail point enabled');

    // ----------------- Begins running operations only on the rollback node ------------------
    rollbackTest.transitionToRollbackOperations();

    // Allow the downgrade to complete the collMod section.
    assert.commandWorked(
        rollbackNode.adminCommand({configureFailPoint: 'hangBeforeDatabaseUpgrade', mode: 'off'}));
    checkLog.contains(rollbackNode, 'Finished updating UUID schema version for downgrade');

    // Tests that when we resync a two-phase dropped collection with a UUID on the sync source,
    // the UUID is assigned correctly.
    // Also tests that two-phase dropped collections are dropped eventually.
    assert(rollbackNode.getDB(dbName)['tToDrop'].drop());

    // Tests that when we resync a collection in a rename, the collection's UUID is
    // assigned properly.
    assert.commandWorked(rollbackNode.getDB(dbName)['tToRename'].renameCollection('tRenamed'));

    // Tests that collections resynced due to dropIndexes commands are assigned UUIDs correctly.
    assert.commandWorked(rollbackNode.getDB(dbName)['tToDropIndexes'].dropIndexes());

    // ----------------- Begins running operations only on the sync source node ---------------
    rollbackTest.transitionToSyncSourceOperationsBeforeRollback();

    // Fake removing the UUID on the sync source, to test when neither node has a UUID for
    // completeness.
    assert.commandWorked(syncSourceNode.getDB(dbName).runCommand({
        applyOps: [
            {op: 'c', ns: dbName + '.$cmd', o: {collMod: 'tToRemoveUUID'}},
        ]
    }));

    // ----------------- Allows rollback to occur and checks for consistency ------------------
    rollbackTest.transitionToSyncSourceOperationsDuringRollback();
    rollbackTest.transitionToSteadyStateOperations();

    waitForDowngradeToFinish();

    rollbackTest.stop();
})();
