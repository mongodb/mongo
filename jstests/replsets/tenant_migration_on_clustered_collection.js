/**
 * Tests that clustered collections (both empty and non-empty) are successfully migrated in a basic
 * tenant migration.
 *
 * @tags: [
 *   requires_fcv_53,
 *   assumes_against_mongod_not_mongos,
 *   assumes_unsharded_collection,
 *   # Basic tags for tenant migration tests.
 *   incompatible_with_macos,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/clustered_collections/clustered_collection_util.js");  // ClusteredCollectionUtil
load("jstests/libs/parallelTester.js");                                   // Thread()
load("jstests/libs/uuid_util.js");                                        // extractUUIDFromObject()
load("jstests/replsets/libs/tenant_migration_test.js");                   // TenantMigrationTest
load("jstests/replsets/libs/tenant_migration_util.js");                   // TenantMigrationUtil

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});

const kTenantId = "testTenantId1";
const kDbName = tenantMigrationTest.tenantDB(kTenantId, "testDB");
const kEmptyCollName = "testEmptyColl";
const kNonEmptyCollName = "testNonEmptyColl";

// The documents used to populate the non-empty collection.
const documents = [{_id: 1, a: 1, b: 1}, {_id: 2, a: 2, b: 2}, {_id: 3, a: 3, b: 3}];

const clusteredCreateOptions = {
    clusteredIndex: {key: {_id: 1}, name: "index_on_id", unique: true}
};

// Generates the clustered collections and populates the non-empty collection.
const createClusteredCollections = () => {
    const donorPrimary = tenantMigrationTest.getDonorPrimary();
    const donorDB = donorPrimary.getDB(kDbName);

    // Create a non-empty clustered collection and store it's original contents.
    assert.commandWorked(donorDB.createCollection(kNonEmptyCollName, clusteredCreateOptions));
    assert.commandWorked(donorDB[kNonEmptyCollName].insert(documents));

    // Create an empty clustered collection.
    assert.commandWorked(donorDB.createCollection(kEmptyCollName, clusteredCreateOptions));

    // Account for test environments that may change default write concern.
    tenantMigrationTest.getDonorRst().awaitReplication();
};

// Runs the entire tenant migration start to finish.
const runTenantMigration = () => {
    const migrationId = UUID();
    const migrationOpts = {
        migrationIdString: extractUUIDFromObject(migrationId),
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
        tenantId: kTenantId,
    };
    const donorRstArgs = TenantMigrationUtil.createRstArgs(tenantMigrationTest.getDonorRst());
    const migrationThread =
        new Thread(TenantMigrationUtil.runMigrationAsync, migrationOpts, donorRstArgs);
    migrationThread.start();

    TenantMigrationTest.assertCommitted(migrationThread.returnData());
};

// Validates the clustered collections migrated to the recipient.
const validateMigrationResults = () => {
    const recipientPrimary = tenantMigrationTest.getRecipientPrimary();
    const recipientDB = recipientPrimary.getDB(kDbName);

    // Confirm the data was transferred correctly.
    const nonEmptyCollDocs = recipientDB[kNonEmptyCollName].find().toArray();
    assert.sameMembers(nonEmptyCollDocs, documents);
    assert.eq(0,
              recipientDB[kEmptyCollName].find().itcount(),
              tojson(recipientDB[kEmptyCollName].find().toArray()));

    ClusteredCollectionUtil.validateListCollections(
        recipientDB, kNonEmptyCollName, clusteredCreateOptions);
    ClusteredCollectionUtil.validateListCollections(
        recipientDB, kEmptyCollName, clusteredCreateOptions);

    ClusteredCollectionUtil.validateListIndexes(
        recipientDB, kNonEmptyCollName, clusteredCreateOptions);
    ClusteredCollectionUtil.validateListIndexes(
        recipientDB, kEmptyCollName, clusteredCreateOptions);
};

createClusteredCollections();
runTenantMigration();
validateMigrationResults();

tenantMigrationTest.stop();
})();
