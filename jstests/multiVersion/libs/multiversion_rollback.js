/**
 * The purpose of this test is to verify that simple CRUD operations are rolled back
 * successfully in multiversion replica sets. This test induces communication between
 * the rollback node and the sync source during rollback. This is done in order to
 * exercise rollback via refetch in the case that refetch is necessary.
 */

'use strict';

load("jstests/replsets/libs/rollback_test.js");
load("jstests/libs/collection_drop_recreate.js");
load('jstests/libs/parallel_shell_helpers.js');
load("jstests/libs/fail_point_util.js");
load("jstests/libs/feature_flag_util.js");

function printFCVDoc(nodeAdminDB, logMessage) {
    const fcvDoc = nodeAdminDB.system.version.findOne({_id: 'featureCompatibilityVersion'});
    jsTestLog(logMessage + ` ${tojson(fcvDoc)}`);
}

function CommonOps(dbName, node) {
    // Insert four documents on both nodes.
    assert.commandWorked(node.getDB(dbName)["bothNodesKeep"].insert({a: 1}));
    assert.commandWorked(node.getDB(dbName)["rollbackNodeDeletes"].insert({b: 1}));
    assert.commandWorked(node.getDB(dbName)["rollbackNodeUpdates"].insert({c: 1}));
    assert.commandWorked(node.getDB(dbName)["bothNodesUpdate"].insert({d: 1}));
}

function RollbackOps(dbName, node) {
    // Perform operations only on the rollback node:
    //   1. Delete a document.
    //   2. Update a document only on this node.
    //   3. Update a document on both nodes.
    // All three documents will be refetched during rollback.
    assert.commandWorked(node.getDB(dbName)["rollbackNodeDeletes"].remove({b: 1}));
    assert.commandWorked(node.getDB(dbName)["rollbackNodeUpdates"].update({c: 1}, {c: 0}));
    assert.commandWorked(node.getDB(dbName)["bothNodesUpdate"].update({d: 1}, {d: 0}));
}

function SyncSourceOps(dbName, node) {
    // Perform operations only on the sync source:
    //   1. Make a conflicting write on one of the documents the rollback node updates.
    //   2. Insert a new document.
    assert.commandWorked(node.getDB(dbName)["bothNodesUpdate"].update({d: 1}, {d: 2}));
    assert.commandWorked(node.getDB(dbName)["syncSourceInserts"].insert({e: 1}));
}

/**
 * Executes and validates rollback between a pair of nodes with the given versions.
 *
 * @param {string} testName the name of the test being run
 * @param {string} rollbackNodeVersion the desired version for the rollback node
 * @param {string} syncSourceVersion the desired version for the sync source
 *
 */
function testMultiversionRollback(testName, rollbackNodeVersion, syncSourceVersion) {
    jsTestLog("Started multiversion rollback test for versions: {rollbackNode: " +
              rollbackNodeVersion + ", syncSource: " + syncSourceVersion + "}.");

    let dbName = testName;

    // Set up replica set.
    let replSet = setupReplicaSet(testName, rollbackNodeVersion, syncSourceVersion);

    // Set up Rollback Test.
    let rollbackTest = new RollbackTest(testName, replSet);

    CommonOps(dbName, rollbackTest.getPrimary());

    // Perform operations that will be rolled back.
    let rollbackNode = rollbackTest.transitionToRollbackOperations();
    RollbackOps(dbName, rollbackNode);

    // Perform different operations only on the sync source.
    let syncSource = rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
    SyncSourceOps(dbName, syncSource);

    // Wait for rollback to finish.
    rollbackTest.transitionToSyncSourceOperationsDuringRollback();
    rollbackTest.transitionToSteadyStateOperations();

    rollbackTest.stop();
}

// Test rollback between latest rollback node and downgrading sync node.
function testMultiversionRollbackLatestFromDowngrading(testName, upgradeImmediately) {
    const dbName = testName;
    const replSet = new ReplSetTest(
        {name: testName, nodes: 3, useBridge: true, settings: {chainingAllowed: false}});

    replSet.startSet();
    replSet.initiateWithHighElectionTimeout();

    const config = replSet.getReplSetConfigFromNode();
    config.members[1].priority = 0;
    reconfig(replSet, config, true);

    const rollbackTest = new RollbackTest(testName, replSet);

    const primary = rollbackTest.getPrimary();
    const primaryAdminDB = primary.getDB("admin");
    const secondary = rollbackTest.getSecondary();
    const secondaryAdminDB = secondary.getDB("admin");

    printFCVDoc(primaryAdminDB, "Primary's FCV at start: ");
    checkFCV(primaryAdminDB, latestFCV);
    printFCVDoc(secondaryAdminDB, "Secondary's FCV at start: ");
    checkFCV(secondaryAdminDB, latestFCV);

    CommonOps(dbName, rollbackTest.getPrimary());

    printFCVDoc(primaryAdminDB, "Primary's FCV before RollbackOps: ");
    checkFCV(primaryAdminDB, latestFCV);
    printFCVDoc(secondaryAdminDB, "Secondary's FCV before RollbackOps: ");
    checkFCV(secondaryAdminDB, latestFCV);

    const rollbackNode = rollbackTest.transitionToRollbackOperations();
    const rollbackAdminDB = rollbackNode.getDB("admin");

    // Do some operations to be rolled back.
    RollbackOps(dbName, rollbackNode);

    const syncSource = rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
    const syncSourceAdminDB = syncSource.getDB("admin");

    printFCVDoc(rollbackAdminDB, "Rollback node's FCV before SyncSourceOps: ");
    checkFCV(rollbackAdminDB, latestFCV);
    printFCVDoc(syncSourceAdminDB, "Sync source's FCV before SyncSourceOps: ");
    checkFCV(syncSourceAdminDB, latestFCV);

    // Restart server replication on the tiebreaker node so that we can replicate the FCV change.
    let tiebreaker = rollbackTest.getTieBreaker();
    restartServerReplication(tiebreaker);

    jsTestLog("Starting SyncSourceOps");
    SyncSourceOps(dbName, syncSource);
    jsTestLog("Setting sync source FCV to downgrading to lastLTS");

    // Set the failpoint so that downgrading will fail.
    assert.commandWorked(
        syncSource.adminCommand({configureFailPoint: "failDowngrading", mode: "alwaysOn"}));

    assert.commandFailed(syncSource.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

    // Sync source's FCV should be in downgrading to lastLTS while the rollback node's FCV should be
    // in latest.
    printFCVDoc(rollbackAdminDB, "Rollback node's FCV after SyncSourceOps: ");
    checkFCV(rollbackAdminDB, latestFCV);
    printFCVDoc(syncSourceAdminDB, "Sync source's FCV after SyncSourceOps: ");
    checkFCV(syncSourceAdminDB, lastLTSFCV, lastLTSFCV);

    rollbackTest.transitionToSyncSourceOperationsDuringRollback();

    rollbackTest.transitionToSteadyStateOperations();

    // Rollback node should now have synced from the sync source and its FCV should be downgrading
    // to lastLTS.
    printFCVDoc(rollbackAdminDB, "Rollback node's FCV after rolling back: ");
    checkFCV(rollbackAdminDB, lastLTSFCV, lastLTSFCV);

    const newPrimary = rollbackTest.getPrimary();
    const newPrimaryAdminDB = newPrimary.getDB("admin");

    printFCVDoc(newPrimaryAdminDB, "New primary's FCV after rolling back: ");
    checkFCV(newPrimaryAdminDB, lastLTSFCV, lastLTSFCV);

    if (upgradeImmediately &&
        FeatureFlagUtil.isEnabled(newPrimaryAdminDB,
                                  "DowngradingToUpgrading",
                                  null /* user not specified */,
                                  true /* ignores FCV */)) {
        // We can upgrade immediately.
        assert.commandWorked(newPrimary.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

        printFCVDoc(newPrimaryAdminDB, "New primary's FCV after completing upgrade: ");
        checkFCV(newPrimaryAdminDB, latestFCV);
    } else {
        // We can finish downgrading.
        assert.commandWorked(
            newPrimary.adminCommand({configureFailPoint: "failDowngrading", mode: "off"}));
        assert.commandWorked(newPrimary.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

        printFCVDoc(newPrimaryAdminDB, "New primary's FCV after completing downgrade: ");
        checkFCV(newPrimaryAdminDB, lastLTSFCV);
    }

    rollbackTest.stop();
}

// Test rollback between downgrading rollback node and lastLTS sync node.
function testMultiversionRollbackDowngradingFromLastLTS(testName) {
    const dbName = testName;
    const replSet = new ReplSetTest(
        {name: testName, nodes: 3, useBridge: true, settings: {chainingAllowed: false}});

    replSet.startSet();
    replSet.initiateWithHighElectionTimeout();

    const config = replSet.getReplSetConfigFromNode();
    config.members[1].priority = 0;
    reconfig(replSet, config, true);

    const rollbackTest = new RollbackTest(testName, replSet);

    const primary = rollbackTest.getPrimary();
    const primaryAdminDB = primary.getDB("admin");
    const secondary = rollbackTest.getSecondary();
    const secondaryAdminDB = secondary.getDB("admin");

    printFCVDoc(primaryAdminDB, "Primary's FCV at start: ");
    checkFCV(primaryAdminDB, latestFCV);
    printFCVDoc(secondaryAdminDB, "Secondary's FCV at start: ");
    checkFCV(secondaryAdminDB, latestFCV);

    // Execute common operations, and make the set get into downgrading to lastLTS.
    CommonOps(dbName, rollbackTest.getPrimary());

    // Set the failpoint so that downgrading will fail.
    assert.commandWorked(
        primary.adminCommand({configureFailPoint: "failDowngrading", mode: "alwaysOn"}));
    assert.commandFailed(primary.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

    printFCVDoc(primaryAdminDB, "Primary's FCV before RollbackOps: ");
    checkFCV(primaryAdminDB, lastLTSFCV, lastLTSFCV);
    printFCVDoc(secondaryAdminDB, "Secondary's FCV before RollbackOps: ");
    checkFCV(secondaryAdminDB, lastLTSFCV, lastLTSFCV);

    const rollbackNode = rollbackTest.transitionToRollbackOperations();
    const rollbackAdminDB = rollbackNode.getDB("admin");

    // Do some operations to be rolled back.
    RollbackOps(dbName, rollbackNode);

    const syncSource = rollbackTest.transitionToSyncSourceOperationsBeforeRollback();
    const syncSourceAdminDB = syncSource.getDB("admin");

    printFCVDoc(rollbackAdminDB, "Rollback node's FCV before SyncSourceOps: ");
    checkFCV(rollbackAdminDB, lastLTSFCV, lastLTSFCV);
    printFCVDoc(syncSourceAdminDB, "Sync source's FCV before SyncSourceOps: ");
    checkFCV(syncSourceAdminDB, lastLTSFCV, lastLTSFCV);

    // Restart server replication on the tiebreaker node so that we can replicate the FCV change.
    let tiebreaker = rollbackTest.getTieBreaker();
    restartServerReplication(tiebreaker);

    jsTestLog("Starting SyncSourceOps");
    SyncSourceOps(dbName, syncSource);
    jsTestLog("Setting sync source FCV to lastLTS");
    assert.commandWorked(syncSource.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

    // Sync source's FCV should be in lastLTS while the rollback node's FCV should be in downgrading
    // to lastLTS.
    printFCVDoc(rollbackAdminDB, "Rollback node's FCV after sync source ops: ");
    checkFCV(rollbackAdminDB, lastLTSFCV, lastLTSFCV);
    printFCVDoc(syncSourceAdminDB, "Sync source's FCV after sync source ops: ");
    checkFCV(syncSourceAdminDB, lastLTSFCV);

    rollbackTest.transitionToSyncSourceOperationsDuringRollback();

    rollbackTest.transitionToSteadyStateOperations();

    // Rollback node should now have synced from the sync source and its FCV should be lastLTS.
    printFCVDoc(rollbackAdminDB, "Rollback node's FCV after rolling back: ");
    checkFCV(rollbackAdminDB, lastLTSFCV);

    const newPrimaryAdminDB = rollbackTest.getPrimary().getDB("admin");
    printFCVDoc(newPrimaryAdminDB, "New primary's FCV after rolling back: ");
    checkFCV(newPrimaryAdminDB, lastLTSFCV);

    rollbackTest.stop();
}

/**
 * Sets up a multiversion replica set.
 *
 * Note that, regardless of which node in the rollbackNode-syncSource pair requires
 * which version, there is only one possible way to start up such a cluster:
 *
 * 1. Start up the first two nodes with the higher of the two versions.
 * 2. Set the FCV to the lower version in order to be able to include the third node.
 * 3. Bring up the third node and add it to the set, with the lower binary version.
 * 4. This always results in a higher-version primary and a lower-version secondary,
 *    so if a test case specifies the lower version on the rollback node (i.e. the
 *    opposite setup), this function will force the current primary and secondary
 *    to switch roles.
 *
 * This function returns a replica set with the intended rollback node as the primary.
 *
 * @param {string} testName the name of the test being run
 * @param {string} rollbackNodeVersion the desired version for the rollback node
 * @param {string} syncSourceVersion the desired version for the sync source
 */
function setupReplicaSet(testName, rollbackNodeVersion, syncSourceVersion) {
    jsTestLog(
        `[${testName}] Beginning cluster setup with versions: {rollbackNode: ${rollbackNodeVersion},
            syncSource: ${syncSourceVersion}}.`);

    let sortedVersions =
        [rollbackNodeVersion, syncSourceVersion].sort(MongoRunner.compareBinVersions);
    let lowerVersion = MongoRunner.getBinVersionFor(sortedVersions[0]);
    let higherVersion = MongoRunner.getBinVersionFor(sortedVersions[1]);

    jsTestLog(`[${testName}] Starting up first two nodes with version: ${higherVersion}`);
    var initialNodes = {n1: {binVersion: higherVersion}, n2: {binVersion: higherVersion}};

    // Start up a two-node cluster first. This cluster contains two data bearing nodes, but the
    // second node will be priority: 0 to ensure that it will never become primary. This, in
    // addition to stopping/restarting server replication should make the node exhibit similar
    // behavior to an arbiter.
    var rst = new ReplSetTest(
        {name: testName, nodes: initialNodes, useBridge: true, settings: {chainingAllowed: false}});
    rst.startSet();
    rst.initiateWithHighElectionTimeout();

    // Wait for both nodes to be up.
    waitForState(rst.nodes[0], ReplSetTest.State.PRIMARY);
    waitForState(rst.nodes[1], ReplSetTest.State.SECONDARY);

    const initialPrimary = rst.getPrimary();

    // Set FCV to accommodate third node.
    jsTestLog(
        `[${testName} - ${initialPrimary.host}] Setting FCV to ${lowerVersion} on the primary.`);
    assert.commandWorked(
        initialPrimary.adminCommand({setFeatureCompatibilityVersion: lowerVersion}));

    jsTestLog(`[${testName}] Bringing up third node with version ${lowerVersion}`);
    rst.add({binVersion: lowerVersion});
    rst.reInitiate();

    let config = rst.getReplSetConfigFromNode();
    config.members[1].priority = 0;
    reconfig(rst, config, true);

    jsTestLog(
        `[${testName} - ${rst.nodes[2].host}] Waiting for the newest node to become a secondary.`);
    rst.awaitSecondaryNodes();

    let primary = rst.nodes[0];
    let secondary = rst.nodes[2];
    let tiebreakerNode = rst.nodes[1];

    // Make sure we still have the right node as the primary.
    assert.eq(rst.getPrimary(), primary);

    // Also make sure the other two nodes are in their expected states.
    assert.eq(ReplSetTest.State.SECONDARY,
              tiebreakerNode.adminCommand({replSetGetStatus: true}).myState);
    assert.eq(ReplSetTest.State.SECONDARY,
              secondary.adminCommand({replSetGetStatus: true}).myState);

    jsTestLog(`[${testName}] Cluster now running with versions: {primary: ${higherVersion},
            secondary: ${lowerVersion}, tiebreakerNode: ${higherVersion}}.`);

    // Some test cases require that the primary (future rollback node) is running the lower
    // version, which at this point is always on the secondary, so we elect that node instead.
    if (rollbackNodeVersion === lowerVersion) {
        jsTestLog(
            `[${testName}] Test case requires opposite versions for primary and secondary. Swapping roles.`);

        // Force the current secondary to become the primary.
        rst.stepUp(secondary);

        let newPrimary = secondary;
        secondary = primary;
        primary = newPrimary;

        jsTestLog(`[${testName}] Cluster now running with versions: {primary: ${lowerVersion},
            secondary: ${higherVersion}, tiebreakerNode: ${higherVersion}}.`);
    }

    return rst;
}
