/**
 * Tests dropping the donor state doc collections after the shard split decision promise is
 * fulfilled.
 *
 * @tags: [
 *   incompatible_with_eft,
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 *   requires_fcv_63
 * ]
 */

import {findSplitOperation, ShardSplitTest} from "jstests/serverless/libs/shard_split_test.js";

load("jstests/libs/fail_point_util.js");

TestData.skipCheckDBHashes = true;

function testDroppingStateDocCollections(
    test,
    fpName,
    {dropDonorsCollection = false, retryWithDifferentMigrationId = false, expectedAbortReason}) {
    jsTest.log(`Testing with failpoint: ${fpName} dropDonorsCollection: ${
        dropDonorsCollection}, retryWithDifferentMigrationId: ${retryWithDifferentMigrationId}`);

    test.addRecipientNodes();
    let donorPrimary = test.donor.getPrimary();

    const tenantIds = [ObjectId(), ObjectId()];

    const operation = test.createSplitOperation(tenantIds);
    let migrationId = operation.migrationId;

    let commitShardSplitThread = undefined;

    let fp = configureFailPoint(donorPrimary.getDB("admin"), fpName);
    commitShardSplitThread = operation.commitAsync();
    fp.wait();

    if (dropDonorsCollection) {
        assert(donorPrimary.getCollection(ShardSplitTest.kConfigSplitDonorsNS).drop());
        let donorDoc = findSplitOperation(donorPrimary, migrationId);
        assert.eq(donorDoc, null);

        const currOpDonor = assert.commandWorked(
            donorPrimary.adminCommand({currentOp: true, desc: "shard split operation"}));
        assert.eq(currOpDonor.inprog.length, 0);

        // Trigger stepup to allow the donor service to rebuild.
        assert.commandWorked(donorPrimary.adminCommand({replSetStepDown: 30, force: true}));
    }

    fp.off();
    commitShardSplitThread.join();
    if (expectedAbortReason) {
        const data = commitShardSplitThread.returnData();
        assert.commandFailedWithCode(data, expectedAbortReason);
        assert.eq(data.code, expectedAbortReason);
    }
    test.removeRecipientNodesFromDonor();
    if (!dropDonorsCollection) {
        operation.forget();
        test.waitForGarbageCollection(migrationId, tenantIds);
    }
    test.removeAndStopRecipientNodes();
    test.reconfigDonorSetAfterSplit();

    test.addRecipientNodes();

    const operation2 =
        retryWithDifferentMigrationId ? test.createSplitOperation(tenantIds) : operation;
    migrationId = operation2.migrationId;
    assert.commandWorked(operation2.commit());

    operation2.forget();

    test.cleanupSuccesfulCommitted(migrationId, tenantIds);
}

jsTest.log("Test dropping donor and recipient state doc collections during a shard split.");
const test = new ShardSplitTest({quickGarbageCollection: true});

const fpName = "pauseShardSplitAfterDecision";
testDroppingStateDocCollections(test, fpName, {dropDonorsCollection: true});

testDroppingStateDocCollections(
    test, fpName, {dropDonorsCollection: true, retryWithDifferentMigrationId: true});

if (fpName) {
    // if we do not have a failpoint and we do not drop the donor state doc there is no need
    // to run a test.
    testDroppingStateDocCollections(test, fpName, {dropDonorsCollection: false});

    testDroppingStateDocCollections(test, fpName, {
        dropDonorsCollection: false,
        expectedAbortReason: (fpName == "abortShardSplitBeforeLeavingBlockingState")
            ? ErrorCodes.TenantMigrationAborted
            : null
    });
}

test.stop();
