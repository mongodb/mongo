/**
 * Tests that the donor blocks writes that are executed while the migration in the blocking state,
 * then rejects the writes when the migration completes.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 * ]
 */
(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");
load("jstests/replsets/tenant_migration_concurrent_writes_on_donor_util.js");

const tenantMigrationTest = new TenantMigrationTest({
    name: jsTestName(),
    quickGarbageCollection: true,
});

const donorRst = tenantMigrationTest.getDonorRst();
const donorPrimary = donorRst.getPrimary();

const kCollName = "testColl";

const kTenantDefinedDbName = "0";

const kMaxTimeMS = 1 * 1000;

/**
 * Tests that the donor blocks writes that are executed in the blocking state.
 */
function testBlockWritesAfterMigrationEnteredBlocking(testCase, testOpts) {
    const tenantId = testOpts.dbName.split('_')[0];
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId,
    };

    let blockingFp =
        configureFailPoint(testOpts.primaryDB, "pauseTenantMigrationBeforeLeavingBlockingState");

    assert.commandWorked(
        tenantMigrationTest.startMigration(migrationOpts, {enableDonorStartMigrationFsync: true}));

    // Run the command after the migration enters the blocking state.
    blockingFp.wait();
    testOpts.command.maxTimeMS = kMaxTimeMS;
    runCommandForConcurrentWritesTest(testOpts, ErrorCodes.MaxTimeMSExpired);

    // Allow the migration to complete.
    blockingFp.off();
    TenantMigrationTest.assertCommitted(tenantMigrationTest.waitForMigrationToComplete(
        migrationOpts, false /* retryOnRetryableErrors */));

    testCase.assertCommandFailed(testOpts.primaryDB, testOpts.dbName, testOpts.collName);
    checkTenantMigrationAccessBlockerForConcurrentWritesTest(
        testOpts.primaryDB, tenantId, {numBlockedWrites: 1});

    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
    tenantMigrationTest.waitForMigrationGarbageCollection(migrationOpts.migrationIdString);
}

const testCases = TenantMigrationConcurrentWriteUtil.testCases;

// Run test cases while the migration is in blocking state.
for (const [commandName, testCase] of Object.entries(testCases)) {
    let baseDbName = commandName + "-inBlocking0";

    if (testCase.skip) {
        print("Skipping " + commandName + ": " + testCase.skip);
        continue;
    }

    runTestForConcurrentWritesTest(donorPrimary,
                                   testCase,
                                   testBlockWritesAfterMigrationEnteredBlocking,
                                   baseDbName + "Basic_" + kTenantDefinedDbName,
                                   kCollName);

    if (testCase.testInTransaction) {
        runTestForConcurrentWritesTest(donorPrimary,
                                       testCase,
                                       testBlockWritesAfterMigrationEnteredBlocking,
                                       baseDbName + "Txn_" + kTenantDefinedDbName,
                                       kCollName,
                                       {testInTransaction: true});
    }

    if (testCase.testAsRetryableWrite) {
        runTestForConcurrentWritesTest(donorPrimary,
                                       testCase,
                                       testBlockWritesAfterMigrationEnteredBlocking,
                                       baseDbName + "Retryable_" + kTenantDefinedDbName,
                                       kCollName,
                                       {testAsRetryableWrite: true});
    }
}

tenantMigrationTest.stop();
})();
