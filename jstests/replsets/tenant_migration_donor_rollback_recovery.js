/**
 * Tests that tenant migrations that go through donor rollback are recovered correctly.
 *
 * Incompatible with shard merge, which can't handle rollback.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_shard_merge,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/replsets/libs/rollback_test.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const kTenantId = "testTenantId";

const kMaxSleepTimeMS = 250;

// Set the delay before a state doc is garbage collected to be short to speed up the test but long
// enough for the state doc to still be around after the donor is back in the replication steady
// state.
const kGarbageCollectionDelayMS = 30 * 1000;

const migrationX509Options = TenantMigrationUtil.makeX509OptionsForTest();

const recipientRst = new ReplSetTest({
    name: "recipientRst",
    nodes: 1,
    nodeOptions: Object.assign({}, migrationX509Options.recipient, {
        setParameter: {
            tenantMigrationGarbageCollectionDelayMS: kGarbageCollectionDelayMS,
            ttlMonitorSleepSecs: 1,
        }
    })
});
recipientRst.startSet();
recipientRst.initiate();

function makeMigrationOpts(migrationId, tenantId) {
    return {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: tenantId,
        recipientConnString: recipientRst.getURL(),
        readPreference: {mode: "primary"},
    };
}

/**
 * Starts a donor ReplSetTest and creates a TenantMigrationTest for it. Runs 'setUpFunc' after
 * initiating the donor. Then, runs 'rollbackOpsFunc' while replication is disabled on the
 * secondaries, shuts down the primary and restarts it after re-election to force the operations in
 * 'rollbackOpsFunc' to be rolled back. Finally, runs 'steadyStateFunc' after it is back in the
 * replication steady state.
 */
function testRollBack(setUpFunc, rollbackOpsFunc, steadyStateFunc) {
    const donorRst = new ReplSetTest({
        name: "donorRst",
        nodes: 3,
        nodeOptions: Object.assign({}, migrationX509Options.donor, {
            setParameter: {
                tenantMigrationGarbageCollectionDelayMS: kGarbageCollectionDelayMS,
                ttlMonitorSleepSecs: 1,
            }
        })
    });
    donorRst.startSet();
    donorRst.initiate();
    // The default WC is majority and stopServerReplication will prevent satisfying any majority
    // writes.
    assert.commandWorked(donorRst.getPrimary().adminCommand(
        {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));
    donorRst.awaitReplication();

    const tenantMigrationTest =
        new TenantMigrationTest({name: jsTestName(), donorRst, recipientRst});
    const donorRstArgs = TenantMigrationUtil.createRstArgs(donorRst);
    setUpFunc(tenantMigrationTest, donorRstArgs);

    let originalDonorPrimary = donorRst.getPrimary();
    const originalDonorSecondaries = donorRst.getSecondaries();
    donorRst.awaitLastOpCommitted();

    // Disable replication on the secondaries so that writes during this step will be rolled back.
    stopServerReplication(originalDonorSecondaries);
    rollbackOpsFunc(tenantMigrationTest, donorRstArgs);

    // Shut down the primary and re-enable replication to allow one of the secondaries to get
    // elected, and make the writes above get rolled back on the original primary when it comes
    // back up.
    donorRst.stop(originalDonorPrimary);
    restartServerReplication(originalDonorSecondaries);
    const newDonorPrimary = donorRst.getPrimary();
    assert.neq(originalDonorPrimary, newDonorPrimary);

    // Restart the original primary.
    originalDonorPrimary =
        donorRst.start(originalDonorPrimary, {waitForConnect: true}, true /* restart */);
    originalDonorPrimary.setSecondaryOk();
    donorRst.awaitReplication();

    steadyStateFunc(tenantMigrationTest);

    donorRst.stopSet();
}

/**
 * Starts a migration and waits for the donor's primary to insert the donor's state doc. Forces the
 * write to be rolled back. After the replication steady state is reached, asserts that
 * donorStartMigration can restart the migration on the new primary.
 */
function testRollbackInitialState() {
    const migrationId = UUID();
    const migrationOpts = makeMigrationOpts(migrationId, kTenantId + "-initial");
    let migrationThread;

    let setUpFunc = (tenantMigrationTest, donorRstArgs) => {};

    let rollbackOpsFunc = (tenantMigrationTest, donorRstArgs) => {
        const donorPrimary = tenantMigrationTest.getDonorPrimary();

        // Start the migration asynchronously and wait for the primary to insert the state doc.
        migrationThread = new Thread(TenantMigrationUtil.runMigrationAsync,
                                     migrationOpts,
                                     donorRstArgs,
                                     {retryOnRetryableErrors: true});
        migrationThread.start();
        assert.soon(() => {
            return 1 === donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS).count({
                _id: migrationId
            });
        });
    };

    let steadyStateFunc = (tenantMigrationTest) => {
        // Verify that the migration restarted successfully on the new primary despite rollback.
        TenantMigrationTest.assertCommitted(assert.commandWorked(migrationThread.returnData()));
        tenantMigrationTest.waitForDonorNodesToReachState(
            tenantMigrationTest.getDonorRst().nodes,
            migrationId,
            migrationOpts.tenantId,
            TenantMigrationTest.DonorState.kCommitted);
        assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
    };

    testRollBack(setUpFunc, rollbackOpsFunc, steadyStateFunc);
}

/**
 * Starts a migration after enabling 'pauseFailPoint' (must pause the migration) and
 * 'setUpFailPoints' on the donor's primary. Waits for the primary to do the write to transition
 * to 'nextState' after reaching 'pauseFailPoint', then forces the write to be rolled back. After
 * the replication steady state is reached, asserts that the migration is resumed successfully by
 * new primary regardless of what the rolled back state transition is.
 */
function testRollBackStateTransition(pauseFailPoint, setUpFailPoints, nextState) {
    jsTest.log(`Test roll back the write to transition to state "${
        nextState}" after reaching failpoint "${pauseFailPoint}"`);

    const migrationId = UUID();
    const migrationOpts = makeMigrationOpts(migrationId, kTenantId + "-" + nextState);
    let migrationThread, pauseFp;

    let setUpFunc = (tenantMigrationTest, donorRstArgs) => {
        const donorPrimary = tenantMigrationTest.getDonorPrimary();
        setUpFailPoints.forEach(failPoint => configureFailPoint(donorPrimary, failPoint));
        pauseFp = configureFailPoint(donorPrimary, pauseFailPoint);

        migrationThread = new Thread(TenantMigrationUtil.runMigrationAsync,
                                     migrationOpts,
                                     donorRstArgs,
                                     {retryOnRetryableErrors: true});
        migrationThread.start();
        pauseFp.wait();
    };

    let rollbackOpsFunc = (tenantMigrationTest, donorRstArgs) => {
        const donorPrimary = tenantMigrationTest.getDonorPrimary();
        // Resume the migration and wait for the primary to do the write for the state transition.
        pauseFp.off();
        assert.soon(() => {
            return 1 === donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS).count({
                _id: migrationId,
                state: nextState
            });
        });
    };

    let steadyStateFunc = (tenantMigrationTest) => {
        // Verify that the migration resumed successfully on the new primary despite the rollback.
        TenantMigrationTest.assertCommitted(migrationThread.returnData());
        tenantMigrationTest.waitForDonorNodesToReachState(
            tenantMigrationTest.getDonorRst().nodes,
            migrationId,
            migrationOpts.tenantId,
            TenantMigrationTest.DonorState.kCommitted);
        assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
    };

    testRollBack(setUpFunc, rollbackOpsFunc, steadyStateFunc);
}

/**
 * Runs donorForgetMigration after completing a migration. Waits for the donor's primary to
 * mark the donor's state doc as garbage collectable, then forces the write to be rolled back.
 * After the replication steady state is reached, asserts that donorForgetMigration can be retried
 * on the new primary and that the state doc is eventually garbage collected.
 */
function testRollBackMarkingStateGarbageCollectable() {
    const migrationId = UUID();
    const migrationOpts = makeMigrationOpts(migrationId, kTenantId + "-markGarbageCollectable");
    let forgetMigrationThread;

    let setUpFunc = (tenantMigrationTest, donorRstArgs) => {
        TenantMigrationTest.assertCommitted(tenantMigrationTest.runMigration(
            migrationOpts, {retryOnRetryableErrors: true, automaticForgetMigration: false}));
    };

    let rollbackOpsFunc = (tenantMigrationTest, donorRstArgs) => {
        const donorPrimary = tenantMigrationTest.getDonorPrimary();

        // Run donorForgetMigration and wait for the primary to do the write to mark the state doc
        // as garbage collectable.
        forgetMigrationThread = new Thread(TenantMigrationUtil.forgetMigrationAsync,
                                           migrationOpts.migrationIdString,
                                           donorRstArgs,
                                           true /* retryOnRetryableErrors */);
        forgetMigrationThread.start();
        assert.soon(() => {
            return 1 === donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS).count({
                _id: migrationId,
                expireAt: {$exists: 1}
            });
        });
    };

    let steadyStateFunc = (tenantMigrationTest) => {
        // Verify that the migration state got garbage collected successfully despite the rollback.
        assert.commandWorked(forgetMigrationThread.returnData());
        tenantMigrationTest.waitForMigrationGarbageCollection(migrationId, migrationOpts.tenantId);
    };

    testRollBack(setUpFunc, rollbackOpsFunc, steadyStateFunc);
}

/**
 * Starts a migration and forces the donor's primary to go through rollback after a random amount
 * of time. After the replication steady state is reached, asserts that the migration is resumed
 * successfully.
 */
function testRollBackRandom() {
    const migrationId = UUID();
    const migrationOpts = makeMigrationOpts(migrationId, kTenantId + "-random");
    let migrationThread;

    let setUpFunc = (tenantMigrationTest, donorRstArgs) => {
        migrationThread = new Thread((donorRstArgs, migrationOpts) => {
            load("jstests/replsets/libs/tenant_migration_util.js");
            assert.commandWorked(TenantMigrationUtil.runMigrationAsync(
                migrationOpts, donorRstArgs, {retryOnRetryableErrors: true}));
            assert.commandWorked(TenantMigrationUtil.forgetMigrationAsync(
                migrationOpts.migrationIdString, donorRstArgs, true /* retryOnRetryableErrors */));
        }, donorRstArgs, migrationOpts);

        // Start the migration and wait for a random amount of time before transitioning to the
        // rollback operations state.
        migrationThread.start();
        sleep(Math.random() * kMaxSleepTimeMS);
    };

    let rollbackOpsFunc = (tenantMigrationTest, donorRstArgs) => {
        // Let the migration run in the rollback operations state for a random amount of time.
        sleep(Math.random() * kMaxSleepTimeMS);
    };

    let steadyStateFunc = (tenantMigrationTest) => {
        // Verify that the migration completed and was garbage collected successfully despite the
        // rollback.
        migrationThread.join();
        tenantMigrationTest.waitForDonorNodesToReachState(
            tenantMigrationTest.getDonorRst().nodes,
            migrationId,
            migrationOpts.tenantId,
            TenantMigrationTest.DonorState.kCommitted);
        tenantMigrationTest.waitForMigrationGarbageCollection(migrationId, migrationOpts.tenantId);
    };

    testRollBack(setUpFunc, rollbackOpsFunc, steadyStateFunc);
}

jsTest.log("Test roll back donor's state doc insert");
testRollbackInitialState();

jsTest.log("Test roll back donor's state doc update");
[{pauseFailPoint: "pauseTenantMigrationBeforeLeavingDataSyncState", nextState: "blocking"},
 {pauseFailPoint: "pauseTenantMigrationBeforeLeavingBlockingState", nextState: "committed"},
 {
     pauseFailPoint: "pauseTenantMigrationBeforeLeavingBlockingState",
     setUpFailPoints: ["abortTenantMigrationBeforeLeavingBlockingState"],
     nextState: "aborted"
 }].forEach(({pauseFailPoint, setUpFailPoints = [], nextState}) => {
    testRollBackStateTransition(pauseFailPoint, setUpFailPoints, nextState);
});

jsTest.log("Test roll back marking the donor's state doc as garbage collectable");
testRollBackMarkingStateGarbageCollectable();

jsTest.log("Test roll back random");
testRollBackRandom();

recipientRst.stopSet();
}());
