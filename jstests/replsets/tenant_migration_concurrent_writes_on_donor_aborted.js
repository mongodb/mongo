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

import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {getTenantMigrationAccessBlocker} from "jstests/replsets/libs/tenant_migration_util.js";
import {
    checkTenantMigrationAccessBlockerForConcurrentWritesTest,
    makeTestOptionsForConcurrentWritesTest,
    runCommandForConcurrentWritesTest,
    setupTestForConcurrentWritesTest,
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

const kTenantID = ObjectId().str;
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(UUID()),
    tenantId: kTenantID,
};

/**
 * Tests that the donor does not reject writes after the migration aborts.
 */
function testDoNotRejectWritesAfterMigrationAborted(testCase, testOpts) {
    const tenantId = testOpts.dbName.split('_')[0];

    // Wait until the in-memory migration state is updated after the migration has majority
    // committed the abort decision. Otherwise, the command below is expected to block and then get
    // rejected.
    assert.soon(() => {
        const mtab = getTenantMigrationAccessBlocker({donorNode: testOpts.primaryDB, tenantId});
        return mtab.donor.state === TenantMigrationTest.DonorAccessState.kAborted;
    });

    runCommandForConcurrentWritesTest(testOpts);
    testCase.assertCommandSucceeded(testOpts.primaryDB, testOpts.dbName, testOpts.collName);
    checkTenantMigrationAccessBlockerForConcurrentWritesTest(
        testOpts.primaryDB, tenantId, {numTenantMigrationAbortedErrors: 0});
}

const testCases = TenantMigrationConcurrentWriteUtil.testCases;
const testOptsMap = {};

/**
 * run the setup for each cases before the migration starts
 */
function setupTestsBeforeMigration() {
    for (const [commandName, testCase] of Object.entries(testCases)) {
        let baseDbName = kTenantID + "_" + commandName + "-Committed-";

        if (testCase.skip) {
            print("Skipping " + commandName + ": " + testCase.skip);
            continue;
        }

        let basicFullDb = baseDbName + "B-" + kTenantDefinedDbName;
        const basicTestOpts = makeTestOptionsForConcurrentWritesTest(
            donorPrimary, testCase, basicFullDb, kCollName, false, false);
        testOptsMap[basicFullDb] = basicTestOpts;
        setupTestForConcurrentWritesTest(testCase, kCollName, basicTestOpts);

        if (testCase.testInTransaction) {
            let TxnFullDb = baseDbName + "T-" + kTenantDefinedDbName;
            const txnTestOpts = makeTestOptionsForConcurrentWritesTest(
                donorPrimary, testCase, TxnFullDb, kCollName, true, false);
            testOptsMap[TxnFullDb] = txnTestOpts;
            setupTestForConcurrentWritesTest(testCase, kCollName, txnTestOpts);
        }

        if (testCase.testAsRetryableWrite) {
            let retryableFullDb = baseDbName + "R-" + kTenantDefinedDbName;
            const retryableTestOpts = makeTestOptionsForConcurrentWritesTest(
                donorPrimary, testCase, retryableFullDb, kCollName, false, true);
            testOptsMap[retryableFullDb] = retryableTestOpts;
            setupTestForConcurrentWritesTest(testCase, kCollName, retryableTestOpts);
        }
    }
}

/**
 * Run the test cases after the migration has committed
 */
function runTestsAfterMigration() {
    for (const [commandName, testCase] of Object.entries(testCases)) {
        let baseDbName = kTenantID + "_" + commandName + "-Committed-";
        if (testCase.skip) {
            continue;
        }

        const basicTesTOpts = testOptsMap[baseDbName + "B-" + kTenantDefinedDbName];
        testDoNotRejectWritesAfterMigrationAborted(testCase, basicTesTOpts);

        if (testCase.testInTransaction) {
            const txnTesTOpts = testOptsMap[baseDbName + "T-" + kTenantDefinedDbName];
            testDoNotRejectWritesAfterMigrationAborted(testCase, txnTesTOpts);
        }

        if (testCase.testAsRetryableWrite) {
            const retryableTestOpts = testOptsMap[baseDbName + "R-" + kTenantDefinedDbName];
            testDoNotRejectWritesAfterMigrationAborted(testCase, retryableTestOpts);
        }
    }
}

setupTestsBeforeMigration();

let abortFp = configureFailPoint(donorPrimary, "abortTenantMigrationBeforeLeavingBlockingState");

// verify the migration aborts.
TenantMigrationTest.assertAborted(tenantMigrationTest.runMigration(migrationOpts, {
    retryOnRetryableErrors: false,
    automaticForgetMigration: false,
    enableDonorStartMigrationFsync: true
}));

// Allow the migration to complete and abort.
abortFp.off();

// run test after the migration has aborted.
runTestsAfterMigration();

assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
tenantMigrationTest.waitForMigrationGarbageCollection(migrationOpts.migrationIdString);

tenantMigrationTest.stop();
