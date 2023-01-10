/**
 * Tests that during tenant migration, when the migration has reached but not completed the
 * migration committed state, a new node added to the donor replica set is still able to
 * successfully complete initial sync.
 *
 * @tags: [
 *    # Running tenant migrations on macOS depends on plumbing transient SSL params through the
 *    # Apple SSL manager (see SERVER-56100).
 *      incompatible_with_macos,
 *    # Shard merge not resilient to failover, which will be caused by issuing startClean on an
 *    # existing node to force an initial sync.
 *      incompatible_with_shard_merge,
 *    # Running tenant migrations on Windows with TLS depends on plumbing transient SSL params
 *    # through the Windows SSL manager (see SERVER-53883 and SERVER-53763).
 *      incompatible_with_windows_tls,
 *    # failpoint pauseTenantMigrationBeforeLeavingCommittedState only exists on the latest branch.
 *      requires_fcv_60,
 *    # Tenant migrations will not be run with enableMajorityReadConcern=false in production.
 *      requires_majority_read_concern,
 *    # Tenant migrations will not be run with the inMemory storage engine in production.
 *      requires_persistence,
 *    # Tenant migrations are only used in serverless.
 *      serverless,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");
load('jstests/replsets/rslib.js');  // for waitForNewlyAddedRemovalForNodeToBeCommitted

const testDBName = 'testDB';
const testCollName = 'testColl';
const tenantId = 'tenantId1';

const tenantMigrationTest = new TenantMigrationTest({
    name: jsTestName(),
    allowStaleReadsOnDonor: false  // turn off tenantMigrationDonorAllowsNonTimestampedReads fp
});

// This test does the following:
// 1. Configures a failpoint on the donor primary
// 2. Starts a tenant migration.
// 3. Waits for the donor failpoint to be hit. Restarts a node to undergo initial sync.
// 4. Makes sure the initial sync completes successfully
// 5. Releases the donor failpoint, and allows the migration to complete.

const migrationOpts = {
    migrationIdString: extractUUIDFromObject(UUID()),
    tenantId: tenantId
};
const dbName = tenantMigrationTest.tenantDB(tenantId, testDBName);
const donorRst = tenantMigrationTest.getDonorRst();
const originalDonorPrimary = tenantMigrationTest.getDonorPrimary();

// In order to validate the fix, we want to ensure we hit a server failpoint that will cause an
// error should the fix be bypassed/fail.  Since initial sync does not produce a readTimestamp, this
// means hitting the BlockerState::State::kRejected in order to generate an error in
// getCanReadFuture, which can only happen after the state doc has been updated to kCommitted.
const fpOnDonor =
    configureFailPoint(originalDonorPrimary, "pauseTenantMigrationBeforeLeavingCommittedState");
tenantMigrationTest.insertDonorDB(dbName, testCollName);

jsTestLog(`Starting a tenant migration with migrationID ${
    migrationOpts.migrationIdString}, and tenantId ${tenantId}`);
assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));

jsTestLog("Waiting for failpoint where read access blockers are enabled on donor");
fpOnDonor.wait();

// Restart a node and allow it to undergo initial sync.
jsTestLog("Restarting a node from the donor replica set.");

let initialSyncNode = donorRst.getSecondaries()[0];
initialSyncNode = donorRst.restart(initialSyncNode, {startClean: true, skipValidation: true});

// Allow the new node to finish initial sync.
waitForNewlyAddedRemovalForNodeToBeCommitted(originalDonorPrimary,
                                             donorRst.getNodeId(initialSyncNode));
donorRst.awaitSecondaryNodes();
donorRst.awaitReplication();

fpOnDonor.off();

jsTestLog("Ensure that the new node's documents match up with the primary's.");

// reenable tenantMigrationDonorAllowsNonTimestampedReads to allow us to read from both nodes
// without hitting reroute errors
configureFailPoint(
    originalDonorPrimary, "tenantMigrationDonorAllowsNonTimestampedReads", {mode: 'alwaysOn'});
configureFailPoint(
    initialSyncNode, "tenantMigrationDonorAllowsNonTimestampedReads", {mode: 'alwaysOn'});

// Make sure the documents on the new node matches that on the donor primary
let donorDocOnPrimary = undefined;
let donorDocOnNewNode = undefined;
assert.soon(() => {
    donorDocOnPrimary = originalDonorPrimary.getDB(dbName).getCollection(testCollName).findOne();
    donorDocOnNewNode = initialSyncNode.getDB(dbName).getCollection(testCollName).findOne();

    return donorDocOnPrimary.state == donorDocOnNewNode.state;
}, `Documents never matched, primary: ${donorDocOnPrimary}, on new node: ${donorDocOnNewNode}`);

// Allow the migration to run to completion.
jsTestLog("Allowing migration to run to completion.");
TenantMigrationTest.assertCommitted(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));

assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));

tenantMigrationTest.stop();
})();
