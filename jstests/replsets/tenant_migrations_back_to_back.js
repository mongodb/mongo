/**
 * Tests a back-to-back migration scenario where we migrate immediately from replica sets A->B->C.
 * Specifically, this tests that when replica set B has both a recipient and donor access blocker,
 * old reads will continue to be blocked by the recipient access blocker even while it acts as a
 * donor for a newly initiated migration.
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

load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");
load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallelTester.js");  // for 'Thread'
load("jstests/libs/uuid_util.js");
load("jstests/replsets/rslib.js");  // for 'getLastOpTime'

const kTenantId = "testTenantId";
const tenantMigrationTest =
    new TenantMigrationTest({name: jsTestName(), insertDataForTenant: kTenantId});

const kDbName = tenantMigrationTest.tenantDB(kTenantId, "testDb");
const kCollName = "testColl";

const donorPrimary = tenantMigrationTest.getDonorPrimary();
const recipientPrimary = tenantMigrationTest.getRecipientPrimary();
const recipientRst = tenantMigrationTest.getRecipientRst();

const migrationId = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    recipientConnString: tenantMigrationTest.getRecipientConnString(),
    tenantId: kTenantId,
};

// Select a read timestamp < blockTimestamp.
const preMigrationTimestamp = getLastOpTime(donorPrimary).ts;
let waitForRejectReadsBeforeTsFp = configureFailPoint(
    recipientPrimary, "fpAfterWaitForRejectReadsBeforeTimestamp", {action: "hang"});

const donorRstArgs = TenantMigrationUtil.createRstArgs(tenantMigrationTest.getDonorRst());
const migrationThread =
    new Thread(TenantMigrationUtil.runMigrationAsync, migrationOpts, donorRstArgs);
migrationThread.start();
waitForRejectReadsBeforeTsFp.wait();

const donorDoc =
    donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS).findOne({tenantId: kTenantId});
assert.lt(preMigrationTimestamp, donorDoc.blockTimestamp);
waitForRejectReadsBeforeTsFp.off();
// Wait for the migration to complete.
jsTest.log("Waiting for migration to complete");
TenantMigrationTest.assertCommitted(migrationThread.returnData());

tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString);

recipientRst.nodes.forEach(node => {
    const db = node.getDB(kDbName);
    const cmd = {
        find: kCollName,
        readConcern: {
            level: "snapshot",
            atClusterTime: preMigrationTimestamp,
        }
    };
    const res = db.runCommand(cmd);
    assert.commandFailedWithCode(res, ErrorCodes.SnapshotTooOld, tojson(cmd));
    assert.eq(res.errmsg, "Tenant read is not allowed before migration completes");
});

jsTestLog("Running a back-to-back migration");
const tenantMigrationTest2 = new TenantMigrationTest(
    {name: jsTestName() + "2", donorRst: tenantMigrationTest.getRecipientRst()});
const donor2Primary = tenantMigrationTest2.getDonorPrimary();
const donor2RstArgs = TenantMigrationUtil.createRstArgs(tenantMigrationTest2.getDonorRst());
const migration2Id = UUID();
const migrationOpts2 = {
    migrationIdString: extractUUIDFromObject(migration2Id),
    recipientConnString: tenantMigrationTest2.getRecipientConnString(),
    tenantId: kTenantId,
};

const newDonorRst = recipientRst;

let waitAfterCreatingMtab =
    configureFailPoint(donor2Primary, "pauseTenantMigrationBeforeLeavingBlockingState");
const migration2Thread =
    new Thread(TenantMigrationUtil.runMigrationAsync, migrationOpts2, donor2RstArgs);
migration2Thread.start();
// At this point, 'donor2Primary' should have both a recipient and donor access blocker. The donor
// access blocker has entered the blocking state, and the recipient access blocker should
// still be blocking reads with timestamps < rejectReadsBeforeTimestamp from the previous migration.
waitAfterCreatingMtab.wait();
// Check that the current serverStatus reflects the recipient access blocker.
const mtabStatus = tenantMigrationTest.getTenantMigrationAccessBlocker(
    {donorNode: donor2Primary, tenantId: kTenantId});
assert.eq(
    mtabStatus.recipient.state, TenantMigrationTest.RecipientAccessState.kRejectBefore, mtabStatus);
assert(mtabStatus.recipient.hasOwnProperty("rejectBeforeTimestamp"), mtabStatus);

// The server value representation of the donor blocking state.
const kBlocking = 3;
const res = assert.commandWorked(
    donor2Primary.adminCommand({currentOp: true, desc: "tenant donor migration"}));
assert.eq(bsonWoCompare(res.inprog[0].instanceID, migration2Id), 0, tojson(res.inprog));
assert.eq(res.inprog[0].lastDurableState, kBlocking, tojson(res.inprog));

// Get the block timestamp for this new migration.
const donorDoc2 =
    donor2Primary.getCollection(TenantMigrationTest.kConfigDonorsNS).findOne({tenantId: kTenantId});
assert.eq(
    mtabStatus.donor.state, TenantMigrationTest.DonorAccessState.kBlockWritesAndReads, mtabStatus);
assert(mtabStatus.donor.hasOwnProperty("blockTimestamp"), mtabStatus);
assert.eq(mtabStatus.donor["blockTimestamp"], donorDoc2.blockTimestamp, mtabStatus);

const blockTimestamp2 = donorDoc2.blockTimestamp;

// The donor access blocker should block reads after the blockTimestamp of the new migration.
newDonorRst.nodes.forEach(node => {
    jsTestLog("Test that read times out on node: " + node);
    const db = node.getDB(kDbName);
    assert.commandFailedWithCode(db.runCommand({
        find: kCollName,
        readConcern: {
            afterClusterTime: blockTimestamp2,
        },
        maxTimeMS: 2 * 1000,
    }),
                                 ErrorCodes.MaxTimeMSExpired);
});

// The recipient access blocker should fail reads before the blockTimestamp of the old migration.
newDonorRst.nodes.forEach(node => {
    jsTestLog("Test that read fails on node: " + node);
    const db = node.getDB(kDbName);
    const cmd = {
        find: kCollName,
        readConcern: {
            level: "snapshot",
            atClusterTime: preMigrationTimestamp,
        }
    };
    const res = db.runCommand(cmd);
    assert.commandFailedWithCode(res, ErrorCodes.SnapshotTooOld, tojson(cmd));
    assert.eq(res.errmsg, "Tenant read is not allowed before migration completes");
});

waitAfterCreatingMtab.off();
TenantMigrationTest.assertCommitted(migration2Thread.returnData());

tenantMigrationTest2.stop();
tenantMigrationTest.stop();
})();
