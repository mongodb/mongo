/**
 * Tests that the donor blocks writes that are executed after the shard split committed state are
 * rejected.
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 *   requires_fcv_62
 * ]
 */
(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/tenant_migration_concurrent_writes_on_donor_util.js");
load("jstests/serverless/libs/shard_split_test.js");

TestData.skipCheckDBHashes = true;
const recipientTagName = "recipientNode";
const recipientSetName = "recipient";
const tenantMigrationTest = new ShardSplitTest({
    recipientTagName,
    recipientSetName,
    quickGarbageCollection: true,
    allowStaleReadsOnDonor: true,
    initiateWithShortElectionTimeout: true
});

const donorPrimary = tenantMigrationTest.getDonorPrimary();

const kCollName = "testColl";
const kTenantDefinedDbName = "0";

const testCases = TenantMigrationConcurrentWriteUtil.testCases;
const kTenantID = "tenantId";

let countTenantMigrationCommittedErrors = 0;

function setupTest(testCase, collName, testOpts) {
    if (testCase.explicitlyCreateCollection) {
        createCollectionAndInsertDocsForConcurrentWritesTest(
            testOpts.primaryDB, collName, testCase.isCapped);
    }

    if (testCase.setUp) {
        testCase.setUp(testOpts.primaryDB, collName, testOpts.testInTransaction);
    }
}

/**
 * Tests that the donor rejects writes after a migration has already committed.
 */
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
        setupTest(testCase, kCollName, basicTestOpts);

        if (testCase.testInTransaction) {
            let TxnFullDb = baseDbName + "Txn-" + kTenantDefinedDbName;
            const txnTestOpts = makeTestOptionsForConcurrentWritesTest(
                donorPrimary, testCase, TxnFullDb, kCollName, true, false);
            testOptsMap[TxnFullDb] = txnTestOpts;
            setupTest(testCase, kCollName, txnTestOpts);
        }

        if (testCase.testAsRetryableWrite) {
            let retryableFullDb = baseDbName + "Retryable-" + kTenantDefinedDbName;
            const retryableTestOpts = makeTestOptionsForConcurrentWritesTest(
                donorPrimary, testCase, retryableFullDb, kCollName, false, true);
            testOptsMap[retryableFullDb] = retryableTestOpts;
            setupTest(testCase, kCollName, retryableTestOpts);
        }
    }
}

/**
 * Run the test cases after the migration has committed
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

tenantMigrationTest.addRecipientNodes();
const tenantIds = [kTenantID];
const operation = tenantMigrationTest.createSplitOperation(tenantIds);

setupTestsBeforeMigration();

assert.commandWorked(
    operation.commit({retryOnRetryableErrors: false}, {enableDonorStartMigrationFsync: true}));

runTestsAfterMigration();
ShardSplitTest.checkShardSplitAccessBlocker(donorPrimary, kTenantID, {
    numTenantMigrationCommittedErrors: countTenantMigrationCommittedErrors
});

tenantMigrationTest.stop();
})();
