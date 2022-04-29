/**
 * Tests a full tenant migration, assuming no failover.
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

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
const tenantId = "testTenantId";

const dbNames = ["db0", "db1", "db2"];
const tenantDBs = dbNames.map(dbName => tenantMigrationTest.tenantDB(tenantId, dbName));
const nonTenantDBs = dbNames.map(dbName => tenantMigrationTest.nonTenantDB(tenantId, dbName));
const collNames = ["coll0", "coll1"];

for (const db of [...tenantDBs, ...nonTenantDBs]) {
    for (const coll of collNames) {
        tenantMigrationTest.insertDonorDB(db, coll);
    }
}

const migrationId = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    tenantId,
};

TenantMigrationTest.assertCommitted(
    tenantMigrationTest.runMigration(migrationOpts, {enableDonorStartMigrationFsync: true}));

for (const db of [...tenantDBs, ...nonTenantDBs]) {
    for (const coll of collNames) {
        tenantMigrationTest.verifyRecipientDB(tenantId, db, coll);
    }
}

tenantMigrationTest.stop();
})();
