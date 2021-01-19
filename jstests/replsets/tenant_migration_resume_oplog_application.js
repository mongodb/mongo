/**
 * Tests that in a tenant migration, the recipient primary will resume oplog application on
 * failover.
 * @tags: [requires_majority_read_concern, requires_fcv_49, incompatible_with_windows_tls]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");            // for 'extractUUIDFromObject'
load("jstests/libs/parallelTester.js");       // for 'Thread'
load("jstests/libs/write_concern_util.js");   // for 'stopReplicationOnSecondaries'
load("jstests/aggregation/extras/utils.js");  // For assertArrayEq.
load("jstests/replsets/libs/tenant_migration_test.js");
load("jstests/replsets/libs/tenant_migration_util.js");

const recipientRst = new ReplSetTest({
    nodes: 3,
    name: jsTestName() + "_recipient",
    // Use a batch size of 2 so that we can hang in the middle of tenant oplog application.
    nodeOptions: Object.assign(TenantMigrationUtil.makeX509OptionsForTest().recipient,
                               {setParameter: {tenantApplierBatchSizeOps: 2}})
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

const donorPrimary = tenantMigrationTest.getDonorPrimary();
const recipientPrimary = tenantMigrationTest.getRecipientPrimary();
const donorRst = tenantMigrationTest.getDonorRst();
const donorTestColl = donorPrimary.getDB(dbName).getCollection(collName);

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

// Configure fail point to have the recipient primary hang after the cloner completes and the oplog
// applier has started.
let waitAfterDatabaseClone = configureFailPoint(
    recipientPrimary, "fpAfterStartingOplogApplierMigrationRecipientInstance", {action: "hang"});
// Configure fail point to hang the tenant oplog applier after it applies the first batch.
let waitInOplogApplier = configureFailPoint(recipientPrimary, "hangInTenantOplogApplication");

// Start a migration and wait for recipient to hang in the tenant database cloner.
const donorRstArgs = TenantMigrationUtil.createRstArgs(donorRst);
const migrationThread =
    new Thread(TenantMigrationUtil.runMigrationAsync, migrationOpts, donorRstArgs);
migrationThread.start();
waitAfterDatabaseClone.wait();

// Insert some writes that will eventually be picked up by the tenant oplog applier on the
// recipient.
const docsToApply = [{_id: 2, x: 2}, {_id: 3, x: 3}, {_id: 4, x: 4}];
tenantMigrationTest.insertDonorDB(dbName, collName, docsToApply);

// Wait for the applied oplog batch to be replicated.
waitInOplogApplier.wait();
recipientRst.awaitReplication();
let local = recipientPrimary.getDB("local");
let appliedNoOps = local.oplog.rs.find({fromTenantMigration: migrationId, op: "n"});
let resultsArr = appliedNoOps.toArray();
// We should have applied the no-op oplog entries for the first batch of documents (size 2).
assert.eq(2, appliedNoOps.count(), appliedNoOps);
// No-op entries will be in the same order.
assert.eq(docsToApply[0], resultsArr[0].o.o, resultsArr);
assert.eq(docsToApply[1], resultsArr[1].o.o, resultsArr);

// Step up a new node in the recipient set and trigger a failover. The new primary should resume
// fetching starting from the unapplied documents.
const newRecipientPrimary = recipientRst.getSecondaries()[0];
assert.commandWorked(newRecipientPrimary.adminCommand({replSetStepUp: 1}));
waitAfterDatabaseClone.off();
waitInOplogApplier.off();
recipientRst.getPrimary();

// The migration should go through after recipient failover.
assert.commandWorked(migrationThread.returnData());
// Validate that the last no-op entry is applied.
local = newRecipientPrimary.getDB("local");
appliedNoOps = local.oplog.rs.find({fromTenantMigration: migrationId, op: "n"});
resultsArr = appliedNoOps.toArray();
assert.eq(3, appliedNoOps.count(), appliedNoOps);
assert.eq(docsToApply[2], resultsArr[2].o.o, resultsArr);

tenantMigrationTest.checkTenantDBHashes(tenantId);
tenantMigrationTest.stop();
recipientRst.stopSet();
})();