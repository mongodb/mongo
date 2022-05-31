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

const testCases = TenantMigrationConcurrentWriteUtil.testCases;
const kTenantID = "tenantId";

const migrationOpts = {
    migrationIdString: extractUUIDFromObject(UUID()),
    tenantId: kTenantID
};

/**
 * Tests that the donor rejects writes after a migration has already committed.
 */
let countTenantMigrationCommittedErrors = 0;
function testRejectWritesAfterMigrationCommitted(testCase, testOpts) {
    runCommandForConcurrentWritesTest(testOpts, ErrorCodes.TenantMigrationCommitted);
    testCase.assertCommandFailed(testOpts.primaryDB, testOpts.dbName, testOpts.collName);
}

const testOptsMap = {};
/**
 * run the setup for each cases before the migration starts
 */
function setupTestsBeforeMigration() {
    for (const [commandName, testCase] of Object.entries(testCases)) {
        let baseDbName = kTenantID + "_" + commandName + "-inCommitted0";

        if (testCase.skip) {
            print("Skipping " + commandName + ": " + testCase.skip);
            continue;
        }

        let basicFullDb = baseDbName + "Basic-" + kTenantDefinedDbName;
        const basicTestOpts = makeTestOptionsForConcurrentWritesTest(
            donorPrimary, testCase, basicFullDb, kCollName, false, false);
        testOptsMap[basicFullDb] = basicTestOpts;
        setupTestForConcurrentWritesTest(testCase, kCollName, basicTestOpts);

        if (testCase.testInTransaction) {
            let TxnFullDb = baseDbName + "Txn-" + kTenantDefinedDbName;
            const txnTestOpts = makeTestOptionsForConcurrentWritesTest(
                donorPrimary, testCase, TxnFullDb, kCollName, true, false);
            testOptsMap[TxnFullDb] = txnTestOpts;
            setupTestForConcurrentWritesTest(testCase, kCollName, txnTestOpts);
        }

        if (testCase.testAsRetryableWrite) {
            let retryableFullDb = baseDbName + "Retryable-" + kTenantDefinedDbName;
            const retryableTestOpts = makeTestOptionsForConcurrentWritesTest(
                donorPrimary, testCase, retryableFullDb, kCollName, false, true);
            testOptsMap[retryableFullDb] = retryableTestOpts;
            setupTestForConcurrentWritesTest(testCase, kCollName, retryableTestOpts);
        }
    }
}

/**
 * Run the test cases after the migration has committed.
 */
function runTestsAfterMigration() {
    for (const [commandName, testCase] of Object.entries(testCases)) {
        let baseDbName = kTenantID + "_" + commandName + "-inCommitted0";
        if (testCase.skip) {
            continue;
        }

        const basicTesTOpts = testOptsMap[baseDbName + "Basic-" + kTenantDefinedDbName];
        testRejectWritesAfterMigrationCommitted(testCase, basicTesTOpts);
        countTenantMigrationCommittedErrors += 1;

        if (testCase.testInTransaction) {
            const txnTesTOpts = testOptsMap[baseDbName + "Txn-" + kTenantDefinedDbName];
            testRejectWritesAfterMigrationCommitted(testCase, txnTesTOpts);
            countTenantMigrationCommittedErrors += 1;
        }

        if (testCase.testAsRetryableWrite) {
            const retryableTestOpts = testOptsMap[baseDbName + "Retryable-" + kTenantDefinedDbName];
            testRejectWritesAfterMigrationCommitted(testCase, retryableTestOpts);
            countTenantMigrationCommittedErrors += 1;
        }
    }
}

setupTestsBeforeMigration();

// verify the migration commits.
TenantMigrationTest.assertCommitted(tenantMigrationTest.runMigration(migrationOpts, {
    retryOnRetryableErrors: false,
    automaticForgetMigration: false,
    enableDonorStartMigrationFsync: true
}));

// run the tests after the migration has committed.
runTestsAfterMigration();

checkTenantMigrationAccessBlockerForConcurrentWritesTest(donorPrimary, kTenantID, {
    numTenantMigrationCommittedErrors: countTenantMigrationCommittedErrors
});

// cleanup the migration
assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
tenantMigrationTest.waitForMigrationGarbageCollection(migrationOpts.migrationIdString);

tenantMigrationTest.stop();
})();
