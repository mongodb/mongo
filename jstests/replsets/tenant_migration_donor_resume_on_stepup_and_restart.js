/**
 * Tests that tenant migrations resume successfully on stepup and restart.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, requires_persistence,
 * incompatible_with_eft]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const kMaxSleepTimeMS = 100;
const kTenantId = "testTenantId";
const kMigrationFpNames = [
    "pauseTenantMigrationBeforeLeavingDataSyncState",
    "pauseTenantMigrationBeforeLeavingBlockingState",
    "abortTenantMigrationBeforeLeavingBlockingState",
    ""
];

// Set the delay before a donor state doc is garbage collected to be short to speed up the test.
const kGarbageCollectionDelayMS = 30 * 1000;

// Set the TTL monitor to run at a smaller interval to speed up the test.
const kTTLMonitorSleepSecs = 1;

const migrationX509Options = TenantMigrationUtil.makeX509OptionsForTest();

/**
 * If the donor state doc for the migration 'migrationId' exists on the donor (i.e. the donor's
 * primary stepped down or shut down after inserting the doc), asserts that the migration
 * eventually commits.
 */
function assertMigrationCommitsIfDurableStateExists(tenantMigrationTest, migrationId, tenantId) {
    const donorRst = tenantMigrationTest.getDonorRst();
    const donorPrimary = tenantMigrationTest.getDonorPrimary();

    const configDonorsColl = donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS);
    if (configDonorsColl.count({_id: migrationId}) > 0) {
        tenantMigrationTest.waitForNodesToReachState(
            donorRst.nodes, migrationId, tenantId, TenantMigrationTest.State.kCommitted);
        assert.commandWorked(
            tenantMigrationTest.forgetMigration(extractUUIDFromObject(migrationId)));
    }
}

/**
 * Runs the donorStartMigration command to start a migration, and interrupts the migration on the
 * donor using the 'interruptFunc', and asserts that migration eventually commits.
 */
function testDonorStartMigrationInterrupt(interruptFunc) {
    const donorRst =
        new ReplSetTest({nodes: 3, name: "donorRst", nodeOptions: migrationX509Options.donor});

    donorRst.startSet();
    donorRst.initiate();

    // TODO SERVER-52719: Remove 'enableRecipientTesting: false'.
    const tenantMigrationTest =
        new TenantMigrationTest({name: jsTestName(), donorRst, enableRecipientTesting: false});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        donorRst.stopSet();
        return;
    }
    const donorPrimary = tenantMigrationTest.getDonorPrimary();

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: kTenantId,
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
    };
    const donorRstArgs = TenantMigrationUtil.createRstArgs(donorRst);

    const runMigrationThread = new Thread(TenantMigrationUtil.runMigrationAsync,
                                          migrationOpts,
                                          donorRstArgs,
                                          true /* retryOnRetryableErrors */);
    runMigrationThread.start();

    // Wait for donorStartMigration command to start.
    assert.soon(() => donorPrimary.adminCommand({currentOp: true, desc: "tenant donor migration"})
                          .inprog.length > 0);

    sleep(Math.random() * kMaxSleepTimeMS);
    interruptFunc(donorRst);

    assert.commandWorked(runMigrationThread.returnData());
    assertMigrationCommitsIfDurableStateExists(
        tenantMigrationTest, migrationId, migrationOpts.tenantId);

    tenantMigrationTest.stop();
    donorRst.stopSet();
}

/**
 * Starts a migration and waits for it to commit, then runs the donorForgetMigration, and interrupts
 * the donor using the 'interruptFunc', and asserts that the migration state is eventually garbage
 * collected.
 */
function testDonorForgetMigrationInterrupt(interruptFunc) {
    const donorRst = new ReplSetTest({
        nodes: 3,
        name: "donorRst",
        nodeOptions: Object.assign(migrationX509Options.donor, {
            setParameter: {
                tenantMigrationGarbageCollectionDelayMS: kGarbageCollectionDelayMS,
                ttlMonitorSleepSecs: kTTLMonitorSleepSecs,
            }
        })
    });
    const recipientRst = new ReplSetTest({
        nodes: 1,
        name: "recipientRst",
        nodeOptions: Object.assign(migrationX509Options.recipient, {
            setParameter: {
                // TODO SERVER-52719: Remove the failpoint
                // 'returnResponseOkForRecipientSyncDataCmd'.
                'failpoint.returnResponseOkForRecipientSyncDataCmd': tojson({mode: 'alwaysOn'}),
                tenantMigrationGarbageCollectionDelayMS: kGarbageCollectionDelayMS,
                ttlMonitorSleepSecs: kTTLMonitorSleepSecs,
            }
        })
    });

    donorRst.startSet();
    donorRst.initiate();

    recipientRst.startSet();
    recipientRst.initiate();

    const tenantMigrationTest =
        new TenantMigrationTest({name: jsTestName(), donorRst, recipientRst});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        donorRst.stopSet();
        recipientRst.stopSet();
        return;
    }
    let donorPrimary = tenantMigrationTest.getDonorPrimary();

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: kTenantId,
        recipientConnString: recipientRst.getURL(),
    };
    const donorRstArgs = TenantMigrationUtil.createRstArgs(donorRst);

    assert.commandWorked(tenantMigrationTest.runMigration(
        migrationOpts, false /* retryOnRetryableErrors */, false /* automaticForgetMigration */));
    const forgetMigrationThread = new Thread(TenantMigrationUtil.forgetMigrationAsync,
                                             migrationOpts.migrationIdString,
                                             donorRstArgs,
                                             true /* retryOnRetryableErrors */);
    forgetMigrationThread.start();

    // Wait for donorForgetMigration command to start.
    assert.soon(() => {
        const res = assert.commandWorked(
            donorPrimary.adminCommand({currentOp: true, desc: "tenant donor migration"}));
        return res.inprog[0].expireAt != null;
    });

    sleep(Math.random() * kMaxSleepTimeMS);
    interruptFunc(donorRst);

    donorPrimary = donorRst.getPrimary();
    assert.commandWorkedOrFailedWithCode(
        tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString),
        ErrorCodes.NoSuchTenantMigration);

    assert.commandWorked(forgetMigrationThread.returnData());
    // After forgetMigrationThread returns, check that the recipient state doc is correctly marked
    // as garbage collectable.
    const recipientPrimary = tenantMigrationTest.getRecipientPrimary();
    const recipientStateDoc =
        recipientPrimary.getCollection(TenantMigrationTest.kConfigRecipientsNS).findOne({
            _id: migrationId
        });
    assert(recipientStateDoc.expireAt);
    tenantMigrationTest.waitForMigrationGarbageCollection(
        donorRst.nodes, migrationId, migrationOpts.tenantId);

    tenantMigrationTest.stop();
    donorRst.stopSet();
    recipientRst.stopSet();
}

/**
 * Starts a migration and sets the passed in failpoint, then runs the donorAbortMigration, and
 * interrupts the donor using the 'interruptFunc', and asserts that the migration state is
 * eventually garbage collected.
 */
function testDonorAbortMigrationInterrupt(interruptFunc, fpName, isShutdown = false) {
    const donorRst = new ReplSetTest({
        nodes: 3,
        name: "donorRst",
        nodeOptions: Object.assign(migrationX509Options.donor, {
            setParameter: {
                tenantMigrationGarbageCollectionDelayMS: kGarbageCollectionDelayMS,
                ttlMonitorSleepSecs: kTTLMonitorSleepSecs,
            }
        })
    });
    const recipientRst = new ReplSetTest({
        nodes: 1,
        name: "recipientRst",
        nodeOptions: Object.assign(migrationX509Options.recipient, {
            setParameter: {
                // TODO SERVER-52719: Remove the failpoint
                // 'returnResponseOkForRecipientSyncDataCmd'.
                'failpoint.returnResponseOkForRecipientSyncDataCmd': tojson({mode: 'alwaysOn'}),
                tenantMigrationGarbageCollectionDelayMS: kGarbageCollectionDelayMS,
                ttlMonitorSleepSecs: kTTLMonitorSleepSecs,
            }
        })
    });

    donorRst.startSet();
    donorRst.initiate();

    recipientRst.startSet();
    recipientRst.initiate();

    const tenantMigrationTest =
        new TenantMigrationTest({name: jsTestName(), donorRst, recipientRst});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        donorRst.stopSet();
        recipientRst.stopSet();
        return;
    }

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: kTenantId,
        recipientConnString: recipientRst.getURL(),
    };
    const donorRstArgs = TenantMigrationUtil.createRstArgs(donorRst);
    let donorPrimary = tenantMigrationTest.getDonorPrimary();

    // If we passed in a valid failpoint we set it, otherwise we let the migration run normally.
    let fp;
    if (fpName) {
        fp = configureFailPoint(donorPrimary, fpName);
    }

    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

    const tryAbortThread = new Thread(TenantMigrationUtil.tryAbortMigrationAsync,
                                      {migrationIdString: migrationOpts.migrationIdString},
                                      donorRstArgs,
                                      TenantMigrationUtil.runTenantMigrationCommand,
                                      true /* retryOnRetryableErrors */);
    tryAbortThread.start();

    // Wait for donorAbortMigration command to start.
    assert.soon(() => {
        const res = assert.commandWorked(
            donorPrimary.adminCommand({currentOp: true, desc: "tenant donor migration"}));
        return res.inprog[0].receivedCancelation;
    });

    interruptFunc(donorRst);

    if (fp && !isShutdown) {
        // Turn off failpoint in order to allow the migration to resume after stepup.
        fp.off();
    }

    tryAbortThread.join();

    let res = tryAbortThread.returnData();
    assert.commandWorkedOrFailedWithCode(res, ErrorCodes.TenantMigrationCommitted);

    donorPrimary = tenantMigrationTest.getDonorPrimary();
    let configDonorsColl = donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS);
    let donorDoc = configDonorsColl.findOne({tenantId: kTenantId});

    if (!res.ok) {
        assert.eq(donorDoc.state, TenantMigrationTest.State.kCommitted);
    } else {
        assert.eq(donorDoc.state, TenantMigrationTest.State.kAborted);
    }

    tenantMigrationTest.stop();
    donorRst.stopSet();
    recipientRst.stopSet();
}

(() => {
    jsTest.log("Test that the migration resumes on stepup");
    testDonorStartMigrationInterrupt((donorRst) => {
        // Force the primary to step down but make it likely to step back up.
        const donorPrimary = donorRst.getPrimary();
        assert.commandWorked(
            donorPrimary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
        assert.commandWorked(donorPrimary.adminCommand({replSetFreeze: 0}));
    });
})();

(() => {
    jsTest.log("Test that the migration resumes after restart");
    testDonorStartMigrationInterrupt((donorRst) => {
        donorRst.stopSet(null /* signal */, true /*forRestart */);
        donorRst.startSet({restart: true});
    });
})();

(() => {
    jsTest.log("Test that the donorForgetMigration command can be retried on stepup");
    testDonorForgetMigrationInterrupt((donorRst) => {
        // Force the primary to step down but make it likely to step back up.
        const donorPrimary = donorRst.getPrimary();
        assert.commandWorked(
            donorPrimary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
        assert.commandWorked(donorPrimary.adminCommand({replSetFreeze: 0}));
    });
})();

(() => {
    jsTest.log("Test that the donorForgetMigration command can be retried after restart");
    testDonorForgetMigrationInterrupt((donorRst) => {
        donorRst.stopSet(null /* signal */, true /*forRestart */);
        donorRst.startSet({restart: true});
    });
})();

(() => {
    jsTest.log("Test that the donorAbortMigration command can be retried after restart");

    kMigrationFpNames.forEach(fpName => {
        if (!fpName) {
            jsTest.log("Testing without setting a failpoint.");
        } else {
            jsTest.log("Testing with failpoint: " + fpName);
        }

        testDonorAbortMigrationInterrupt((donorRst) => {
            donorRst.stopSet(null /* signal */, true /*forRestart */);
            donorRst.startSet({restart: true});
        }, fpName, true);
    });
})();

(() => {
    jsTest.log("Test that the donorAbortMigration command can be retried on stepup");
    kMigrationFpNames.forEach(fpName => {
        if (!fpName) {
            jsTest.log("Testing without setting a failpoint.");
        } else {
            jsTest.log("Testing with failpoint: " + fpName);
        }

        testDonorAbortMigrationInterrupt((donorRst) => {
            // Force the primary to step down but make it likely to step back up.
            const donorPrimary = donorRst.getPrimary();
            assert.commandWorked(donorPrimary.adminCommand(
                {replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
            assert.commandWorked(donorPrimary.adminCommand({replSetFreeze: 0}));
        }, fpName);
    });
})();
})();
