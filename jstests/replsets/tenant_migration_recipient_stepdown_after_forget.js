/**
 * Tests whether the new recipient primary properly processes a forgetMigration when the original
 * primary is made to step down after marking as garbage collectable. The oplog buffer collection
 * must be dropped.
 *
 * @tags: [requires_fcv_49, requires_replication, incompatible_with_windows_tls]
 */

(function() {

"use strict";
load("jstests/libs/uuid_util.js");        // For extractUUIDFromObject().
load("jstests/libs/fail_point_util.js");  // For configureFailPoint().
load("jstests/libs/parallelTester.js");   // For Thread(), used for async forgetMigration.
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const tenantMigrationTest =
    new TenantMigrationTest({name: jsTestName(), sharedOptions: {nodes: 2}});

if (!tenantMigrationTest.isFeatureFlagEnabled()) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    tenantMigrationTest.stop();
    return;
}

const kMigrationId = UUID();
const kTenantId = 'testTenantId';
const kReadPreference = {
    mode: "primary"
};
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(kMigrationId),
    tenantId: kTenantId,
    readPreference: kReadPreference
};

tenantMigrationTest.runMigration(
    migrationOpts, true /* retryOnRetryableErrors */, false /* automaticForgetMigration */);

const fpBeforeDroppingOplogBufferCollection =
    configureFailPoint(tenantMigrationTest.getRecipientPrimary(),
                       "fpBeforeDroppingOplogBufferCollection",
                       {action: "hang"});

jsTestLog("Issuing a forget migration command.");
const forgetMigrationThread =
    new Thread(TenantMigrationUtil.forgetMigrationAsync,
               migrationOpts.migrationIdString,
               TenantMigrationUtil.createRstArgs(tenantMigrationTest.getDonorRst()),
               true /* retryOnRetryableErrors */);
forgetMigrationThread.start();

fpBeforeDroppingOplogBufferCollection.wait();

jsTestLog("Step up a new recipient primary.");
assert.commandWorked(tenantMigrationTest.getRecipientRst().getSecondaries()[0].adminCommand(
    {replSetStepUp: ReplSetTest.kForeverSecs, force: true}));

fpBeforeDroppingOplogBufferCollection.off();

jsTestLog("Waiting for forget migration to complete.");
assert.commandWorked(forgetMigrationThread.returnData());

const configDBCollections =
    tenantMigrationTest.getRecipientPrimary().getDB('config').getCollectionNames();
assert(!configDBCollections.includes('repl.migration.oplog_' + migrationOpts.migrationIdString),
       configDBCollections);

tenantMigrationTest.stop();
})();