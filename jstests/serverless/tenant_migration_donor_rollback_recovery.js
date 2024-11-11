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
 *   requires_fcv_71,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {restartServerReplication, stopServerReplication} from "jstests/libs/write_concern_util.js";
import {forceSyncSource} from "jstests/replsets/libs/sync_source.js";
import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {
    forgetMigrationAsync,
    isShardMergeEnabled,
    runMigrationAsync,
} from "jstests/replsets/libs/tenant_migration_util.js";
import {createRstArgs} from "jstests/replsets/rslib.js";

const kMaxSleepTimeMS = 250;

/**
 * Starts a donor ReplSetTest and creates a TenantMigrationTest for it. Runs 'setUpFunc' after
 * initiating the donor. Then, runs 'rollbackOpsFunc' while replication is disabled on the
 * secondaries, shuts down the primary and restarts it after re-election to force the operations in
 * 'rollbackOpsFunc' to be rolled back. Finally, runs 'steadyStateFunc' after it is back in the
 * replication steady state.
 */
function testRollBack(setUpFunc, rollbackOpsFunc, steadyStateFunc) {
    const donorRst = new ReplSetTest(
        {name: "donorRst", nodes: 3, serverless: true, settings: {chainingAllowed: false}});
    donorRst.startSet();
    donorRst.initiateWithHighElectionTimeout();

    let originalDonorPrimary = donorRst.getPrimary();
    const originalDonorSecondaries = donorRst.getSecondaries();
    const OriginalDonorSecondary = originalDonorSecondaries[0];
    const donorTieBreakerNode = originalDonorSecondaries[1];

    if (isShardMergeEnabled(originalDonorPrimary.getDB("admin"))) {
        donorRst.stopSet();
        jsTestLog("Skipping this shard merge incompatible test.");
        quit();
    }

    // This step ensures we can successfully step up the OriginalDonorSecondary node later in the
    // test.
    forceSyncSource(donorRst, donorTieBreakerNode, OriginalDonorSecondary);

    // The default WC is majority and stopServerReplication will prevent satisfying any majority
    // writes.
    assert.commandWorked(donorRst.getPrimary().adminCommand(
        {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));
    donorRst.awaitReplication();

    const tenantMigrationTest =
        new TenantMigrationTest({name: jsTestName(), donorRst, sharedOptions: {nodes: 1}});

    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId: ObjectId().str,
        recipientConnString: tenantMigrationTest.getRecipientRst().getURL(),
        readPreference: {mode: "primary"},
    };

    const donorRstArgs = createRstArgs(donorRst);
    setUpFunc(tenantMigrationTest, donorRstArgs, migrationOpts);

    donorRst.awaitLastOpCommitted();

    // Disable replication on the secondaries so that writes during this step will be rolled back.
    stopServerReplication(OriginalDonorSecondary);
    rollbackOpsFunc(tenantMigrationTest, donorRstArgs, migrationOpts);

    // Shut down the primary and re-enable replication to allow one of the secondaries to get
    // elected, and make the writes above get rolled back on the original primary when it comes
    // back up.
    donorRst.stop(originalDonorPrimary);
    restartServerReplication(OriginalDonorSecondary);

    // Step up the donor secondary.
    assert.commandWorked(OriginalDonorSecondary.adminCommand({replSetStepUp: 1}));
    const newDonorPrimary = donorRst.getPrimary();
    assert.eq(OriginalDonorSecondary, newDonorPrimary);

    // Restart the original primary.
    originalDonorPrimary =
        donorRst.start(originalDonorPrimary, {waitForConnect: true}, true /* restart */);
    originalDonorPrimary.setSecondaryOk();
    donorRst.awaitReplication();

    steadyStateFunc(tenantMigrationTest, migrationOpts);

    tenantMigrationTest.stop();
    donorRst.stopSet();
}

/**
 * Starts a migration and waits for the donor's primary to insert the donor's state doc. Forces the
 * write to be rolled back. After the replication steady state is reached, asserts that
 * donorStartMigration can restart the migration on the new primary.
 */
function testRollbackInitialState() {
    let migrationThread;

    let setUpFunc = (tenantMigrationTest, donorRstArgs, migrationOpts) => {};

    let rollbackOpsFunc = (tenantMigrationTest, donorRstArgs, migrationOpts) => {
        const donorPrimary = tenantMigrationTest.getDonorPrimary();

        // Start the migration asynchronously and wait for the primary to insert the state doc.
        migrationThread = new Thread(
            runMigrationAsync, migrationOpts, donorRstArgs, {retryOnRetryableErrors: true});
        migrationThread.start();
        assert.soon(() => {
            return 1 === donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS).count({
                _id: UUID(migrationOpts.migrationIdString)
            });
        });
    };

    let steadyStateFunc = (tenantMigrationTest, migrationOpts) => {
        // Verify that the migration restarted successfully on the new primary despite rollback.
        TenantMigrationTest.assertCommitted(assert.commandWorked(migrationThread.returnData()));
        tenantMigrationTest.waitForDonorNodesToReachState(
            tenantMigrationTest.getDonorRst().nodes,
            UUID(migrationOpts.migrationIdString),
            migrationOpts.tenantId,
            TenantMigrationTest.DonorState.kCommitted);
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

    let migrationThread, pauseFp;

    let setUpFunc = (tenantMigrationTest, donorRstArgs, migrationOpts) => {
        const donorPrimary = tenantMigrationTest.getDonorPrimary();
        setUpFailPoints.forEach(failPoint => configureFailPoint(donorPrimary, failPoint));
        pauseFp = configureFailPoint(donorPrimary, pauseFailPoint);

        migrationThread = new Thread(
            runMigrationAsync, migrationOpts, donorRstArgs, {retryOnRetryableErrors: true});
        migrationThread.start();
        pauseFp.wait();
    };

    let rollbackOpsFunc = (tenantMigrationTest, donorRstArgs, migrationOpts) => {
        const donorPrimary = tenantMigrationTest.getDonorPrimary();
        // Resume the migration and wait for the primary to do the write for the state transition.
        pauseFp.off();
        assert.soon(() => {
            return 1 === donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS).count({
                _id: UUID(migrationOpts.migrationIdString),
                state: nextState
            });
        });
    };

    let steadyStateFunc = (tenantMigrationTest, migrationOpts) => {
        // Verify that the migration resumed successfully on the new primary despite the rollback.
        TenantMigrationTest.assertCommitted(migrationThread.returnData());
        tenantMigrationTest.waitForDonorNodesToReachState(
            tenantMigrationTest.getDonorRst().nodes,
            UUID(migrationOpts.migrationIdString),
            migrationOpts.tenantId,
            TenantMigrationTest.DonorState.kCommitted);
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
    let forgetMigrationThread;

    let setUpFunc = (tenantMigrationTest, donorRstArgs, migrationOpts) => {
        TenantMigrationTest.assertCommitted(tenantMigrationTest.runMigration(
            migrationOpts, {retryOnRetryableErrors: true, automaticForgetMigration: false}));
    };

    let rollbackOpsFunc = (tenantMigrationTest, donorRstArgs, migrationOpts) => {
        const donorPrimary = tenantMigrationTest.getDonorPrimary();

        // Run donorForgetMigration and wait for the primary to do the write to mark the state doc
        // as garbage collectable.
        forgetMigrationThread = new Thread(forgetMigrationAsync,
                                           migrationOpts.migrationIdString,
                                           donorRstArgs,
                                           true /* retryOnRetryableErrors */);
        forgetMigrationThread.start();
        assert.soon(() => {
            let docs =
                donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS).find().toArray();
            // There is a ttl index on `expireAt`. Thus we know the state doc is marked as garbage
            // collectible either when:
            //
            // 1) It has an `expireAt`.
            // 2) The document is deleted/the collection is empty.
            return 1 === donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS).count({
                _id: UUID(migrationOpts.migrationIdString),
                expireAt: {$exists: 1}
            }) ||
                donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS).count({
                    _id: UUID(migrationOpts.migrationIdString)
                }) === 0;
        });
    };

    let steadyStateFunc = (tenantMigrationTest, migrationOpts) => {
        // Verify that the migration state got garbage collected successfully despite the rollback.
        assert.commandWorked(forgetMigrationThread.returnData());
    };

    testRollBack(setUpFunc, rollbackOpsFunc, steadyStateFunc);
}

/**
 * Starts a migration and forces the donor's primary to go through rollback after a random amount
 * of time. After the replication steady state is reached, asserts that the migration is resumed
 * successfully.
 */
function testRollBackRandom() {
    let migrationThread;

    let setUpFunc = (tenantMigrationTest, donorRstArgs, migrationOpts) => {
        migrationThread = new Thread(async (donorRstArgs, migrationOpts) => {
            const {runMigrationAsync, forgetMigrationAsync} =
                await import("jstests/replsets/libs/tenant_migration_util.js");
            assert.commandWorked(await runMigrationAsync(
                migrationOpts, donorRstArgs, {retryOnRetryableErrors: true}));
        }, donorRstArgs, migrationOpts);

        // Start the migration and wait for a random amount of time before transitioning to the
        // rollback operations state.
        migrationThread.start();
        sleep(Math.random() * kMaxSleepTimeMS);
    };

    let rollbackOpsFunc = (tenantMigrationTest, donorRstArgs, migrationOpts) => {
        // Let the migration run in the rollback operations state for a random amount of time.
        sleep(Math.random() * kMaxSleepTimeMS);
    };

    let steadyStateFunc = (tenantMigrationTest, migrationOpts) => {
        // Verify that the migration completed and was garbage collected successfully despite the
        // rollback.
        migrationThread.join();
        tenantMigrationTest.waitForDonorNodesToReachState(
            tenantMigrationTest.getDonorRst().nodes,
            UUID(migrationOpts.migrationIdString),
            migrationOpts.tenantId,
            TenantMigrationTest.DonorState.kCommitted);
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
