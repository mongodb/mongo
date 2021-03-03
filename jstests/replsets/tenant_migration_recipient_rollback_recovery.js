/**
 * Tests that tenant migrations that go through recipient rollback are recovered correctly.
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

// Set the delay before a state doc is garbage collected to be short to speed up the test but long
// enough for the state doc to still be around after the recipient is back in the replication steady
// state.
const kGarbageCollectionDelayMS = 30 * 1000;

const migrationX509Options = TenantMigrationUtil.makeX509OptionsForTest();

const donorRst = new ReplSetTest({
    name: "donorRst",
    nodes: 1,
    nodeOptions: Object.assign(migrationX509Options.donor, {
        setParameter: {
            tenantMigrationGarbageCollectionDelayMS: kGarbageCollectionDelayMS,
            ttlMonitorSleepSecs: 1,
            // TODO (SERVER-54893): Make tenant_migration_recipient_rollback_recovery.js not use
            // RollbackTest.
            tenantMigrationDisableX509Auth: true
        }
    })
});
donorRst.startSet();
donorRst.initiate();
const donorRstArgs = TenantMigrationUtil.createRstArgs(donorRst);

if (!TenantMigrationUtil.isFeatureFlagEnabled(donorRst.getPrimary())) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    donorRst.stopSet();
    return;
}

function makeMigrationOpts(tenantMigrationTest, migrationId, tenantId) {
    return {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: tenantId,
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
        readPreference: {mode: "primary"},
    };
}

/**
 * Starts a recipient ReplSetTest and creates a TenantMigrationTest for it. Runs 'setUpFunc' and
 * then starts a RollbackTest from the recipient ReplSetTest. Runs 'rollbackOpsFunc' while it is in
 * rollback operations state (operations run in this state will be rolled back). Finally, runs
 * 'steadyStateFunc' after it is back in the replication steady state.
 *
 * See rollback_test.js for more information about RollbackTest.
 */
function testRollBack(setUpFunc, rollbackOpsFunc, steadyStateFunc) {
    const recipientRst = new ReplSetTest({
        name: "recipientRst",
        nodes: 3,
        useBridge: true,
        settings: {chainingAllowed: false},
        nodeOptions: Object.assign(migrationX509Options.recipient, {
            setParameter: {
                tenantMigrationGarbageCollectionDelayMS: kGarbageCollectionDelayMS,
                ttlMonitorSleepSecs: 1,
                tenantMigrationDisableX509Auth: true
            }
        })
    });
    recipientRst.startSet();
    let config = recipientRst.getReplSetConfig();
    config.members[2].priority = 0;
    recipientRst.initiateWithHighElectionTimeout(config);

    const tenantMigrationTest =
        new TenantMigrationTest({name: jsTestName(), donorRst, recipientRst});
    setUpFunc(tenantMigrationTest, donorRstArgs);

    const recipientRollbackTest = new RollbackTest("recipientRst", recipientRst);
    let recipientPrimary = recipientRollbackTest.getPrimary();
    recipientRollbackTest.awaitLastOpCommitted();

    // Writes during this state will be rolled back.
    recipientRollbackTest.transitionToRollbackOperations();
    rollbackOpsFunc(tenantMigrationTest, donorRstArgs);

    // Transition to replication steady state.
    recipientRollbackTest.transitionToSyncSourceOperationsBeforeRollback();
    recipientRollbackTest.transitionToSyncSourceOperationsDuringRollback();
    recipientRollbackTest.transitionToSteadyStateOperations();

    // Get the correct primary and secondary after the topology changes. The recipient replica set
    // contains 3 nodes, and replication is disabled on the tiebreaker node. So there is only one
    // secondary that the primary replicates data onto.
    recipientPrimary = recipientRollbackTest.getPrimary();
    let recipientSecondary = recipientRollbackTest.getSecondary();
    steadyStateFunc(tenantMigrationTest, recipientPrimary, recipientSecondary);

    recipientRollbackTest.stop();
}

/**
 * Starts a migration and waits for the recipient's primary to insert the recipient's state doc.
 * Forces the write to be rolled back. After the replication steady state is reached, asserts that
 * recipientSyncData can restart the migration on the new primary.
 */
function testRollbackInitialState() {
    const migrationId = UUID();
    let migrationOpts;
    let migrationThread;

    let setUpFunc = (tenantMigrationTest, donorRstArgs) => {};

    let rollbackOpsFunc = (tenantMigrationTest, donorRstArgs) => {
        const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

        // Start the migration asynchronously and wait for the primary to insert the state doc.
        migrationOpts = makeMigrationOpts(tenantMigrationTest, migrationId, kTenantId + "-initial");
        migrationThread = new Thread(TenantMigrationUtil.runMigrationAsync,
                                     migrationOpts,
                                     donorRstArgs,
                                     false /* retryOnRetryableErrors */);
        migrationThread.start();
        assert.soon(() => {
            return 1 ===
                recipientPrimary.getCollection(TenantMigrationTest.kConfigRecipientsNS).count({
                    _id: migrationId
                });
        });
    };

    let steadyStateFunc = (tenantMigrationTest, recipientPrimary, recipientSecondary) => {
        // Verify that the migration restarted successfully on the new primary despite rollback.
        const stateRes = assert.commandWorked(migrationThread.returnData());
        assert.eq(stateRes.state, TenantMigrationTest.DonorState.kCommitted);
        tenantMigrationTest.assertRecipientNodesInExpectedState(
            [recipientPrimary, recipientSecondary],
            migrationId,
            migrationOpts.tenantId,
            TenantMigrationTest.RecipientState.kConsistent,
            TenantMigrationTest.RecipientAccessState.kRejectBefore);
        assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
    };

    testRollBack(setUpFunc, rollbackOpsFunc, steadyStateFunc);
}

/**
 * Starts a migration after enabling 'pauseFailPoint' (must pause the migration) and
 * 'setUpFailPoints' on the recipient's primary. Waits for the primary to do the write to transition
 * to 'nextState' after reaching 'pauseFailPoint' (i.e. the state doc matches 'query'), then forces
 * the write to be rolled back. After the replication steady state is reached, asserts that the
 * migration is resumed successfully by new primary regardless of what the rolled back state
 * transition is.
 */
function testRollBackStateTransition(pauseFailPoint, setUpFailPoints, nextState, query) {
    jsTest.log(`Test roll back the write to transition to state "${
        nextState}" after reaching failpoint "${pauseFailPoint}"`);

    const migrationId = UUID();
    let migrationOpts;
    let migrationThread, pauseFp;

    let setUpFunc = (tenantMigrationTest, donorRstArgs) => {
        const recipientPrimary = tenantMigrationTest.getRecipientPrimary();
        setUpFailPoints.forEach(failPoint => configureFailPoint(recipientPrimary, failPoint));
        pauseFp = configureFailPoint(recipientPrimary, pauseFailPoint, {action: "hang"});

        migrationOpts =
            makeMigrationOpts(tenantMigrationTest, migrationId, kTenantId + "-" + nextState);
        migrationThread = new Thread(TenantMigrationUtil.runMigrationAsync,
                                     migrationOpts,
                                     donorRstArgs,
                                     false /* retryOnRetryableErrors */);
        migrationThread.start();
        pauseFp.wait();
    };

    let rollbackOpsFunc = (tenantMigrationTest, donorRstArgs) => {
        const recipientPrimary = tenantMigrationTest.getRecipientPrimary();
        // Resume the migration and wait for the primary to do the write for the state transition.
        pauseFp.off();
        assert.soon(() => {
            return 1 ===
                recipientPrimary.getCollection(TenantMigrationTest.kConfigRecipientsNS)
                    .count(Object.assign({_id: migrationId}, query));
        });
    };

    let steadyStateFunc = (tenantMigrationTest, recipientPrimary, recipientSecondary) => {
        // Verify that the migration resumed successfully on the new primary despite the rollback.
        const stateRes = assert.commandWorked(migrationThread.returnData());
        assert.eq(stateRes.state, TenantMigrationTest.DonorState.kCommitted);
        tenantMigrationTest.waitForRecipientNodesToReachState(
            [recipientPrimary, recipientSecondary],
            migrationId,
            migrationOpts.tenantId,
            TenantMigrationTest.RecipientState.kConsistent,
            TenantMigrationTest.RecipientAccessState.kRejectBefore);
        assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
    };

    testRollBack(setUpFunc, rollbackOpsFunc, steadyStateFunc);
}

/**
 * Runs donorForgetMigration after completing a migration. Waits for the recipient's primary to
 * mark the recipient's state doc as garbage collectable, then forces the write to be rolled back.
 * After the replication steady state is reached, asserts that recipientForgetMigration can be
 * retried on the new primary and that the state doc is eventually garbage collected.
 */
function testRollBackMarkingStateGarbageCollectable() {
    const migrationId = UUID();
    let migrationOpts;
    let forgetMigrationThread;

    let setUpFunc = (tenantMigrationTest, donorRstArgs) => {
        migrationOpts = makeMigrationOpts(
            tenantMigrationTest, migrationId, kTenantId + "-markGarbageCollectable");
        const stateRes = assert.commandWorked(
            tenantMigrationTest.runMigration(migrationOpts,
                                             false /* retryOnRetryableErrors */,
                                             false /* automaticForgetMigration */));
        assert.eq(stateRes.state, TenantMigrationTest.DonorState.kCommitted);
    };

    let rollbackOpsFunc = (tenantMigrationTest, donorRstArgs) => {
        const recipientPrimary = tenantMigrationTest.getRecipientPrimary();
        // Run donorForgetMigration and wait for the primary to do the write to mark the state doc
        // as garbage collectable.
        forgetMigrationThread = new Thread(TenantMigrationUtil.forgetMigrationAsync,
                                           migrationOpts.migrationIdString,
                                           donorRstArgs,
                                           false /* retryOnRetryableErrors */);
        forgetMigrationThread.start();
        assert.soon(() => {
            return 1 ===
                recipientPrimary.getCollection(TenantMigrationTest.kConfigRecipientsNS)
                    .count({_id: migrationId, expireAt: {$exists: 1}});
        });
    };

    let steadyStateFunc = (tenantMigrationTest, recipientPrimary, recipientSecondary) => {
        // Verify that the migration state got garbage collected successfully despite the rollback.
        assert.commandWorked(forgetMigrationThread.returnData());
        tenantMigrationTest.waitForMigrationGarbageCollection(
            migrationId,
            migrationOpts.tenantId,
            tenantMigrationTest.getDonorRst().nodes,
            [recipientPrimary, recipientSecondary]);
    };

    testRollBack(setUpFunc, rollbackOpsFunc, steadyStateFunc);
}

/**
 * Starts a migration and forces the recipient's primary to go through rollback after a random
 * amount of time. After the replication steady state is reached, asserts that the migration is
 * resumed successfully.
 */
function testRollBackRandom() {
    const migrationId = UUID();
    let migrationOpts;
    let migrationThread;

    let setUpFunc = (tenantMigrationTest, donorRstArgs) => {
        migrationOpts = makeMigrationOpts(tenantMigrationTest, migrationId, kTenantId + "-random");
        migrationThread = new Thread((donorRstArgs, migrationOpts) => {
            load("jstests/replsets/libs/tenant_migration_util.js");
            assert.commandWorked(TenantMigrationUtil.runMigrationAsync(
                migrationOpts, donorRstArgs, false /* retryOnRetryableErrors */));
            assert.commandWorked(TenantMigrationUtil.forgetMigrationAsync(
                migrationOpts.migrationIdString, donorRstArgs, false /* retryOnRetryableErrors */));
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

    let steadyStateFunc = (tenantMigrationTest, recipientPrimary, recipientSecondary) => {
        // Verify that the migration completed and was garbage collected successfully despite the
        // rollback.
        migrationThread.join();
        tenantMigrationTest.waitForRecipientNodesToReachState(
            [recipientPrimary, recipientSecondary],
            migrationId,
            migrationOpts.tenantId,
            TenantMigrationTest.RecipientState.kDone,
            TenantMigrationTest.RecipientAccessState.kRejectBefore);
        tenantMigrationTest.waitForMigrationGarbageCollection(
            migrationId,
            migrationOpts.tenantId,
            tenantMigrationTest.getDonorRst().nodes,
            [recipientPrimary, recipientSecondary]);
    };

    testRollBack(setUpFunc, rollbackOpsFunc, steadyStateFunc);
}

jsTest.log("Test roll back recipient's state doc insert");
testRollbackInitialState();

jsTest.log("Test roll back recipient's state doc update");
[{
    pauseFailPoint: "fpBeforeMarkingCollectionClonerDone",
    nextState: "reject",
    query: {dataConsistentStopDonorOpTime: {$exists: 1}}
},
 {
     pauseFailPoint: "fpBeforePersistingRejectReadsBeforeTimestamp",
     nextState: "rejectBefore",
     query: {rejectReadsBeforeTimestamp: {$exists: 1}}
 }].forEach(({pauseFailPoint, setUpFailPoints = [], nextState, query}) => {
    testRollBackStateTransition(pauseFailPoint, setUpFailPoints, nextState, query);
});

jsTest.log("Test roll back marking the donor's state doc as garbage collectable");
testRollBackMarkingStateGarbageCollectable();

jsTest.log("Test roll back random");
testRollBackRandom();

donorRst.stopSet();
}());
