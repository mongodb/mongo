/**
 * Tests whether the new recipient primary properly processes a forgetMigration when the original
 * primary is made to step down after marking as garbage collectable. The oplog buffer collection
 * must be dropped.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_persistence,
 *   requires_replication,
 *   serverless,
 * ]
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

TenantMigrationTest.assertCommitted(tenantMigrationTest.runMigration(
    migrationOpts, {retryOnRetryableErrors: true, automaticForgetMigration: false}));

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
tenantMigrationTest.getRecipientRst().stepUp(
    tenantMigrationTest.getRecipientRst().getSecondaries()[0]);

fpBeforeDroppingOplogBufferCollection.off();

jsTestLog("Waiting for forget migration to complete.");
assert.commandWorked(forgetMigrationThread.returnData());

const configDBCollections =
    tenantMigrationTest.getRecipientPrimary().getDB('config').getCollectionNames();
assert(!configDBCollections.includes('repl.migration.oplog_' + migrationOpts.migrationIdString),
       configDBCollections);

tenantMigrationTest.stop();
})();
