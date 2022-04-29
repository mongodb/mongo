/**
 * Tests tenant migration with time-series collections.
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

load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});

const donorPrimary = tenantMigrationTest.getDonorPrimary();

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
