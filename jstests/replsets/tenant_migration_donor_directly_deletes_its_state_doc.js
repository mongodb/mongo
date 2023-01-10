/**
 * Tests that a tenant migration donor instance shows up as active in serverStatus metrics until it
 * has directly deleted its state doc (even if the state doc has actually already been deleted by
 * the TTL monitor).
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   # Uses the pauseTenantMigrationDonorBeforeDeletingStateDoc failpoint, which was added in 6.1.
 *   requires_fcv_61,
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

    const donorPrimary = tmt.getDonorPrimary();
    const fp = configureFailPoint(donorPrimary, "pauseTenantMigrationDonorBeforeDeletingStateDoc");

    jsTest.log("Confirm serverStatus does not show any active or completed tenant migrations.");
    let donorStats = tmt.getTenantMigrationStats(donorPrimary);
    jsTest.log("donorStats: " + tojson(donorStats));
    assert.eq(0, donorStats.currentMigrationsDonating);
    assert.eq(0, donorStats.totalMigrationDonationsCommitted);
    assert.eq(0, donorStats.totalMigrationDonationsAborted);

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
        return 0 == donorPrimary.getDB("config").getCollection("tenantMigrationDonors").count();
    });

    jsTest.log("Confirm the instance still shows up as active in serverStatus.");
    donorStats = tmt.getTenantMigrationStats(donorPrimary);
    jsTest.log("donorStats: " + tojson(donorStats));
    assert.eq(1, donorStats.currentMigrationsDonating);
    assert.eq(1, donorStats.totalMigrationDonationsCommitted);
    assert.eq(0, donorStats.totalMigrationDonationsAborted);

    // TODO (SERVER-61717): Confirm the instance still shows up in the POS map. Currently, the
    // instance is removed from the map as soon as its' state doc is deleted by the TTL monitor.

    jsTest.log("Turn off the failpoint.");
    fp.off();

    jsTest.log("Confirm the instance eventually stops showing up as active in serverStatus");
    assert.soon(() => {
        donorStats = tmt.getTenantMigrationStats(donorPrimary);
        return 0 == donorStats.currentMigrationsDonating &&
            1 == donorStats.totalMigrationDonationsCommitted &&
            0 == donorStats.totalMigrationDonationsAborted;
    });

    // TODO (SERVER-61717): Confirm the instance eventually stops showing up in the POS map.

    tmt.stop();
})();

(() => {
    jsTest.log("Test case where the instance deletes the state doc first");

    const tmt = new TenantMigrationTest({name: jsTestName(), quickGarbageCollection: true});

    const donorPrimary = tmt.getDonorPrimary();

    jsTest.log("Confirm the TTL index exists");
    const listIndexesRes1 = assert.commandWorked(
        donorPrimary.getDB("config").runCommand({listIndexes: "tenantMigrationDonors"}));
    assert(listIndexesRes1.cursor.firstBatch.some(
        elem => elem.name === "TenantMigrationDonorTTLIndex" && elem.key.expireAt === 1));

    jsTest.log("Drop the TTL index");
    assert.commandWorked(donorPrimary.getDB("config").runCommand(
        {dropIndexes: "tenantMigrationDonors", index: "TenantMigrationDonorTTLIndex"}));

    jsTest.log("Confirm the TTL index no longer exists");
    const listIndexesRes2 = assert.commandWorked(
        donorPrimary.getDB("config").runCommand({listIndexes: "tenantMigrationDonors"}));
    assert(listIndexesRes2.cursor.firstBatch.every(elem => elem.key.expireAt == null));

    jsTest.log("Confirm serverStatus does not show any active or completed tenant migrations.");
    let donorStats = tmt.getTenantMigrationStats(donorPrimary);
    jsTest.log("donorStats: " + tojson(donorStats));
    assert.eq(0, donorStats.currentMigrationsDonating);
    assert.eq(0, donorStats.totalMigrationDonationsCommitted);
    assert.eq(0, donorStats.totalMigrationDonationsAborted);

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
        return 0 == donorPrimary.getDB("config").getCollection("tenantMigrationDonors").count();
    });

    jsTest.log("Confirm the instance eventually stops showing up as active in serverStatus");
    assert.soon(() => {
        donorStats = tmt.getTenantMigrationStats(donorPrimary);
        return 0 == donorStats.currentMigrationsDonating &&
            1 == donorStats.totalMigrationDonationsCommitted &&
            0 == donorStats.totalMigrationDonationsAborted;
    });

    // TODO (SERVER-61717): Confirm the instance eventually stops showing up in the POS map.

    tmt.stop();
})();
