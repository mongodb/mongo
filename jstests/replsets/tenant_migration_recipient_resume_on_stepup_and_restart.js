/**
 * Tests that tenant migrations resume successfully on recipient stepup and restart.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   incompatible_with_shard_merge,
 *   # Some tenant migration statistics field names were changed in 6.1.
 *   requires_fcv_61,
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
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");
load('jstests/replsets/rslib.js');  // 'createRstArgs'

const kMaxSleepTimeMS = 100;
const kTenantId = ObjectId().str;

// Set the delay before a state doc is garbage collected to be short to speed up the test but long
// enough for the state doc to still be around after stepup or restart.
const kGarbageCollectionDelayMS = 30 * 1000;

// Set the TTL monitor to run at a smaller interval to speed up the test.
const kTTLMonitorSleepSecs = 1;

const migrationX509Options = makeX509OptionsForTest();

/**
 * Runs the donorStartMigration command to start a migration, and interrupts the migration on the
 * recipient using the 'interruptFunc' after the migration starts on the recipient side, and
 * asserts that migration eventually commits.
 * @param {recipientRestarted} bool is needed to properly assert the tenant migrations stat count.
 */
function testRecipientSyncDataInterrupt(interruptFunc, recipientRestarted) {
    const recipientRst = new ReplSetTest({
        nodes: 3,
        name: "recipientRst",
        serverless: true,
        nodeOptions: migrationX509Options.recipient
    });
    recipientRst.startSet();
    recipientRst.initiate();

    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName(), recipientRst});

    const donorRst = tenantMigrationTest.getDonorRst();
    const donorPrimary = tenantMigrationTest.getDonorPrimary();
    let recipientPrimary = tenantMigrationTest.getRecipientPrimary();

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: kTenantId,
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
    };
    const donorRstArgs = createRstArgs(donorRst);

    const runMigrationThread = new Thread(runMigrationAsync, migrationOpts, donorRstArgs);
    runMigrationThread.start();

    // Wait for recipientSyncData command to start.
    assert.soon(
        () => recipientPrimary.adminCommand({currentOp: true, desc: "tenant recipient migration"})
                  .inprog.length > 0);

    sleep(Math.random() * kMaxSleepTimeMS);
    interruptFunc(recipientRst);

    TenantMigrationTest.assertCommitted(runMigrationThread.returnData());
    tenantMigrationTest.waitForDonorNodesToReachState(donorRst.nodes,
                                                      migrationId,
                                                      migrationOpts.tenantId,
                                                      TenantMigrationTest.DonorState.kCommitted);
    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

    const donorStats = tenantMigrationTest.getTenantMigrationStats(donorPrimary);
    assert.eq(1, donorStats.totalMigrationDonationsCommitted);

    tenantMigrationTest.stop();
    recipientRst.stopSet();
}

/**
 * Starts a migration and waits for it to commit, then runs the donorForgetMigration, and interrupts
 * the recipient using the 'interruptFunc', and asserts that the migration state is eventually
 * garbage collected.
 */
function testRecipientForgetMigrationInterrupt(interruptFunc) {
    const donorRst = new ReplSetTest({
        nodes: 1,
        name: "donorRst",
        serverless: true,
        nodeOptions: Object.assign({}, migrationX509Options.donor, {
            setParameter: {
                tenantMigrationGarbageCollectionDelayMS: kGarbageCollectionDelayMS,
                ttlMonitorSleepSecs: kTTLMonitorSleepSecs,
            }
        })
    });
    const recipientRst = new ReplSetTest({
        nodes: 3,
        name: "recipientRst",
        serverless: true,
        nodeOptions: Object.assign({}, migrationX509Options.recipient, {
            setParameter: {
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
    const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: kTenantId,
        recipientConnString: recipientRst.getURL(),
    };
    const donorRstArgs = createRstArgs(donorRst);

    TenantMigrationTest.assertCommitted(
        tenantMigrationTest.runMigration(migrationOpts, {automaticForgetMigration: false}));
    const forgetMigrationThread = new Thread(forgetMigrationAsync,
                                             migrationOpts.migrationIdString,
                                             donorRstArgs,
                                             false /* retryOnRetryableErrors */);
    forgetMigrationThread.start();

    // Wait for recipientForgetMigration command to start.
    assert.soon(() => {
        const res = assert.commandWorked(
            recipientPrimary.adminCommand({currentOp: true, desc: "tenant recipient migration"}));
        return res.inprog[0].expireAt != null;
    });
    sleep(Math.random() * kMaxSleepTimeMS);
    interruptFunc(recipientRst);

    assert.commandWorkedOrFailedWithCode(
        tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString),
        ErrorCodes.NoSuchTenantMigration);

    assert.commandWorked(forgetMigrationThread.returnData());
    tenantMigrationTest.waitForMigrationGarbageCollection(migrationId, migrationOpts.tenantId);

    tenantMigrationTest.stop();
    donorRst.stopSet();
    recipientRst.stopSet();
}

(() => {
    jsTest.log("Test that the migration resumes on stepup");
    testRecipientSyncDataInterrupt((recipientRst) => {
        // Force the primary to step down but make it likely to step back up.
        const recipientPrimary = recipientRst.getPrimary();
        assert.commandWorked(recipientPrimary.adminCommand(
            {replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
        assert.commandWorked(recipientPrimary.adminCommand({replSetFreeze: 0}));
    }, false);
})();

(() => {
    jsTest.log("Test that the migration resumes after restart");
    testRecipientSyncDataInterrupt((recipientRst) => {
        recipientRst.stopSet(null /* signal */, true /*forRestart */);
        recipientRst.startSet({restart: true});
        recipientRst.awaitSecondaryNodes();
        recipientRst.getPrimary();
    }, true);
})();

(() => {
    jsTest.log("Test that the recipientForgetMigration command can be retried on stepup");
    testRecipientForgetMigrationInterrupt((recipientRst) => {
        // Force the primary to step down but make it likely to step back up.
        const recipientPrimary = recipientRst.getPrimary();
        assert.commandWorked(recipientPrimary.adminCommand(
            {replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
        assert.commandWorked(recipientPrimary.adminCommand({replSetFreeze: 0}));
    });
})();

(() => {
    jsTest.log("Test that the recipientForgetMigration command can be retried after restart");
    testRecipientForgetMigrationInterrupt((recipientRst) => {
        recipientRst.stopSet(null /* signal */, true /*forRestart */);
        recipientRst.startSet({restart: true});
        recipientRst.awaitSecondaryNodes();
        recipientRst.getPrimary();
    });
})();
