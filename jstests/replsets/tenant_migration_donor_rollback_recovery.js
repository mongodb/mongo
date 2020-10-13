/**
 * Tests that tenant migrations that go through rollback are recovered correctly.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, incompatible_with_eft]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/replsets/libs/rollback_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const kConfigDonorsNS = "config.tenantMigrationDonors";
const kTenantId = "testTenantId";

const kMaxSleepTimeMS = 250;
const kGarbageCollectionDelayMS = 5 * 1000;

const recipientRst = new ReplSetTest(
    {name: "recipientRst", nodes: 1, nodeOptions: {setParameter: {enableTenantMigrations: true}}});
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
        nodeOptions: {
            setParameter: {
                enableTenantMigrations: true,
                // Set the delay before a donor state doc is garbage collected to be short to speed
                // up the test.
                tenantMigrationGarbageCollectionDelayMS: kGarbageCollectionDelayMS,
                ttlMonitorSleepSecs: 1,
            }
        }
    });
    donorRst.startSet();
    let config = donorRst.getReplSetConfig();
    config.members[2].priority = 0;
    donorRst.initiateWithHighElectionTimeout(config);

    const donorRollbackTest = new RollbackTest("donorRst", donorRst);
    const donorRstArgs = {
        name: donorRst.name,
        nodeHosts: donorRst.nodes.map(node => `127.0.0.1:${node.port}`),
        nodeOptions: donorRst.nodeOptions,
        keyFile: donorRst.keyFile,
        host: donorRst.host,
        waitForKeys: false,
    };

    let donorPrimary = donorRollbackTest.getPrimary();
    donorPrimary.getCollection(kConfigDonorsNS).createIndex({expireAt: 1}, {expireAfterSeconds: 0});

    setUpFunc(donorRstArgs, donorPrimary);
    donorRollbackTest.awaitLastOpCommitted();

    // Writes during this state will be rolled back.
    donorRollbackTest.transitionToRollbackOperations();
    rollbackOpsFunc(donorRstArgs, donorPrimary);

    // Transition to replication steady state.
    donorRollbackTest.transitionToSyncSourceOperationsBeforeRollback();
    donorRollbackTest.transitionToSyncSourceOperationsDuringRollback();
    donorRollbackTest.transitionToSteadyStateOperations();

    // Get the correct primary and secondary after the topology changes. The donor replica set
    // contains 3 nodes, and replication is disabled on the tiebreaker node. So there is only one
    // secondary that the primary replicates data onto.
    donorPrimary = donorRollbackTest.getPrimary();
    let donorSecondary = donorRollbackTest.getSecondary();
    steadyStateFunc(donorPrimary, donorSecondary);

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

    let setUpFunc = (donorRstArgs, donorPrimary) => {};

    let rollbackOpsFunc = (donorRstArgs, donorPrimary) => {
        // Start the migration and wait for the primary to insert the state doc.
        migrationThread = new Thread(
            TenantMigrationUtil.startMigrationRetryOnRetryableErrors, donorRstArgs, migrationOpts);
        migrationThread.start();
        assert.soon(() => {
            return 1 === donorPrimary.getCollection(kConfigDonorsNS).count({_id: migrationId});
        });
    };

    let steadyStateFunc = (donorPrimary, donorSecondary) => {
        // Verify that the migration restarted successfully on the new primary despite rollback.
        assert.commandWorked(migrationThread.returnData());
        TenantMigrationUtil.assertMigrationCommitted(
            [donorPrimary, donorSecondary], migrationId, migrationOpts.tenantId);
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

    let setUpFunc = (donorRstArgs, donorPrimary) => {
        setUpFailPoints.forEach(failPoint => configureFailPoint(donorPrimary, failPoint));
        pauseFp = configureFailPoint(donorPrimary, pauseFailPoint);

        migrationThread = new Thread(
            TenantMigrationUtil.startMigrationRetryOnRetryableErrors, donorRstArgs, migrationOpts);
        migrationThread.start();
        pauseFp.wait();
    };

    let rollbackOpsFunc = (donorRstArgs, donorPrimary) => {
        // Resume the migration and wait for the primary to do the write for the state transition.
        pauseFp.off();
        assert.soon(() => {
            return 1 === donorPrimary.getCollection(kConfigDonorsNS).count({
                _id: migrationId,
                state: nextState
            });
        });
    };

    let steadyStateFunc = (donorPrimary, donorSecondary) => {
        // Verify that the migration resumed successfully on the new primary despite the rollback.
        assert.commandWorked(migrationThread.returnData());
        TenantMigrationUtil.waitForMigrationToCommit(
            [donorPrimary, donorSecondary], migrationId, migrationOpts.tenantId);
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

    let setUpFunc = (donorRstArgs, donorPrimary) => {
        const res = assert.commandWorked(
            TenantMigrationUtil.startMigration(donorPrimary.host, migrationOpts));
        assert.eq("committed", res.state);
    };

    let rollbackOpsFunc = (donorRstArgs, donorPrimary) => {
        // Run donorForgetMigration and wait for the primary to do the write to mark the state doc
        // as garbage collectable.
        forgetMigrationThread =
            new Thread(TenantMigrationUtil.forgetMigrationRetryOnRetryableErrors,
                       donorRstArgs,
                       migrationOpts.migrationIdString);
        forgetMigrationThread.start();
        assert.soon(() => {
            return 1 === donorPrimary.getCollection(kConfigDonorsNS).count({
                _id: migrationId,
                expireAt: {$exists: 1}
            });
        });
    };

    let steadyStateFunc = (donorPrimary, donorSecondary) => {
        // Verify that the migration state got garbage collected successfully despite the rollback.
        assert.commandWorked(forgetMigrationThread.returnData());
        TenantMigrationUtil.waitForMigrationGarbageCollection(
            [donorPrimary, donorSecondary], migrationId, migrationOpts.tenantId);
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

    let setUpFunc = (donorRstArgs, donorPrimary) => {
        migrationThread = new Thread((donorRstArgs, migrationOpts) => {
            load("jstests/replsets/libs/tenant_migration_util.js");
            assert.commandWorked(TenantMigrationUtil.startMigrationRetryOnRetryableErrors(
                donorRstArgs, migrationOpts));
            assert.commandWorked(TenantMigrationUtil.forgetMigrationRetryOnRetryableErrors(
                donorRstArgs, migrationOpts.migrationIdString));
        }, donorRstArgs, migrationOpts);

        // Start the migration and wait for a random amount of time before transitioning to the
        // rollback operations state.
        migrationThread.start();
        sleep(Math.random() * kMaxSleepTimeMS);
    };

    let rollbackOpsFunc = (donorRstArgs, donorPrimary) => {
        // Let the migration run in the rollback operations state for a random amount of time.
        sleep(Math.random() * kMaxSleepTimeMS);
    };

    let steadyStateFunc = (donorPrimary, donorSecondary) => {
        // Verify that the migration completed and was garbage collected successfully despite the
        // rollback.
        migrationThread.join();
        if (donorPrimary.getCollection(kConfigDonorsNS).count({_id: migrationId}) > 0) {
            TenantMigrationUtil.waitForMigrationToCommit(
                [donorPrimary, donorSecondary], migrationId, migrationOpts.tenantId);
        }
    };

    testRollBack(setUpFunc, rollbackOpsFunc, steadyStateFunc);
}

jsTest.log("Test roll back donor's state doc insert");
testRollbackInitialState();

jsTest.log("Test roll back donor's state doc update");
[{pauseFailPoint: "pauseTenantMigrationAfterDataSync", nextState: "blocking"},
 {pauseFailPoint: "pauseTenantMigrationAfterBlockingStarts", nextState: "committed"},
 {
     pauseFailPoint: "pauseTenantMigrationAfterBlockingStarts",
     setUpFailPoints: ["abortTenantMigrationAfterBlockingStarts"],
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
