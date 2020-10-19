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
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_util.js");

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

    assert.soon(() =>
                    0 === donorPrimary.getCollection(kConfigDonorsNS).count({tenantId: tenantId}));
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
const recipientRst = new ReplSetTest({
    nodes: 1,
    name: "recipient",
    nodeOptions: {
        setParameter: {
            enableTenantMigrations: true,
            // TODO SERVER-51734: Remove the failpoint 'returnResponseOkForRecipientSyncDataCmd'.
            'failpoint.returnResponseOkForRecipientSyncDataCmd': tojson({mode: 'alwaysOn'})
        }
    }
});

donorRst.startSet();
donorRst.initiate();

recipientRst.startSet();
recipientRst.initiate();

const donorPrimary = donorRst.getPrimary();
const recipientPrimary = recipientRst.getPrimary();
const kRecipientConnString = recipientRst.getURL();

const kTenantId = "testDb";
const kConfigDonorsNS = "config.tenantMigrationDonors";

let configDonorsColl = donorPrimary.getCollection(kConfigDonorsNS);
configDonorsColl.createIndex({expireAt: 1}, {expireAfterSeconds: 0});

(() => {
    jsTest.log("Test the case where the migration commits");
    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        recipientConnString: recipientRst.getURL(),
        tenantId: kTenantId,
        readPreference: {mode: "primary"},
    };

    let migrationThread =
        new Thread(TenantMigrationUtil.startMigration, donorPrimary.host, migrationOpts);
    let blockingFp = configureFailPoint(donorPrimary, "pauseTenantMigrationAfterBlockingStarts");
    migrationThread.start();

    // Wait for the migration to enter the blocking state.
    blockingFp.wait();

    let mtab = donorPrimary.adminCommand({serverStatus: 1}).tenantMigrationAccessBlocker;
    assert.eq(mtab[kTenantId].access, TenantMigrationUtil.accessState.kBlockingReadsAndWrites);
    assert(mtab[kTenantId].blockTimestamp);

    let donorDoc = configDonorsColl.findOne({tenantId: kTenantId});
    let blockOplogEntry = donorPrimary.getDB("local").oplog.rs.findOne(
        {ns: kConfigDonorsNS, op: "u", "o.tenantId": kTenantId});
    assert.eq(donorDoc.state, "blocking");
    assert.eq(donorDoc.blockTimestamp, blockOplogEntry.ts);

    // Verify that donorForgetMigration fails since the decision has not been made.
    assert.commandFailedWithCode(
        donorPrimary.adminCommand({donorForgetMigration: 1, migrationId: migrationId}),
        ErrorCodes.TenantMigrationInProgress);

    // Allow the migration to complete.
    blockingFp.off();
    migrationThread.join();
    const res = assert.commandWorked(migrationThread.returnData());
    assert.eq(res.state, "committed");

    donorDoc = configDonorsColl.findOne({tenantId: kTenantId});
    let commitOplogEntry =
        donorPrimary.getDB("local").oplog.rs.findOne({ns: kConfigDonorsNS, op: "u", o: donorDoc});
    assert.eq(donorDoc.state, "committed");
    assert.eq(donorDoc.commitOrAbortOpTime.ts, commitOplogEntry.ts);

    assert.soon(() => {
        mtab = donorPrimary.adminCommand({serverStatus: 1}).tenantMigrationAccessBlocker;
        return mtab[kTenantId].access === TenantMigrationUtil.accessState.kReject;
    });
    assert(mtab[kTenantId].commitOrAbortOpTime);

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
        recipientConnString: recipientRst.getURL(),
        tenantId: kTenantId,
        readPreference: {mode: "primary"},
    };

    let abortFp = configureFailPoint(donorPrimary, "abortTenantMigrationAfterBlockingStarts");
    const res =
        assert.commandWorked(TenantMigrationUtil.startMigration(donorPrimary.host, migrationOpts));
    assert.eq(res.state, "aborted");
    abortFp.off();

    const donorDoc = configDonorsColl.findOne({tenantId: kTenantId});
    const abortOplogEntry =
        donorPrimary.getDB("local").oplog.rs.findOne({ns: kConfigDonorsNS, op: "u", o: donorDoc});
    assert.eq(donorDoc.state, "aborted");
    assert.eq(donorDoc.commitOrAbortOpTime.ts, abortOplogEntry.ts);
    assert.eq(donorDoc.abortReason.code, ErrorCodes.InternalError);

    let mtab;
    assert.soon(() => {
        mtab = donorPrimary.adminCommand({serverStatus: 1}).tenantMigrationAccessBlocker;
        return mtab[kTenantId].access === TenantMigrationUtil.accessState.kAllow;
    });
    assert(mtab[kTenantId].commitOrAbortOpTime);

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
        recipientConnString: recipientRst.getURL(),
        tenantId: kTenantId,
        readPreference: {mode: "primary"},
    };

    // Verify that donorForgetMigration fails since the migration hasn't started.
    assert.commandFailedWithCode(
        donorPrimary.adminCommand({donorForgetMigration: 1, migrationId: migrationId}),
        ErrorCodes.NoSuchTenantMigration);

    const res =
        assert.commandWorked(TenantMigrationUtil.startMigration(donorPrimary.host, migrationOpts));
    assert.eq(res.state, "committed");
    assert.commandWorked(
        donorPrimary.adminCommand({donorForgetMigration: 1, migrationId: migrationId}));

    // Verify that the retry succeeds.
    assert.commandWorked(
        donorPrimary.adminCommand({donorForgetMigration: 1, migrationId: migrationId}));
})();

donorRst.stopSet();
recipientRst.stopSet();
})();
