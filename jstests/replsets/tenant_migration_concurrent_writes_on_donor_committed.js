/**
 * Tests that the donor blocks writes that are executed after the migration committed are rejected.
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
 * Tests that the donor rejects writes after the migration commits.
 */
function testRejectWritesAfterMigrationCommitted(testCase, testOpts) {
    const tenantId = testOpts.dbName.split('_')[0];
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId,
    };

    TenantMigrationTest.assertCommitted(tenantMigrationTest.runMigration(migrationOpts, {
        retryOnRetryableErrors: false,
        automaticForgetMigration: false,
        enableDonorStartMigrationFsync: true
    }));

    runCommandForConcurrentWritesTest(testOpts, ErrorCodes.TenantMigrationCommitted);
    testCase.assertCommandFailed(testOpts.primaryDB, testOpts.dbName, testOpts.collName);
    checkTenantMigrationAccessBlockerForConcurrentWritesTest(
        testOpts.primaryDB, tenantId, {numTenantMigrationCommittedErrors: 1});

    assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
    tenantMigrationTest.waitForMigrationGarbageCollection(migrationOpts.migrationIdString);
}

const testCases = TenantMigrationConcurrentWriteUtil.testCases;

// Run test cases after the migration has committed.
for (const [commandName, testCase] of Object.entries(testCases)) {
    let baseDbName = commandName + "-inCommitted0";

    if (testCase.skip) {
        print("Skipping " + commandName + ": " + testCase.skip);
        continue;
    }

    runTestForConcurrentWritesTest(donorPrimary,
                                   testCase,
                                   testRejectWritesAfterMigrationCommitted,
                                   baseDbName + "Basic_" + kTenantDefinedDbName,
                                   kCollName);

    if (testCase.testInTransaction) {
        runTestForConcurrentWritesTest(donorPrimary,
                                       testCase,
                                       testRejectWritesAfterMigrationCommitted,
                                       baseDbName + "Txn_" + kTenantDefinedDbName,
                                       kCollName,
                                       {testInTransaction: true});
    }

    if (testCase.testAsRetryableWrite) {
        runTestForConcurrentWritesTest(donorPrimary,
                                       testCase,
                                       testRejectWritesAfterMigrationCommitted,
                                       baseDbName + "Retryable_" + kTenantDefinedDbName,
                                       kCollName,
                                       {testAsRetryableWrite: true});
    }
}

tenantMigrationTest.stop();
})();
