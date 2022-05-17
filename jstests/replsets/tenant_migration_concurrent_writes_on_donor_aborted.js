/**
 * Tests that the donor accepts writes after the migration aborts.
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

/**
 * Tests that the donor does not reject writes after the migration aborts.
 */
function testDoNotRejectWritesAfterMigrationAborted(testCase, testOpts) {
    const tenantId = testOpts.dbName.split('_')[0];
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId,
    };

    let abortFp =
        configureFailPoint(testOpts.primaryDB, "abortTenantMigrationBeforeLeavingBlockingState");
    TenantMigrationTest.assertAborted(tenantMigrationTest.runMigration(migrationOpts, {
        retryOnRetryableErrors: false,
        automaticForgetMigration: false,
        enableDonorStartMigrationFsync: true
    }));
    abortFp.off();

    // Wait until the in-memory migration state is updated after the migration has majority
    // committed the abort decision. Otherwise, the command below is expected to block and then get
    // rejected.
    assert.soon(() => {
        const mtab = TenantMigrationUtil.getTenantMigrationAccessBlocker(
            {donorNode: testOpts.primaryDB, tenantId});
        return mtab.donor.state === TenantMigrationTest.DonorAccessState.kAborted;
    });

    runCommandForConcurrentWritesTest(testOpts);
    testCase.assertCommandSucceeded(testOpts.primaryDB, testOpts.dbName, testOpts.collName);
    checkTenantMigrationAccessBlockerForConcurrentWritesTest(
        testOpts.primaryDB, tenantId, {numTenantMigrationAbortedErrors: 0});

    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
    tenantMigrationTest.waitForMigrationGarbageCollection(migrationOpts.migrationIdString);
}

const testCases = TenantMigrationConcurrentWriteUtil.testCases;

// Run test cases after an aborted migration.
for (const [commandName, testCase] of Object.entries(testCases)) {
    let baseDbName = commandName + "-inAborted0";

    if (testCase.skip) {
        print("Skipping " + commandName + ": " + testCase.skip);
        continue;
    }

    runTestForConcurrentWritesTest(donorPrimary,
                                   testCase,
                                   testDoNotRejectWritesAfterMigrationAborted,
                                   baseDbName + "Basic_" + kTenantDefinedDbName,
                                   kCollName);

    if (testCase.testInTransaction) {
        runTestForConcurrentWritesTest(donorPrimary,
                                       testCase,
                                       testDoNotRejectWritesAfterMigrationAborted,
                                       baseDbName + "Txn_" + kTenantDefinedDbName,
                                       kCollName,
                                       {testInTransaction: true});
    }

    if (testCase.testAsRetryableWrite) {
        runTestForConcurrentWritesTest(donorPrimary,
                                       testCase,
                                       testDoNotRejectWritesAfterMigrationAborted,
                                       baseDbName + "Retryable_" + kTenantDefinedDbName,
                                       kCollName,
                                       {testAsRetryableWrite: true});
    }
}

tenantMigrationTest.stop();
})();
