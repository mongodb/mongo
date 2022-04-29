/**
 * Tests that in a tenant migration, the recipient primary will use majority read concern when
 * cloning documents from the donor.
 *
 * TODO (SERVER-63517): Remove this test, it requires failover, and the ability to write to the
 * donor during migration, and it tests a cloning method superseded by Shard Merge.
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
load("jstests/libs/uuid_util.js");           // for 'extractUUIDFromObject'
load("jstests/libs/parallelTester.js");      // for 'Thread'
load("jstests/libs/write_concern_util.js");  // for 'stopReplicationOnSecondaries'
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});

const tenantId = "testTenantId";
const dbName = tenantMigrationTest.tenantDB(tenantId, "testDB");
const collName = "testColl";

const donorPrimary = tenantMigrationTest.getDonorPrimary();
const recipientPrimary = tenantMigrationTest.getRecipientPrimary();
const donorRst = tenantMigrationTest.getDonorRst();
const donorTestColl = donorPrimary.getDB(dbName).getCollection(collName);

// The default WC is majority and stopReplicationOnSecondaries will prevent satisfying any majority
assert.commandWorked(recipientPrimary.adminCommand(
    {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));
tenantMigrationTest.getRecipientRst().awaitReplication();

// Populate the donor replica set with some initial data and make sure it is majority committed.
const majorityCommittedDocs = [{_id: 0, x: 0}, {_id: 1, x: 1}];
assert.commandWorked(donorTestColl.insert(majorityCommittedDocs, {writeConcern: {w: "majority"}}));
assert.eq(2, donorTestColl.find().readConcern("majority").itcount());

const migrationId = UUID();
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(migrationId),
    recipientConnString: tenantMigrationTest.getRecipientConnString(),
    tenantId: tenantId,
};

// Configure fail point to have the recipient primary hang before the query stage.
const recipientDb = tenantMigrationTest.getRecipientPrimary().getDB(dbName);
const failPointData = {
    cloner: "TenantCollectionCloner",
    stage: "query",
    nss: dbName + "." + collName,
};
const waitBeforeCloning = configureFailPoint(recipientDb, "hangBeforeClonerStage", failPointData);

// Start a migration and wait for recipient to hang before querying the donor in the cloning phase.
// At this point, we have waited for the listIndex results to be majority committed on the donor,
// so we can stop server replication.
const donorRstArgs = TenantMigrationUtil.createRstArgs(tenantMigrationTest.getDonorRst());
const migrationThread =
    new Thread(TenantMigrationUtil.runMigrationAsync, migrationOpts, donorRstArgs);
migrationThread.start();
waitBeforeCloning.wait();
stopReplicationOnSecondaries(donorRst);

// Insert some writes that won't be majority committed. These writes should not show up in the
// recipient cloner queries.
const nonCommittedDocs = [{_id: 2, x: 2}, {_id: 3, x: 3}];
assert.commandWorked(donorTestColl.insert(nonCommittedDocs));
assert.eq(4, donorTestColl.find().itcount());
assert.eq(2, donorTestColl.find().readConcern("majority").itcount());

// Let the cloner finish.
const waitBeforeFetchingTransactions =
    configureFailPoint(recipientDb, "fpBeforeFetchingCommittedTransactions", {action: "hang"});
waitBeforeCloning.off();

// Wait for the cloning phase to finish. Check that the recipient has only cloned documents that are
// majority committed on the donor replica set.
waitBeforeFetchingTransactions.wait();
// Tenant migration recipient rejects all reads until it has applied data past the blockTimestamp
// (returnAfterReachingTimestamp). Use this failpoint to allow the find command below to succeed.
const notRejectReadsFp =
    configureFailPoint(recipientPrimary, "tenantMigrationRecipientNotRejectReads");
const recipientColl = recipientPrimary.getDB(dbName).getCollection(collName);
assert.eq(2, recipientColl.find().itcount());
notRejectReadsFp.off();

// Restart secondary replication in the donor replica set and complete the migration.
restartReplicationOnSecondaries(donorRst);
waitBeforeFetchingTransactions.off();
TenantMigrationTest.assertCommitted(migrationThread.returnData());
tenantMigrationTest.stop();
})();
