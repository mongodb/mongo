/**
 * Tests that tenant migration donor's in memory state is initialized correctly on initial sync.
 * This test randomly selects a point during the migration to add a node to the donor replica set.
 *
 * Tenant migrations are not expected to be run on servers with ephemeralForTest.
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

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/libs/write_concern_util.js");
load("jstests/replsets/libs/tenant_migration_test.js");

const tenantMigrationTest = new TenantMigrationTest({name: jsTestName()});

const kMaxSleepTimeMS = 1000;
const kTenantId = 'testTenantId';

let donorPrimary = tenantMigrationTest.getDonorPrimary();

// Force the migration to pause after entering a randomly selected state.
Random.setRandomSeed();
const kMigrationFpNames = [
    "pauseTenantMigrationBeforeLeavingDataSyncState",
    "pauseTenantMigrationBeforeLeavingBlockingState",
    "abortTenantMigrationBeforeLeavingBlockingState"
];
let fp;
const index = Random.randInt(kMigrationFpNames.length + 1);
if (index < kMigrationFpNames.length) {
    fp = configureFailPoint(donorPrimary, kMigrationFpNames[index]);
}

const donorRst = tenantMigrationTest.getDonorRst();
const hangInDonorAfterReplicatingKeys =
    configureFailPoint(donorRst.getPrimary(), "pauseTenantMigrationAfterFetchingAndStoringKeys");
const migrationOpts = {
    migrationIdString: extractUUIDFromObject(UUID()),
    tenantId: kTenantId
};
assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));
// We must wait for the migration to have finished replicating the recipient keys on the donor set
// before starting initial sync, otherwise the migration will hang while waiting for initial sync to
// complete. We wait for the keys to be replicated with 'w: all' write concern.
hangInDonorAfterReplicatingKeys.wait();

// Add the initial sync node and make sure that it does not step up. We must add this node before
// sending the first 'recipientSyncData' command to avoid the scenario where a new donor node is
// added in-between 'recipientSyncData' commands to the recipient, prompting a
// 'ConflictingOperationInProgress' error. We do not support reconfigs that add/removes nodes during
// a migration.
const initialSyncNode = donorRst.add({
    rsConfig: {priority: 0, votes: 0},
    setParameter: {"failpoint.initialSyncHangBeforeChoosingSyncSource": tojson({mode: "alwaysOn"})}
});
donorRst.reInitiate();
donorRst.waitForState(initialSyncNode, ReplSetTest.State.STARTUP_2);
// Resume the migration. Wait randomly before resuming initial sync on the new secondary to test
// the various migration states.
hangInDonorAfterReplicatingKeys.off();
sleep(Math.random() * kMaxSleepTimeMS);
if (fp) {
    fp.wait();
}

jsTestLog("Waiting for initial sync to finish: " + initialSyncNode.port);
initialSyncNode.getDB('admin').adminCommand(
    {configureFailPoint: 'initialSyncHangBeforeChoosingSyncSource', mode: "off"});
donorRst.awaitSecondaryNodes();
donorRst.awaitReplication();

// Stop replication on the node so that the TenantMigrationAccessBlocker cannot transition its state
// past what is reflected in the state doc read below.
stopServerReplication(initialSyncNode);

let configDonorsColl = initialSyncNode.getCollection(TenantMigrationTest.kConfigDonorsNS);
let donorDoc = configDonorsColl.findOne({tenantId: kTenantId});
if (donorDoc) {
    jsTestLog("Initial sync completed while migration was in state: " + donorDoc.state);
    switch (donorDoc.state) {
        case TenantMigrationTest.DonorState.kAbortingIndexBuilds:
        case TenantMigrationTest.DonorState.kDataSync:
            assert.soon(() => tenantMigrationTest
                                  .getTenantMigrationAccessBlocker(
                                      {donorNode: initialSyncNode, tenantId: kTenantId})
                                  .donor.state == TenantMigrationTest.DonorAccessState.kAllow);
            break;
        case TenantMigrationTest.DonorState.kBlocking:
            assert.soon(() => tenantMigrationTest
                                  .getTenantMigrationAccessBlocker(
                                      {donorNode: initialSyncNode, tenantId: kTenantId})
                                  .donor.state ==
                            TenantMigrationTest.DonorAccessState.kBlockWritesAndReads);
            assert.soon(() =>
                            bsonWoCompare(tenantMigrationTest
                                              .getTenantMigrationAccessBlocker(
                                                  {donorNode: initialSyncNode, tenantId: kTenantId})
                                              .donor.blockTimestamp,
                                          donorDoc.blockTimestamp) == 0);
            break;
        case TenantMigrationTest.DonorState.kCommitted:
            assert.soon(() => tenantMigrationTest
                                  .getTenantMigrationAccessBlocker(
                                      {donorNode: initialSyncNode, tenantId: kTenantId})
                                  .donor.state == TenantMigrationTest.DonorAccessState.kReject);
            assert.soon(() =>
                            bsonWoCompare(tenantMigrationTest
                                              .getTenantMigrationAccessBlocker(
                                                  {donorNode: initialSyncNode, tenantId: kTenantId})
                                              .donor.commitOpTime,
                                          donorDoc.commitOrAbortOpTime) == 0);
            assert.soon(() =>
                            bsonWoCompare(tenantMigrationTest
                                              .getTenantMigrationAccessBlocker(
                                                  {donorNode: initialSyncNode, tenantId: kTenantId})
                                              .donor.blockTimestamp,
                                          donorDoc.blockTimestamp) == 0);
            break;
        case TenantMigrationTest.DonorState.kAborted:
            assert.soon(() => tenantMigrationTest
                                  .getTenantMigrationAccessBlocker(
                                      {donorNode: initialSyncNode, tenantId: kTenantId})
                                  .donor.state == TenantMigrationTest.DonorAccessState.kAborted);
            assert.soon(() =>
                            bsonWoCompare(tenantMigrationTest
                                              .getTenantMigrationAccessBlocker(
                                                  {donorNode: initialSyncNode, tenantId: kTenantId})
                                              .donor.abortOpTime,
                                          donorDoc.commitOrAbortOpTime) == 0);
            assert.soon(() =>
                            bsonWoCompare(tenantMigrationTest
                                              .getTenantMigrationAccessBlocker(
                                                  {donorNode: initialSyncNode, tenantId: kTenantId})
                                              .donor.blockTimestamp,
                                          donorDoc.blockTimestamp) == 0);
            break;
        default:
            throw new Error(`Invalid state "${state}" from donor doc.`);
    }
}

if (fp) {
    fp.off();
}

restartServerReplication(initialSyncNode);

if (kMigrationFpNames[index] === "abortTenantMigrationBeforeLeavingBlockingState") {
    TenantMigrationTest.assertAborted(
        tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
} else {
    TenantMigrationTest.assertCommitted(
        tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
}
assert.commandWorked(tenantMigrationTest.forgetMigration(migrationOpts.migrationIdString));
tenantMigrationTest.stop();
})();
