/**
 * Tests that the shard merge recipient correctly fetches retryable writes.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   featureFlagShardMerge,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/retryable_writes_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/libs/uuid_util.js");  // For extractUUIDFromObject().

if (!RetryableWritesUtil.storageEngineSupportsRetryableWrites(jsTest.options().storageEngine)) {
    jsTestLog("Retryable writes are not supported, skipping test");
    return;
}

const kParams = {
    ttlMonitorSleepSecs: 1,
};

const tenantMigrationTest = new TenantMigrationTest({
    name: jsTestName(),
    sharedOptions: {nodes: 1, setParameter: kParams},
    quickGarbageCollection: true
});

const kTenantId = "testTenantId";
const tenantDB = tenantMigrationTest.tenantDB(kTenantId, "database");

const donorRst = tenantMigrationTest.getDonorRst();
const donorPrimary = tenantMigrationTest.getDonorPrimary();
const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

jsTestLog("Run retryable write prior to the migration");

const lsid = UUID();
const cmd = {
    insert: "collection",
    documents: [{_id: 1}, {_id: 2}],
    ordered: false,
    lsid: {id: lsid},
    txnNumber: NumberLong(123),
};

assert.commandWorked(donorPrimary.getDB(tenantDB).runCommand(cmd));
assert.eq(2, donorPrimary.getDB(tenantDB).collection.find().itcount());

const migrationId = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    tenantId: kTenantId,
};

jsTestLog(`Starting migration: ${tojson(migrationOpts)}`);
TenantMigrationTest.assertCommitted(tenantMigrationTest.runMigration(migrationOpts));

const {ok, n} = assert.commandWorked(recipientPrimary.getDB(tenantDB).runCommand(cmd));
assert.eq(1, ok);
assert.eq(2, n);
assert.eq(2, recipientPrimary.getDB(tenantDB).collection.find().itcount());

tenantMigrationTest.stop();
})();
