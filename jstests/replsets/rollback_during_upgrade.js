/**
 * Test that rollback via refetch with no UUID support succeeds during upgrade. This runs various
 * ddl commands that are expected to cause different UUID conflicts among the two nodes.
 */

(function() {
    'use strict';

    load('jstests/replsets/libs/rollback_test.js');

    const testName = 'rollback_during_upgrade';
    const dbName = testName;

    // Set up Rollback Test.
    let rollbackTest = new RollbackTest(testName);

    let rollbackNode = rollbackTest.getPrimary();
    let syncSourceNode = rollbackTest.getSecondary();

    // Begin in fCV 3.4.
    assert.commandWorked(rollbackNode.adminCommand({setFeatureCompatibilityVersion: '3.4'}));

    // Create some collections.
    assert.writeOK(rollbackNode.getDB(dbName)['tToDropIndexes'].insert({t: 5}));
    assert.commandWorked(rollbackNode.getDB(dbName)['tToDropIndexes'].createIndex({t: 1}));

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

    // Tests that collections synced due to dropIndexes commands are cloned correctly.
    assert.commandWorked(rollbackNode.getDB(dbName)['tToDropIndexes'].dropIndexes());

    // ----------------- Begins running operations only on the sync source node ---------------
    rollbackTest.transitionToSyncSourceOperations();

    // Fake giving the collections UUIDs.
    assert.commandWorked(syncSourceNode.getDB(dbName).runCommand({
        applyOps: [
            {op: 'c', ns: dbName + '.$cmd', ui: UUID(), o: {collMod: 'tToDropIndexes'}},
        ]
    }));

    // ----------------- Allows rollback to occur and checks for consistency ------------------
    rollbackTest.transitionToSteadyStateOperations({waitForRollback: true});

    waitForUpgradeToFinish();

    rollbackTest.stop();
})();
