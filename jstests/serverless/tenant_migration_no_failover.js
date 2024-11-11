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
 *   requires_fcv_71,
 * ]
 */

import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {isShardMergeEnabled, makeTenantDB} from "jstests/replsets/libs/tenant_migration_util.js";

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});
const tenantId = ObjectId().str;

if (isShardMergeEnabled(tenantMigrationTest.getDonorPrimary().getDB("admin"))) {
    tenantMigrationTest.stop();
    jsTestLog("Skipping this shard merge incompatible test.");
    quit();
}

const dbNames = ["db0", "db1", "db2"];
const tenantDBs = dbNames.map(dbName => makeTenantDB(tenantId, dbName));
const nonTenantDBs = dbNames.map(dbName => makeTenantDB(ObjectId().str, dbName));
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

TenantMigrationTest.assertCommitted(tenantMigrationTest.runMigration(migrationOpts));

for (const db of [...tenantDBs, ...nonTenantDBs]) {
    for (const coll of collNames) {
        tenantMigrationTest.verifyRecipientDB(tenantId, db, coll);
    }
}

tenantMigrationTest.stop();
