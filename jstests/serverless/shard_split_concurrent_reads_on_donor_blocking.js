/**
 * Tests that the donor
 * - blocks reads with atClusterTime/afterClusterTime >= blockOpTime that are executed while the
 *   split is in the blocking state but does not block linearizable reads.
 *
 * @tags: [
 *   incompatible_with_eft,
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 *   requires_fcv_52,
 *   featureFlagShardSplit
 * ]
 */

(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");
load("jstests/serverless/libs/basic_serverless_test.js");
load("jstests/serverless/shard_split_concurrent_reads_on_donor_util.js");

const kCollName = "testColl";
const kTenantDefinedDbName = "0";

const kMaxTimeMS = 1 * 1000;

/**
 * Tests that in the blocking state, the donor blocks reads with atClusterTime/afterClusterTime >=
 * blockOpTime but does not block linearizable reads.
 */
let countBlockedReadsPrimary = 0;
let countBlockedReadsSecondaries = 0;
function testBlockReadsAfterMigrationEnteredBlocking(testCase, primary, dbName, collName) {
    const donorDoc = findSplitOperation(primary, operation.migrationId);
    const command = testCase.requiresReadTimestamp
        ? testCase.command(collName, donorDoc.blockOpTime.ts)
        : testCase.command(collName);
    const shouldBlock = !testCase.isLinearizableRead;
    if (shouldBlock) {
        command.maxTimeMS = kMaxTimeMS;
        countBlockedReadsPrimary += 1;
    }
    let nodes = [primary];
    if (testCase.isSupportedOnSecondaries) {
        nodes = donorRst.nodes;

        if (shouldBlock) {
            countBlockedReadsSecondaries += 1;
        }
    }
    nodes.forEach(node => {
        const db = node.getDB(dbName);
        runCommandForConcurrentReadTest(
            db, command, shouldBlock ? ErrorCodes.MaxTimeMSExpired : null, testCase.isTransaction);
    });
}

const testCases = shardSplitConcurrentReadTestCases;

const tenantId = "tenantId";
const test = new BasicServerlessTest({
    recipientTagName: "recipientTag",
    recipientSetName: "recipientSet",
    quickGarbageCollection: true
});
test.addRecipientNodes();

const donorRst = test.donor;
const donorPrimary = donorRst.getPrimary();

let blockingFp = configureFailPoint(donorPrimary, "pauseShardSplitAfterBlocking");

const operation = test.createSplitOperation([tenantId]);
const splitThread = operation.commitAsync();

// Wait for the split to enter the blocking state.
blockingFp.wait();

// Wait for the last oplog entry on the primary to be visible in the committed snapshot view of
// the oplog on all secondaries to ensure that snapshot reads on the secondaries with
// unspecified atClusterTime have read timestamp >= blockOpTime.
donorRst.awaitLastOpCommitted();

for (const [testCaseName, testCase] of Object.entries(testCases)) {
    jsTest.log(`Testing inBlocking with testCase ${testCaseName}`);
    const dbName = `${tenantId}_${testCaseName}-inBlocking-${kTenantDefinedDbName}`;
    testBlockReadsAfterMigrationEnteredBlocking(testCase, donorPrimary, dbName, kCollName);
}

// check on primary
BasicServerlessTest.checkShardSplitAccessBlocker(
    donorPrimary, tenantId, {numBlockedReads: countBlockedReadsPrimary});

// check on secondaries
const secondaries = donorRst.getSecondaries();
secondaries.forEach(node => {
    BasicServerlessTest.checkShardSplitAccessBlocker(
        node, tenantId, {numBlockedReads: countBlockedReadsSecondaries});
});

blockingFp.off();

splitThread.join();
assert.commandWorked(splitThread.returnData());

test.stop();
})();
