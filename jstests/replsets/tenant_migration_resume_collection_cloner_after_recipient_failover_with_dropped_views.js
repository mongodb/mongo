/**
 * Tests that in tenant migration, the collection recreated on a dropped view namespace is handled
 * correctly on resuming the logical tenant collection cloning phase due to recipient failover.
 * @tags: [
 *   incompatible_with_eft,
 *   incompatible_with_macos,
 *   incompatible_with_shard_merge,
 *   incompatible_with_windows_tls,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   serverless,
 * ]
 */

(function() {
"use strict";

const tenantMigrationFailoverTest = function(isTimeSeries, createCollFn) {
    load("jstests/libs/fail_point_util.js");
    load("jstests/libs/uuid_util.js");  // for 'extractUUIDFromObject'
    load("jstests/replsets/libs/tenant_migration_test.js");
    load("jstests/replsets/libs/tenant_migration_util.js");

    const recipientRst = new ReplSetTest({
        nodes: 2,
        name: jsTestName() + "_recipient",
        nodeOptions: Object.assign(TenantMigrationUtil.makeX509OptionsForTest().recipient, {
            setParameter: {
                // Allow reads on recipient before migration completes for testing.
                'failpoint.tenantMigrationRecipientNotRejectReads': tojson({mode: 'alwaysOn'}),
            }
        })
    });

    recipientRst.startSet();
    recipientRst.initiate();

    const tenantMigrationTest =
        new TenantMigrationTest({name: jsTestName(), recipientRst: recipientRst});

    const donorRst = tenantMigrationTest.getDonorRst();
    const donorPrimary = donorRst.getPrimary();

    const tenantId = "testTenantId";
    const dbName = tenantMigrationTest.tenantDB(tenantId, "testDB");
    const donorDB = donorPrimary.getDB(dbName);
    const collName = "testColl";
    const donorColl = donorDB[collName];

    let getCollectionInfo = function(conn) {
        return conn.getDB(dbName).getCollectionInfos().filter(coll => {
            return coll.name === collName;
        });
    };

    // Create a timeseries collection or a regular view.
    assert.commandWorked(createCollFn(donorDB, collName));
    donorRst.awaitReplication();

    const migrationId = UUID();
    const migrationIdString = extractUUIDFromObject(migrationId);
    const migrationOpts = {
        migrationIdString: migrationIdString,
        recipientConnString: tenantMigrationTest.getRecipientConnString(),
        tenantId: tenantId,
    };

    const recipientPrimary = recipientRst.getPrimary();
    const recipientDb = recipientPrimary.getDB(dbName);
    const recipientSystemViewsColl = recipientDb.getCollection("system.views");

    // Configure a fail point to have the recipient primary hang after cloning
    // "testTenantId_testDB.system.views" collection.
    const hangDuringCollectionClone =
        configureFailPoint(recipientPrimary,
                           "tenantMigrationHangCollectionClonerAfterHandlingBatchResponse",
                           {nss: recipientSystemViewsColl.getFullName()});

    // Start the migration and wait for the migration to hang after cloning
    // "testTenantId_testDB.system.views" collection.
    assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));
    hangDuringCollectionClone.wait();

    assert.soon(() => recipientSystemViewsColl.find().itcount() >= 1);
    recipientRst.awaitLastOpCommitted();
    const newRecipientPrimary = recipientRst.getSecondaries()[0];

    // Verify that a view has been registered for "testTenantId_testDB.testColl" on the new
    // recipient primary.
    let collectionInfo = getCollectionInfo(newRecipientPrimary);
    assert.eq(1, collectionInfo.length);
    assert(collectionInfo[0].type === (isTimeSeries ? "timeseries" : "view"),
           "data store type mismatch: " + tojson(collectionInfo[0]));

    // Drop the view and create a regular collection with the same namespace as the
    // dropped view on donor.
    assert(donorColl.drop());
    assert.commandWorked(donorDB.createCollection(collName));

    // We need to skip TenantDatabaseCloner::listExistingCollectionsStage() to make sure
    // the recipient always clone the above newly created regular collection after the failover.
    // Currently, we restart cloning after a failover, only from the collection whose UUID is
    // greater than or equal to the last collection we have on disk.
    const skiplistExistingCollectionsStage =
        configureFailPoint(newRecipientPrimary, "skiplistExistingCollectionsStage");

    // Step up a new node in the recipient set and trigger a failover.
    recipientRst.stepUp(newRecipientPrimary);
    hangDuringCollectionClone.off();

    // The migration should go through after recipient failover.
    TenantMigrationTest.assertCommitted(
        tenantMigrationTest.waitForMigrationToComplete(migrationOpts));

    // Check that recipient has dropped the view and and re-created the regular collection as part
    // of migration oplog catchup phase.
    collectionInfo = getCollectionInfo(newRecipientPrimary);
    assert.eq(1, collectionInfo.length);
    assert(collectionInfo[0].type === "collection",
           "data store type mismatch: " + tojson(collectionInfo[0]));

    tenantMigrationTest.stop();
    recipientRst.stopSet();
};

jsTestLog("Running tenant migration test for time-series collection");
// Creating a timeseries collection, implicity creates a view on the 'collName' collection
// namespace.
tenantMigrationFailoverTest(true,
                            (db, collName) => db.createCollection(
                                collName, {timeseries: {timeField: "time", metaField: "bucket"}}));

jsTestLog("Running tenant migration test for regular view");
tenantMigrationFailoverTest(false,
                            (db, collName) => db.createView(collName, "sourceCollection", []));
})();
