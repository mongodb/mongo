/**
 * Tests that in tenant migration, the recipient set can resume collection cloning from the last
 * document cloned after a failover even if the collection has been renamed on the donor.
 * @tags: [requires_majority_read_concern, requires_fcv_49, incompatible_with_windows_tls]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");       // for 'extractUUIDFromObject'
load("jstests/libs/parallelTester.js");  // for 'Thread'
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const recipientRst = new ReplSetTest({
    nodes: 2,
    name: jsTestName() + "_recipient",
    // Use a batch size of 2 so that collection cloner requires more than a single batch to
    // complete.
    nodeOptions: Object.assign(TenantMigrationUtil.makeX509OptionsForTest().recipient,
                               {setParameter: {collectionClonerBatchSize: 2}})
});

recipientRst.startSet();
recipientRst.initiate();
if (!TenantMigrationUtil.isFeatureFlagEnabled(recipientRst.getPrimary())) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    recipientRst.stopSet();
    return;
}

const tenantMigrationTest =
    new TenantMigrationTest({name: jsTestName(), recipientRst: recipientRst});
const tenantId = "testTenantId";
const dbName = tenantMigrationTest.tenantDB(tenantId, "testDB");
const collName = "testColl";

const recipientPrimary = tenantMigrationTest.getRecipientPrimary();
const donorPrimary = tenantMigrationTest.getDonorPrimary();

// Test _id with mixed bson types.
const docs = [{_id: 0}, {_id: "string"}, {_id: UUID()}, {_id: new Date()}];
tenantMigrationTest.insertDonorDB(dbName, collName, docs);

const migrationId = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    recipientConnString: tenantMigrationTest.getRecipientConnString(),
    tenantId: tenantId,
};

// Configure a fail point to have the recipient primary hang after cloning 2 documents.
const recipientDb = recipientPrimary.getDB(dbName);
let recipientColl = recipientDb.getCollection(collName);
const hangDuringCollectionClone =
    configureFailPoint(recipientDb,
                       "tenantMigrationHangCollectionClonerAfterHandlingBatchResponse",
                       {nss: recipientColl.getFullName()});

// Start a migration and wait for recipient to hang after cloning 2 documents.
const donorRstArgs = TenantMigrationUtil.createRstArgs(tenantMigrationTest.getDonorRst());
const migrationThread =
    new Thread(TenantMigrationUtil.runMigrationAsync, migrationOpts, donorRstArgs);
migrationThread.start();
hangDuringCollectionClone.wait();
assert.eq(2, recipientColl.find().itcount());

// Step up a new node in the recipient set and trigger a failover. The new primary should resume
// cloning starting from the third document.
recipientRst.awaitLastOpCommitted();
const newRecipientPrimary = recipientRst.getSecondaries()[0];
const newRecipientDb = newRecipientPrimary.getDB(dbName);
const hangAfterStartingOplogFetcher = configureFailPoint(
    newRecipientDb, "fpAfterStartingOplogFetcherMigrationRecipientInstance", {action: "hang"});
assert.commandWorked(newRecipientPrimary.adminCommand({replSetStepUp: 1}));
hangDuringCollectionClone.off();
recipientRst.getPrimary();

// Wait until the new recipient primary starts the oplog fetcher. This failpoint is needed only
// because tenant migration didn't fully support resuming after a failover (specifically, not
// preserving the startApplyingDonorOpTime) when this test was written. Otherwise, we can simply
// rename the collection on the donor before stepping up the new recipient primary.
hangAfterStartingOplogFetcher.wait();

// Rename the collection on the donor.
const donorColl = donorPrimary.getDB(dbName).getCollection(collName);
const collNameRenamed = collName + "_renamed";
assert.commandWorked(donorColl.renameCollection(collNameRenamed));

// Resume collection cloning on the new recipient primary.
hangAfterStartingOplogFetcher.off();

// The migration should go through after recipient failover.
assert.commandWorked(migrationThread.returnData());

// Check that recipient has cloned all documents in the renamed collection.
recipientColl = newRecipientPrimary.getDB(dbName).getCollection(collNameRenamed);
assert.eq(4, recipientColl.find().itcount());
assert.eq(recipientColl.find().sort({_id: 1}).toArray(), docs);
tenantMigrationTest.checkTenantDBHashes(tenantId);

tenantMigrationTest.stop();
recipientRst.stopSet();
})();
