/**
 * Tests that the donor blocks writes that are executed while the shard split in the blocking state,
 * then rejects the writes when the migration completes.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 *   requires_fcv_63
 * ]
 */

import {
    createCollectionAndInsertDocsForConcurrentWritesTest,
    makeTestOptionsForConcurrentWritesTest,
    runCommandForConcurrentWritesTest,
    TenantMigrationConcurrentWriteUtil
} from "jstests/replsets/tenant_migration_concurrent_writes_on_donor_util.js";
import {ShardSplitTest} from "jstests/serverless/libs/shard_split_test.js";

load("jstests/libs/fail_point_util.js");

TestData.skipCheckDBHashes = true;
const shardSplitTest = new ShardSplitTest({
    quickGarbageCollection: true,
    allowStaleReadsOnDonor: true,
    // Increase timeout because blocking in the critical section contributes to operation latency.
    nodeOptions: {setParameter: {shardSplitTimeoutMS: 100000}}
});

const donorPrimary = shardSplitTest.getDonorPrimary();

const kCollName = "testColl";
const kTenantDefinedDbName = "0";

const testCases = TenantMigrationConcurrentWriteUtil.testCases;
const kTenantID = ObjectId();

const kMaxTimeMS = 1 * 1000;

let countBlockedWrites = 0;

/**
 * Tests that the donor blocks writes that are executed in the blocking state and increase the
 * countBlockedWrites count.
 */
function testBlockWritesAfterMigrationEnteredBlocking(testOpts) {
    testOpts.command.maxTimeMS = kMaxTimeMS;
    runCommandForConcurrentWritesTest(testOpts, ErrorCodes.MaxTimeMSExpired);
}

function setupTest(testCase, collName, testOpts) {
    if (testCase.explicitlyCreateCollection) {
        createCollectionAndInsertDocsForConcurrentWritesTest(
            testOpts.primaryDB, collName, testCase.isCapped);
    }

    if (testCase.setUp) {
        testCase.setUp(testOpts.primaryDB, collName, testOpts.testInTransaction);
    }
}

const testOptsMap = {};

/**
 * run the setup for each cases before the migration starts
 */
function setupTestsBeforeMigration() {
    for (const [commandName, testCase] of Object.entries(testCases)) {
        let baseDbName = kTenantID.str + "_" + commandName;

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
function runTestsWhileBlocking() {
    for (const [commandName, testCase] of Object.entries(testCases)) {
        let baseDbName = kTenantID.str + "_" + commandName;
        if (testCase.skip) {
            continue;
        }

        testBlockWritesAfterMigrationEnteredBlocking(
            testOptsMap[baseDbName + "Basic-" + kTenantDefinedDbName]);
        countBlockedWrites += 1;

        if (testCase.testInTransaction) {
            testBlockWritesAfterMigrationEnteredBlocking(
                testOptsMap[baseDbName + "Txn-" + kTenantDefinedDbName]);
            countBlockedWrites += 1;
        }

        if (testCase.testAsRetryableWrite) {
            testBlockWritesAfterMigrationEnteredBlocking(
                testOptsMap[baseDbName + "Retryable-" + kTenantDefinedDbName]);
            countBlockedWrites += 1;
        }
    }
}

/**
 * Run the test cases after the migration has committed
 */
function runTestsAfterMigrationCommitted() {
    for (const [commandName, testCase] of Object.entries(testCases)) {
        let baseDbName = kTenantID.str + "_" + commandName;
        if (testCase.skip) {
            continue;
        }

        const basicTesTOpts = testOptsMap[baseDbName + "Basic-" + kTenantDefinedDbName];
        testCase.assertCommandFailed(
            basicTesTOpts.primaryDB, basicTesTOpts.dbName, basicTesTOpts.collName);

        if (testCase.testInTransaction) {
            const txnTesTOpts = testOptsMap[baseDbName + "Txn-" + kTenantDefinedDbName];
            testCase.assertCommandFailed(
                txnTesTOpts.primaryDB, txnTesTOpts.dbName, txnTesTOpts.collName);
        }

        if (testCase.testAsRetryableWrite) {
            const retryableTestOpts = testOptsMap[baseDbName + "Retryable-" + kTenantDefinedDbName];
            testCase.assertCommandFailed(
                retryableTestOpts.primaryDB, retryableTestOpts.dbName, retryableTestOpts.collName);
        }
    }
}

shardSplitTest.addRecipientNodes();
const tenantIds = [kTenantID];
const operation = shardSplitTest.createSplitOperation(tenantIds);

setupTestsBeforeMigration();

let blockFp = configureFailPoint(donorPrimary, "pauseShardSplitAfterBlocking");

// start a shard split operation asynchronously.
const splitThread = operation.commitAsync();

// Run the command after the migration enters the blocking state.
blockFp.wait();

// Run test cases while the migration is in blocking state.
runTestsWhileBlocking();

// Allow the migration to complete.
blockFp.off();
splitThread.join();

assert.commandWorked(splitThread.returnData());

// run test after blocking is over and the migration committed.
runTestsAfterMigrationCommitted();

ShardSplitTest.checkShardSplitAccessBlocker(
    donorPrimary, kTenantID, {numBlockedWrites: countBlockedWrites});

shardSplitTest.stop();
