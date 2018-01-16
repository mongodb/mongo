/**
 * Test that rollback via refetch with no UUID support succeeds during upgrade. This runs various
 * ddl commands that are expected to cause different UUID conflicts among the two nodes.
 */

(function() {
    'use strict';

    load('jstests/replsets/libs/rollback_test.js');
    load("jstests/libs/check_log.js");

    const testName = 'rollback_during_upgrade';
    const dbName = testName;

    // Set up Rollback Test.
    let rollbackTest = new RollbackTest(testName);

    let rollbackNode = rollbackTest.getPrimary();
    let syncSourceNode = rollbackTest.getSecondary();

    // Begin in fCV 3.4.
    assert.commandWorked(rollbackNode.adminCommand({setFeatureCompatibilityVersion: '3.4'}));

    // Create some collections.
    assert.writeOK(rollbackNode.getDB(dbName)['tToAssignUUID'].insert({t: 1}));
    assert.writeOK(rollbackNode.getDB(dbName)['tToDropAndNotAssignUUID'].insert({t: 2}));
    assert.writeOK(rollbackNode.getDB(dbName)['tToDrop'].insert({t: 3}));
    assert.writeOK(rollbackNode.getDB(dbName)['tToRename'].insert({t: 4}));
    assert.writeOK(rollbackNode.getDB(dbName)['tToDropIndexes'].insert({t: 5}));
    assert.commandWorked(rollbackNode.getDB(dbName)['tToDropIndexes'].createIndex({t: 1}));
    assert.writeOK(rollbackNode.getDB(dbName)['tToRemoveUUIDLocally'].insert({t: 6}));
    assert.writeOK(rollbackNode.getDB(dbName)['tToNotAssignUUID'].insert({t: 7}));

    // Stop upgrade in the middle of the collMod section.
    assert.commandWorked(rollbackNode.adminCommand({
        configureFailPoint: 'hangBeforeDatabaseUpgrade',
        data: {database: dbName},
        mode: 'alwaysOn'
    }));

    // Upgrade the cluster in the background.
    let upgradeFn = function() {
        jsTestLog('Upgrading replica set');
        let adminDB = db.getSiblingDB('admin');
        try {
            adminDB.runCommand({setFeatureCompatibilityVersion: '3.6'});
        } catch (e) {
            if (!isNetworkError(e)) {
                throw e;
            }
            print("Upgrading set threw expected network error: " + tojson(e));
        }
    };
    let waitForUpgradeToFinish = startParallelShell(upgradeFn, rollbackNode.port);

    checkLog.contains(rollbackNode, 'collMod - hangBeforeDatabaseUpgrade fail point enabled');

    // ----------------- Begins running operations only on the rollback node ------------------
    rollbackTest.transitionToRollbackOperations();

    // Allow the upgrade to complete the collMod section.
    assert.commandWorked(
        rollbackNode.adminCommand({configureFailPoint: 'hangBeforeDatabaseUpgrade', mode: 'off'}));
    checkLog.contains(rollbackNode, 'Finished updating UUID schema version for upgrade');

    // Tests that we resolve UUID conflicts correctly when we resync a two-phase dropped collection
    // with a different UUID on both nodes.
    // Also tests that two-phase dropped collections are dropped eventually.
    assert(rollbackNode.getDB(dbName)['tToDrop'].drop());

    // Tests that when we resync a two-phase dropped collection where only the rolling back node
    // has a UUID, that the node removes its UUID.
    assert(rollbackNode.getDB(dbName)['tToDropAndNotAssignUUID'].drop());

    // Tests that we correctly assign the sync source's UUID when we resync collections in a rename.
    assert.commandWorked(rollbackNode.getDB(dbName)['tToRename'].renameCollection('tRenamed'));

    // Tests that we correctly assign the sync source's UUID when we resync collections during
    // rollback of dropIndexes commands.
    assert.commandWorked(rollbackNode.getDB(dbName)['tToDropIndexes'].dropIndexes());

    // Tests that collections get assigned the correct UUID when the sync source has a
    // UUID but there is no UUID locally.
    assert.commandWorked(rollbackNode.getDB(dbName).runCommand({
        applyOps: [
            {op: 'c', ns: dbName + '.$cmd', o: {collMod: 'tToRemoveUUIDLocally'}},
        ]
    }));

    // ----------------- Begins running operations only on the sync source node ---------------
    rollbackTest.transitionToSyncSourceOperationsBeforeRollback();

    // Fake giving the collections UUIDs to simulate an upgrade on the sync source.
    // The rollback test fixture only has two data bearing nodes so we cannot run an upgrade using
    // the `setFeatureCompatibilityVersion` command.
    assert.commandWorked(syncSourceNode.getDB(dbName).runCommand({
        applyOps: [
            {op: 'c', ns: dbName + '.$cmd', ui: UUID(), o: {collMod: 'tToAssignUUID'}},
            {op: 'c', ns: dbName + '.$cmd', ui: UUID(), o: {collMod: 'tToDrop'}},
            {op: 'c', ns: dbName + '.$cmd', ui: UUID(), o: {collMod: 'tToRename'}},
            {op: 'c', ns: dbName + '.$cmd', ui: UUID(), o: {collMod: 'tToDropIndexes'}},
        ]
    }));

    // ----------------- Allows rollback to occur and checks for consistency ------------------
    rollbackTest.transitionToSyncSourceOperationsDuringRollback();
    rollbackTest.transitionToSteadyStateOperations();
    waitForUpgradeToFinish();

    rollbackTest.stop();
})();
