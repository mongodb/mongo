/**
 * Tests a full tenant migration using multitenant migration protocol, assuming no failover.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_shard_merge,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 * ]
 */

import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
const tenantId = ObjectId().str;

const dbNames = ["db0", "db1", "db2"];
const tenantDBs = dbNames.map(dbName => tenantMigrationTest.tenantDB(tenantId, dbName));
const nonTenantDBs = dbNames.map(dbName => tenantMigrationTest.tenantDB(ObjectId().str, dbName));
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
