/**
 * Tests the TenantMigrationAccessBlocker and donor state document are updated correctly at each
 * stage of the migration, and are eventually removed after the donorForgetMigration has returned.
 *
 * Tenant migrations are not expected to be run on servers with ephemeralForTest, and in particular
 * this test fails on ephemeralForTest because the donor has to wait for the write to set the
 * migration state to "committed" and "aborted" to be majority committed but it cannot do that on
 * ephemeralForTest.
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

let expectedNumRecipientSyncDataCmdSent = 0;
let expectedNumRecipientForgetMigrationCmdSent = 0;
let expectedRecipientSyncDataMetricsFailed = 0;

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
        assert.soon(() => null ==
                        TenantMigrationUtil.getTenantMigrationAccessBlocker({donorNode: node}));
    });

    assert.soon(() => 0 === donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS).count({
        tenantId: tenantId
    }));
    assert.soon(() => 0 ===
                    donorPrimary.adminCommand({serverStatus: 1})
                        .repl.primaryOnlyServices.TenantMigrationDonorService.numInstances);

    const donorRecipientMonitorPoolStats =
        donorPrimary.adminCommand({connPoolStats: 1}).replicaSets;
    assert.eq(Object.keys(donorRecipientMonitorPoolStats).length, 0);

    // Wait for garbage collection on recipient.
    recipientRst.nodes.forEach((node) => {
        assert.soon(() => null ==
                        TenantMigrationUtil.getTenantMigrationAccessBlocker({recipientNode: node}));
    });

    assert.soon(() => 0 ===
                    recipientPrimary.getCollection(TenantMigrationTest.kConfigRecipientsNS).count({
                        tenantId: tenantId
                    }));
    assert.soon(() => 0 ===
                    recipientPrimary.adminCommand({serverStatus: 1})
                        .repl.primaryOnlyServices.TenantMigrationRecipientService.numInstances);

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

const donorPrimary = tenantMigrationTest.getDonorPrimary();
const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

const kTenantId = "testDb";

let configDonorsColl = donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS);

function testStats(node, {
    currentMigrationsDonating = 0,
    currentMigrationsReceiving = 0,
    totalSuccessfulMigrationsDonated = 0,
    totalSuccessfulMigrationsReceived = 0,
    totalFailedMigrationsDonated = 0,
    totalFailedMigrationsReceived = 0
}) {
    const stats = tenantMigrationTest.getTenantMigrationStats(node);
    jsTestLog(stats);
    assert.eq(currentMigrationsDonating, stats.currentMigrationsDonating);
    assert.eq(currentMigrationsReceiving, stats.currentMigrationsReceiving);
    assert.eq(totalSuccessfulMigrationsDonated, stats.totalSuccessfulMigrationsDonated);
    assert.eq(totalSuccessfulMigrationsReceived, stats.totalSuccessfulMigrationsReceived);
    assert.eq(totalFailedMigrationsDonated, stats.totalFailedMigrationsDonated);
    assert.eq(totalFailedMigrationsReceived, stats.totalFailedMigrationsReceived);
}

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

    let mtab = TenantMigrationUtil.getTenantMigrationAccessBlocker(
        {donorNode: donorPrimary, tenantId: kTenantId});
    assert.eq(mtab.donor.state, TenantMigrationTest.DonorAccessState.kBlockWritesAndReads);
    assert(mtab.donor.blockTimestamp);

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

    testStats(donorPrimary, {currentMigrationsDonating: 1});
    testStats(recipientPrimary, {currentMigrationsReceiving: 1});

    // Allow the migration to complete.
    blockingFp.off();
    TenantMigrationTest.assertCommitted(
        tenantMigrationTest.waitForMigrationToComplete(migrationOpts));

    donorDoc = configDonorsColl.findOne({tenantId: kTenantId});
    let commitOplogEntry = donorPrimary.getDB("local").oplog.rs.findOne(
        {ns: TenantMigrationTest.kConfigDonorsNS, op: "u", o: donorDoc});
    assert.eq(donorDoc.state, TenantMigrationTest.DonorState.kCommitted);
    assert.eq(donorDoc.commitOrAbortOpTime.ts, commitOplogEntry.ts);

    assert.soon(() => {
        mtab = TenantMigrationUtil.getTenantMigrationAccessBlocker(
            {donorNode: donorPrimary, tenantId: kTenantId});
        return mtab.donor.state === TenantMigrationTest.DonorAccessState.kReject;
    });
    assert(mtab.donor.commitOpTime);

    expectedNumRecipientSyncDataCmdSent += 2;
    const recipientSyncDataMetrics =
        recipientPrimary.adminCommand({serverStatus: 1}).metrics.commands.recipientSyncData;
    assert.eq(recipientSyncDataMetrics.failed, 0);
    assert.eq(recipientSyncDataMetrics.total, expectedNumRecipientSyncDataCmdSent);

    testDonorForgetMigrationAfterMigrationCompletes(donorRst, recipientRst, migrationId, kTenantId);

    testStats(donorPrimary, {totalSuccessfulMigrationsDonated: 1});
    testStats(recipientPrimary, {totalSuccessfulMigrationsReceived: 1});
})();

(() => {
    jsTest.log(
        "Test the case where the migration aborts after data becomes consistent on the recipient " +
        "but before setting the consistent promise.");
    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: kTenantId,
    };

    let abortRecipientFp =
        configureFailPoint(recipientPrimary,
                           "fpBeforeFulfillingDataConsistentPromise",
                           {action: "stop", stopErrorCode: ErrorCodes.InternalError});
    TenantMigrationTest.assertAborted(
        tenantMigrationTest.runMigration(migrationOpts, {automaticForgetMigration: false}));
    abortRecipientFp.off();

    const donorDoc = configDonorsColl.findOne({tenantId: kTenantId});
    const abortOplogEntry = donorPrimary.getDB("local").oplog.rs.findOne(
        {ns: TenantMigrationTest.kConfigDonorsNS, op: "u", o: donorDoc});
    assert.eq(donorDoc.state, TenantMigrationTest.DonorState.kAborted);
    assert.eq(donorDoc.commitOrAbortOpTime.ts, abortOplogEntry.ts);
    assert.eq(donorDoc.abortReason.code, ErrorCodes.InternalError);

    let mtab;
    assert.soon(() => {
        mtab = TenantMigrationUtil.getTenantMigrationAccessBlocker(
            {donorNode: donorPrimary, tenantId: kTenantId});
        return mtab.donor.state === TenantMigrationTest.DonorAccessState.kAborted;
    });
    assert(mtab.donor.abortOpTime);

    expectedRecipientSyncDataMetricsFailed++;
    expectedNumRecipientSyncDataCmdSent++;
    const recipientSyncDataMetrics =
        recipientPrimary.adminCommand({serverStatus: 1}).metrics.commands.recipientSyncData;
    assert.eq(recipientSyncDataMetrics.failed, expectedRecipientSyncDataMetricsFailed);
    assert.eq(recipientSyncDataMetrics.total, expectedNumRecipientSyncDataCmdSent);

    testDonorForgetMigrationAfterMigrationCompletes(donorRst, recipientRst, migrationId, kTenantId);

    testStats(donorPrimary, {totalSuccessfulMigrationsDonated: 1, totalFailedMigrationsDonated: 1});
    testStats(recipientPrimary,
              {totalSuccessfulMigrationsReceived: 1, totalFailedMigrationsReceived: 1});
})();

(() => {
    jsTest.log("Test the case where the migration aborts");
    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        tenantId: kTenantId,
    };

    let abortDonorFp =
        configureFailPoint(donorPrimary, "abortTenantMigrationBeforeLeavingBlockingState");
    TenantMigrationTest.assertAborted(
        tenantMigrationTest.runMigration(migrationOpts, {automaticForgetMigration: false}));
    abortDonorFp.off();

    const donorDoc = configDonorsColl.findOne({tenantId: kTenantId});
    const abortOplogEntry = donorPrimary.getDB("local").oplog.rs.findOne(
        {ns: TenantMigrationTest.kConfigDonorsNS, op: "u", o: donorDoc});
    assert.eq(donorDoc.state, TenantMigrationTest.DonorState.kAborted);
    assert.eq(donorDoc.commitOrAbortOpTime.ts, abortOplogEntry.ts);
    assert.eq(donorDoc.abortReason.code, ErrorCodes.InternalError);

    let mtab;
    assert.soon(() => {
        mtab = TenantMigrationUtil.getTenantMigrationAccessBlocker(
            {donorNode: donorPrimary, tenantId: kTenantId});
        return mtab.donor.state === TenantMigrationTest.DonorAccessState.kAborted;
    });
    assert(mtab.donor.abortOpTime);

    expectedNumRecipientSyncDataCmdSent += 2;
    const recipientSyncDataMetrics =
        recipientPrimary.adminCommand({serverStatus: 1}).metrics.commands.recipientSyncData;
    assert.eq(recipientSyncDataMetrics.failed, expectedRecipientSyncDataMetricsFailed);
    assert.eq(recipientSyncDataMetrics.total, expectedNumRecipientSyncDataCmdSent);

    testDonorForgetMigrationAfterMigrationCompletes(donorRst, recipientRst, migrationId, kTenantId);

    testStats(donorPrimary, {totalSuccessfulMigrationsDonated: 1, totalFailedMigrationsDonated: 2});
    // The recipient had a chance to synchronize data and from its side the migration succeeded.
    testStats(recipientPrimary,
              {totalSuccessfulMigrationsReceived: 2, totalFailedMigrationsReceived: 1});
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

    TenantMigrationTest.assertCommitted(
        tenantMigrationTest.runMigration(migrationOpts, {automaticForgetMigration: false}));
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
