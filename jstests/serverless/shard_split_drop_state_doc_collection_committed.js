/**
 * Tests dropping the donor state doc collections after the shard split has committed.
 *
 * @tags: [
 *   incompatible_with_eft,
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 *   requires_fcv_62
 * ]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/serverless/libs/shard_split_test.js");

const recipientTagName = "recipientNode";
const recipientSetName = "recipient";

TestData.skipCheckDBHashes = true;

function testDroppingStateDocCollections(
    test,
    fpName,
    {dropDonorsCollection = false, retryWithDifferentMigrationId = false, expectedAbortReason}) {
    jsTest.log(`Testing with failpoint: ${fpName} dropDonorsCollection: ${
        dropDonorsCollection}, retryWithDifferentMigrationId: ${retryWithDifferentMigrationId}`);

    test.addRecipientNodes();
    let donorPrimary = test.donor.getPrimary();

    const tenantIds = ["tenant1", "tenant2"];

    const operation = test.createSplitOperation(tenantIds);
    let migrationId = operation.migrationId;

    assert.commandWorked(operation.commit());
    operation.forget();

    test.cleanupSuccesfulCommitted(migrationId, tenantIds);

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

    test.addRecipientNodes();

    const operation2 =
        retryWithDifferentMigrationId ? test.createSplitOperation(tenantIds) : operation;
    migrationId = operation2.migrationId;
    const runMigrationRes = operation2.commit();

    assert.commandWorked(runMigrationRes);
    assert.eq(runMigrationRes.state, "committed");

    operation2.forget();

    test.cleanupSuccesfulCommitted(migrationId, tenantIds);
}

jsTest.log("Test dropping donor and recipient state doc collections during a shard split.");
const test = new ShardSplitTest({
    recipientTagName,
    recipientSetName,
    quickGarbageCollection: true,
    initiateWithShortElectionTimeout: true
});

const fpName = undefined;
testDroppingStateDocCollections(test, fpName, {dropDonorsCollection: true});

testDroppingStateDocCollections(
    test, fpName, {dropDonorsCollection: true, retryWithDifferentMigrationId: true});

test.stop();
})();
