/**
 * Tests the TenantMigrationAccessBlocker and donor state document are updated correctly after
 * the donorStartMigration command is run.
 *
 * @tags: [requires_fcv_46]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");

// An object that mirrors the access states for the TenantMigrationAccessBlocker.
const accessState = {
    kAllow: 0,
    kBlockingWrites: 1,
    kBlockingReadsAndWrites: 2,
    kReject: 3
};

const donorRst =
    new ReplSetTest({nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}]});
const recipientRst = new ReplSetTest({nodes: 1});

donorRst.startSet();
donorRst.initiate();

const donorPrimary = donorRst.getPrimary();
const kRecipientConnString = recipientRst.getURL();
const kDBPrefix = 'testDb';
const kConfigDonorsNS = "config.tenantMigrationDonors";

(() => {
    // Test the case where the migration commits.
    const dbName = kDBPrefix + "Commit";

    function startMigration(host, recipientConnString, dbName) {
        const primary = new Mongo(host);
        assert.commandWorked(primary.adminCommand({
            donorStartMigration: 1,
            migrationId: UUID(),
            recipientConnectionString: recipientConnString,
            databasePrefix: dbName,
            readPreference: {mode: "primary"}
        }));
    }

    let migrationThread =
        new Thread(startMigration, donorPrimary.host, kRecipientConnString, dbName);
    let blockingFp = configureFailPoint(donorPrimary, "pauseTenantMigrationAfterBlockingStarts");
    migrationThread.start();

    // Wait for the migration to enter the blocking state.
    blockingFp.wait();

    let mtab = donorPrimary.adminCommand({serverStatus: 1}).tenantMigrationAccessBlocker;
    assert.eq(mtab[dbName].access, accessState.kBlockingReadsAndWrites);
    assert(mtab[dbName].blockTimestamp);

    let donorDoc = donorPrimary.getCollection(kConfigDonorsNS).findOne({databasePrefix: dbName});
    let blockOplogEntry = donorPrimary.getDB("local").oplog.rs.findOne(
        {ns: kConfigDonorsNS, op: "u", "o.databasePrefix": dbName});
    assert.eq(donorDoc.state, "blocking");
    assert.eq(donorDoc.blockTimestamp, blockOplogEntry.ts);

    // Allow the migration to complete.
    blockingFp.off();
    migrationThread.join();

    // TODO (SERVER-49176): test that mtab is updated correctly.

    donorDoc = donorPrimary.getCollection(kConfigDonorsNS).findOne({databasePrefix: dbName});
    let commitOplogEntry =
        donorPrimary.getDB("local").oplog.rs.findOne({ns: kConfigDonorsNS, op: "u", o: donorDoc});
    assert.eq(donorDoc.state, "committed");
    assert.eq(donorDoc.commitOrAbortOpTime.ts, commitOplogEntry.ts);
})();

(() => {
    // Test the case where the migration aborts.
    const dbName = kDBPrefix + "Abort";

    let abortFp = configureFailPoint(donorPrimary, "abortTenantMigrationAfterBlockingStarts");
    assert.commandFailedWithCode(donorPrimary.adminCommand({
        donorStartMigration: 1,
        migrationId: UUID(),
        recipientConnectionString: kRecipientConnString,
        databasePrefix: dbName,
        readPreference: {mode: "primary"}
    }),
                                 ErrorCodes.InternalError);
    abortFp.off();

    // TODO (SERVER-49176): test that mtab is updated correctly.

    const donorDoc = donorPrimary.getCollection(kConfigDonorsNS).findOne({databasePrefix: dbName});
    const abortOplogEntry =
        donorPrimary.getDB("local").oplog.rs.findOne({ns: kConfigDonorsNS, op: "u", o: donorDoc});
    assert.eq(donorDoc.state, "aborted");
    assert.eq(donorDoc.commitOrAbortOpTime.ts, abortOplogEntry.ts);
})();

donorRst.stopSet();
})();
