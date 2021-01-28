/**
 * Tests that tenant migrations are interrupted successfully on stepdown and shutdown.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, requires_persistence,
 * incompatible_with_eft, incompatible_with_windows_tls]
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

/**
 * Runs the donorStartMigration command to start a migration, and interrupts the migration on the
 * donor using the 'interruptFunc', and verifies the command response using the
 * 'verifyCmdResponseFunc'.
 */
function testDonorStartMigrationInterrupt(interruptFunc, verifyCmdResponseFunc) {
    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        return;
    }

    const donorRst = tenantMigrationTest.getDonorRst();
    const donorPrimary = tenantMigrationTest.getDonorPrimary();

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: kTenantId,
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
    };

    const donorRstArgs = TenantMigrationUtil.createRstArgs(donorRst);

    const runMigrationThread =
        new Thread(TenantMigrationUtil.runMigrationAsync, migrationOpts, donorRstArgs);
    runMigrationThread.start();

    // Wait for donorStartMigration command to start.
    assert.soon(() => donorPrimary.adminCommand({currentOp: true, desc: "tenant donor migration"})
                          .inprog.length > 0);

    sleep(Math.random() * kMaxSleepTimeMS);
    interruptFunc(donorRst, migrationId, kTenantId);
    verifyCmdResponseFunc(runMigrationThread);

    tenantMigrationTest.stop();
}

/**
 * Starts a migration and waits for it to commit, then runs the donorForgetMigration, and interrupts
 * the donor using the 'interruptFunc', and verifies the command response using the
 * 'verifyCmdResponseFunc'.
 */
function testDonorForgetMigrationInterrupt(interruptFunc, verifyCmdResponseFunc) {
    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        return;
    }

    const donorRst = tenantMigrationTest.getDonorRst();
    const donorPrimary = tenantMigrationTest.getDonorPrimary();

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: kTenantId,
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
    };

    const donorRstArgs = TenantMigrationUtil.createRstArgs(donorRst);

    assert.commandWorked(tenantMigrationTest.runMigration(
        migrationOpts, false /* retryOnRetryableErrors */, false /* automaticForgetMigration */));
    const forgetMigrationThread = new Thread(
        TenantMigrationUtil.forgetMigrationAsync, migrationOpts.migrationIdString, donorRstArgs);
    forgetMigrationThread.start();

    // Wait for the donorForgetMigration command to start.
    assert.soon(() => {
        const res = assert.commandWorked(
            donorPrimary.adminCommand({currentOp: true, desc: "tenant donor migration"}));
        return res.inprog[0].expireAt != null;
    });

    sleep(Math.random() * kMaxSleepTimeMS);
    interruptFunc(donorRst, migrationId, migrationOpts.tenantId);
    verifyCmdResponseFunc(forgetMigrationThread);

    tenantMigrationTest.stop();
}

/**
 * Starts a migration and sets the passed in failpoint during the migration, then runs the
 * donorAbortMigration, and interrupts the donor using the 'interruptFunc', and verifies the command
 * response using the 'verifyCmdResponseFunc'.
 */
function testDonorAbortMigrationInterrupt(interruptFunc, verifyCmdResponseFunc, fpName) {
    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
    if (!tenantMigrationTest.isFeatureFlagEnabled()) {
        jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
        return;
    }

    const donorRst = tenantMigrationTest.getDonorRst();
    const donorPrimary = tenantMigrationTest.getDonorPrimary();

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: kTenantId,
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
    };

    const donorRstArgs = TenantMigrationUtil.createRstArgs(donorRst);

    // If we passed in a valid failpoint we set it, otherwise we let the migration run normally.
    if (fpName) {
        configureFailPoint(donorPrimary, fpName);
    }

    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

    const tryAbortThread = new Thread(TenantMigrationUtil.tryAbortMigrationAsync,
                                      {migrationIdString: migrationOpts.migrationIdString},
                                      donorRstArgs);
    tryAbortThread.start();

    // Wait for donorAbortMigration command to start.
    assert.soon(() => {
        const res = assert.commandWorked(
            donorPrimary.adminCommand({currentOp: true, desc: "tenant donor migration"}));
        return res.inprog[0].receivedCancelation;
    });

    interruptFunc(donorRst, migrationId, migrationOpts.tenantId);
    verifyCmdResponseFunc(tryAbortThread);

    tenantMigrationTest.stop();
}

/**
 * Asserts the command either succeeded or failed with a NotPrimary error.
 */
function assertCmdSucceededOrInterruptedDueToStepDown(cmdThread) {
    const res = cmdThread.returnData();
    assert(res.ok || ErrorCodes.isNotPrimaryError(res.code));
}

/**
 * Asserts the command either succeeded or failed with a NotPrimary or shutdown or network error.
 */
function assertCmdSucceededOrInterruptedDueToShutDown(cmdThread) {
    const res = cmdThread.returnData();
    try {
        assert(res.ok || ErrorCodes.isNotPrimaryError(res.code) ||
               ErrorCodes.isShutdownError(res.code));
    } catch (e) {
        if (isNetworkError(e)) {
            jsTestLog(`Ignoring network error due to node shutting down ${tojson(e)}`);
        } else {
            throw e;
        }
    }
}

(() => {
    jsTest.log("Test that the donorStartMigration command is interrupted successfully on stepdown");
    testDonorStartMigrationInterrupt((donorRst) => {
        assert.commandWorked(
            donorRst.getPrimary().adminCommand({replSetStepDown: 1000, force: true}));
    }, assertCmdSucceededOrInterruptedDueToStepDown);
})();

(() => {
    jsTest.log("Test that the donorStartMigration command is interrupted successfully on shutdown");
    testDonorStartMigrationInterrupt((donorRst) => {
        donorRst.stopSet();
    }, assertCmdSucceededOrInterruptedDueToShutDown);
})();

(() => {
    jsTest.log("Test that the donorForgetMigration is interrupted successfully on stepdown");
    testDonorForgetMigrationInterrupt((donorRst) => {
        assert.commandWorked(
            donorRst.getPrimary().adminCommand({replSetStepDown: 1000, force: true}));
    }, assertCmdSucceededOrInterruptedDueToStepDown);
})();

(() => {
    jsTest.log("Test that the donorForgetMigration is interrupted successfully on shutdown");
    testDonorForgetMigrationInterrupt((donorRst) => {
        donorRst.stopSet();
    }, assertCmdSucceededOrInterruptedDueToShutDown);
})();

(() => {
    jsTest.log("Test that the donorAbortMigration is interrupted successfully on stepdown");
    kMigrationFpNames.forEach(fpName => {
        if (!fpName) {
            jsTest.log("Testing without setting a failpoint.");
        } else {
            jsTest.log("Testing with failpoint: " + fpName);
        }

        testDonorAbortMigrationInterrupt((donorRst) => {
            assert.commandWorked(
                donorRst.getPrimary().adminCommand({replSetStepDown: 1000, force: true}));
        }, assertCmdSucceededOrInterruptedDueToStepDown, fpName);
    });
})();

(() => {
    jsTest.log("Test that the donorAbortMigration is interrupted successfully on shutdown");
    kMigrationFpNames.forEach(fpName => {
        if (!fpName) {
            jsTest.log("Testing without setting a failpoint.");
        } else {
            jsTest.log("Testing with failpoint: " + fpName);
        }

        testDonorAbortMigrationInterrupt((donorRst) => {
            donorRst.stopSet();
        }, assertCmdSucceededOrInterruptedDueToShutDown, fpName);
    });
})();
})();
