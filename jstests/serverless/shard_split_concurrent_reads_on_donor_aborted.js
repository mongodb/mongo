/**
 * Tests that the donor
 * - does not rejects reads with atClusterTime/afterClusterTime >= blockOpTime reads and
 * linearizable reads after the split aborts.
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

/**
 * Tests that after the split abort, the donor does not reject linearizable reads or reads with
 * atClusterTime/afterClusterTime >= blockOpTime.
 */
function testDoNotRejectReadsAfterMigrationAborted(testCase, dbName, collName) {
    const tenantId = dbName.split('_')[0];
    const donorDoc = findSplitOperation(donorPrimary, operation.migrationId);
    const nodes = testCase.isSupportedOnSecondaries ? donorRst.nodes : [donorPrimary];
    nodes.forEach(node => {
        const db = node.getDB(dbName);
        if (testCase.requiresReadTimestamp) {
            runCommandForConcurrentReadTest(db,
                                            testCase.command(collName, donorDoc.blockOpTime.ts),
                                            null,
                                            testCase.isTransaction);
            runCommandForConcurrentReadTest(
                db,
                testCase.command(collName, donorDoc.commitOrAbortOpTime.ts),
                null,
                testCase.isTransaction);
            BasicServerlessTest.checkShardSplitAccessBlocker(
                node, tenantId, {numTenantMigrationAbortedErrors: 0});
        } else {
            runCommandForConcurrentReadTest(
                db, testCase.command(collName), null, testCase.isTransaction);
            BasicServerlessTest.checkShardSplitAccessBlocker(
                node, tenantId, {numTenantMigrationAbortedErrors: 0});
        }
    });
}

const testCases = shardSplitConcurrentReadTestCases;

const test = new BasicServerlessTest({
    recipientTagName: "recipientTag",
    recipientSetName: "recipientSet",
    quickGarbageCollection: true
});
test.addRecipientNodes();

const tenantId = "tenantId";

const donorRst = test.donor;
const donorPrimary = test.getDonorPrimary();

// Force the donor to preserve all snapshot history to ensure that transactional reads do not
// fail with TransientTransactionError "Read timestamp is older than the oldest available
// timestamp".
donorRst.nodes.forEach(node => {
    configureFailPoint(node, "WTPreserveSnapshotHistoryIndefinitely");
});

let blockFp = configureFailPoint(donorPrimary, "pauseShardSplitAfterBlocking");

const operation = test.createSplitOperation([tenantId]);
const splitThread = operation.commitAsync();

blockFp.wait();
operation.abort();

blockFp.off();

splitThread.join();
assert.commandFailed(splitThread.returnData());
assertMigrationState(donorPrimary, operation.migrationId, "aborted");

// Wait for the last oplog entry on the primary to be visible in the committed snapshot view of
// the oplog on all the secondaries. This is to ensure that snapshot reads on secondaries with
// unspecified atClusterTime have read timestamp >= abortTimestamp.
donorRst.awaitLastOpCommitted();

for (const [testCaseName, testCase] of Object.entries(testCases)) {
    jsTest.log(`Testing inAborted with testCase ${testCaseName}`);
    const dbName = `${tenantId}_${testCaseName}-inAborted-${kTenantDefinedDbName}`;
    testDoNotRejectReadsAfterMigrationAborted(testCase, dbName, kCollName);
}

test.stop();
})();
