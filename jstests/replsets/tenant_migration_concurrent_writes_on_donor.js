/**
 * Tests that writes on the donor set succeeds when there is no migration.
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
 * Tests that the write succeeds when there is no migration.
 */
function testWritesNoMigration(testCase, testOpts) {
    runCommandForConcurrentWritesTest(testOpts);
    testCase.assertCommandSucceeded(testOpts.primaryDB, testOpts.dbName, testOpts.collName);
}

const testCases = TenantMigrationConcurrentWriteUtil.testCases;

// Run test cases with no migration.
for (const [commandName, testCase] of Object.entries(testCases)) {
    if (testCase.skip) {
        print("Skipping " + commandName + ": " + testCase.skip);
        continue;
    }

    runTestForConcurrentWritesTest(donorPrimary,
                                   testCase,
                                   testWritesNoMigration,
                                   ObjectId().str + "_NoMigration-B-" + kTenantDefinedDbName,
                                   kCollName);

    if (testCase.testInTransaction) {
        runTestForConcurrentWritesTest(donorPrimary,
                                       testCase,
                                       testWritesNoMigration,
                                       ObjectId().str + "_NoMigration-T-" + kTenantDefinedDbName,
                                       kCollName,
                                       {testInTransaction: true});
    }

    if (testCase.testAsRetryableWrite) {
        runTestForConcurrentWritesTest(donorPrimary,
                                       testCase,
                                       testWritesNoMigration,
                                       ObjectId().str + "_NoMigration-R-" + kTenantDefinedDbName,
                                       kCollName,
                                       {testAsRetryableWrite: true});
    }
}

tenantMigrationTest.stop();
