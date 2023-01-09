/**
 * Tests that the donor blocks writes that are executed while the migration in the blocking state,
 * then rejects the writes when the migration aborted.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 * ]
 */

import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {
    checkTenantMigrationAccessBlockerForConcurrentWritesTest,
    runCommandForConcurrentWritesTest,
    runTestForConcurrentWritesTest,
    TenantMigrationConcurrentWriteUtil
} from "jstests/replsets/tenant_migration_concurrent_writes_on_donor_util.js";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");

const tenantMigrationTest = new TenantMigrationTest({
    name: jsTestName(),
    quickGarbageCollection: true,
});

const donorRst = tenantMigrationTest.getDonorRst();
const donorPrimary = donorRst.getPrimary();

const kCollName = "testColl";

const kTenantDefinedDbName = "0";

/**
 * To be used to resume a migration that is paused after entering the blocking state. Waits for the
 * number of blocked reads to reach 'targetNumBlockedWrites' and unpauses the migration.
 */
async function resumeMigrationAfterBlockingWrite(host, tenantId, targetNumBlockedWrites) {
    const {getNumBlockedWrites} = await import("jstests/replsets/libs/tenant_migration_util.js");
    const primary = new Mongo(host);
    assert.soon(() => getNumBlockedWrites(primary, tenantId) == targetNumBlockedWrites);
    assert.commandWorked(primary.adminCommand(
        {configureFailPoint: "pauseTenantMigrationBeforeLeavingBlockingState", mode: "off"}));
}

/**
 * Tests that the donor blocks writes that are executed in the blocking state and rejects them after
 * the migration aborts.
 */
function testRejectBlockedWritesAfterMigrationAborted(testCase, testOpts) {
    const tenantId = testOpts.dbName.split('_')[0];
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId,
    };

    let blockingFp =
        configureFailPoint(testOpts.primaryDB, "pauseTenantMigrationBeforeLeavingBlockingState");
    let abortFp =
        configureFailPoint(testOpts.primaryDB, "abortTenantMigrationBeforeLeavingBlockingState");

    let resumeMigrationThread =
        new Thread(resumeMigrationAfterBlockingWrite, testOpts.primaryHost, tenantId, 1);

    // Run the command after the migration enters the blocking state.
    assert.commandWorked(
        tenantMigrationTest.startMigration(migrationOpts, {enableDonorStartMigrationFsync: true}));
    resumeMigrationThread.start();
    blockingFp.wait();

    // The migration should unpause and abort after the write is blocked. Verify that the write is
    // rejected.
    runCommandForConcurrentWritesTest(testOpts, ErrorCodes.TenantMigrationAborted);

    // Verify that the migration aborted due to the simulated error.
    resumeMigrationThread.join();
    TenantMigrationTest.assertAborted(tenantMigrationTest.waitForMigrationToComplete(
        migrationOpts, false /* retryOnRetryableErrors */));
    abortFp.off();

    testCase.assertCommandFailed(testOpts.primaryDB, testOpts.dbName, testOpts.collName);
    checkTenantMigrationAccessBlockerForConcurrentWritesTest(
        testOpts.primaryDB, tenantId, {numBlockedWrites: 1, numTenantMigrationAbortedErrors: 1});

    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
    tenantMigrationTest.waitForMigrationGarbageCollection(migrationOpts.migrationIdString);
}

const testCases = TenantMigrationConcurrentWriteUtil.testCases;

// Run test cases while the migration is blocked and then rejects after aborted.
for (const [commandName, testCase] of Object.entries(testCases)) {
    if (testCase.skip) {
        print("Skipping " + commandName + ": " + testCase.skip);
        continue;
    }

    runTestForConcurrentWritesTest(donorPrimary,
                                   testCase,
                                   testRejectBlockedWritesAfterMigrationAborted,
                                   ObjectId().str + "_Blocking-B-" + kTenantDefinedDbName,
                                   kCollName);

    if (testCase.testInTransaction) {
        runTestForConcurrentWritesTest(donorPrimary,
                                       testCase,
                                       testRejectBlockedWritesAfterMigrationAborted,
                                       ObjectId().str + "_Blocking-T-" + kTenantDefinedDbName,
                                       kCollName,
                                       {testInTransaction: true});
    }

    if (testCase.testAsRetryableWrite) {
        runTestForConcurrentWritesTest(donorPrimary,
                                       testCase,
                                       testRejectBlockedWritesAfterMigrationAborted,
                                       ObjectId().str + "_Blocking-R-" + kTenantDefinedDbName,
                                       kCollName,
                                       {testAsRetryableWrite: true});
    }
}

tenantMigrationTest.stop();
