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

/**
 * Tests that after the split commits, the donor rejects linearizable reads and reads with
 * atClusterTime/afterClusterTime >= blockOpTime.
 */
let countTenantMigrationCommittedErrorsPrimary = 0;
let countTenantMigrationCommittedErrorsSecondaries = 0;
function testRejectReadsAfterMigrationCommitted(testCase, primary, dbName, collName, migrationId) {
    const donorDoc = findSplitOperation(primary, migrationId);

    let nodes = [primary];
    if (testCase.isSupportedOnSecondaries) {
        nodes = donorRst.nodes;

        if (testCase.requiresReadTimestamp) {
            countTenantMigrationCommittedErrorsSecondaries += 2;
        } else {
            countTenantMigrationCommittedErrorsSecondaries += 1;
        }
    }

    if (testCase.requiresReadTimestamp) {
        countTenantMigrationCommittedErrorsPrimary += 2;
    } else {
        countTenantMigrationCommittedErrorsPrimary += 1;
    }

    nodes.forEach(node => {
        const db = node.getDB(dbName);
        if (testCase.requiresReadTimestamp) {
            runCommandForConcurrentReadTest(db,
                                            testCase.command(collName, donorDoc.blockOpTime.ts),
                                            ErrorCodes.TenantMigrationCommitted,
                                            testCase.isTransaction);
            runCommandForConcurrentReadTest(
                db,
                testCase.command(collName, donorDoc.commitOrAbortOpTime.ts),
                ErrorCodes.TenantMigrationCommitted,
                testCase.isTransaction);
        } else {
            runCommandForConcurrentReadTest(db,
                                            testCase.command(collName),
                                            ErrorCodes.TenantMigrationCommitted,
                                            testCase.isTransaction);
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

let donorRst = test.donor;
const donorPrimary = test.getDonorPrimary();

// Force the donor to preserve all snapshot history to ensure that transactional reads do not
// fail with TransientTransactionError "Read timestamp is older than the oldest available
// timestamp".
donorRst.nodes.forEach(node => {
    configureFailPoint(node, "WTPreserveSnapshotHistoryIndefinitely");
});

const operation = test.createSplitOperation([tenantId]);
assert.commandWorked(operation.commit());

test.removeRecipientNodesFromDonor();

// Wait for the last oplog entry on the primary to be visible in the committed snapshot view of
// the oplog on all the secondaries. This is to ensure that snapshot reads on secondaries with
// unspecified atClusterTime have read timestamp >= commitTimestamp.
donorRst.awaitLastOpCommitted();

for (const [testCaseName, testCase] of Object.entries(testCases)) {
    jsTest.log(`Testing inCommitted with testCase ${testCaseName}`);
    const dbName = `${tenantId}_${testCaseName}-inCommitted-${kTenantDefinedDbName}`;
    testRejectReadsAfterMigrationCommitted(
        testCase, donorPrimary, dbName, kCollName, operation.migrationId);
}

// check on primary
BasicServerlessTest.checkShardSplitAccessBlocker(donorPrimary, tenantId, {
    numTenantMigrationCommittedErrors: countTenantMigrationCommittedErrorsPrimary
});
let secondaries = donorRst.getSecondaries();
// check on secondaries
secondaries.forEach(node => {
    BasicServerlessTest.checkShardSplitAccessBlocker(node, tenantId, {
        numTenantMigrationCommittedErrors: countTenantMigrationCommittedErrorsSecondaries
    });
});

test.stop();
})();
