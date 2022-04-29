/**
 * Tests that tenant migrations correctly clone 'system.views' collections that belong to the
 * tenant.
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

const donorRst = tenantMigrationTest.getDonorRst();
const donorPrimary = tenantMigrationTest.getDonorPrimary();
const recipientPrimary = tenantMigrationTest.getRecipientPrimary();

const tenantId = "testTenantId";
const tenantDBName = tenantMigrationTest.tenantDB(tenantId, "testDB");
const donorTenantDB = donorPrimary.getDB(tenantDBName);
const collName = "testColl";
const donorTenantColl = donorTenantDB.getCollection(collName);

const viewName = 'tenantView';
const doc1 = {
    _id: 1,
    a: 1
};
const doc2 = {
    _id: 2,
    b: 2
};

// Create a view on the tenant DB and insert documents into the tenant collection.
assert.commandWorked(donorTenantDB.createView(viewName, collName, [{$match: {a: 1}}]));
assert.commandWorked(donorTenantColl.insert([doc1, doc2]));
donorRst.awaitReplication();

const migrationOpts = {
    migrationIdString: extractUUIDFromObject(UUID()),
    tenantId,
};

TenantMigrationTest.assertCommitted(tenantMigrationTest.runMigration(migrationOpts));

// Verify that the view was cloned correctly.
const recipientView = recipientPrimary.getDB(tenantDBName)[viewName];

const findRes = recipientView.find().toArray();
assert.eq(1, findRes.length, `find result: ${tojson(findRes)}`);
assert.eq([doc1], findRes);

tenantMigrationTest.stop();
})();
