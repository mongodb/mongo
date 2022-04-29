/**
 * Tests dropping the donor and recipient state doc collections in the middle of a tenant migration.
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
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const kMigrationFpNames = [
    "pauseTenantMigrationAfterPersistingInitialDonorStateDoc",
    "pauseTenantMigrationBeforeLeavingDataSyncState",
    "pauseTenantMigrationBeforeLeavingBlockingState",
    "abortTenantMigrationBeforeLeavingBlockingState",
    null,
];
const kTenantId = "testTenantId";
let testNum = 0;

function makeTenantId() {
    return kTenantId + testNum++;
}

function makeMigrationOpts(tenantMigrationTest, tenantId) {
    return {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId: tenantId,
        recipientConnString: tenantMigrationTest.getRecipientConnString()
    };
}

/**
 * Starts a migration and then either waits for the failpoint or lets the migration run to
 * completion. Next, drops the donor and/or recipient state doc collections and asserts that the
 * migration is no longer running on the donor and/or recipient. Then, retries the migration (with a
 * different migration id if 'retryWithDifferentMigrationId' is true) and verifies that the retry
 * succeeds or fails as expected.
 */
function testDroppingStateDocCollections(tenantMigrationTest, fpName, {
    dropDonorsCollection = false,
    dropRecipientsCollection = false,
    retryWithDifferentMigrationId = false,
    expectedRunMigrationError,
    expectedForgetMigrationError,
    expectedAbortReason
}) {
    assert(dropDonorsCollection || dropRecipientsCollection);

    jsTest.log(`Testing with failpoint: ${fpName} dropDonorsCollection: ${
        dropDonorsCollection}, dropRecipientsCollection: ${
        dropRecipientsCollection}, retryWithDifferentMigrationId: ${
        retryWithDifferentMigrationId}`);

    const tenantId = makeTenantId();
    const migrationOptsBeforeDrop = makeMigrationOpts(tenantMigrationTest, tenantId);
    let donorPrimary = tenantMigrationTest.getDonorPrimary();
    let recipientPrimary = tenantMigrationTest.getRecipientPrimary();

    let fp;
    if (fpName) {
        fp = configureFailPoint(donorPrimary, fpName, {tenantId: tenantId});
        assert.commandWorked(tenantMigrationTest.startMigration(migrationOptsBeforeDrop));
        fp.wait();
    } else {
        TenantMigrationTest.assertCommitted(tenantMigrationTest.runMigration(
            migrationOptsBeforeDrop, {automaticForgetMigration: false}));
    }

    if (dropDonorsCollection) {
        assert(donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS).drop());
        let donorDoc = donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS).findOne({
            tenantId: tenantId
        });
        assert.eq(donorDoc, null);

        const currOpDonor = assert.commandWorked(
            donorPrimary.adminCommand({currentOp: true, desc: "tenant donor migration"}));
        assert.eq(currOpDonor.inprog.length, 0);

        // Trigger stepup to allow the donor service to rebuild.
        assert.commandWorked(donorPrimary.adminCommand({replSetStepDown: 30, force: true}));
        donorPrimary = tenantMigrationTest.getDonorRst().getPrimary();
    }

    if (dropRecipientsCollection) {
        assert(recipientPrimary.getCollection(TenantMigrationTest.kConfigRecipientsNS).drop({
            writeConcern: {w: "majority"}
        }));
        let recipientDoc =
            recipientPrimary.getCollection(TenantMigrationTest.kConfigRecipientsNS).findOne({
                tenantId: tenantId
            });
        assert.eq(recipientDoc, null);
        const currOpRecipient = assert.commandWorked(
            recipientPrimary.adminCommand({currentOp: true, desc: "tenant recipient migration"}));
        assert.eq(currOpRecipient.inprog.length, 0);

        // Trigger stepup to allow the recipient service to rebuild.
        assert.commandWorked(recipientPrimary.adminCommand({replSetStepDown: 30, force: true}));
        recipientPrimary = tenantMigrationTest.getRecipientRst().getPrimary();
    }

    if (fp) {
        fp.off();
    }
    const migrationOptsAfterDrop = retryWithDifferentMigrationId
        ? makeMigrationOpts(tenantMigrationTest, tenantId)
        : migrationOptsBeforeDrop;
    const runMigrationRes =
        tenantMigrationTest.runMigration(migrationOptsAfterDrop, {automaticForgetMigration: false});
    if (expectedRunMigrationError) {
        assert.commandFailedWithCode(runMigrationRes, expectedRunMigrationError);
    } else {
        assert.commandWorked(runMigrationRes);
        if (expectedAbortReason) {
            assert.eq(runMigrationRes.state, TenantMigrationTest.DonorState.kAborted);
            assert.eq(runMigrationRes.abortReason.code, expectedAbortReason);
        } else {
            assert.eq(runMigrationRes.state, TenantMigrationTest.DonorState.kCommitted);
        }

        const forgetMigrationRes =
            tenantMigrationTest.forgetMigration(migrationOptsAfterDrop.migrationIdString);
        if (expectedForgetMigrationError) {
            assert.commandFailedWithCode(forgetMigrationRes, expectedForgetMigrationError);
        } else {
            assert.commandWorked(forgetMigrationRes);
            tenantMigrationTest.waitForMigrationGarbageCollection(
                UUID(migrationOptsAfterDrop.migrationIdString));
        }
    }

    if (retryWithDifferentMigrationId && !dropDonorsCollection) {
        assert(dropRecipientsCollection);
        // The original migration will still run to completion after the recipient service rebuilds
        // since the donor will retry the recipientSyncData command on Interrupted error. Wait for
        // the migration to complete and clean up to avoid concurrent migrations when the next test
        // case starts.
        assert.commandWorked(
            tenantMigrationTest.waitForMigrationToComplete(migrationOptsBeforeDrop));
        assert.commandWorked(
            tenantMigrationTest.forgetMigration(migrationOptsBeforeDrop.migrationIdString));
        tenantMigrationTest.waitForMigrationGarbageCollection(
            UUID(migrationOptsAfterDrop.migrationIdString));
    }
}

jsTest.log("Test dropping donor and recipient state doc collections during a migration.");
kMigrationFpNames.forEach(fpName => {
    const tenantMigrationTest = new TenantMigrationTest({
        name: jsTestName(),
        quickGarbageCollection: true,
        initiateRstWithHighElectionTimeout: false
    });

    testDroppingStateDocCollections(
        tenantMigrationTest, fpName, {dropDonorsCollection: true, dropRecipientsCollection: true});

    testDroppingStateDocCollections(tenantMigrationTest, fpName, {
        dropDonorsCollection: true,
        dropRecipientsCollection: true,
        retryWithDifferentMigrationId: true
    });

    testDroppingStateDocCollections(tenantMigrationTest, fpName, {
        dropDonorsCollection: false,
        dropRecipientsCollection: true,
        expectedAbortReason: (fpName == "abortTenantMigrationBeforeLeavingBlockingState")
            ? ErrorCodes.InternalError
            : null
    });

    testDroppingStateDocCollections(tenantMigrationTest, fpName, {
        dropDonorsCollection: false,
        dropRecipientsCollection: true,
        retryWithDifferentMigrationId: true,
        // The original migration is still running on the donor so the retry is expected to fail
        // with ConflictingOperationInProgress.
        expectedRunMigrationError: ErrorCodes.ConflictingOperationInProgress
    });

    testDroppingStateDocCollections(tenantMigrationTest, fpName, {
        dropDonorsCollection: true,
        dropRecipientsCollection: false,
    });

    const originalMigrationStartedOnRecipient =
        fpName != "pauseTenantMigrationAfterPersistingInitialDonorStateDoc";
    testDroppingStateDocCollections(tenantMigrationTest, fpName, {
        dropDonorsCollection: true,
        dropRecipientsCollection: false,
        retryWithDifferentMigrationId: true,
        // If the original migration has started running on the recipient, the retry will lead to
        // a conflicting migration.
        expectedAbortReason:
            originalMigrationStartedOnRecipient ? ErrorCodes.ConflictingOperationInProgress : null,
        expectedForgetMigrationError:
            originalMigrationStartedOnRecipient ? ErrorCodes.ConflictingOperationInProgress : null
    });
    tenantMigrationTest.stop();
});
})();
