/**
 * Tests initial sync's recovery to a tenant migration's in-memory state.
 *
 * Tenant migrations are not expected to be run on servers with ephemeralForTest.
 *
 * @tags: [requires_fcv_47, requires_majority_read_concern, requires_persistence,
 * incompatible_with_eft]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");
load("jstests/libs/parallelTester.js");
load("jstests/replsets/libs/tenant_migration_test.js");

// TODO SERVER-53110: Remove 'enableRecipientTesting: false'.
const tenantMigrationTest =
    new TenantMigrationTest({name: jsTestName(), enableRecipientTesting: false});
if (!tenantMigrationTest.isFeatureFlagEnabled()) {
    jsTestLog("Skipping test because the tenant migrations feature flag is disabled");
    return;
}

const kMaxSleepTimeMS = 1000;
const kTenantId = 'testTenantId';

let donorPrimary = tenantMigrationTest.getDonorPrimary();

// Force the migration to pause after entering a randomly selected state to simulate a failure.
Random.setRandomSeed();
const kMigrationFpNames = [
    "pauseTenantMigrationAfterDataSync",
    "pauseTenantMigrationAfterBlockingStarts",
    "abortTenantMigrationAfterBlockingStarts"
];
let fp;
const index = Random.randInt(kMigrationFpNames.length + 1);
if (index < kMigrationFpNames.length) {
    fp = configureFailPoint(donorPrimary, kMigrationFpNames[index]);
}

const migrationOpts = {
    migrationIdString: extractUUIDFromObject(UUID()),
    tenantId: kTenantId
};
assert.commandWorked(tenantMigrationTest.startMigration(migrationOpts));
sleep(Math.random() * kMaxSleepTimeMS);

// Add the initial sync node and make sure that it does not step up.
const donorRst = tenantMigrationTest.getDonorRst();
const initialSyncNode = donorRst.add({rsConfig: {priority: 0, votes: 0}});

donorRst.reInitiate();
jsTestLog("Waiting for initial sync to finish.");
donorRst.awaitSecondaryNodes();

let configDonorsColl = initialSyncNode.getCollection(TenantMigrationTest.kConfigDonorsNS);
let donorDoc = configDonorsColl.findOne({tenantId: kTenantId});
if (donorDoc) {
    let state = donorDoc.state;
    switch (state) {
        case TenantMigrationTest.State.kDataSync:
            assert.soon(() => tenantMigrationTest
                                  .getTenantMigrationAccessBlocker(initialSyncNode, kTenantId)
                                  .state == TenantMigrationTest.AccessState.kAllow);
            break;
        case TenantMigrationTest.State.kBlocking:
            assert.soon(() => tenantMigrationTest
                                  .getTenantMigrationAccessBlocker(initialSyncNode, kTenantId)
                                  .state == TenantMigrationTest.AccessState.kBlockWritesAndReads);
            assert.soon(
                () => bsonWoCompare(tenantMigrationTest
                                        .getTenantMigrationAccessBlocker(initialSyncNode, kTenantId)
                                        .blockTimestamp,
                                    donorDoc.blockTimestamp) == 0);
            break;
        case TenantMigrationTest.State.kCommitted:
            assert.soon(() => tenantMigrationTest
                                  .getTenantMigrationAccessBlocker(initialSyncNode, kTenantId)
                                  .state == TenantMigrationTest.AccessState.kReject);
            assert.soon(
                () => bsonWoCompare(tenantMigrationTest
                                        .getTenantMigrationAccessBlocker(initialSyncNode, kTenantId)
                                        .commitOrAbortOpTime,
                                    donorDoc.commitOrAbortOpTime) == 0);
            assert.soon(
                () => bsonWoCompare(tenantMigrationTest
                                        .getTenantMigrationAccessBlocker(initialSyncNode, kTenantId)
                                        .blockTimestamp,
                                    donorDoc.blockTimestamp) == 0);
            break;
        case TenantMigrationTest.State.kAborted:
            assert.soon(() => tenantMigrationTest
                                  .getTenantMigrationAccessBlocker(initialSyncNode, kTenantId)
                                  .state == TenantMigrationTest.AccessState.kAborted);
            assert.soon(
                () => bsonWoCompare(tenantMigrationTest
                                        .getTenantMigrationAccessBlocker(initialSyncNode, kTenantId)
                                        .commitOrAbortOpTime,
                                    donorDoc.commitOrAbortOpTime) == 0);
            assert.soon(
                () => bsonWoCompare(tenantMigrationTest
                                        .getTenantMigrationAccessBlocker(initialSyncNode, kTenantId)
                                        .blockTimestamp,
                                    donorDoc.blockTimestamp) == 0);
            break;
        default:
            throw new Error(`Invalid state "${state}" from donor doc.`);
    }
}

if (fp) {
    fp.off();
}

assert.commandWorked(tenantMigrationTest.waitForMigrationToComplete(migrationOpts));
tenantMigrationTest.stop();
})();
