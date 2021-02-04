/**
 * Tests that tenant migrations that go through rollback are recovered correctly.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, incompatible_with_eft,
 * incompatible_with_windows_tls]
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

// Set the delay before a donor state doc is garbage collected to be short to speed up the test.
const kGarbageCollectionDelayMS = 5 * 1000;

const migrationX509Options = TenantMigrationUtil.makeX509OptionsForTest();

const recipientRst = new ReplSetTest({
    name: "recipientRst",
    nodes: 1,
    nodeOptions: Object.assign(migrationX509Options.recipient, {
        setParameter: {
            // TODO SERVER-52719: Remove the failpoint 'returnResponseOkForRecipientSyncDataCmd'.
            'failpoint.returnResponseOkForRecipientSyncDataCmd': tojson({mode: 'alwaysOn'}),
            tenantMigrationGarbageCollectionDelayMS: kGarbageCollectionDelayMS,
            ttlMonitorSleepSecs: 1,
        }
    })
});
recipientRst.startSet();
recipientRst.initiate();
if (!TenantMigrationUtil.isFeatureFlagEnabled(recipientRst.getPrimary())) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    recipientRst.stopSet();
    return;
}

function makeMigrationOpts(migrationId, tenantId) {
    return {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: tenantId,
        recipientConnString: recipientRst.getURL(),
        readPreference: {mode: "primary"},
    };
}

/**
 * Starts a RollbackTest donor replica set. Runs 'setUpFunc' after the replica set reaches the
 * replication steady state. Then runs 'rollbackOpsFunc' while it is in rollback operations state
 * (operations run in this state will be rolled back). Finally, runs 'steadyStateFunc' after it is
 * back in the replication steady state.
 *
 * See rollback_test.js for more information about RollbackTest.
 */
function testRollBack(setUpFunc, rollbackOpsFunc, steadyStateFunc) {
    const donorRst = new ReplSetTest({
        name: "donorRst",
        nodes: 3,
        useBridge: true,
        settings: {chainingAllowed: false},
        nodeOptions: Object.assign(migrationX509Options.donor, {
            setParameter: {
                tenantMigrationGarbageCollectionDelayMS: kGarbageCollectionDelayMS,
                ttlMonitorSleepSecs: 1,
            }
        })
    });
    donorRst.startSet();
    let config = donorRst.getReplSetConfig();
    config.members[2].priority = 0;
    donorRst.initiateWithHighElectionTimeout(config);

    const tenantMigrationTest =
        new TenantMigrationTest({name: jsTestName(), recipientRst, donorRst});

    const donorRollbackTest = new RollbackTest("donorRst", donorRst);
    const donorRstArgs = TenantMigrationUtil.createRstArgs(donorRst);

    let donorPrimary = donorRollbackTest.getPrimary();

    setUpFunc(tenantMigrationTest, donorRstArgs);
    donorRollbackTest.awaitLastOpCommitted();

    // Writes during this state will be rolled back.
    donorRollbackTest.transitionToRollbackOperations();
    rollbackOpsFunc(tenantMigrationTest, donorRstArgs);

    // Transition to replication steady state.
    donorRollbackTest.transitionToSyncSourceOperationsBeforeRollback();
    donorRollbackTest.transitionToSyncSourceOperationsDuringRollback();
    donorRollbackTest.transitionToSteadyStateOperations();

    // Get the correct primary and secondary after the topology changes. The donor replica set
    // contains 3 nodes, and replication is disabled on the tiebreaker node. So there is only one
    // secondary that the primary replicates data onto.
    donorPrimary = donorRollbackTest.getPrimary();
    let donorSecondary = donorRollbackTest.getSecondary();
    steadyStateFunc(tenantMigrationTest, donorPrimary, donorSecondary);

    donorRollbackTest.stop();
}

/**
 * Starts a migration and waits for the donor's primary to insert the donor's state doc. Forces the
 * write to be rolled back. After the replication steady state is reached, asserts that there is no
 * state doc and that the migration can be restarted on the new primary.
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
                                     true /* retryOnRetryableErrors */);
        migrationThread.start();
        assert.soon(() => {
            return 1 === donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS).count({
                _id: migrationId
            });
        });
    };

    let steadyStateFunc = (tenantMigrationTest, donorPrimary, donorSecondary) => {
        // Verify that the migration restarted successfully on the new primary despite rollback.
        assert.commandWorked(migrationThread.returnData());
        tenantMigrationTest.assertNodesInExpectedState([donorPrimary, donorSecondary],
                                                       migrationId,
                                                       migrationOpts.tenantId,
                                                       TenantMigrationTest.State.kCommitted);
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
                                     true /* retryOnRetryableErrors */);
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

    let steadyStateFunc = (tenantMigrationTest, donorPrimary, donorSecondary) => {
        // Verify that the migration resumed successfully on the new primary despite the rollback.
        assert.commandWorked(migrationThread.returnData());
        tenantMigrationTest.waitForNodesToReachState([donorPrimary, donorSecondary],
                                                     migrationId,
                                                     migrationOpts.tenantId,
                                                     TenantMigrationTest.State.kCommitted);
        assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
    };

    testRollBack(setUpFunc, rollbackOpsFunc, steadyStateFunc);
}

/**
 * Runs donorForgetMigration after completing a migration. Waits for the donor's primary to
 * mark the donor's state doc as garbage collectable, then forces the write to be rolled back.
 * After the replication steady state is reached, asserts that the state doc doesn't get garbage
 * collected until donorForgetMigration is sent to the new primary.
 */
function testRollBackMarkingStateGarbageCollectable() {
    const migrationId = UUID();
    const migrationOpts = makeMigrationOpts(migrationId, kTenantId + "-markGarbageCollectable");
    let forgetMigrationThread;

    let setUpFunc = (tenantMigrationTest, donorRstArgs) => {
        const res = assert.commandWorked(tenantMigrationTest.runMigration(migrationOpts));
        assert.eq(TenantMigrationTest.State.kCommitted, res.state);
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

    let steadyStateFunc = (tenantMigrationTest, donorPrimary, donorSecondary) => {
        // Verify that the migration state got garbage collected successfully despite the rollback.
        assert.commandWorked(forgetMigrationThread.returnData());
        tenantMigrationTest.waitForMigrationGarbageCollection(
            migrationId, migrationOpts.tenantId, [donorPrimary, donorSecondary]);
    };

    testRollBack(setUpFunc, rollbackOpsFunc, steadyStateFunc);
}

/**
 * Starts a migration and forces the donor's primary to go through rollback after a random amount
 * of time. After the replication steady state is reached, asserts that the migration is resumed
 * if the donor's doc insertion did not roll back.
 */
function testRollBackRandom() {
    const migrationId = UUID();
    const migrationOpts = makeMigrationOpts(migrationId, kTenantId + "-random");
    let migrationThread;

    let setUpFunc = (tenantMigrationTest, donorRstArgs) => {
        migrationThread = new Thread((donorRstArgs, migrationOpts) => {
            load("jstests/replsets/libs/tenant_migration_util.js");
            assert.commandWorked(TenantMigrationUtil.runMigrationAsync(
                migrationOpts, donorRstArgs, true /* retryOnRetryableErrors */));
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

    let steadyStateFunc = (tenantMigrationTest, donorPrimary, donorSecondary) => {
        // Verify that the migration completed and was garbage collected successfully despite the
        // rollback.
        migrationThread.join();
        if (donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS).count({
                _id: migrationId
            }) > 0) {
            tenantMigrationTest.waitForNodesToReachState([donorPrimary, donorSecondary],
                                                         migrationId,
                                                         migrationOpts.tenantId,
                                                         TenantMigrationTest.State.kCommitted);
            assert.commandWorked(
                tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
        }
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
