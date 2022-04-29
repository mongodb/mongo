/**
 * Tests that tenant migration donor can peacefully shut down when there are reads being blocked due
 * to an in-progress migration.
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

load("jstests/libs/parallelTester.js");
load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});

const kTenantId = "testTenantId";
const kDbName = kTenantId + "_testDb";
const kCollName = "testColl";

const donorRst = tenantMigrationTest.getDonorRst();
const donorPrimary = tenantMigrationTest.getDonorPrimary();
const testDb = donorPrimary.getDB(kDbName);

assert.commandWorked(testDb.runCommand({insert: kCollName, documents: [{_id: 0}]}));

const migrationId = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    tenantId: kTenantId,
};

let fp = configureFailPoint(donorPrimary, "pauseTenantMigrationBeforeLeavingBlockingState");
assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

fp.wait();
const donorDoc =
    donorPrimary.getCollection(TenantMigrationTest.kConfigDonorsNS).findOne({tenantId: kTenantId});
assert.neq(null, donorDoc);

let readThread = new Thread((host, dbName, collName, afterClusterTime) => {
    const node = new Mongo(host);
    const db = node.getDB(dbName);
    const res = db.runCommand({
        find: collName,
        readConcern: {afterClusterTime: Timestamp(afterClusterTime.t, afterClusterTime.i)}
    });
    assert.commandFailedWithCode(res, ErrorCodes.InterruptedAtShutdown);
}, donorPrimary.host, kDbName, kCollName, donorDoc.blockTimestamp);
readThread.start();

// Shut down the donor after the read starts blocking.
assert.soon(() => TenantMigrationUtil.getNumBlockedReads(donorPrimary, kTenantId) == 1);
donorRst.stop(donorPrimary);
readThread.join();

donorRst.stopSet();
tenantMigrationTest.stop();
})();
