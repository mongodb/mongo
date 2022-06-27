/**
 * Test that in tenant migration, the recipient does not change sync source
 * even after its current sync source steps down as primary.
 *
 * TODO SERVER-63517: incompatible_with_shard_merge because this relies on
 * logical cloning behavior.
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

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

// Verify the recipient's current sync source is the expected one.
const verifySyncSource = function(conn, migrationId, expectedSyncSource) {
    const res = conn.adminCommand({currentOp: true, desc: "tenant recipient migration"});
    assert.eq(res.inprog.length, 1);
    const currOp = res.inprog[0];
    assert.eq(bsonWoCompare(currOp.instanceID, migrationId), 0);
    assert.eq(currOp.donorSyncSource, expectedSyncSource, tojson(res));
};

const batchSize = 2;
const recipientRst = new ReplSetTest({
    nodes: 2,
    name: jsTestName() + "_recipient",
    nodeOptions: Object.assign(TenantMigrationUtil.makeX509OptionsForTest().recipient, {
        setParameter: {
            // Use a batch size of 2 so that collection cloner requires more than a single
            // batch to complete.
            collectionClonerBatchSize: batchSize,
            // Allow reads on recipient before migration completes for testing.
            'failpoint.tenantMigrationRecipientNotRejectReads': tojson({mode: 'alwaysOn'}),
        }
    })
});

recipientRst.startSet();
recipientRst.initiateWithHighElectionTimeout();

const tenantMigrationTest =
    new TenantMigrationTest({name: jsTestName(), recipientRst: recipientRst});
const donorRst = tenantMigrationTest.getDonorRst();
const donorPrimary = donorRst.getPrimary();

const tenantId = "testTenantId";
const dbName = tenantMigrationTest.tenantDB(tenantId, "testDB");
const collName = "testColl";

const recipientPrimary = tenantMigrationTest.getRecipientPrimary();
const docs1 = [{_id: 0}, {_id: 1}, {_id: 2}, {_id: 3}];
const docs2 = [{_id: 4}, {_id: 5}];

tenantMigrationTest.insertDonorDB(dbName, collName, docs1);

const migrationId = UUID();
const migrationIdString = extractUUIDFromObject(migrationId);
const migrationOpts = {
    migrationIdString: migrationIdString,
    recipientConnString: tenantMigrationTest.getRecipientConnString(),
    tenantId: tenantId,
    readPreference: {mode: "primary"},  // only sync from donor's primary
};

const recipientDb = recipientPrimary.getDB(dbName);
const recipientColl = recipientDb.getCollection(collName);

// Fail point to have the recipient primary hang creating connections to donor.
const hangRecipientPrimaryAfterCreatingConnections = configureFailPoint(
    recipientPrimary, "fpAfterStartingOplogFetcherMigrationRecipientInstance", {action: "hang"});
// Fail point to have the recipient primary hang after cloning 2 documents.
const hangDuringCollectionClone =
    configureFailPoint(recipientPrimary,
                       "tenantMigrationHangCollectionClonerAfterHandlingBatchResponse",
                       {nss: recipientColl.getFullName()});

jsTestLog("Starting the tenant migration");
assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

// Wait for the connection to donor's primary to be created and verify recipient's
// sync source is donor's primary as specified by the read preference.
hangRecipientPrimaryAfterCreatingConnections.wait();
verifySyncSource(recipientPrimary, migrationId, donorPrimary.host);
hangRecipientPrimaryAfterCreatingConnections.off();

// Wait for recipient to hang after cloning 2 documents.
hangDuringCollectionClone.wait();
assert.soon(() => recipientColl.find().itcount() === batchSize);
verifySyncSource(recipientPrimary, migrationId, donorPrimary.host);

// Steps down the current donor's primary and wait for the new primary to be discovered.
donorRst.stepUp(donorRst.getSecondary());
const newDonorPrimary = donorRst.getPrimary();
assert.neq(newDonorPrimary.host, donorPrimary.host);

// Insert some new documents so that the recipient's oplog fetcher needs to continue
// fetching documents after donor replSet changes primary in order to be consistent.
tenantMigrationTest.insertDonorDB(dbName, collName, docs2);
hangDuringCollectionClone.off();

// After recipient syncs new documents, becomes consistent, and finishes migration,
// verify the sync source is still the donor's old primary.
TenantMigrationTest.assertCommitted(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
assert.eq(recipientColl.find().itcount(), docs1.length + docs2.length);
assert.docEq(recipientColl.find().sort({_id: 1}).toArray(), docs1.concat(docs2));
verifySyncSource(recipientPrimary, migrationId, donorPrimary.host);

tenantMigrationTest.stop();
recipientRst.stopSet();
})();
