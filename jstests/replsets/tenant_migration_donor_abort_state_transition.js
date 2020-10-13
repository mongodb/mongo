/**
 * Tests that the migration still proceeds successfully after a state transition write aborts.
 *
 * @tags: [requires_fcv_47, incompatible_with_eft, requires_majority_read_concern]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const kTenantId = "testTenantId";

const donorRst = new ReplSetTest(
    {nodes: 1, name: "donorRst", nodeOptions: {setParameter: {enableTenantMigrations: true}}});
const recipientRst = new ReplSetTest(
    {nodes: 1, name: "recipientRst", nodeOptions: {setParameter: {enableTenantMigrations: true}}});

donorRst.startSet();
donorRst.initiate();

recipientRst.startSet();
recipientRst.initiate();

/**
 * Starts a migration and forces the write to insert the donor's state doc to abort on the first few
 * tries. Asserts that the migration still completes successfully.
 */
function testAbortInitialState(donorRst) {
    const donorPrimary = donorRst.getPrimary();

    // Create the config.tenantMigrationDonors collection so the first storage transaction after
    // the migration starts corresponds to the donor's state doc insert.
    assert.commandWorked(donorPrimary.getDB("config").createCollection("tenantMigrationDonors"));

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: kTenantId + "-initial",
        recipientConnString: recipientRst.getURL(),
        readPreference: {mode: "primary"},
    };

    // Force the storage transaction for the insert to abort prior to inserting the WiredTiger
    // record.
    let writeConflictFp = configureFailPoint(donorPrimary, "WTWriteConflictException");
    let migrationThread =
        new Thread(TenantMigrationUtil.startMigration, donorPrimary.host, migrationOpts);
    migrationThread.start();
    writeConflictFp.wait();

    // Next, force the storage transaction for the insert to abort after inserting the WiredTiger
    // record and initializing the in-memory migration state.
    let opObserverFp = configureFailPoint(donorPrimary, "donorOpObserverFailAfterOnInsert");
    writeConflictFp.off();
    opObserverFp.wait();
    opObserverFp.off();

    // Verify that the migration completes successfully.
    assert.commandWorked(migrationThread.returnData());
    TenantMigrationUtil.waitForMigrationToCommit(
        donorRst.nodes, migrationId, migrationOpts.tenantId);
}

/**
 * Starts a migration after enabling 'pauseFailPoint' (must pause the migration) and
 * 'setUpFailPoints' on the donor's primary. Forces the write to transition to 'nextState' after
 * reaching 'pauseFailPoint' to abort on the first few tries. Asserts that the migration still
 * completes successfully.
 */
function testAbortStateTransition(donorRst, pauseFailPoint, setUpFailPoints, nextState) {
    jsTest.log(`Test aborting the write to transition to state "${
        nextState}" after reaching failpoint "${pauseFailPoint}"`);

    const donorPrimary = donorRst.getPrimary();

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: kTenantId + "-" + nextState,
        recipientConnString: recipientRst.getURL(),
        readPreference: {mode: "primary"},
    };

    setUpFailPoints.forEach(failPoint => configureFailPoint(donorPrimary, failPoint));
    let pauseFp = configureFailPoint(donorPrimary, pauseFailPoint);

    let migrationThread =
        new Thread(TenantMigrationUtil.startMigration, donorPrimary.host, migrationOpts);
    migrationThread.start();
    pauseFp.wait();

    // Force the storage transaction for the write to transition to the next state to abort prior to
    // updating the WiredTiger record.
    let writeConflictFp = configureFailPoint(donorPrimary, "WTWriteConflictException");
    pauseFp.off();
    writeConflictFp.wait();

    // Next, force the storage transaction for the write to abort after updating the WiredTiger
    // record and registering the change.
    let opObserverFp = configureFailPoint(donorPrimary, "donorOpObserverFailAfterOnUpdate");
    writeConflictFp.off();
    opObserverFp.wait();
    opObserverFp.off();

    // Verify that the migration completes successfully.
    assert.commandWorked(migrationThread.returnData());
    if (nextState == "aborted") {
        TenantMigrationUtil.waitForMigrationToAbort(
            donorRst.nodes, migrationId, migrationOpts.tenantId);
    } else {
        TenantMigrationUtil.waitForMigrationToCommit(
            donorRst.nodes, migrationId, migrationOpts.tenantId);
    }
}

jsTest.log("Test aborting donor's state doc insert");
testAbortInitialState(donorRst);

jsTest.log("Test aborting donor's state doc update");
[{pauseFailPoint: "pauseTenantMigrationAfterDataSync", nextState: "blocking"},
 {pauseFailPoint: "pauseTenantMigrationAfterBlockingStarts", nextState: "committed"},
 {
     pauseFailPoint: "pauseTenantMigrationAfterBlockingStarts",
     setUpFailPoints: ["abortTenantMigrationAfterBlockingStarts"],
     nextState: "aborted"
 }].forEach(({pauseFailPoint, setUpFailPoints = [], nextState}) => {
    testAbortStateTransition(donorRst, pauseFailPoint, setUpFailPoints, nextState);
});

donorRst.stopSet();
recipientRst.stopSet();
}());
