/**
 * Tests that a tenant migration recipient instance shows up as active in serverStatus metrics until
 * it has directly deleted its state doc.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   # Shard merge recipient state doc deletion is no longer managed by TTL monitor.
 *   incompatible_with_shard_merge,
 *   # Uses pauseTenantMigrationRecipientBeforeDeletingStateDoc failpoint, which was added in 6.2
 *   requires_fcv_62,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 *   requires_fcv_71,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {isShardMergeEnabled} from "jstests/replsets/libs/tenant_migration_util.js";

jsTest.log("Test case where the instance deletes the state doc");

const tmt = new TenantMigrationTest({name: jsTestName(), quickGarbageCollection: true});

const recipientPrimary = tmt.getRecipientPrimary();

if (isShardMergeEnabled(recipientPrimary.getDB("admin"))) {
    tmt.stop();
    jsTestLog("Skipping this shard merge incompatible test.");
    quit();
}

jsTest.log("Confirm serverStatus does not show any active or completed tenant migrations.");
let recipientStats = tmt.getTenantMigrationStats(recipientPrimary);
jsTest.log("recipientStats: " + tojson(recipientStats));
assert.eq(0, recipientStats.currentMigrationsReceiving);

jsTest.log("Start a tenant migration.");
const tenantId = ObjectId().str;
const migrationId = extractUUIDFromObject(UUID());
const migrationOpts = {
    migrationIdString: migrationId,
    tenantId: tenantId,
    recipientConnString: tmt.getRecipientConnString(),
};
TenantMigrationTest.assertCommitted(tmt.runMigration(migrationOpts));

jsTest.log("Wait for the instance to delete the state doc");
assert.soon(() => {
    return 0 == recipientPrimary.getDB("config").getCollection("tenantMigrationRecipients").count();
});

jsTest.log("Confirm the instance eventually stops showing up as active in serverStatus");
assert.soon(() => {
    recipientStats = tmt.getTenantMigrationStats(recipientPrimary);
    return 0 == recipientStats.currentMigrationsReceiving;
});

// TODO (SERVER-61717): Confirm the instance eventually stops showing up in the POS map.

tmt.stop();
