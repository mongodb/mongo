/**
 * Test that the shard merge is resilient to WriteConflict exception thrown while importing
 * collections. We will get WriteConflict exception if we try to import the files with timestamp
 * older than the stable timestamp.
 *
 * TODO (SERVER-61133): Adapt or delete this test once file copy works.
 *
 * @tags: [
 *   does_not_support_encrypted_storage_engine,
 *   featureFlagShardMerge,
 *   incompatible_with_eft,
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_fcv_52,
 *   requires_journaling,
 *   requires_replication,
 *   requires_persistence,
 *   requires_wiredtiger,
 * ]
 */
(function() {
"use strict";

// TODO (SERVER-61144): Recipient secondaries don't import donor files yet, so dbhash will mismatch.
TestData.skipCheckDBHashes = true;

load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/libs/fail_point_util.js");

const migrationId = UUID();
const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
const donorPrimary = tenantMigrationTest.getDonorPrimary();
const recipientPrimary = tenantMigrationTest.getRecipientPrimary();
const kDataDir =
    `${recipientPrimary.dbpath}/migrationTmpFiles.${extractUUIDFromObject(migrationId)}`;
assert.eq(runNonMongoProgram("mkdir", "-p", kDataDir), 0);

(function() {
jsTestLog("Generate test data: open a backup cursor on the donor and copy files");

const db = donorPrimary.getDB("myDatabase");
const collection = db["myCollection"];
const capped = db["myCappedCollection"];
assert.commandWorked(db.createCollection("myCappedCollection", {capped: true, size: 100}));
for (let c of [collection, capped]) {
    c.insertMany([{_id: 0}, {_id: 1}, {_id: 2}], {writeConcern: {w: "majority"}});
}

assert.commandWorked(db.runCommand({
    createIndexes: "myCollection",
    indexes: [{key: {a: 1}, name: "a_1"}],
    writeConcern: {w: "majority"}
}));

// Ensure our new collections appear in the backup cursor's checkpoint.
assert.commandWorked(db.adminCommand({fsync: 1}));

const reply = assert.commandWorked(
    donorPrimary.adminCommand({aggregate: 1, cursor: {}, pipeline: [{"$backupCursor": {}}]}));
const cursor = reply.cursor;

jsTestLog(`Backup cursor metadata: ${tojson(cursor.firstBatch[0].metadata)}`);
jsTestLog("Copy files to local data dir");
for (let f of cursor.firstBatch) {
    if (!f.hasOwnProperty("filename")) {
        continue;
    }

    assert(f.filename.startsWith(donorPrimary.dbpath));
    const suffix = f.filename.slice(donorPrimary.dbpath.length);

    /*
     * Create directories as needed, e.g. copy
     * /data/db/job0/mongorunner/test-0/journal/WiredTigerLog.01 to
     * /data/db/job0/mongorunner/test-1/migrationTmpFiles.migrationId/journal/WiredTigerLog.01,
     * by passing "--relative /data/db/job0/mongorunner/test-0/./journal/WiredTigerLog.01".
     * Note the "/./" marker.
     */
    assert.eq(runNonMongoProgram(
                  "rsync", "-a", "--relative", `${donorPrimary.dbpath}/.${suffix}`, kDataDir),
              0);
}

jsTestLog("Kill backup cursor");
donorPrimary.adminCommand({killCursors: "$cmd.aggregate", cursors: [cursor.id]});
})();

// Enable Failpoints to simulate WriteConflict exception while importing donor files.
configureFailPoint(
    recipientPrimary, "WTWriteConflictExceptionForImportCollection", {} /* data */, {times: 1});
configureFailPoint(
    recipientPrimary, "WTWriteConflictExceptionForImportIndex", {} /* data */, {times: 1});

jsTestLog("Run migration");
// The old multitenant migrations won't copy myDatabase since it doesn't start with testTenantId,
// but shard merge copies everything so we still expect myDatabase on the recipient, below.
const kTenantId = "testTenantId";
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    tenantId: kTenantId,
};
TenantMigrationTest.assertCommitted(tenantMigrationTest.runMigration(migrationOpts));
for (let collectionName of ["myCollection", "myCappedCollection"]) {
    jsTestLog(`Checking ${collectionName}`);
    // Use "countDocuments" to check actual docs, "count" to check sizeStorer data.
    assert.eq(donorPrimary.getDB("myDatabase")[collectionName].countDocuments({}),
              recipientPrimary.getDB("myDatabase")[collectionName].countDocuments({}),
              "countDocuments");
    assert.eq(donorPrimary.getDB("myDatabase")[collectionName].count(),
              recipientPrimary.getDB("myDatabase")[collectionName].count(),
              "count");
}

tenantMigrationTest.stop();
})();
