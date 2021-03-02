/**
 * Tests the TenantMigrationAccessBlocker and donor state document are updated correctly at each
 * stage of the migration, and are eventually removed after the donorForgetMigration has returned.
 *
 * Tenant migrations are not expected to be run on servers with ephemeralForTest, and in particular
 * this test fails on ephemeralForTest because the donor has to wait for the write to set the
 * migration state to "committed" and "aborted" to be majority committed but it cannot do that on
 * ephemeralForTest.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, incompatible_with_eft,
 * incompatible_with_windows_tls]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");

let expectedNumRecipientSyncDataCmdSent = 0;
let expectedNumRecipientForgetMigrationCmdSent = 0;

/**
 * Runs the donorForgetMigration command and asserts that the TenantMigrationAccessBlocker and donor
 * state document are eventually removed from the donor.
 */
function testDonorForgetMigrationAfterMigrationCompletes(
    donorRst, recipientRst, migrationId, tenantId) {
    jsTest.log("Test donorForgetMigration after the migration completes");
    const donorPrimary = donorRst.getPrimary();
    const recipientPrimary = recipientRst.getPrimary();

    assert.commandWorked(
        donorPrimary.adminCommand({donorForgetMigration: 1, migrationId: migrationId}));

    expectedNumRecipientForgetMigrationCmdSent++;
    const recipientForgetMigrationMetrics =
        recipientPrimary.adminCommand({serverStatus: 1}).metrics.commands.recipientForgetMigration;
    assert.eq(recipientForgetMigrationMetrics.failed, 0);
    assert.eq(recipientForgetMigrationMetrics.total, expectedNumRecipientForgetMigrationCmdSent);

    // Wait for garbage collection on donor.
    donorRst.nodes.forEach((node) => {
        assert.soon(() =>
                        null == node.adminCommand({serverStatus: 1}).tenantMigrationAccessBlocker);
    });

    assert.soon(() => 0 === donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS).count({
        tenantId: tenantId
    }));
    assert.soon(() => 0 ===
                    donorPrimary.adminCommand({serverStatus: 1})
                        .repl.primaryOnlyServices.TenantMigrationDonorService);

    const donorRecipientMonitorPoolStats =
        donorPrimary.adminCommand({connPoolStats: 1}).replicaSets;
    assert.eq(Object.keys(donorRecipientMonitorPoolStats).length, 0);

    // Wait for garbage collection on recipient.
    recipientRst.nodes.forEach((node) => {
        assert.soon(() =>
                        null == node.adminCommand({serverStatus: 1}).tenantMigrationAccessBlocker);
    });

    assert.soon(() => 0 ===
                    recipientPrimary.getCollection(TenantMigrationTest.kConfigRecipientsNS).count({
                        tenantId: tenantId
                    }));
    assert.soon(() => 0 ===
                    recipientPrimary.adminCommand({serverStatus: 1})
                        .repl.primaryOnlyServices.TenantMigrationRecipientService);

    const recipientRecipientMonitorPoolStats =
        recipientPrimary.adminCommand({connPoolStats: 1}).replicaSets;
    assert.eq(Object.keys(recipientRecipientMonitorPoolStats).length, 0);
}

const sharedOptions = {
    setParameter: {
        // Set the delay before a state doc is garbage collected to be short to speed up the test.
        tenantMigrationGarbageCollectionDelayMS: 3 * 1000,
        ttlMonitorSleepSecs: 1,
    }
};
const x509Options = TenantMigrationUtil.makeX509OptionsForTest();

const donorRst = new ReplSetTest({
    nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}],
    name: "donor",
    nodeOptions: Object.assign(x509Options.donor, sharedOptions)
});

const recipientRst = new ReplSetTest({
    nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}],
    name: "recipient",
    nodeOptions: Object.assign(x509Options.recipient, sharedOptions)
});

donorRst.startSet();
donorRst.initiate();

recipientRst.startSet();
recipientRst.initiate();

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName(), donorRst, recipientRst});
if (!tenantMigrationTest.isFeatureFlagEnabled()) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    donorRst.stopSet();
    recipientRst.stopSet();
    return;
}

const donorPrimary = tenantMigrationTest.getDonorPrimary();
const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

const kTenantId = "testDb";

let configDonorsColl = donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS);

(() => {
    jsTest.log("Test the case where the migration commits");
    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: kTenantId,
    };

    let blockingFp =
        configureFailPoint(donorPrimary, "pauseTenantMigrationBeforeLeavingBlockingState");
    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

    // Wait for the migration to enter the blocking state.
    blockingFp.wait();

    let mtabs = donorPrimary.adminCommand({serverStatus: 1}).tenantMigrationAccessBlocker;
    assert.eq(mtabs[kTenantId].state, TenantMigrationTest.DonorAccessState.kBlockWritesAndReads);
    assert(mtabs[kTenantId].blockTimestamp);

    let donorDoc = configDonorsColl.findOne({tenantId: kTenantId});
    let blockOplogEntry =
        donorPrimary.getDB("local")
            .oplog.rs
            .find({ns: TenantMigrationTest.kConfigDonorsNS, op: "u", "o.tenantId": kTenantId})
            .sort({"$natural": -1})
            .limit(1)
            .next();
    assert.eq(donorDoc.state, "blocking");
    assert.eq(donorDoc.blockTimestamp, blockOplogEntry.ts);

    // Verify that donorForgetMigration fails since the decision has not been made.
    assert.commandFailedWithCode(
        donorPrimary.adminCommand({donorForgetMigration: 1, migrationId: migrationId}),
        ErrorCodes.TenantMigrationInProgress);

    // Allow the migration to complete.
    blockingFp.off();
    const stateRes =
        assert.commandWorked(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
    assert.eq(stateRes.state, TenantMigrationTest.DonorState.kCommitted);

    donorDoc = configDonorsColl.findOne({tenantId: kTenantId});
    let commitOplogEntry = donorPrimary.getDB("local").oplog.rs.findOne(
        {ns: TenantMigrationTest.kConfigDonorsNS, op: "u", o: donorDoc});
    assert.eq(donorDoc.state, TenantMigrationTest.DonorState.kCommitted);
    assert.eq(donorDoc.commitOrAbortOpTime.ts, commitOplogEntry.ts);

    assert.soon(() => {
        mtabs = donorPrimary.adminCommand({serverStatus: 1}).tenantMigrationAccessBlocker;
        return mtabs[kTenantId].state === TenantMigrationTest.DonorAccessState.kReject;
    });
    assert(mtabs[kTenantId].commitOpTime);

    expectedNumRecipientSyncDataCmdSent += 2;
    const recipientSyncDataMetrics =
        recipientPrimary.adminCommand({serverStatus: 1}).metrics.commands.recipientSyncData;
    assert.eq(recipientSyncDataMetrics.failed, 0);
    assert.eq(recipientSyncDataMetrics.total, expectedNumRecipientSyncDataCmdSent);

    testDonorForgetMigrationAfterMigrationCompletes(donorRst, recipientRst, migrationId, kTenantId);
})();

(() => {
    jsTest.log("Test the case where the migration aborts");
    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: kTenantId,
    };

    let abortFp =
        configureFailPoint(donorPrimary, "abortTenantMigrationBeforeLeavingBlockingState");
    const stateRes = assert.commandWorked(tenantMigrationTest.runMigration(
        migrationOpts, false /* retryOnRetryableErrors */, false /* automaticForgetMigration */));
    assert.eq(stateRes.state, TenantMigrationTest.DonorState.kAborted);
    abortFp.off();

    const donorDoc = configDonorsColl.findOne({tenantId: kTenantId});
    const abortOplogEntry = donorPrimary.getDB("local").oplog.rs.findOne(
        {ns: TenantMigrationTest.kConfigDonorsNS, op: "u", o: donorDoc});
    assert.eq(donorDoc.state, TenantMigrationTest.DonorState.kAborted);
    assert.eq(donorDoc.commitOrAbortOpTime.ts, abortOplogEntry.ts);
    assert.eq(donorDoc.abortReason.code, ErrorCodes.InternalError);

    let mtabs;
    assert.soon(() => {
        mtabs = donorPrimary.adminCommand({serverStatus: 1}).tenantMigrationAccessBlocker;
        return mtabs[kTenantId].state === TenantMigrationTest.DonorAccessState.kAborted;
    });
    assert(mtabs[kTenantId].abortOpTime);

    expectedNumRecipientSyncDataCmdSent += 2;
    const recipientSyncDataMetrics =
        recipientPrimary.adminCommand({serverStatus: 1}).metrics.commands.recipientSyncData;
    assert.eq(recipientSyncDataMetrics.failed, 0);
    assert.eq(recipientSyncDataMetrics.total, expectedNumRecipientSyncDataCmdSent);

    testDonorForgetMigrationAfterMigrationCompletes(donorRst, recipientRst, migrationId, kTenantId);
})();

// Drop the TTL index to make sure that the migration state is still available when the
// donorForgetMigration command is retried.
configDonorsColl.dropIndex({expireAt: 1});

(() => {
    jsTest.log("Test that donorForgetMigration can be run multiple times");
    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: kTenantId,
    };

    // Verify that donorForgetMigration fails since the migration hasn't started.
    assert.commandFailedWithCode(
        donorPrimary.adminCommand({donorForgetMigration: 1, migrationId: migrationId}),
        ErrorCodes.NoSuchTenantMigration);

    const stateRes = assert.commandWorked(tenantMigrationTest.runMigration(
        migrationOpts, false /* retryOnRetryableErrors */, false /* automaticForgetMigration */));
    assert.eq(stateRes.state, TenantMigrationTest.DonorState.kCommitted);
    assert.commandWorked(
        donorPrimary.adminCommand({donorForgetMigration: 1, migrationId: migrationId}));

    // Verify that the retry succeeds.
    assert.commandWorked(
        donorPrimary.adminCommand({donorForgetMigration: 1, migrationId: migrationId}));
})();

tenantMigrationTest.stop();
donorRst.stopSet();
recipientRst.stopSet();
})();
