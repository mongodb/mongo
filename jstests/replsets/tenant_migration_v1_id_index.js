/**
 * Tests that the index spec used for the '_id' index on the donor for a particular collection is
 * maintained on the recipient after migration.
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

const tenantId = 'testTenantId';
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(UUID()),
    tenantId: tenantId
};
const dbName = tenantMigrationTest.tenantDB(tenantId, "testDB");

// Collection names for the collections with "v: 1" and "v: 2" '_id' indexes.
const collWithV1Index = "testCollV1";
const collWithV2Index = "testCollV2";

const donorPrimary = tenantMigrationTest.getDonorPrimary();
const tenantDb = donorPrimary.getDB(dbName);

jsTestLog("Creating collections on donor.");
// Create collections with the appropriate default '_id' indexes.
assert.commandWorked(
    tenantDb.createCollection(collWithV1Index, {idIndex: {key: {_id: 1}, name: "_id_", v: 1}}));
assert.commandWorked(
    tenantDb.createCollection(collWithV2Index, {idIndex: {key: {_id: 1}, name: "_id_", v: 2}}));

// Insert documents into the collections.
tenantMigrationTest.insertDonorDB(
    dbName,
    collWithV1Index,
    [...Array(30).keys()].map((i) => ({a: i, job: "Musician", name: "Dr. BMK"})));
tenantMigrationTest.insertDonorDB(
    dbName,
    collWithV2Index,
    [...Array(30).keys()].map((i) => ({a: i, job: "Professor", name: "Donald Knuth"})));

jsTestLog(`Starting a tenant migration with migrationID ${
    migrationOpts.migrationIdString}, and tenantId ${tenantId}`);
assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

// Allow the migration to run to completion. This will check the db hashes between the donor and
// recipient to make sure everything (including collection attributes such as indexes) is identical.
jsTestLog("Allowing migration to run to completion.");
TenantMigrationTest.assertCommitted(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));

assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

tenantMigrationTest.stop();
})();
