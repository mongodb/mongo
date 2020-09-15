/**
 * Tests the TenantMigrationAccessBlocker and donor state document are updated correctly at each
 * stage of the migration, and are eventually removed after the donorForgetMigration has returned.
 *
 * Tenant migrations are not expected to be run on servers with ephemeralForTest, and in particular
 * this test fails on ephemeralForTest because the donor has to wait for the write to set the
 * migration state to "committed" and "aborted" to be majority committed but it cannot do that on
 * ephemeralForTest.
 *
 * @tags: [requires_fcv_47, incompatible_with_eft]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/uuid_util.js");

let expectedNumRecipientSyncDataCmdSent = 0;
let expectedNumRecipientForgetMigrationCmdSent = 0;

/**
 * Runs the donorForgetMigration command and asserts that the TenantMigrationAccessBlocker and donor
 * state document are eventually removed from the donor.
 */
function testDonorForgetMigration(donorRst, recipientRst, migrationId, dbPrefix) {
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

    assert.soon(
        () => donorPrimary.getCollection(kConfigDonorsNS).count({databasePrefix: dbPrefix}) === 0);

    const donorRecipientMonitorPoolStats =
        donorPrimary.adminCommand({connPoolStats: 1}).replicaSets;
    assert.eq(Object.keys(donorRecipientMonitorPoolStats).length, 0);
}

// An object that mirrors the access states for the TenantMigrationAccessBlocker.
const accessState = {
    kAllow: 0,
    kBlockingWrites: 1,
    kBlockingReadsAndWrites: 2,
    kReject: 3
};

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
const recipientRst = new ReplSetTest(
    {nodes: 1, name: "recipient", nodeOptions: {setParameter: {enableTenantMigrations: true}}});

donorRst.startSet();
donorRst.initiate();

recipientRst.startSet();
recipientRst.initiate();

const donorPrimary = donorRst.getPrimary();
const recipientPrimary = recipientRst.getPrimary();
const kRecipientConnString = recipientRst.getURL();

const kDBPrefix = "testDb";
const kConfigDonorsNS = "config.tenantMigrationDonors";

let configDonorsColl = donorPrimary.getCollection(kConfigDonorsNS);
configDonorsColl.createIndex({expireAt: 1}, {expireAfterSeconds: 0});

(() => {
    jsTest.log("Test the case where the migration commits");
    const migrationId = UUID();

    function startMigration(host, recipientConnString, dbPrefix, migrationIdString) {
        const primary = new Mongo(host);
        assert.commandWorked(primary.adminCommand({
            donorStartMigration: 1,
            migrationId: UUID(migrationIdString),
            recipientConnectionString: recipientConnString,
            databasePrefix: dbPrefix,
            readPreference: {mode: "primary"}
        }));
    }

    let migrationThread = new Thread(startMigration,
                                     donorPrimary.host,
                                     kRecipientConnString,
                                     kDBPrefix,
                                     extractUUIDFromObject(migrationId));
    let blockingFp = configureFailPoint(donorPrimary, "pauseTenantMigrationAfterBlockingStarts");
    migrationThread.start();

    // Wait for the migration to enter the blocking state.
    blockingFp.wait();

    let mtab = donorPrimary.adminCommand({serverStatus: 1}).tenantMigrationAccessBlocker;
    assert.eq(mtab[kDBPrefix].access, accessState.kBlockingReadsAndWrites);
    assert(mtab[kDBPrefix].blockTimestamp);

    let donorDoc = configDonorsColl.findOne({databasePrefix: kDBPrefix});
    let blockOplogEntry = donorPrimary.getDB("local").oplog.rs.findOne(
        {ns: kConfigDonorsNS, op: "u", "o.databasePrefix": kDBPrefix});
    assert.eq(donorDoc.state, "blocking");
    assert.eq(donorDoc.blockTimestamp, blockOplogEntry.ts);

    // Allow the migration to complete.
    blockingFp.off();
    migrationThread.join();

    mtab = donorPrimary.adminCommand({serverStatus: 1}).tenantMigrationAccessBlocker;
    assert.eq(mtab[kDBPrefix].access, accessState.kReject);
    assert(mtab[kDBPrefix].commitOrAbortOpTime);

    donorDoc = configDonorsColl.findOne({databasePrefix: kDBPrefix});
    let commitOplogEntry =
        donorPrimary.getDB("local").oplog.rs.findOne({ns: kConfigDonorsNS, op: "u", o: donorDoc});
    assert.eq(donorDoc.state, "committed");
    assert.eq(donorDoc.commitOrAbortOpTime.ts, commitOplogEntry.ts);

    expectedNumRecipientSyncDataCmdSent += 2;
    const recipientSyncDataMetrics =
        recipientPrimary.adminCommand({serverStatus: 1}).metrics.commands.recipientSyncData;
    assert.eq(recipientSyncDataMetrics.failed, 0);
    assert.eq(recipientSyncDataMetrics.total, expectedNumRecipientSyncDataCmdSent);

    testDonorForgetMigration(donorRst, recipientRst, migrationId, kDBPrefix);
})();

(() => {
    jsTest.log("Test the case where the migration aborts");
    const migrationId = UUID();

    let abortFp = configureFailPoint(donorPrimary, "abortTenantMigrationAfterBlockingStarts");
    assert.commandFailedWithCode(donorPrimary.adminCommand({
        donorStartMigration: 1,
        migrationId: migrationId,
        recipientConnectionString: kRecipientConnString,
        databasePrefix: kDBPrefix,
        readPreference: {mode: "primary"}
    }),
                                 ErrorCodes.TenantMigrationAborted);
    abortFp.off();

    const mtab = donorPrimary.adminCommand({serverStatus: 1}).tenantMigrationAccessBlocker;
    assert.eq(mtab[kDBPrefix].access, accessState.kAllow);
    assert(!mtab[kDBPrefix].commitOrAbortOpTime);

    const donorDoc = configDonorsColl.findOne({databasePrefix: kDBPrefix});
    const abortOplogEntry =
        donorPrimary.getDB("local").oplog.rs.findOne({ns: kConfigDonorsNS, op: "u", o: donorDoc});
    assert.eq(donorDoc.state, "aborted");
    assert.eq(donorDoc.commitOrAbortOpTime.ts, abortOplogEntry.ts);

    expectedNumRecipientSyncDataCmdSent += 2;
    const recipientSyncDataMetrics =
        recipientPrimary.adminCommand({serverStatus: 1}).metrics.commands.recipientSyncData;
    assert.eq(recipientSyncDataMetrics.failed, 0);
    assert.eq(recipientSyncDataMetrics.total, expectedNumRecipientSyncDataCmdSent);

    testDonorForgetMigration(donorRst, recipientRst, migrationId, kDBPrefix);
})();

// Drop the TTL index to make sure that the migration state is still available when the
// donorForgetMigration command is retried.
configDonorsColl.dropIndex({expireAt: 1});

(() => {
    jsTest.log("Test that donorForgetMigration can be run multiple times");
    const migrationId = UUID();

    assert.commandWorked(donorPrimary.adminCommand({
        donorStartMigration: 1,
        migrationId: migrationId,
        recipientConnectionString: kRecipientConnString,
        databasePrefix: kDBPrefix,
        readPreference: {mode: "primary"}
    }));

    assert.commandWorked(
        donorPrimary.adminCommand({donorForgetMigration: 1, migrationId: migrationId}));

    assert.commandWorked(
        donorPrimary.adminCommand({donorForgetMigration: 1, migrationId: migrationId}));
})();

donorRst.stopSet();
recipientRst.stopSet();
})();
