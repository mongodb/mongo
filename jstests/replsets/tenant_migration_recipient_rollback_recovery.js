/**
 * Tests that tenant migrations that go through recipient rollback are recovered correctly.
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

import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {
    forgetMigrationAsync,
    makeX509OptionsForTest,
    runMigrationAsync,
} from "jstests/replsets/libs/tenant_migration_util.js";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/replsets/libs/rollback_test.js");
load("jstests/replsets/rslib.js");  // 'createRstArgs'

const kTenantId = ObjectId().str;

const kMaxSleepTimeMS = 250;

// Set the delay before a state doc is garbage collected to be short to speed up the test but long
// enough for the state doc to still be around after the recipient is back in the replication steady
// state.
const kGarbageCollectionDelayMS = 30 * 1000;

const migrationX509Options = makeX509OptionsForTest();

function makeMigrationOpts(tenantMigrationTest, migrationId, tenantId) {
    return {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: tenantId,
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
        readPreference: {mode: "primary"},
    };
}

/**
 * Starts a recipient ReplSetTest and creates a TenantMigrationTest for it. Runs 'setUpFunc' after
 * initiating the recipient. Then, runs 'rollbackOpsFunc' while replication is disabled on the
 * secondaries, shuts down the primary and restarts it after re-election to force the operations in
 * 'rollbackOpsFunc' to be rolled back. Finally, runs 'steadyStateFunc' after it is back in the
 * replication steady state.
 */
function testRollBack(setUpFunc, rollbackOpsFunc, steadyStateFunc) {
    const donorRst = new ReplSetTest({
        name: "donorRst",
        nodes: 1,
        serverless: true,
        nodeOptions: Object.assign({}, migrationX509Options.donor, {
            setParameter: {
                tenantMigrationGarbageCollectionDelayMS: kGarbageCollectionDelayMS,
                ttlMonitorSleepSecs: 1,
            }
        })
    });
    donorRst.startSet();
    donorRst.initiate();

    const donorRstArgs = createRstArgs(donorRst);

    const recipientRst = new ReplSetTest({
        name: "recipientRst",
        nodes: 3,
        serverless: true,
        nodeOptions: Object.assign({}, migrationX509Options.recipient, {
            setParameter: {
                tenantMigrationGarbageCollectionDelayMS: kGarbageCollectionDelayMS,
                ttlMonitorSleepSecs: 1,
            }
        })
    });
    recipientRst.startSet();
    recipientRst.initiate();

    const tenantMigrationTest =
        new TenantMigrationTest({name: jsTestName(), donorRst, recipientRst});
    setUpFunc(tenantMigrationTest, donorRstArgs);

    let originalRecipientPrimary = recipientRst.getPrimary();
    const originalRecipientSecondaries = recipientRst.getSecondaries();
    // The default WC is majority and stopServerReplication will prevent satisfying any majority
    // writes.
    assert.commandWorked(originalRecipientPrimary.adminCommand(
        {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));
    recipientRst.awaitLastOpCommitted();

    // Disable replication on the secondaries so that writes during this step will be rolled back.
    stopServerReplication(originalRecipientSecondaries);
    rollbackOpsFunc(tenantMigrationTest, donorRstArgs);

    // Shut down the primary and re-enable replication to allow one of the secondaries to get
    // elected, and make the writes above get rolled back on the original primary when it comes
    // back up.
    recipientRst.stop(originalRecipientPrimary);
    restartServerReplication(originalRecipientSecondaries);
    const newRecipientPrimary = recipientRst.getPrimary();
    assert.neq(originalRecipientPrimary, newRecipientPrimary);

    // Restart the original primary.
    originalRecipientPrimary =
        recipientRst.start(originalRecipientPrimary, {waitForConnect: true}, true /* restart */);
    originalRecipientPrimary.setSecondaryOk();
    recipientRst.awaitReplication();

    steadyStateFunc(tenantMigrationTest);

    donorRst.stopSet();
    recipientRst.stopSet();
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
        migrationOpts = makeMigrationOpts(tenantMigrationTest, migrationId, ObjectId().str);
        migrationThread = new Thread(runMigrationAsync, migrationOpts, donorRstArgs);
        migrationThread.start();
        assert.soon(() => {
            return 1 ===
                recipientPrimary.getCollection(TenantMigrationTest.kConfigRecipientsNS).count({
                    _id: migrationId
                });
        });
    };

    let steadyStateFunc = (tenantMigrationTest) => {
        // Verify that the migration restarted successfully on the new primary despite rollback.
        TenantMigrationTest.assertCommitted(migrationThread.returnData());
        tenantMigrationTest.assertRecipientNodesInExpectedState({
            nodes: tenantMigrationTest.getRecipientRst().nodes,
            migrationId: migrationId,
            tenantId: migrationOpts.tenantId,
            expectedState: TenantMigrationTest.RecipientState.kConsistent,
            expectedAccessState: TenantMigrationTest.RecipientAccessState.kRejectBefore
        });
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

        migrationOpts = makeMigrationOpts(tenantMigrationTest, migrationId, ObjectId().str);
        migrationThread = new Thread(runMigrationAsync, migrationOpts, donorRstArgs);
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

    let steadyStateFunc = (tenantMigrationTest) => {
        // Verify that the migration resumed successfully on the new primary despite the rollback.
        TenantMigrationTest.assertCommitted(migrationThread.returnData());
        tenantMigrationTest.waitForRecipientNodesToReachState(
            tenantMigrationTest.getRecipientRst().nodes,
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
        migrationOpts = makeMigrationOpts(tenantMigrationTest, migrationId, ObjectId().str);
        TenantMigrationTest.assertCommitted(
            tenantMigrationTest.runMigration(migrationOpts, {automaticForgetMigration: false}));
    };

    let rollbackOpsFunc = (tenantMigrationTest, donorRstArgs) => {
        const recipientPrimary = tenantMigrationTest.getRecipientPrimary();
        // Run donorForgetMigration and wait for the primary to do the write to mark the state doc
        // as garbage collectable.
        forgetMigrationThread = new Thread(forgetMigrationAsync,
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

    let steadyStateFunc = (tenantMigrationTest) => {
        // Verify that the migration state got garbage collected successfully despite the rollback.
        assert.commandWorked(forgetMigrationThread.returnData());
        tenantMigrationTest.waitForMigrationGarbageCollection(
            migrationId,
            migrationOpts.tenantId,
            tenantMigrationTest.getDonorRst().nodes,
            tenantMigrationTest.getRecipientRst().nodes);
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
        migrationOpts = makeMigrationOpts(tenantMigrationTest, migrationId, ObjectId().str);
        migrationThread = new Thread(async (donorRstArgs, migrationOpts) => {
            const {runMigrationAsync, forgetMigrationAsync} =
                await import("jstests/replsets/libs/tenant_migration_util.js");
            assert.commandWorked(await runMigrationAsync(migrationOpts, donorRstArgs));
            assert.commandWorked(await forgetMigrationAsync(
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

    let steadyStateFunc = (tenantMigrationTest) => {
        // Verify that the migration completed and was garbage collected successfully despite the
        // rollback.
        migrationThread.join();
        tenantMigrationTest.waitForRecipientNodesToReachState(
            tenantMigrationTest.getRecipientRst().nodes,
            migrationId,
            migrationOpts.tenantId,
            TenantMigrationTest.RecipientState.kDone,
            TenantMigrationTest.RecipientAccessState.kRejectBefore);
        tenantMigrationTest.waitForMigrationGarbageCollection(
            migrationId,
            migrationOpts.tenantId,
            tenantMigrationTest.getDonorRst().nodes,
            tenantMigrationTest.getRecipientRst().nodes);
    };

    testRollBack(setUpFunc, rollbackOpsFunc, steadyStateFunc);
}

jsTest.log("Test roll back recipient's state doc insert");
testRollbackInitialState();

jsTest.log("Test roll back recipient's state doc update");
[{
    pauseFailPoint: "fpBeforeMarkingCloneSuccess",
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
