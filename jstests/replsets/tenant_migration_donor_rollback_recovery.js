/**
 * Tests that tenant migrations that go through rollback are recovered correctly.
 *
 * @tags: [requires_fcv_47, incompatible_with_eft, requires_majority_read_concern]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/replsets/libs/rollback_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const kConfigDonorsNS = "config.tenantMigrationDonors";
const kDBPrefix = "testDbPrefix";

const kMaxSleepTimeMS = 250;
const kGarbageCollectionDelayMS = 5 * 1000;

const recipientRst = new ReplSetTest(
    {name: "recipientRst", nodes: 1, nodeOptions: {setParameter: {enableTenantMigrations: true}}});
recipientRst.startSet();
recipientRst.initiate();

function makeMigrationOpts(migrationId, dbPrefix) {
    return {
        migrationIdString: extractUUIDFromObject(migrationId),
        dbPrefix: dbPrefix,
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
    let donorPrimary = donorRollbackTest.getPrimary();
    donorPrimary.getCollection(kConfigDonorsNS).createIndex({expireAt: 1}, {expireAfterSeconds: 0});

    setUpFunc(donorPrimary);
    donorRollbackTest.awaitLastOpCommitted();

    // Writes during this state will be rolled back.
    donorRollbackTest.transitionToRollbackOperations();
    rollbackOpsFunc(donorPrimary);

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
    const migrationOpts = makeMigrationOpts(migrationId, kDBPrefix + "-initial");
    let migrationThread;

    let setUpFunc = (donorPrimary) => {};

    let rollbackOpsFunc = (donorPrimary) => {
        // Start the migration and wait for the primary to insert the state doc.
        migrationThread =
            new Thread(TenantMigrationUtil.startMigration, donorPrimary.host, migrationOpts);
        migrationThread.start();
        assert.soon(() => {
            return 1 === donorPrimary.getCollection(kConfigDonorsNS).count({_id: migrationId});
        });
    };

    let steadyStateFunc = (donorPrimary, donorSecondary) => {
        const configDonorsColl = donorPrimary.getCollection(kConfigDonorsNS);

        // Verify that the migration was interrupted due to the original primary stepping down.
        migrationThread.join();
        const res = migrationThread.returnData();
        assert(ErrorCodes.isNotPrimaryError(res.code), tojson(res));
        assert.eq(0, configDonorsColl.count({_id: migrationId}));

        // Verify that the migration can be restarted on the new primary.
        assert.commandWorked(TenantMigrationUtil.startMigration(donorPrimary.host, migrationOpts));
        TenantMigrationUtil.assertMigrationCommitted(
            [donorPrimary, donorSecondary], migrationId, migrationOpts.dbPrefix);
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
    const migrationOpts = makeMigrationOpts(migrationId, kDBPrefix + "-" + nextState);
    let migrationThread, pauseFp;

    let setUpFunc = (donorPrimary) => {
        setUpFailPoints.forEach(failPoint => configureFailPoint(donorPrimary, failPoint));
        pauseFp = configureFailPoint(donorPrimary, pauseFailPoint);

        migrationThread =
            new Thread(TenantMigrationUtil.startMigration, donorPrimary.host, migrationOpts);
        migrationThread.start();
        pauseFp.wait();
    };

    let rollbackOpsFunc = (donorPrimary) => {
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
        // Verify that the migration was interrupted due to the original primary stepping down.
        migrationThread.join();
        const res = migrationThread.returnData();
        assert(ErrorCodes.isNotPrimaryError(res.code), tojson(res));

        // Verify that the migration resumed successfully on the new primary after rollback.
        TenantMigrationUtil.waitForMigrationToCommit(
            [donorPrimary, donorSecondary], migrationId, migrationOpts.dbPrefix);
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
    const migrationOpts = makeMigrationOpts(migrationId, kDBPrefix + "-markGarbageCollectable");
    let forgetMigrationThread;

    let setUpFunc = (donorPrimary) => {
        const res = assert.commandWorked(
            TenantMigrationUtil.startMigration(donorPrimary.host, migrationOpts));
        assert.eq("committed", res.state);
    };

    let rollbackOpsFunc = (donorPrimary) => {
        // Run donorForgetMigration and wait for the primary to do the write to mark the state doc
        // as garbage collectable.
        forgetMigrationThread = new Thread(TenantMigrationUtil.forgetMigration,
                                           donorPrimary.host,
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
        // Verify that the donorForgetMigration command was interrupted due to the original primary
        // stepping down.
        forgetMigrationThread.join();
        const res = forgetMigrationThread.returnData();
        assert(ErrorCodes.isNotPrimaryError(res.code), tojson(res));

        // Verify that the migration does not get garbage collected due to rollback.
        sleep(kGarbageCollectionDelayMS);
        assert.eq(1, donorPrimary.getCollection(kConfigDonorsNS).count({_id: migrationId}));

        // Verify that it can be garbage collected on retry.
        assert.commandWorked(TenantMigrationUtil.forgetMigration(donorPrimary.host,
                                                                 migrationOpts.migrationIdString));
        TenantMigrationUtil.waitForMigrationGarbageCollection(
            [donorPrimary, donorSecondary], migrationId, migrationOpts.dbPrefix);
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
    const migrationOpts = makeMigrationOpts(migrationId, kDBPrefix + "-random");
    let migrationThread;

    let setUpFunc = (donorPrimary) => {
        migrationThread = new Thread((donorPrimaryHost, migrationOpts) => {
            load("jstests/replsets/libs/tenant_migration_util.js");
            const startMigrationRes =
                TenantMigrationUtil.startMigration(donorPrimaryHost, migrationOpts);
            if (!startMigrationRes.ok) {
                return startMigrationRes;
            }
            return TenantMigrationUtil.forgetMigration(donorPrimaryHost,
                                                       migrationOpts.migrationIdString);
        }, donorPrimary.host, migrationOpts);

        // Start the migration and wait for a random amount of time before transitioning to the
        // rollback operations state.
        migrationThread.start();
        sleep(Math.random() * kMaxSleepTimeMS);
    };

    let rollbackOpsFunc = (donorPrimary) => {
        // Let the migration run in the rollback operations state for a random amount of time.
        sleep(Math.random() * kMaxSleepTimeMS);
    };

    let steadyStateFunc = (donorPrimary, donorSecondary) => {
        // Verify that the migration either succeeded or was interrupted due to the original primary
        // stepping down.
        migrationThread.join();
        const res = migrationThread.returnData();
        assert(res.ok || ErrorCodes.isNotPrimaryError(res.code), tojson(res));

        // Verify that the migration resumed successfully after rollback if the donor's doc
        // insertion did not roll back.
        const configDonorsColl = donorPrimary.getCollection(kConfigDonorsNS);
        if (configDonorsColl.count({_id: migrationId}) > 0) {
            TenantMigrationUtil.waitForMigrationToCommit(
                [donorPrimary, donorSecondary], migrationId, migrationOpts.dbPrefix);
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
