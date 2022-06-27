/**
 * Tests that in tenant migration, the recipient set can resume collection cloning from the last
 * document cloned after a failover even if the collection has been renamed on the donor.
 * @tags: [
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

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");       // for 'extractUUIDFromObject'
load("jstests/libs/parallelTester.js");  // for 'Thread'
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const recipientRst = new ReplSetTest({
    nodes: 2,
    name: jsTestName() + "_recipient",
    nodeOptions: Object.assign(TenantMigrationUtil.makeX509OptionsForTest().recipient, {
        setParameter: {
            // Use a batch size of 2 so that collection cloner requires more than a single batch to
            // complete.
            collectionClonerBatchSize: 2,
            // Allow reads on recipient before migration completes for testing.
            'failpoint.tenantMigrationRecipientNotRejectReads': tojson({mode: 'alwaysOn'}),
        }
    })
});

recipientRst.startSet();
recipientRst.initiate();

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
const migrationIdString = extractUUIDFromObject(migrationId);
const migrationOpts = {
    migrationIdString: migrationIdString,
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
assert.soon(() => recipientColl.find().itcount() === 2);

// Insert some documents that will be fetched by the recipient. This is to test that on failover,
// the fetcher will resume fetching from where it left off. The system is expected to crash if
// the recipient fetches a duplicate oplog entry upon resuming the migration.
tenantMigrationTest.insertDonorDB(dbName, "aNewColl", [{_id: "docToBeFetched"}]);
assert.soon(() => {
    const configDb = recipientPrimary.getDB("config");
    const oplogBuffer = configDb.getCollection("repl.migration.oplog_" + migrationIdString);
    return oplogBuffer.find({"entry.o._id": "docToBeFetched"}).count() === 1;
});

recipientRst.awaitLastOpCommitted();

// Set a failpoint to prevent the new recipient primary from completing the migration before the
// donor renames the collection.
const newRecipientPrimary = recipientRst.getSecondaries()[0];
const fpPauseAtStartOfMigration =
    configureFailPoint(newRecipientPrimary, "pauseAfterRunTenantMigrationRecipientInstance");

// Step up a new node in the recipient set and trigger a failover. The new primary should resume
// cloning starting from the third document.
recipientRst.stepUp(newRecipientPrimary);
hangDuringCollectionClone.off();
recipientRst.getPrimary();

// Rename the collection on the donor.
const donorColl = donorPrimary.getDB(dbName).getCollection(collName);
const collNameRenamed = collName + "_renamed";
assert.commandWorked(donorColl.renameCollection(collNameRenamed));

// The migration should go through after recipient failover.
fpPauseAtStartOfMigration.off();
TenantMigrationTest.assertCommitted(migrationThread.returnData());

// Check that recipient has cloned all documents in the renamed collection.
recipientColl = newRecipientPrimary.getDB(dbName).getCollection(collNameRenamed);
assert.eq(4, recipientColl.find().itcount());
assert.eq(recipientColl.find().sort({_id: 1}).toArray(), docs);
TenantMigrationUtil.checkTenantDBHashes(
    tenantMigrationTest.getDonorRst(), tenantMigrationTest.getRecipientRst(), tenantId);

tenantMigrationTest.stop();
recipientRst.stopSet();
})();
