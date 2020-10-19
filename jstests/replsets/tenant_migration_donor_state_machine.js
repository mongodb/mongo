/**
 * Tests the TenantMigrationAccessBlocker and donor state document are updated correctly at each
 * stage of the migration, and are eventually removed after the donorForgetMigration has returned.
 *
 * Tenant migrations are not expected to be run on servers with ephemeralForTest, and in particular
 * this test fails on ephemeralForTest because the donor has to wait for the write to set the
 * migration state to "committed" and "aborted" to be majority committed but it cannot do that on
 * ephemeralForTest.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, incompatible_with_eft]
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
}

const donorRst = new ReplSetTest({
    nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}],
    name: "donor",
    nodeOptions: {
        setParameter: {
            enableTenantMigrations: true,

            // Set the delay before a donor state doc is garbage collected to be short to speed up
            // the test.
            tenantMigrationGarbageCollectionDelayMS: 3 * 1000,

            // Set the TTL monitor to run at a smaller interval to speed up the test.
            ttlMonitorSleepSecs: 1,
        }
    }
});

donorRst.startSet();
donorRst.initiate();

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName(), donorRst});
const recipientRst = tenantMigrationTest.getRecipientRst();

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

    let blockingFp = configureFailPoint(donorPrimary, "pauseTenantMigrationAfterBlockingStarts");
    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

    // Wait for the migration to enter the blocking state.
    blockingFp.wait();

    let mtabs = donorPrimary.adminCommand({serverStatus: 1}).tenantMigrationAccessBlocker;
    assert.eq(mtabs[kTenantId].state, TenantMigrationTest.AccessState.kBlockWritesAndReads);
    assert(mtabs[kTenantId].blockTimestamp);

    let donorDoc = configDonorsColl.findOne({tenantId: kTenantId});
    let blockOplogEntry = donorPrimary.getDB("local").oplog.rs.findOne(
        {ns: TenantMigrationTest.kConfigDonorsNS, op: "u", "o.tenantId": kTenantId});
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
    assert.eq(stateRes.state, TenantMigrationTest.State.kCommitted);

    donorDoc = configDonorsColl.findOne({tenantId: kTenantId});
    let commitOplogEntry = donorPrimary.getDB("local").oplog.rs.findOne(
        {ns: TenantMigrationTest.kConfigDonorsNS, op: "u", o: donorDoc});
    assert.eq(donorDoc.state, TenantMigrationTest.State.kCommitted);
    assert.eq(donorDoc.commitOrAbortOpTime.ts, commitOplogEntry.ts);

    assert.soon(() => {
        mtabs = donorPrimary.adminCommand({serverStatus: 1}).tenantMigrationAccessBlocker;
        return mtabs[kTenantId].state === TenantMigrationTest.AccessState.kReject;
    });
    assert(mtabs[kTenantId].commitOrAbortOpTime);

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

    let abortFp = configureFailPoint(donorPrimary, "abortTenantMigrationAfterBlockingStarts");
    const stateRes = assert.commandWorked(tenantMigrationTest.runMigration(migrationOpts));
    assert.eq(stateRes.state, TenantMigrationTest.State.kAborted);
    abortFp.off();

    const donorDoc = configDonorsColl.findOne({tenantId: kTenantId});
    const abortOplogEntry = donorPrimary.getDB("local").oplog.rs.findOne(
        {ns: TenantMigrationTest.kConfigDonorsNS, op: "u", o: donorDoc});
    assert.eq(donorDoc.state, TenantMigrationTest.State.kAborted);
    assert.eq(donorDoc.commitOrAbortOpTime.ts, abortOplogEntry.ts);
    assert.eq(donorDoc.abortReason.code, ErrorCodes.InternalError);

    let mtabs;
    assert.soon(() => {
        mtabs = donorPrimary.adminCommand({serverStatus: 1}).tenantMigrationAccessBlocker;
        return mtabs[kTenantId].state === TenantMigrationTest.AccessState.kAborted;
    });
    assert(mtabs[kTenantId].commitOrAbortOpTime);

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

    const stateRes = assert.commandWorked(tenantMigrationTest.runMigration(migrationOpts));
    assert.eq(stateRes.state, TenantMigrationTest.State.kCommitted);
    assert.commandWorked(
        donorPrimary.adminCommand({donorForgetMigration: 1, migrationId: migrationId}));

    // Verify that the retry succeeds.
    assert.commandWorked(
        donorPrimary.adminCommand({donorForgetMigration: 1, migrationId: migrationId}));
})();

tenantMigrationTest.stop();
donorRst.stopSet();
})();
