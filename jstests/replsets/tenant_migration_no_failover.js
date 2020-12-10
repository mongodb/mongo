/**
 * Tests a full tenant migration, assuming no failover.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
if (!tenantMigrationTest.isFeatureFlagEnabled()) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    return;
}
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

const stateRes = assert.commandWorked(tenantMigrationTest.runMigration(migrationOpts));
assert.eq(stateRes.state, TenantMigrationTest.State.kCommitted);

for (const db of [...tenantDBs, ...nonTenantDBs]) {
    for (const coll of collNames) {
        tenantMigrationTest.verifyReceipientDB(tenantId, db, coll);
    }
}

tenantMigrationTest.stop();
})();
