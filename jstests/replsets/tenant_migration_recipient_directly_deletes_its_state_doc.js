/**
 * Tests that a tenant migration recipient instance shows up as active in serverStatus metrics until
 * it has directly deleted its state doc (even if the state doc has actually already been deleted by
 * the TTL monitor).
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   # Uses pauseTenantMigrationRecipientBeforeDeletingStateDoc failpoint, which was added in 6.2
 *   requires_fcv_62,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 * ]
 */

import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");

(() => {
    jsTest.log("Test case where the TTL monitor deletes the state doc first");

    const tmt = new TenantMigrationTest({name: jsTestName(), quickGarbageCollection: true});

    const recipientPrimary = tmt.getRecipientPrimary();
    const fp =
        configureFailPoint(recipientPrimary, "pauseTenantMigrationRecipientBeforeDeletingStateDoc");

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

    jsTest.log("Wait for the migration to reach the failpoint before deleting the state doc.");
    fp.wait();

    jsTest.log("Confirm the state doc has been deleted by the TTL monitor");
    assert.soon(() => {
        return 0 ==
            recipientPrimary.getDB("config").getCollection("tenantMigrationRecipients").count();
    });

    jsTest.log("Confirm the instance still shows up as active in serverStatus.");
    recipientStats = tmt.getTenantMigrationStats(recipientPrimary);
    jsTest.log("recipientStats: " + tojson(recipientStats));
    assert.eq(1, recipientStats.currentMigrationsReceiving);

    // TODO (SERVER-61717): Confirm the instance still shows up in the POS map. Currently, the
    // instance is removed from the map as soon as its' state doc is deleted by the TTL monitor.

    jsTest.log("Turn off the failpoint.");
    fp.off();

    jsTest.log("Confirm the instance eventually stops showing up as active in serverStatus");
    assert.soon(() => {
        recipientStats = tmt.getTenantMigrationStats(recipientPrimary);
        return 0 == recipientStats.currentMigrationsReceiving;
    });

    // TODO (SERVER-61717): Confirm the instance eventually stops showing up in the POS map.

    tmt.stop();
})();

(() => {
    jsTest.log("Test case where the instance deletes the state doc first");

    const tmt = new TenantMigrationTest({name: jsTestName(), quickGarbageCollection: true});

    const recipientPrimary = tmt.getRecipientPrimary();

    jsTest.log("Confirm the TTL index exists");
    const listIndexesRes1 = assert.commandWorked(
        recipientPrimary.getDB("config").runCommand({listIndexes: "tenantMigrationRecipients"}));
    assert(listIndexesRes1.cursor.firstBatch.some(
        elem => elem.name === "TenantMigrationRecipientTTLIndex" && elem.key.expireAt === 1));

    jsTest.log("Drop the TTL index");
    assert.commandWorked(recipientPrimary.getDB("config").runCommand(
        {dropIndexes: "tenantMigrationRecipients", index: "TenantMigrationRecipientTTLIndex"}));

    jsTest.log("Confirm the TTL index no longer exists");
    const listIndexesRes2 = assert.commandWorked(
        recipientPrimary.getDB("config").runCommand({listIndexes: "tenantMigrationRecipients"}));
    assert(listIndexesRes2.cursor.firstBatch.every(elem => elem.key.expireAt == null));

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
        return 0 ==
            recipientPrimary.getDB("config").getCollection("tenantMigrationRecipients").count();
    });

    jsTest.log("Confirm the instance eventually stops showing up as active in serverStatus");
    assert.soon(() => {
        recipientStats = tmt.getTenantMigrationStats(recipientPrimary);
        return 0 == recipientStats.currentMigrationsReceiving;
    });

    // TODO (SERVER-61717): Confirm the instance eventually stops showing up in the POS map.

    tmt.stop();
})();
