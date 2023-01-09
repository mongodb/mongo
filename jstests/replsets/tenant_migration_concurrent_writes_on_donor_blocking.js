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

import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
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

const kMaxTimeMS = 1 * 1000;

const kTenantID = ObjectId().str;
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(UUID()),
    tenantId: kTenantID,
};

let countBlockedWrites = 0;

/**
 * Tests that the donor blocks writes that are executed in the blocking state.
 */
function testBlockWritesAfterMigrationEnteredBlocking_blocking(testOpts) {
    testOpts.command.maxTimeMS = kMaxTimeMS;
    runCommandForConcurrentWritesTest(testOpts, ErrorCodes.MaxTimeMSExpired);
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
function runTestsWhileBlocking() {
    for (const [commandName, testCase] of Object.entries(testCases)) {
        let baseDbName = kTenantID + "_" + commandName + "-Committed-";
        if (testCase.skip) {
            continue;
        }

        testBlockWritesAfterMigrationEnteredBlocking_blocking(
            testOptsMap[baseDbName + "B-" + kTenantDefinedDbName]);
        countBlockedWrites += 1;

        if (testCase.testInTransaction) {
            testBlockWritesAfterMigrationEnteredBlocking_blocking(
                testOptsMap[baseDbName + "T-" + kTenantDefinedDbName]);
            countBlockedWrites += 1;
        }

        if (testCase.testAsRetryableWrite) {
            testBlockWritesAfterMigrationEnteredBlocking_blocking(
                testOptsMap[baseDbName + "R-" + kTenantDefinedDbName]);
            countBlockedWrites += 1;
        }
    }
}

/**
 * Run the test cases after the migration has committed
 */
function runTestsAfterMigrationCommitted() {
    for (const [commandName, testCase] of Object.entries(testCases)) {
        let baseDbName = kTenantID + "_" + commandName + "-Committed-";
        if (testCase.skip) {
            continue;
        }

        const basicTesTOpts = testOptsMap[baseDbName + "B-" + kTenantDefinedDbName];
        testCase.assertCommandFailed(
            basicTesTOpts.primaryDB, basicTesTOpts.dbName, basicTesTOpts.collName);

        if (testCase.testInTransaction) {
            const txnTesTOpts = testOptsMap[baseDbName + "T-" + kTenantDefinedDbName];
            testCase.assertCommandFailed(
                txnTesTOpts.primaryDB, txnTesTOpts.dbName, txnTesTOpts.collName);
        }

        if (testCase.testAsRetryableWrite) {
            const retryableTestOpts = testOptsMap[baseDbName + "R-" + kTenantDefinedDbName];
            testCase.assertCommandFailed(
                retryableTestOpts.primaryDB, retryableTestOpts.dbName, retryableTestOpts.collName);
        }
    }
}

setupTestsBeforeMigration();

assert.commandWorked(
    tenantMigrationTest.startMigration(migrationOpts, {enableDonorStartMigrationFsync: true}));

// Run the command after the migration enters the blocking state.
let blockFp = configureFailPoint(donorPrimary, "pauseTenantMigrationBeforeLeavingBlockingState");
blockFp.wait();

// Run test cases while the migration is in blocking state.
runTestsWhileBlocking();

// Allow the migration to complete.
blockFp.off();
TenantMigrationTest.assertCommitted(tenantMigrationTest.waitForMigrationToComplete(
    migrationOpts, false /* retryOnRetryableErrors */));

// run test after blocking is over and the migration committed.
runTestsAfterMigrationCommitted();
checkTenantMigrationAccessBlockerForConcurrentWritesTest(
    donorPrimary, kTenantID, {numBlockedWrites: countBlockedWrites});

assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
tenantMigrationTest.waitForMigrationGarbageCollection(migrationOpts.migrationIdString);

tenantMigrationTest.stop();
