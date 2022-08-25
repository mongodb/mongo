/**
 * Tests that the donor
 * - rejects reads with atClusterTime/afterClusterTime >= blockOpTime reads and linearizable
 *   reads after the split commits.
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

function getTenantId(dbName) {
    return dbName.split('_')[0];
}

/**
 * To be used to resume a split that is paused after entering the blocking state. Waits for the
 * number of blocked reads to reach 'targetNumBlockedReads' and unpauses the split.
 */
function resumeMigrationAfterBlockingRead(host, tenantId, targetNumBlockedReads) {
    load("jstests/libs/fail_point_util.js");
    load("jstests/serverless/libs/basic_serverless_test.js");
    const primary = new Mongo(host);

    assert.soon(() => BasicServerlessTest.getNumBlockedReads(primary, tenantId) ==
                    targetNumBlockedReads);

    assert.commandWorked(
        primary.adminCommand({configureFailPoint: "pauseShardSplitAfterBlocking", mode: "off"}));
}

/**
 * Tests that the donor rejects the blocked reads (reads with atClusterTime/afterClusterTime >=
 * blockingTimestamp) once the split commits.
 */
function testRejectBlockedReadsAfterMigrationCommitted(testCase, dbName, collName) {
    if (testCase.isLinearizableRead) {
        // Linearizable reads are not blocked.
        return;
    }

    const tenantId = getTenantId(dbName);
    const test = new BasicServerlessTest({
        recipientTagName: "recipientTag",
        recipientSetName: "recipientSet",
        quickGarbageCollection: true
    });
    test.addRecipientNodes();

    const donorRst = test.donor;
    const donorPrimary = test.getDonorPrimary();

    let blockingFp = configureFailPoint(donorPrimary, "pauseShardSplitAfterBlocking");

    const operation = test.createSplitOperation([tenantId]);

    let resumeMigrationThread =
        new Thread(resumeMigrationAfterBlockingRead, donorPrimary.host, tenantId, 1);
    resumeMigrationThread.start();

    // Run the commands after the split enters the blocking state.
    const splitThread = operation.commitAsync();
    blockingFp.wait();

    // Wait for the last oplog entry on the primary to be visible in the committed snapshot view of
    // the oplog on all secondaries to ensure that snapshot reads on the secondaries with
    // unspecified atClusterTime have read timestamp >= blockOpTime.
    donorRst.awaitLastOpCommitted();

    const donorDoc = findSplitOperation(donorPrimary, operation.migrationId);
    const command = testCase.requiresReadTimestamp
        ? testCase.command(collName, donorDoc.blockOpTime.ts)
        : testCase.command(collName);

    // The split should unpause and commit after the read is blocked. Verify that the read
    // is rejected on donor nodes.
    const db = donorPrimary.getDB(dbName);
    runCommandForConcurrentReadTest(
        db, command, ErrorCodes.TenantMigrationCommitted, testCase.isTransaction);
    if (testCase.isSupportedOnSecondaries) {
        const primaryPort = String(donorPrimary).split(":")[1];
        const secondaries = donorRst.nodes.filter(node => node.port != primaryPort);
        secondaries.filter(node => !test.recipientNodes.includes(node)).forEach(node => {
            const db = node.getDB(dbName);
            runCommandForConcurrentReadTest(
                db, command, ErrorCodes.TenantMigrationCommitted, testCase.isTransaction);
        });
    }

    BasicServerlessTest.checkShardSplitAccessBlocker(
        donorPrimary, tenantId, {numBlockedReads: 1, numTenantMigrationCommittedErrors: 1});

    resumeMigrationThread.join();
    // Verify that the split succeeded.
    splitThread.join();
    assert.commandWorked(splitThread.returnData());
    test.removeAndStopRecipientNodes();

    test.stop();
}

const testCases = shardSplitConcurrentReadTestCases;

for (const [testCaseName, testCase] of Object.entries(testCases)) {
    jsTest.log(`Testing inBlockingThenCommitted with testCase ${testCaseName}`);
    const dbName = `${testCaseName}-inBlockingThenCommitted_${kTenantDefinedDbName}`;
    testRejectBlockedReadsAfterMigrationCommitted(testCase, dbName, kCollName);
}
})();
