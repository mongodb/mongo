/**
 * Tests that TTL indexes on the donor are migrated to the recipient and cleanup
 * happens as expected for shard merge.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 *   featureFlagShardMerge,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const tenantMigrationTest =
    new TenantMigrationTest({name: jsTestName(), quickGarbageCollection: true});

const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

// Note: including this explicit early return here due to the fact that multiversion
// suites will execute this test without featureFlagShardMerge enabled (despite the
// presence of the featureFlagShardMerge tag above), which means the test will attempt
// to run a multi-tenant migration and fail.
if (!TenantMigrationUtil.isShardMergeEnabled(recipientPrimary.getDB("admin"))) {
    tenantMigrationTest.stop();
    jsTestLog("Skipping Shard Merge-specific test");
    return;
}

const tenantId = "testTenantId";
const tenantDB = tenantMigrationTest.tenantDB(tenantId, "DB");
const collName = "testColl";

const donorPrimary = tenantMigrationTest.getDonorPrimary();

const expireAfterSeconds = 1;
donorPrimary.getDB(tenantDB)[collName].insertOne({name: "deleteMe", lastModifiedDate: new Date()});
donorPrimary.getDB(tenantDB)[collName].createIndex({"lastModifiedDate": 1}, {expireAfterSeconds});

const hangTTLCollectionCacheAfterRegisteringInfo =
    configureFailPoint(recipientPrimary, "hangTTLCollectionCacheAfterRegisteringInfo");
let hangTTLMonitorBetweenPasses =
    configureFailPoint(recipientPrimary, "hangTTLMonitorBetweenPasses");

// Pause before TTL on the donor to prevent test documents from being cleaned up before migration.
const waitForTTLPassOnDonor = configureFailPoint(donorPrimary, "hangTTLMonitorBetweenPasses");

const migrationUuid = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationUuid),
    readPreference: {mode: 'primary'}
};

assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

// Wait for a TTL pass to start on the Recipient and then block before continuing.
hangTTLMonitorBetweenPasses.wait();

// Wait until we've registered our TTL index in the cache, but block before committing
// the collection in the catalog.
hangTTLCollectionCacheAfterRegisteringInfo.wait();

// Wait for TTL expiry.
sleep(expireAfterSeconds * 1000);

// Unblock the TTL pass on the recipient to let it clean up.
hangTTLMonitorBetweenPasses.off();

// Wait for a full TTL cycle to complete in order to ensure that the TTL cache entry for the
// collection (which does not yet have an entry in the collection catalog) is not deregistered. We
// skip the first pass because it's possible that we can turn off the failpoint and then re-enable
// before the TTL machinery is actually unblocked.
hangTTLMonitorBetweenPasses =
    configureFailPoint(recipientPrimary, "hangTTLMonitorBetweenPasses", {}, {skip: 1});
hangTTLMonitorBetweenPasses.wait();

// Unblock TTL registration, thus allowing the collection to be registered in the catalog.
hangTTLCollectionCacheAfterRegisteringInfo.off();

hangTTLMonitorBetweenPasses.off();

TenantMigrationTest.assertCommitted(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
tenantMigrationTest.waitForMigrationGarbageCollection(migrationOpts);

// Wait for another full TTL pass after the migration completes to ensure we have given the document
// a chance to be deleted.
hangTTLMonitorBetweenPasses =
    configureFailPoint(recipientPrimary, "hangTTLMonitorBetweenPasses", {}, {skip: 1});
hangTTLMonitorBetweenPasses.wait();

const documentCount = recipientPrimary.getDB(tenantDB)[collName].countDocuments({name: "deleteMe"});
assert.eq(documentCount, 0);

hangTTLMonitorBetweenPasses.off();

tenantMigrationTest.stop();
})();
