/**
 * Tests tenant migration with time-series collections.
 *
 * @tags: [
 *   incompatible_with_eft,
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 * ]
 */

(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
if (!tenantMigrationTest.isFeatureFlagEnabled()) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    return;
}

const donorPrimary = tenantMigrationTest.getDonorPrimary();
if (!TimeseriesTest.timeseriesCollectionsEnabled(donorPrimary)) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    tenantMigrationTest.stop();
    return;
}

const tenantId = "testTenantId";
const tsDB = tenantMigrationTest.tenantDB(tenantId, "tsDB");
const donorTSDB = donorPrimary.getDB(tsDB);
assert.commandWorked(donorTSDB.createCollection("tsColl", {timeseries: {timeField: "time"}}));
assert.commandWorked(donorTSDB.runCommand(
    {insert: "tsColl", documents: [{_id: 1, time: ISODate()}, {_id: 2, time: ISODate()}]}));

const migrationId = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    tenantId,
};

TenantMigrationTest.assertCommitted(tenantMigrationTest.runMigration(migrationOpts));

tenantMigrationTest.stop();
})();
