/**
 * Tests that tenant migrations are correctly filtering DBs by tenantId.
 *
 * @tags: [
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 *   requires_fcv_71,
 *   incompatible_with_shard_merge,
 * ]
 */

import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {TenantMigrationTest} from "jstests/replsets/libs/tenant_migration_test.js";
import {
    isNamespaceForTenant,
    isShardMergeEnabled
} from "jstests/replsets/libs/tenant_migration_util.js";

const baseDBName = "testDB";
const collName = "testColl";

const makeBaseTenantId = () => {
    return ObjectId().str;
};

const runTest = (baseTenantId, dbName, shouldMatch) => {
    jsTestLog(`Running tenant migration with dbName ${dbName} and tenantId ${baseTenantId}`);

    const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});

    if (isShardMergeEnabled(tenantMigrationTest.getDonorPrimary().getDB("admin"))) {
        tenantMigrationTest.stop();
        jsTestLog("Skipping this shard merge incompatible test.");
        quit();
    }

    assert.eq(shouldMatch, isNamespaceForTenant(baseTenantId, dbName));
    tenantMigrationTest.insertDonorDB(dbName, collName);

    // Run a migration with the base tenant ID.
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(UUID()),
        tenantId: baseTenantId,
    };
    TenantMigrationTest.assertCommitted(tenantMigrationTest.runMigration(migrationOpts));

    // verifyRecipientDB calls isNamespaceForTenant() to determine if the data should have been
    // migrated, so we can directly call it here.
    tenantMigrationTest.verifyRecipientDB(baseTenantId, dbName, collName);
    tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString);
    tenantMigrationTest.stop();
};

const testCases = [
    {makeTenantId: baseId => baseId, shouldMatch: true},
    {makeTenantId: baseId => `${baseId}_`, shouldMatch: true},
    {makeTenantId: baseId => `${baseId}_${baseId}`, shouldMatch: true},
];

for (const {makeTenantId, shouldMatch} of testCases) {
    const baseTenantId = makeBaseTenantId();
    const tenantId = makeTenantId(baseTenantId);
    runTest(baseTenantId, `${tenantId}_${baseDBName}`, shouldMatch);
}
