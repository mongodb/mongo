/**
 * Tests that when the recipient's stable timestamp is earlier than the startApplyingDonorOpTime,
 * the recipient advances its stable timestamp.
 *
 * @tags: [
 *   featureFlagShardMerge,
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_fcv_52,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 *   __TEMPORARILY_DISABLED__,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/libs/uuid_util.js");  // For extractUUIDFromObject().
load("jstests/replsets/rslib.js");

const kTenantIdPrefix = "testTenantId";
const kUnrelatedDbNameDonor = "unrelatedDBDonor";
const kUnrelatedDbNameRecipient = "unrelatedDBRecipient";
const collName = "foo";
const tenantId = kTenantIdPrefix + "-0";
const migrationId = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    tenantId: tenantId,
};

const tmt = new TenantMigrationTest({name: jsTestName()});
tmt.insertDonorDB(tenantId + "_db", collName);

const donorPrimary = tmt.getDonorPrimary();
const recipientPrimary = tmt.getRecipientPrimary();

// Insert a doc on the recipient with {writeConcern: majority} to advance the stable timestamp. We
// will hold the stable timestamp on the recipient at this ts. This will ensure that when we run the
// tenant migration, the recipient's stable timestamp will be less than the the timestamp it
// receives from the donor to use as the startApplyingDonorOpTime.
let recipientHoldStableTs =
    assert
        .commandWorked(recipientPrimary.getDB(kUnrelatedDbNameRecipient).runCommand({
            insert: collName,
            documents: [{_id: 1}],
            writeConcern: {w: "majority"}
        }))
        .operationTime;

const recipientHoldStablefp = configureFailPoint(
    recipientPrimary, "holdStableTimestampAtSpecificTimestamp", {timestamp: recipientHoldStableTs});

// Advance the stable timestamp on the donor so that it's greater than the timestamp of the
// recipient.
let donorAdvancedStableTs;
assert.soon(function() {
    donorAdvancedStableTs =
        assert
            .commandWorked(donorPrimary.getDB(kUnrelatedDbNameDonor).runCommand({
                insert: collName,
                documents: [{n: 1}],
                writeConcern: {w: "majority"}
            }))
            .operationTime;

    return bsonWoCompare(donorAdvancedStableTs, recipientHoldStableTs) > 0;
});

// Force the tenant migration to hang just before we attempt to advance the stable timestamp on the
// recipient.
const hangBeforeAdvanceStableTsFp =
    configureFailPoint(recipientPrimary, "fpBeforeAdvancingStableTimestamp", {action: "hang"});

// Start the migration.
assert.commandWorked(tmt.startMigration(migrationOpts, {enableDonorStartMigrationFsync: true}));

// The recipient's stable timestamp should be less than the timestamp it receives from the donor to
// use as the startApplyingDonorOpTime, so the recipient should advance its stable timestamp. Wait
// until the recipient hits the failpoint just before it advances the stable timestamp. Then, turn
// off both failpoints so that we no longer hold back the stable timestamp on the recipient, and the
// migration continues.
hangBeforeAdvanceStableTsFp.wait();

recipientHoldStablefp.off();
hangBeforeAdvanceStableTsFp.off();

// Wait until we see the no-op oplog entry. We will check that we see a log line indicating the
// recipient advanced its stable timestamp to the timestamp of this write.
let stableTimestamp;
assert.soon(function() {
    let noopEntry = recipientPrimary.getDB("local").oplog.rs.findOne(
        {"o": {"msg": "Merge recipient advancing stable timestamp"}});
    if (noopEntry)
        stableTimestamp = noopEntry.ts;

    return noopEntry;
});

let majorityWriteTs =
    assert
        .commandWorked(recipientPrimary.getDB(kUnrelatedDbNameRecipient).runCommand({
            insert: collName,
            documents: [{_id: 2}],
            writeConcern: {w: "majority"}
        }))
        .operationTime;

assert(bsonWoCompare(majorityWriteTs, donorAdvancedStableTs) >= 0);

TenantMigrationTest.assertCommitted(tmt.waitForMigrationToComplete(migrationOpts));
tmt.stop();
})();
